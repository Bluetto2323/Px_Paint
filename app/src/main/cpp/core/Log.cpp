#include "core/Log.h"

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>

#include "core/Time.h"

namespace pe {

const char* toString(LogLevel level) {
    switch (level) {
        case LogLevel::Verbose: return "V";
        case LogLevel::Debug: return "D";
        case LogLevel::Info: return "I";
        case LogLevel::Warn: return "W";
        case LogLevel::Error: return "E";
        case LogLevel::Fatal: return "F";
    }
    return "?";
}

const char* toString(LogSource source) {
    switch (source) {
        case LogSource::Engine: return "engine";
        case LogSource::ValidationLayer: return "validation-layer";
        case LogSource::DriverDebug: return "driver-debug";
        case LogSource::Count: break;
    }
    return "?";
}

namespace log {
namespace {

void stderrSink(LogLevel level, const char* tag, const char* message) {
    std::fprintf(stderr, "[%s/%s] %s\n", toString(level), tag, message);
}

constexpr size_t kSources = static_cast<size_t>(LogSource::Count);

std::atomic<LogSink> gSink{&stderrSink};
std::atomic<int> gMinLevel{static_cast<int>(LogLevel::Debug)};
std::array<std::atomic<uint64_t>, kSources> gWarnings{};
std::array<std::atomic<uint64_t>, kSources> gErrors{};
const int64_t gStartNs = nowNs();

constexpr size_t kMaxProblems = 32;
constexpr size_t kMaxLines = 400;
std::mutex gBufferMutex;
std::deque<std::string> gProblems;  // engine problems only
std::deque<std::string> gLines;     // every source, Info+

void vwrite(LogSource source, LogLevel level, const char* tag, const char* fmt, va_list args) {
    const bool problem = level >= LogLevel::Warn;
    const bool keepLine = level >= LogLevel::Info;
    if (static_cast<int>(level) < gMinLevel.load() && !problem && !keepLine) return;

    char stackBuffer[1024];
    va_list argsCopy;
    va_copy(argsCopy, args);
    const int needed = std::vsnprintf(stackBuffer, sizeof(stackBuffer), fmt, args);
    std::string heapBuffer;
    const char* message = stackBuffer;
    if (needed >= static_cast<int>(sizeof(stackBuffer))) {
        heapBuffer.resize(static_cast<size_t>(needed) + 1);
        std::vsnprintf(heapBuffer.data(), heapBuffer.size(), fmt, argsCopy);
        heapBuffer.resize(static_cast<size_t>(needed));
        message = heapBuffer.c_str();
    }
    va_end(argsCopy);

    const size_t s = static_cast<size_t>(source);
    if (problem) (level >= LogLevel::Error ? gErrors[s] : gWarnings[s]).fetch_add(1);
    if (problem || keepLine) {
        char stamp[32];
        std::snprintf(stamp, sizeof(stamp), "+%.3fs ", nsToMs(nowNs() - gStartNs) / 1000.0);
        std::string line = std::string(stamp) + toString(level) + "/" + tag + ": " + message;
        std::lock_guard<std::mutex> lock(gBufferMutex);
        if (problem && source == LogSource::Engine) {
            gProblems.emplace_back(std::string(toString(level)) + "/" + tag + ": " + message);
            while (gProblems.size() > kMaxProblems) gProblems.pop_front();
        }
        gLines.emplace_back(std::move(line));
        while (gLines.size() > kMaxLines) gLines.pop_front();
    }
    if (static_cast<int>(level) >= gMinLevel.load() || problem) gSink.load()(level, tag, message);
}

}  // namespace

void setSink(LogSink sink) { gSink.store(sink ? sink : &stderrSink); }
void setMinLevel(LogLevel level) { gMinLevel.store(static_cast<int>(level)); }
LogLevel minLevel() { return static_cast<LogLevel>(gMinLevel.load()); }

void write(LogLevel level, const char* tag, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vwrite(LogSource::Engine, level, tag, fmt, args);
    va_end(args);
}

void writeFrom(LogSource source, LogLevel level, const char* tag, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vwrite(source, level, tag, fmt, args);
    va_end(args);
}

uint64_t warningCount(LogSource source) { return gWarnings[static_cast<size_t>(source)].load(); }
uint64_t errorCount(LogSource source) { return gErrors[static_cast<size_t>(source)].load(); }

std::vector<std::string> recentProblems(size_t maxCount) {
    std::lock_guard<std::mutex> lock(gBufferMutex);
    const size_t n = std::min(maxCount, gProblems.size());
    return std::vector<std::string>(gProblems.end() - static_cast<long>(n), gProblems.end());
}

std::vector<std::string> recentLines(size_t maxCount) {
    std::lock_guard<std::mutex> lock(gBufferMutex);
    const size_t n = std::min(maxCount, gLines.size());
    return std::vector<std::string>(gLines.end() - static_cast<long>(n), gLines.end());
}

}  // namespace log
}  // namespace pe
