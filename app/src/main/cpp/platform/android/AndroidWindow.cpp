#include "platform/android/AndroidWindow.h"

#include <android/log.h>
#include <android/native_window.h>

#include <cstdio>

#include "core/Log.h"

namespace pe {

AndroidWindow::AndroidWindow(ANativeWindow* acquiredWindow) : window_(acquiredWindow) {}

AndroidWindow::~AndroidWindow() {
    if (window_) ANativeWindow_release(window_);
}

uint32_t AndroidWindow::width() const {
    const int32_t w = window_ ? ANativeWindow_getWidth(window_) : 0;
    return w > 0 ? static_cast<uint32_t>(w) : 0u;
}

uint32_t AndroidWindow::height() const {
    const int32_t h = window_ ? ANativeWindow_getHeight(window_) : 0;
    return h > 0 ? static_cast<uint32_t>(h) : 0u;
}

namespace {

int toAndroidPriority(LogLevel level) {
    switch (level) {
        case LogLevel::Verbose: return ANDROID_LOG_VERBOSE;
        case LogLevel::Debug: return ANDROID_LOG_DEBUG;
        case LogLevel::Info: return ANDROID_LOG_INFO;
        case LogLevel::Warn: return ANDROID_LOG_WARN;
        case LogLevel::Error: return ANDROID_LOG_ERROR;
        case LogLevel::Fatal: return ANDROID_LOG_FATAL;
    }
    return ANDROID_LOG_INFO;
}

void logcatSink(LogLevel level, const char* tag, const char* message) {
    char fullTag[64];
    std::snprintf(fullTag, sizeof(fullTag), "PE.%s", tag);
    __android_log_write(toAndroidPriority(level), fullTag, message);
}

}  // namespace

void installAndroidLogSink() { log::setSink(&logcatSink); }

}  // namespace pe
