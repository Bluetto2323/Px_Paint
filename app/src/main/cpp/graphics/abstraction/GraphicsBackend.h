// Paint Engine - a graphics API entry point (Vulkan instance, EGL display).
//
// A backend is initialized once, then creates at most one GraphicsDevice. It
// must outlive that device (BackendSelection enforces the order).
#pragma once

#include <memory>
#include <string>

#include "graphics/abstraction/GraphicsDevice.h"
#include "graphics/abstraction/GraphicsTypes.h"
#include "platform/PlatformWindow.h"

namespace pe::gfx {

struct BackendInitOptions {
    // Enables Khronos validation layer (Vulkan) / debug context (GLES) when
    // available. Never required: missing layers only produce a warning.
    bool enableValidation = false;
    // Kind of window the device will present to (selects surface extensions).
    WindowKind windowKind = WindowKind::AndroidNativeWindow;
    const char* applicationName = "PaintEngine";
};

class GraphicsBackend {
public:
    virtual ~GraphicsBackend() = default;

    virtual BackendType type() const = 0;

    // Loads the API and creates the instance/display. On failure returns
    // false and fills `error`; the object must still be safely destructible.
    virtual bool initialize(const BackendInitOptions& options, std::string& error) = 0;

    // Creates the logical device. `window` (optional) lets the backend pick a
    // GPU/queue able to present to it. Capabilities are queried here.
    virtual std::unique_ptr<GraphicsDevice> createDevice(PlatformWindow* window, std::string& error) = 0;

    // Instance-level description for diagnostics (loader version, layers...).
    virtual std::string describe() const = 0;
};

}  // namespace pe::gfx
