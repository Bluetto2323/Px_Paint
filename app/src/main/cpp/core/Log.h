// Paint Engine - core logging.
//
// The core layer never talks to a platform API directly: the platform layer
// installs a sink (logcat on Android, stderr on host builds).
//
// Every message has a SOURCE so diagnostics can tell real engine problems
// apart from optional tooling output:
//   Engine          - the engine's own warnings/errors (these mean a bug or a
//                     real runtime failure);
//   ValidationLayer - VK_LAYER_KHRONOS_validation messages (optional layer,
//                     only present in validation builds);
//   DriverDebug     - GL_KHR_debug / driver debug output (informational,
//                     vendor specific).
// Counters are kept per source. A ring buffer of recent lines (all sources,
// Info and above) feeds the on-device report, since phones without adb
// cannot read logcat.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pe {

enum class LogLevel : uint8_t { Verbose = 0, Debug, Info, Warn, Error, Fatal };
enum class LogSource : uint8_t { Engine = 0, ValidationLayer, DriverDebug, Count };

const char* toString(LogLevel level);
const char* toString(LogSource source);

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

void writeFrom(LogSource source, LogLevel level, const char* tag, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;

// Warn / Error+ counts since process start, per source (default: Engine).
uint64_t warningCount(LogSource source = LogSource::Engine);
uint64_t errorCount(LogSource source = LogSource::Engine);

// Most recent Engine warnings/errors (newest last).
std::vector<std::string> recentProblems(size_t maxCount);

// Most recent log lines of every source, Info and above, prefixed with the
// time since process start ("+12.345s"), newest last.
std::vector<std::string> recentLines(size_t maxCount);

}  // namespace log
}  // namespace pe

#define PE_LOGV(tag, ...) ::pe::log::write(::pe::LogLevel::Verbose, tag, __VA_ARGS__)
#define PE_LOGD(tag, ...) ::pe::log::write(::pe::LogLevel::Debug, tag, __VA_ARGS__)
#define PE_LOGI(tag, ...) ::pe::log::write(::pe::LogLevel::Info, tag, __VA_ARGS__)
#define PE_LOGW(tag, ...) ::pe::log::write(::pe::LogLevel::Warn, tag, __VA_ARGS__)
#define PE_LOGE(tag, ...) ::pe::log::write(::pe::LogLevel::Error, tag, __VA_ARGS__)
