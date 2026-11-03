// PxPaint - canvas document + GPU canvas on the render thread.
//
// Owned by the Engine and used ONLY on the render thread. It holds:
//   * the appearance of the open canvas (paper, workspace colour),
//   * the GPU copy of the document's pixels (CanvasSurface) and the renderer,
//   * the canvas GPU self-test state machine.
//
// The content itself lives in the CanvasDocument (CPU raster store + brush
// engine). Every frame the controller lets the document apply queued stroke
// input and uploads the dirty regions; after a device loss, a backend switch
// or a GLES context loss it simply re-uploads every allocated tile.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "canvas/CanvasRenderer.h"
#include "canvas/CanvasSelfTest.h"
#include "canvas/CanvasSurface.h"
#include "canvas/TileGrid.h"
#include "canvas/Viewport.h"
#include "diagnostics/Diagnostics.h"
#include "engine/CanvasDocument.h"
#include "graphics/abstraction/GraphicsDevice.h"

namespace pe {

struct CanvasSpec {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t paperArgb = 0xFFFFFFFF;  // straight ARGB
    bool transparentBackground = false;
};

class CanvasController {
public:
    explicit CanvasController(CanvasDocument& document) : doc_(document) {}

    // Storage limits of a device (real GPU limits + a memory budget).
    // `budgetOverride` (bytes, 0 = none) replaces the API-derived budget.
    static canvas::StorageLimits limitsFor(const gfx::GraphicsCapabilities& caps, uint64_t budgetOverride = 0);
    void setMemoryBudget(uint64_t bytes) { budgetOverride_ = bytes; }

    void open(const CanvasSpec& spec);
    void close();
    bool isOpen() const { return open_; }
    void setWorkspaceColor(uint32_t argb);
    void setPaper(uint32_t argb, bool transparentBackground);
    // Square of `sizePx` centred on a canvas point, clipped to the canvas
    // (empty if it falls outside). Diagnostic stamps (Engine::stampAt).
    static canvas::RectI stampRect(double canvasX, double canvasY, uint32_t sizePx, uint32_t canvasWidth,
                                   uint32_t canvasHeight);
    // The last frame's GPU work may not have executed (submit failed, frame
    // abandoned): rebuild the GPU copy and re-upload everything next frame.
    void invalidateGpuContent() {
        surface_.destroy();
        needFullUpload_ = true;
    }

    // GPU objects are bound to the current device: drop them before it goes.
    void releaseGpu();

    // Renders the canvas (or the self-test canvas while it runs). Returns
    // false if GPU objects could not be created (the frame is still closed).
    bool render(gfx::GraphicsDevice& device, gfx::RenderingContext& ctx, gfx::RenderTarget& target,
                const canvas::Viewport& viewport);

    // --- GPU self-test ------------------------------------------------------
    void requestSelfTest();
    bool selfTestActive() const { return testStep_ >= 0; }
    // After render(): does this frame need a capture for the self-test?
    bool selfTestWantsCapture() const { return testStep_ >= 0 && testRendered_; }
    void onSelfTestCapture(bool produced, const gfx::CaptureResult& capture);
    void onSelfTestUnsupported(const char* why);
    void retrySelfTestStep() { testRendered_ = false; }
    void abortSelfTest(const char* why);

    // The frame rendered by the last render() call was presented at `nowNs`:
    // closes the input-to-display latency measurement of its stroke input.
    void onFramePresented(int64_t nowNs);

    void fillStatus(CanvasStatus& out) const;

private:
    bool ensureGpu(gfx::GraphicsDevice& device, gfx::TextureFormat format);
    void finishSelfTest();

    uint64_t budgetOverride_ = 0;
    bool open_ = false;
    CanvasSpec spec_;
    CanvasDocument& doc_;
    uint64_t surfaceGen_ = ~0ull;  // document generation the GPU copy belongs to
    bool needFullUpload_ = true;
    std::vector<canvas::RectI> dirty_;
    // Input -> presented latency of brush input (render thread).
    int64_t frameInputNs_ = 0;
    double latencyLastMs_ = 0.0, latencyMaxMs_ = 0.0, latencyAvgMs_ = 0.0;
    uint64_t latencySamples_ = 0;
    canvas::CanvasAppearance look_;
    canvas::CanvasSurface surface_;
    canvas::CanvasRenderer renderer_;
    bool gpuFailed_ = false;       // storage could not be created on this device
    std::string error_;
    std::string storageText_;
    uint32_t restores_ = 0;
    canvas::CanvasFrameStats frame_;
    double zoom_ = 0.0, rotation_ = 0.0;

    // Self-test
    int testStep_ = -1;            // -1 = idle
    bool testRendered_ = false;    // current step rendered, waiting for its capture
    bool testLateApplied_ = false;
    canvas::CanvasSurface testTiled_, testSingle_;
    canvas::Viewport testViewport_;
    std::string testDetail_;
    bool testFailed_ = false;
    SelfTestResult selfTest_;
};

}  // namespace pe
