#pragma once

#include "platform/PlatformWindow.h"

struct ANativeWindow;

namespace pe {

// Owns one reference to an ANativeWindow. The reference is acquired by the
// caller of the constructor (ANativeWindow_fromSurface already returns an
// acquired reference) and released in the destructor, which the engine only
// runs on the render thread after the swapchain / EGLSurface is gone.
class AndroidWindow final : public PlatformWindow {
public:
    explicit AndroidWindow(ANativeWindow* acquiredWindow);
    ~AndroidWindow() override;

    AndroidWindow(const AndroidWindow&) = delete;
    AndroidWindow& operator=(const AndroidWindow&) = delete;

    WindowKind kind() const override { return WindowKind::AndroidNativeWindow; }
    void* nativeHandle() const override { return window_; }
    uint32_t width() const override;
    uint32_t height() const override;

private:
    ANativeWindow* window_;
};

// Routes engine logs to logcat (tag prefix "PE.").
void installAndroidLogSink();

}  // namespace pe
