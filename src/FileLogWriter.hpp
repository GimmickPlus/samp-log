#pragma once

#include "Singleton.hpp"

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>


class FileLogWriter : public Singleton<FileLogWriter>
{
	friend class Singleton<FileLogWriter>;

private:
	struct Entry
	{
		std::string path;
		std::string line;
	};

	struct Target
	{
		std::FILE *fp = nullptr;
		std::string buffer;
		std::uint64_t last_flush_ms = 0;
		bool failed = false;
	};

private:
	FileLogWriter() = default;
	~FileLogWriter();

	FileLogWriter(const FileLogWriter&) = delete;
	FileLogWriter &operator=(const FileLogWriter&) = delete;

public:
	void Start();
	void Stop();

	// Returns false when the queue is full (to avoid stalling the server thread).
	bool Enqueue(std::string path, std::string line);

private:
	void ThreadMain();

	static std::uint64_t NowMs();
	static std::string NormalizePath(std::string path);
	static bool EnsureParentDirs(std::string const &path);

	bool OpenTarget(Target &t, std::string const &path);
	void FlushTarget(Target &t);

private:
	std::mutex m_Mutex;
	std::condition_variable m_Cv;

	std::deque<Entry> m_Queue;
	std::size_t m_QueuedBytes = 0;

	bool m_Running = false;
	bool m_StopRequested = false;
	std::thread m_Thread;

	// Only accessed by the background thread.
	std::unordered_map<std::string, Target> m_Targets;
	std::uint64_t m_Dropped = 0;
};

