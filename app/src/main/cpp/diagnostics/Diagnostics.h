// Paint Engine - human-readable diagnostics.
//
// Two outputs:
//  * capabilityReport(): the full hardware/driver report (logcat at startup,
//    "LOG REPORT" button, `adb logcat -s PE.Diagnostics`);
//  * statusSummary(): a compact live view for the on-screen overlay.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "diagnostics/SelfTest.h"
#include "graphics/BackendSelector.h"
#include "graphics/abstraction/FrameScheduler.h"
#include "graphics/abstraction/GraphicsCapabilities.h"
#include "graphics/abstraction/GraphicsDevice.h"
#include "graphics/abstraction/ResourceManager.h"

namespace pe {

struct EngineStatus {
    std::string state = "Created";
    gfx::BackendType backend = gfx::BackendType::None;
    std::string backendDescription;
    std::string gpu;
    std::string api;
    std::string driver;
    std::string windowKind;
    gfx::SurfaceState surface;
    gfx::FrameStats frames;
    gfx::RenderMode renderMode = gfx::RenderMode::OnDemand;
    bool diagnostics = false;
    gfx::ResourceStats resources;
    SelfTestResult selfTest;
    std::vector<gfx::BackendAttempt> attempts;
    uint32_t deviceRecoveries = 0;
    double backendInitMs = 0.0;          // backend selection incl. device + first swapchain
    // Engine problems (bugs / real failures)
    uint64_t warnings = 0;
    uint64_t errors = 0;
    // Optional tooling output, NOT engine errors
    bool validationLayerActive = false;
    uint64_t validationErrors = 0;
    uint64_t validationWarnings = 0;
    uint64_t driverDebugMessages = 0;
    std::vector<std::string> recentProblems;
    bool paused = false;
};

namespace diagnostics {

std::string capabilityReport(const gfx::GraphicsCapabilities& caps, const std::string& backendDescription,
                             const std::vector<gfx::BackendAttempt>& attempts);
std::string statusSummary(const EngineStatus& status);
// Machine-readable status for the app (on-device validation runner).
std::string statusJson(const EngineStatus& status);
// Logs a long multi-line text line by line (logcat truncates ~4 KB entries).
void logMultiline(const char* tag, const std::string& text);

}  // namespace diagnostics
}  // namespace pe
