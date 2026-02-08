#pragma once

#include <samplog/samplog.hpp>
#include "Singleton.hpp"
#include "sdk.hpp"

#include <string>
#include <unordered_map>


class Logger
{
public:
	using Id = unsigned int;

public:
	Logger(std::string name, samplog::Logger_t &&logger, bool debuginfo) :
		m_Name(std::move(name)),
		m_Logger(std::move(logger)),
		m_DebugInfos(debuginfo)
	{ }
	~Logger() = default;

	Logger(const Logger&) = delete;
	Logger &operator=(const Logger&) = delete;
	Logger(Logger &&logger) :
		m_Name(std::move(logger.m_Name)),
		m_Logger(std::move(logger.m_Logger)),
		m_DebugInfos(logger.m_DebugInfos)
	{ }
	Logger &operator=(Logger &&logger)
	{
		if (this != &logger)
		{
			m_Name = std::move(logger.m_Name);
			m_Logger = std::move(logger.m_Logger);
			m_DebugInfos = logger.m_DebugInfos;
		}
		return *this;
	}

public:
	bool Log(samplog::LogLevel level, std::string &&msg, AMX *amx);

	inline bool IsLogLevel(samplog::LogLevel log_level) const
	{
		return m_Logger->IsLogLevel(log_level);
	}

private:
	std::string m_Name;
	samplog::Logger_t m_Logger;

	bool m_DebugInfos;

};


class LogManager : public Singleton<LogManager>
{
	friend class Singleton<LogManager>;
private:
	LogManager() = default;
	~LogManager() = default;

private:
	std::unordered_map<Logger::Id, Logger> m_Logs;

public:
	Logger::Id Create(std::string logname, bool debuginfo);
	inline bool Destroy(Logger::Id logid)
	{
		return m_Logs.erase(logid) == 1;
	}

	inline bool IsValid(Logger::Id logid)
	{
		return m_Logs.find(logid) != m_Logs.end();
	}
	inline Logger &GetLogger(Logger::Id logid)
	{
		return m_Logs.at(logid);
	}
};
