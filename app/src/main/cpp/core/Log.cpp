#include "core/Log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>

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

namespace log {
namespace {

void stderrSink(LogLevel level, const char* tag, const char* message) {
    std::fprintf(stderr, "[%s/%s] %s\n", toString(level), tag, message);
}

std::atomic<LogSink> gSink{&stderrSink};
std::atomic<int> gMinLevel{static_cast<int>(LogLevel::Debug)};
std::atomic<uint64_t> gWarnings{0};
std::atomic<uint64_t> gErrors{0};

constexpr size_t kMaxProblems = 32;
std::mutex gProblemMutex;
std::deque<std::string> gProblems;

}  // namespace

void setSink(LogSink sink) { gSink.store(sink ? sink : &stderrSink); }
void setMinLevel(LogLevel level) { gMinLevel.store(static_cast<int>(level)); }
LogLevel minLevel() { return static_cast<LogLevel>(gMinLevel.load()); }

void write(LogLevel level, const char* tag, const char* fmt, ...) {
    const bool problem = level >= LogLevel::Warn;
    if (static_cast<int>(level) < gMinLevel.load() && !problem) return;

    char stackBuffer[1024];
    va_list args;
    va_start(args, fmt);
    va_list argsCopy;
    va_copy(argsCopy, args);
    const int needed = std::vsnprintf(stackBuffer, sizeof(stackBuffer), fmt, args);
    va_end(args);

    std::string heapBuffer;
    const char* message = stackBuffer;
    if (needed >= static_cast<int>(sizeof(stackBuffer))) {
        heapBuffer.resize(static_cast<size_t>(needed) + 1);
        std::vsnprintf(heapBuffer.data(), heapBuffer.size(), fmt, argsCopy);
        heapBuffer.resize(static_cast<size_t>(needed));
        message = heapBuffer.c_str();
    }
    va_end(argsCopy);

    if (problem) {
        (level >= LogLevel::Error ? gErrors : gWarnings).fetch_add(1);
        std::lock_guard<std::mutex> lock(gProblemMutex);
        gProblems.emplace_back(std::string(toString(level)) + "/" + tag + ": " + message);
        while (gProblems.size() > kMaxProblems) gProblems.pop_front();
    }
    gSink.load()(level, tag, message);
}

uint64_t warningCount() { return gWarnings.load(); }
uint64_t errorCount() { return gErrors.load(); }

std::vector<std::string> recentProblems(size_t maxCount) {
    std::lock_guard<std::mutex> lock(gProblemMutex);
    const size_t n = std::min(maxCount, gProblems.size());
    return std::vector<std::string>(gProblems.end() - static_cast<long>(n), gProblems.end());
}

}  // namespace log
}  // namespace pe
