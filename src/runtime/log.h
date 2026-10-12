#pragma once
#include <cstdarg>
#include <cstdint>

namespace rt {

enum class LogLevel { Trace, Debug, Info, Warn, Error };

void log_init(const char *path);
void log_set_level(LogLevel level);
LogLevel log_level();
void log_write(LogLevel level, const char *tag, const char *fmt, ...);
void log_writev(LogLevel level, const char *tag, const char *fmt, va_list ap);
void log_flush();

[[noreturn]] void fatal(const char *fmt, ...);

} // namespace rt

#define LOG_TRACE(tag, ...) do { if (rt::log_level() <= rt::LogLevel::Trace) rt::log_write(rt::LogLevel::Trace, tag, __VA_ARGS__); } while (0)
#define LOG_DEBUG(tag, ...) do { if (rt::log_level() <= rt::LogLevel::Debug) rt::log_write(rt::LogLevel::Debug, tag, __VA_ARGS__); } while (0)
#define LOG_INFO(tag, ...)  rt::log_write(rt::LogLevel::Info, tag, __VA_ARGS__)
#define LOG_WARN(tag, ...)  rt::log_write(rt::LogLevel::Warn, tag, __VA_ARGS__)
#define LOG_ERROR(tag, ...) rt::log_write(rt::LogLevel::Error, tag, __VA_ARGS__)
