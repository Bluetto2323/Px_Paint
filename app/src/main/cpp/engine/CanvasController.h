// PxPaint - canvas document + GPU canvas on the render thread.
//
// Owned by the Engine and used ONLY on the render thread. It holds:
//   * the open canvas document (size, paper, content journal),
//   * its GPU storage (CanvasSurface) and the renderer,
//   * the canvas GPU self-test state machine.
//
// Content journal: every content write (Block 2: diagnostic stamps only) is
// recorded as a small operation, so the GPU storage can be rebuilt after a
// device loss, a backend switch or a GLES context loss. It is also the seed
// of the future undo history (not implemented in Block 2).
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
    static constexpr size_t kMaxJournalOps = 100000;

    // Storage limits of a device (real GPU limits + a memory budget).
    // `budgetOverride` (bytes, 0 = none) replaces the API-derived budget.
    static canvas::StorageLimits limitsFor(const gfx::GraphicsCapabilities& caps, uint64_t budgetOverride = 0);
    void setMemoryBudget(uint64_t bytes) { budgetOverride_ = bytes; }

    void open(const CanvasSpec& spec);
    void close();
    bool isOpen() const { return open_; }
    void setWorkspaceColor(uint32_t argb);
    void setPaper(uint32_t argb, bool transparentBackground);
    // Diagnostic content write (square stamp centred on a canvas point).
    bool stamp(double canvasX, double canvasY, uint32_t sizePx, uint32_t argb);
    void clearContent();

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

    void fillStatus(CanvasStatus& out) const;

private:
    struct FillOp {
        canvas::RectI rect;
        canvas::Rgba8 color;
    };
    bool ensureGpu(gfx::GraphicsDevice& device, gfx::TextureFormat format);
    void finishSelfTest();

    uint64_t budgetOverride_ = 0;
    bool open_ = false;
    CanvasSpec spec_;
    std::vector<FillOp> journal_;
    std::vector<FillOp> pending_;  // written to the GPU at the next frame
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
