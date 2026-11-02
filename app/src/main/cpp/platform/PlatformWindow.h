// Paint Engine - platform window abstraction.
//
// The graphics layer only needs: what kind of native window this is, the raw
// handle, and its current size. Ownership/ref-counting of the native object
// stays inside the concrete platform implementation.
#pragma once

#include <atomic>
#include <cstdint>

namespace pe {

enum class WindowKind : uint8_t {
    AndroidNativeWindow,  // nativeHandle() is an ANativeWindow*
    Headless,             // no native window: offscreen surface (host tests)
};

inline const char* toString(WindowKind kind) {
    return kind == WindowKind::AndroidNativeWindow ? "ANativeWindow" : "Headless";
}

class PlatformWindow {
public:
    virtual ~PlatformWindow() = default;
    virtual WindowKind kind() const = 0;
    virtual void* nativeHandle() const = 0;
    // Current size of the native window in pixels (0 if unknown).
    virtual uint32_t width() const = 0;
    virtual uint32_t height() const = 0;
};

// Headless window used by host tests. Size is set explicitly to emulate
// surfaceChanged(). Not final: tests derive from it to observe its release.
class HeadlessWindow : public PlatformWindow {
public:
    HeadlessWindow(uint32_t w, uint32_t h) : width_(w), height_(h) {}
    WindowKind kind() const override { return WindowKind::Headless; }
    void* nativeHandle() const override { return nullptr; }
    uint32_t width() const override { return width_.load(); }
    uint32_t height() const override { return height_.load(); }
    void resize(uint32_t w, uint32_t h) { width_.store(w); height_.store(h); }

private:
    std::atomic<uint32_t> width_;
    std::atomic<uint32_t> height_;
};

}  // namespace pe
