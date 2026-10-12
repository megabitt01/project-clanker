#include "log.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace rt {

static FILE *g_file;
static std::mutex g_mutex;
static LogLevel g_level = LogLevel::Info;
static LARGE_INTEGER g_start, g_freq;

void log_init(const char *path)
{
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_start);
    g_file = std::fopen(path, "w");
    if (const char *lv = std::getenv("SWBF2_LOG")) {
        if (!_stricmp(lv, "trace")) g_level = LogLevel::Trace;
        else if (!_stricmp(lv, "debug")) g_level = LogLevel::Debug;
        else if (!_stricmp(lv, "warn")) g_level = LogLevel::Warn;
    }
}

void log_set_level(LogLevel level) { g_level = level; }
LogLevel log_level() { return g_level; }

void log_writev(LogLevel level, const char *tag, const char *fmt, va_list ap)
{
    if (level < g_level)
        return;
    static const char *names[] = {"TRC", "DBG", "INF", "WRN", "ERR"};
    char msg[2048];
    vsnprintf(msg, sizeof msg, fmt, ap);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double t = double(now.QuadPart - g_start.QuadPart) / double(g_freq.QuadPart);
    char line[2300];
    int n = snprintf(line, sizeof line, "[%9.3f %5lu %s %-6s] %s\n", t, GetCurrentThreadId(),
                     names[int(level)], tag, msg);
    std::lock_guard<std::mutex> lock(g_mutex);
    fwrite(line, 1, n, stdout);
    if (g_file) {
        fwrite(line, 1, n, g_file);
        if (level >= LogLevel::Warn)
            fflush(g_file);
    }
}

void log_write(LogLevel level, const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_writev(level, tag, fmt, ap);
    va_end(ap);
}

void log_flush()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    fflush(stdout);
    if (g_file)
        fflush(g_file);
}

void fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_writev(LogLevel::Error, "fatal", fmt, ap);
    va_end(ap);
    log_flush();
    char msg[1024];
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    MessageBoxA(nullptr, msg, "Star Wars Battlefront II - fatal error", MB_ICONERROR);
    TerminateProcess(GetCurrentProcess(), 1);
    for (;;) {}
}

} // namespace rt
