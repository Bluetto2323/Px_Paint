// Paint Engine - the ONLY place where the graphics backend is chosen.
//
// Order: Vulkan first (when allowed), then OpenGL ES 3.x. A candidate is
// accepted only if every stage succeeds:
//   1. initialize()          - library / instance / display
//   2. createDevice()        - GPU + queue selection, capability query
//   3. MinimumRequirements   - verified capabilities, not assumptions
//   4. attachSurface()       - swapchain / window surface really works
// Each attempt (and its failure reason) is recorded for diagnostics.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "graphics/abstraction/GraphicsBackend.h"
#include "graphics/abstraction/GraphicsCapabilities.h"
#include "graphics/abstraction/GraphicsDevice.h"

namespace pe::gfx {

struct BackendAttempt {
    BackendType type = BackendType::None;
    bool success = false;
    std::string failedStage;  // empty on success
    std::string reason;
    double durationMs = 0.0;
};

struct BackendSelection {
    // Declaration order matters: members are destroyed in reverse order, so
    // the device is always destroyed before the backend that created it.
    std::unique_ptr<GraphicsBackend> backend;
    std::unique_ptr<GraphicsDevice> device;
    std::vector<BackendAttempt> attempts;

    explicit operator bool() const { return device != nullptr; }
    void reset() {
        device.reset();
        backend.reset();
    }
};

struct SelectionOptions {
    BackendPreference preference = BackendPreference::Auto;
    bool allowFallback = true;     // with Force*, try the other API on failure
    bool enableValidation = false;
    std::vector<BackendType> excluded;  // backends that already failed at runtime
    MinimumRequirements requirements;
};

class BackendSelector {
public:
    static std::vector<BackendType> candidateOrder(const SelectionOptions& options);
    static BackendSelection select(const SelectionOptions& options, PlatformWindow& window);
    static std::unique_ptr<GraphicsBackend> create(BackendType type);
};

}  // namespace pe::gfx
