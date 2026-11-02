// Paint Engine - core logging.
//
// The core layer never talks to a platform API directly: the platform layer
// installs a sink (logcat on Android, stderr on host builds). Errors and
// warnings are additionally kept in a small ring buffer so the diagnostics
// overlay can show them without parsing logcat.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pe {

enum class LogLevel : uint8_t { Verbose = 0, Debug, Info, Warn, Error, Fatal };

const char* toString(LogLevel level);

// A sink receives fully formatted messages. It must be thread-safe.
using LogSink = void (*)(LogLevel level, const char* tag, const char* message);

namespace log {

void setSink(LogSink sink);
void setMinLevel(LogLevel level);
LogLevel minLevel();

void write(LogLevel level, const char* tag, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

// Diagnostics helpers: number of Warn/Error+ messages since start and the most
// recent ones (newest last).
uint64_t warningCount();
uint64_t errorCount();
std::vector<std::string> recentProblems(size_t maxCount);

}  // namespace log
}  // namespace pe

#define PE_LOGV(tag, ...) ::pe::log::write(::pe::LogLevel::Verbose, tag, __VA_ARGS__)
#define PE_LOGD(tag, ...) ::pe::log::write(::pe::LogLevel::Debug, tag, __VA_ARGS__)
#define PE_LOGI(tag, ...) ::pe::log::write(::pe::LogLevel::Info, tag, __VA_ARGS__)
#define PE_LOGW(tag, ...) ::pe::log::write(::pe::LogLevel::Warn, tag, __VA_ARGS__)
#define PE_LOGE(tag, ...) ::pe::log::write(::pe::LogLevel::Error, tag, __VA_ARGS__)
