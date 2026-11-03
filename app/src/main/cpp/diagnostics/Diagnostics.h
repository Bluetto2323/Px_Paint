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

// Canvas engine state (PxPaint, Block 2). Plain fields: no canvas headers here.
struct CanvasStatus {
    std::string scene = "diagnostic";  // "diagnostic" (Block 1 triangle) | "canvas"
    bool open = false;                 // a canvas document exists
    bool gpuReady = false;             // its GPU storage exists on the current device
    uint32_t width = 0, height = 0;
    std::string storage;               // storage plan / reason
    std::string error;
    uint32_t tileSize = 0, tilesTotal = 0, tilesAllocated = 0;
    uint64_t bytesAllocated = 0;
    uint64_t writes = 0, textureUploads = 0, bytesUploaded = 0;
    double lastWriteMs = 0.0, maxWriteMs = 0.0;
    uint32_t restores = 0;             // GPU copy rebuilt from the CPU raster (new device / backend)
    // CPU raster (authoritative content) and brush engine
    uint64_t rasterTiles = 0, rasterBytes = 0;
    uint32_t layers = 0;                     // Block 6: layer count
    uint64_t compositeTiles = 0;
    double compositeLastMs = 0, compositeMaxMs = 0;
    uint64_t revision = 0;             // content revision (changes on every edit)
    uint64_t brushStrokes = 0, brushCancelled = 0;
    uint64_t brushSamplesReceived = 0, brushSamplesProcessed = 0, brushDabs = 0;
    uint32_t brushPending = 0, brushStrokeTiles = 0;
    double brushLastMs = 0.0, brushMaxMs = 0.0;
    // Brush input -> frame presented (queue time of the oldest sample in the
    // frame to the return of present; the display adds its own scan-out).
    double inputLatencyLastMs = 0.0, inputLatencyAvgMs = 0.0, inputLatencyMaxMs = 0.0;
    uint64_t inputLatencyFrames = 0;
    // Last canvas frame
    uint32_t tilesVisible = 0, piecesDrawn = 0, drawCalls = 0;
    double zoom = 0.0, rotationDeg = 0.0;
    SelfTestResult selfTest;           // canvas GPU self-test
    // Storage limits of the current device (0 = no device yet)
    uint32_t maxTextureSize = 0;
    uint64_t memoryBudgetBytes = 0;
};

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
    CanvasStatus canvas;
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
