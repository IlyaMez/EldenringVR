#include "log.h"
#include <windows.h>
#include <cstdio>
#include <mutex>
#include <ctime>

namespace ervr {
namespace {
FILE* g_file = nullptr;
LogLevel g_min = LogLevel::Info;
std::mutex g_mtx;
const char* Name(LogLevel l) {
    switch (l) { case LogLevel::Debug: return "DEBUG"; case LogLevel::Info: return "INFO "; case LogLevel::Warn: return "WARN "; default: return "ERROR"; }
}
}

void LogInit(const std::wstring& path, LogLevel minLevel) {
    std::lock_guard<std::mutex> g(g_mtx);
    g_min = minLevel;
    if (g_file) fclose(g_file);
    g_file = _wfopen(path.c_str(), L"w");
}

void LogShutdown() {
    std::lock_guard<std::mutex> g(g_mtx);
    if (g_file) { fclose(g_file); g_file = nullptr; }
}

void Log(LogLevel lvl, const char* fmt, ...) {
    if (lvl < g_min) return;
    char msg[2048];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    SYSTEMTIME st; GetLocalTime(&st);
    char line[2200];
    snprintf(line, sizeof(line), "%02d:%02d:%02d:%03d [%5lu] | %s | %s\n",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentThreadId(), Name(lvl), msg);
    std::lock_guard<std::mutex> g(g_mtx);
    if (g_file) { fputs(line, g_file); fflush(g_file); }
    OutputDebugStringA(line);
}
}
