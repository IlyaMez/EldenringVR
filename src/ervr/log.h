#pragma once
#include <string>
#include <cstdarg>

namespace ervr {
enum class LogLevel { Debug = 0, Info, Warn, Error };
void LogInit(const std::wstring& path, LogLevel minLevel);
void LogShutdown();
void Log(LogLevel lvl, const char* fmt, ...);
}

#define LOGD(...) ::ervr::Log(::ervr::LogLevel::Debug, __VA_ARGS__)
#define LOGI(...) ::ervr::Log(::ervr::LogLevel::Info,  __VA_ARGS__)
#define LOGW(...) ::ervr::Log(::ervr::LogLevel::Warn,  __VA_ARGS__)
#define LOGE(...) ::ervr::Log(::ervr::LogLevel::Error, __VA_ARGS__)
