#include "FileLogWriter.hpp"
#include "PluginLog.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>

#if defined(_WIN32)
#	include <windows.h>
#else
#	include <sys/stat.h>
#	include <sys/types.h>
#endif


static const std::size_t kMaxQueuedBytes = 8 * 1024 * 1024;  // 8 MiB
static const std::size_t kFlushThresholdBytes = 64 * 1024;   // per file buffer threshold
static const std::uint64_t kFlushIntervalMs = 250;           // periodic flush interval
static const std::uint64_t kWakeIntervalMs = 50;             // queue polling interval


FileLogWriter::~FileLogWriter()
{
	Stop();
}

void FileLogWriter::Start()
{
	std::lock_guard<std::mutex> lk(m_Mutex);
	if (m_Running)
		return;

	m_StopRequested = false;
	m_Running = true;
	m_Thread = std::thread(&FileLogWriter::ThreadMain, this);
}

void FileLogWriter::Stop()
{
	{
		std::lock_guard<std::mutex> lk(m_Mutex);
		if (!m_Running)
			return;
		m_StopRequested = true;
	}
	m_Cv.notify_all();

	if (m_Thread.joinable())
		m_Thread.join();

	std::lock_guard<std::mutex> lk(m_Mutex);
	m_Running = false;
}

bool FileLogWriter::Enqueue(std::string path, std::string line)
{
	path = NormalizePath(std::move(path));
	if (path.empty() || line.empty())
		return false;

	std::lock_guard<std::mutex> lk(m_Mutex);
	if (!m_Running)
		return false;

	const std::size_t add_bytes = path.size() + line.size();
	if (m_QueuedBytes + add_bytes > kMaxQueuedBytes)
	{
		++m_Dropped;
		return false;
	}

	m_QueuedBytes += add_bytes;
	m_Queue.push_back(Entry{ std::move(path), std::move(line) });
	m_Cv.notify_one();
	return true;
}

std::uint64_t FileLogWriter::NowMs()
{
	using namespace std::chrono;
	return static_cast<std::uint64_t>(
		duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

std::string FileLogWriter::NormalizePath(std::string path)
{
	// Normalize to forward slashes for map keys.
	std::replace(path.begin(), path.end(), '\\', '/');

	// Collapse '//' to '/' (best effort).
	std::string out;
	out.reserve(path.size());
	char prev = '\0';
	for (char c : path)
	{
		if (c == '/' && prev == '/')
			continue;
		out.push_back(c);
		prev = c;
	}
	return out;
}

static bool DirExists(std::string const &path)
{
#if defined(_WIN32)
	DWORD attr = GetFileAttributesA(path.c_str());
	return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
	struct stat st;
	return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static bool MkdirOne(std::string const &path)
{
#if defined(_WIN32)
	if (CreateDirectoryA(path.c_str(), NULL) != 0)
		return true;
	DWORD err = GetLastError();
	return err == ERROR_ALREADY_EXISTS;
#else
	if (mkdir(path.c_str(), 0755) == 0)
		return true;
	return errno == EEXIST;
#endif
}

bool FileLogWriter::EnsureParentDirs(std::string const &path)
{
	const std::size_t last_sep = path.find_last_of("/\\");
	if (last_sep == std::string::npos)
		return true; // no directory component

	std::string dir = path.substr(0, last_sep);
	if (dir.empty())
		return true;

	// Handle Windows drive prefix (e.g. "C:/").
	std::size_t i = 0;
	std::string current;
	current.reserve(dir.size());
	if (dir.size() >= 3 && std::isalpha(static_cast<unsigned char>(dir[0])) && dir[1] == ':' && dir[2] == '/')
	{
		current.assign(dir.begin(), dir.begin() + 3);
		i = 3;
	}
	else if (!dir.empty() && dir[0] == '/')
	{
		current.push_back('/');
		i = 1;
	}

	for (; i < dir.size(); ++i)
	{
		const char c = dir[i];
		current.push_back(c);
		if (c != '/')
			continue;

		if (current.size() <= 1)
			continue;

		// Avoid passing a trailing separator to mkdir/CreateDirectory on Windows.
		std::string to_create = current;
		if (!to_create.empty() && to_create.back() == '/')
			to_create.pop_back();

		if (to_create.empty())
			continue;

		if (!DirExists(to_create))
		{
			if (!MkdirOne(to_create))
				return false;
		}
	}

	// Final component has no trailing '/'.
	if (!current.empty() && current.back() == '/')
		current.pop_back();

	if (!current.empty() && !DirExists(current))
	{
		if (!MkdirOne(current))
			return false;
	}
	return true;
}

bool FileLogWriter::OpenTarget(Target &t, std::string const &path)
{
	if (t.fp != nullptr || t.failed)
		return t.fp != nullptr;

	EnsureParentDirs(path);

	t.fp = std::fopen(path.c_str(), "ab");
	if (!t.fp)
	{
		t.failed = true;
		return false;
	}

	// Increase stdio buffering to reduce syscalls.
	std::setvbuf(t.fp, NULL, _IOFBF, 64 * 1024);
	t.last_flush_ms = NowMs();
	return true;
}

void FileLogWriter::FlushTarget(Target &t)
{
	if (!t.fp || t.buffer.empty())
		return;

	const std::size_t n = std::fwrite(t.buffer.data(), 1, t.buffer.size(), t.fp);
	(void)n; // best-effort
	std::fflush(t.fp);
	t.buffer.clear();
	t.last_flush_ms = NowMs();
}

void FileLogWriter::ThreadMain()
{
	std::deque<Entry> local;
	std::uint64_t last_global_flush = NowMs();

	for (;;)
	{
		{
			std::unique_lock<std::mutex> lk(m_Mutex);
			m_Cv.wait_for(lk, std::chrono::milliseconds(kWakeIntervalMs), [&]() {
				return m_StopRequested || !m_Queue.empty();
			});

			if (!m_Queue.empty())
			{
				local.swap(m_Queue);
				m_QueuedBytes = 0;
			}

			if (m_StopRequested && local.empty() && m_Queue.empty())
				break;
		}

		for (auto &e : local)
		{
			auto &t = m_Targets[e.path]; // creates if missing
			if (!OpenTarget(t, e.path))
				continue;

			t.buffer.append(e.line);
			if (t.buffer.size() >= kFlushThresholdBytes)
				FlushTarget(t);
		}
		local.clear();

		const std::uint64_t now = NowMs();
		if (now - last_global_flush >= kFlushIntervalMs)
		{
			for (auto &kv : m_Targets)
			{
				auto &t = kv.second;
				if (t.fp && !t.buffer.empty() && (now - t.last_flush_ms) >= kFlushIntervalMs)
					FlushTarget(t);
			}
			last_global_flush = now;

			if (m_Dropped != 0 && PluginLog::Get()->IsLogLevel(LogLevel::WARNING))
			{
				PluginLog::Get()->Log(LogLevel::WARNING, "FileLogWriter dropped {} log lines due to a full queue", m_Dropped);
				m_Dropped = 0;
			}
		}
	}

	// Final flush/close.
	for (auto &kv : m_Targets)
	{
		auto &t = kv.second;
		FlushTarget(t);
		if (t.fp)
		{
			std::fclose(t.fp);
			t.fp = nullptr;
		}
	}
	m_Targets.clear();
}
