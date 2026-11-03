// Paint Engine - top-level engine object.
//
// Threading model
//   * The Engine owns ONE render thread. Every graphics API call (device,
//     resources, frames) happens on it. No GPU object is touched elsewhere.
//   * Public methods are called from the Android UI thread (via JNI) and only
//     enqueue tasks. surfaceDestroyed() is the only blocking call: Android
//     requires the window to be released before the callback returns.
//   * The render thread never waits for the UI thread (no JNI upcalls, no
//     shared lock held across a wait), so the UI->render wait cannot deadlock.
//   * Status for the overlay is published into a mutex-protected snapshot,
//     so polling it never blocks the render thread.
//
// Surface release protocol (surfaceDestroyed)
//   1. UI thread sets `surfaceRevoked_`: the render loop starts no new frame
//      on this surface from now on (a frame already in progress finishes; the
//      Surface is still valid until the Android callback returns).
//   2. UI thread posts a release task and wakes the render thread.
//   3. Render thread: detach (vkDeviceWaitIdle / glFinish, then swapchain or
//      EGLSurface destroyed), THEN releases the ANativeWindow reference, then
//      signals completion (RAII: also on exception).
//   4. UI thread waits WITHOUT timeout for that signal or for the render
//      thread to exit (its teardown performs the same ordered release). A
//      watchdog logs every second while waiting, it never gives up: returning
//      early would let Android destroy a window still in use.
//   If detaching fails, the window reference is deliberately leaked rather
//   than released under a live swapchain/EGLSurface.
//
// Lifecycle
//   surfaceCreated  -> select backend (first time) or attach to existing device
//   surfaceChanged  -> swapchain / surface resize
//   surfaceDestroyed-> detach (device and resources survive)
//   pause / resume  -> stop / restart producing frames
//   shutdown        -> scene, device, window, backend released in order
//
// Render modes (independent of diagnostics)
//   Continuous : a frame every vsync.
//   OnDemand   : a frame only after requestRender() / lifecycle events.
//   Diagnostics: extra checks and logging (self-test, periodic perf log). It
//                works in either mode and never forces continuous rendering.
//
// Scenes
//   Diagnostic : the Block 1 triangle (backend validation, triangle self-test).
//   Canvas     : the PxPaint canvas (CanvasController). The viewport is owned
//                here behind viewMutex_: UI-thread calls change it at once
//                (so touch mapping is synchronous and exact) and the render
//                thread copies it once per frame. See docs/ENGINE_API.md.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "canvas/Viewport.h"
#include "core/TaskQueue.h"
#include "diagnostics/Diagnostics.h"
#include "engine/CanvasController.h"
#include "engine/CanvasDocument.h"
#include "engine/TestScene.h"
#include "graphics/BackendSelector.h"
#include "graphics/abstraction/FrameScheduler.h"
#include "platform/PlatformWindow.h"

namespace pe {

enum class SceneMode : uint8_t { Diagnostic, Canvas };
const char* toString(SceneMode mode);

// Viewport operations (Engine::viewportOp). Arguments in screen pixels / radians.
enum class ViewOp : uint8_t {
    Query = 0,          // ()
    Fit = 1,            // ()
    Center = 2,         // ()
    PanBy = 3,          // (dx, dy)
    ZoomAt = 4,         // (factor, focalX, focalY)
    SetZoomAt = 5,      // (zoom, focalX, focalY)
    RotateAt = 6,       // (radians, focalX, focalY)
    SetRotationAt = 7,  // (radians, focalX, focalY)
    Gesture = 8,        // (prevX, prevY, newX, newY, scale, radians)
};

struct ViewportInfo {
    canvas::ViewportState state;
    double minZoom = 0.0, maxZoom = 0.0;
    uint32_t canvasWidth = 0, canvasHeight = 0;
    uint32_t screenWidth = 0, screenHeight = 0;
    uint64_t revision = 0;
};

struct EngineConfig {
    gfx::BackendPreference backend = gfx::BackendPreference::Auto;
    bool allowFallback = true;
    bool enableValidation = false;
    gfx::RenderMode renderMode = gfx::RenderMode::OnDemand;
    bool diagnostics = true;    // periodic perf log + automatic self-test
    bool autoSelfTest = true;   // (with diagnostics, diagnostic scene) verify the first frame after each attach
    SceneMode scene = SceneMode::Diagnostic;
    // Canvas pixel budget. 0 = derive from the GPU report (Vulkan heaps) or a
    // fixed 512 MiB where the API cannot report memory (GLES). The app passes
    // a value derived from the device's total RAM.
    uint64_t canvasMemoryBudgetBytes = 0;
};

class Engine {
public:
    explicit Engine(const EngineConfig& config);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void start();
    // Idempotent and blocking. Must not be called from the render thread.
    void shutdown();

    // --- Android lifecycle (any thread except the render thread) ----------
    void surfaceCreated(std::unique_ptr<PlatformWindow> window);
    void surfaceChanged(uint32_t width, uint32_t height);
    // Blocks, without timeout, until the render thread no longer uses the
    // window and has released it. Returns true if the render thread released
    // it, false if no render thread was running (nothing could use it).
    bool surfaceDestroyed();
    void pause();
    void resume();

    // --- Modes ---------------------------------------------------------------
    void setRenderMode(gfx::RenderMode mode);
    void setDiagnosticsEnabled(bool enabled);

    // --- Canvas (any thread except the render thread; never blocks) ---------
    void setSceneMode(SceneMode mode);
    // Validates the request now ("" = accepted) and creates the GPU storage
    // on the render thread (result in status().canvas).
    std::string createCanvas(const CanvasSpec& spec);
    // Storage decision for a size on the current device, without creating
    // anything (New Canvas dialog). Not ok + reason if no device is known yet.
    canvas::StoragePlan checkCanvasSize(uint32_t width, uint32_t height) const;
    void closeCanvas();
    void setCanvasColors(uint32_t workspaceArgb, uint32_t paperArgb, bool transparentBackground);
    void clearCanvasContent();
    // Diagnostic stamp: a square centred on the canvas point under
    // (screenX, screenY), replace semantics. False if that point is off the canvas.
    bool stampAt(double screenX, double screenY, uint32_t sizePx, uint32_t argb, canvas::Vec2* canvasPoint);
    // Increases on every content change request (strokes, load, clear, new canvas).
    uint64_t canvasContentRevision() const;
    // Legacy (Block 2) projects: replaces the content with rectangle fills.
    // Ops are clipped to the canvas; returns the number of ops kept.
    size_t loadCanvasContent(const std::vector<ContentOp>& ops);

    // --- Brush strokes (any thread except the render thread; never block) ---
    // A sample is kSampleFloats floats: x, y (SCREEN pixels, mapped to canvas
    // pixels with the viewport at the time of the call), t (ms), pressure
    // (normalised 0..1), tilt (radians), orientation (radians, screen space,
    // converted to canvas space here). NaN = axis not reported by the device.
    // Processed in order at the next frame.
    static constexpr size_t kSampleFloats = 6;
    // False if nothing will be painted (no canvas, or the active layer is hidden / locked).
    bool strokeBegin(const brush::BrushSettings& settings, const float* sample);
    void strokeSamples(const float* samples, size_t count);
    void strokeEnd();
    void strokeCancel();

    // --- Persistence (any thread except the render thread) -----------------
    // See CanvasDocument::exportTiles / importTiles / thumbnail.
    size_t exportCanvasTiles(bool changedOnly, size_t maxTiles, std::vector<uint8_t>& out);
    long importCanvasTiles(const uint8_t* data, size_t size);
    bool canvasThumbnail(uint32_t maxSide, uint32_t& width, uint32_t& height, std::vector<uint32_t>& argb);
    CanvasDocument& document() { return doc_; }

    // --- Layers (any thread; Block 6). Results apply at the next frame. ---
    enum class LayerOp : int32_t {
        Add = 0, Delete = 1, Duplicate = 2, Rename = 3, Move = 4, Visible = 5, Locked = 6, Opacity = 7,
        Blend = 8, Active = 9, MergeDown = 10, Flatten = 11, Clear = 12,
    };
    // `arg`: index / bool / blend mode, `value`: opacity, `text`: name.
    // `outId`: the new layer of Add / Duplicate.
    LayerError layerOp(LayerOp op, uint32_t id, int32_t arg, float value, const std::string& text, uint32_t& outId);
    std::vector<LayerSummary> layers(uint32_t& activeId) const { return doc_.layers(activeId); }
    bool setLayerStack(const std::vector<LayerInfo>& stack, uint32_t activeId);
    size_t exportLayerTiles(uint32_t id, bool changedOnly, size_t maxTiles, std::vector<uint8_t>& out) {
        return doc_.exportLayerTiles(id, changedOnly, maxTiles, out);
    }
    long importLayerTiles(uint32_t id, const uint8_t* data, size_t size);
    bool layerThumbnail(uint32_t id, uint32_t maxSide, uint32_t& width, uint32_t& height, std::vector<uint32_t>& argb) {
        return doc_.layerThumbnail(id, maxSide, width, height, argb);
    }
    void runCanvasSelfTest();

    // --- Undo / redo history (any thread; Block 7). Applies at the next frame.
    enum class HistoryOp : int32_t { Undo = 0, Redo = 1, Clear = 2 };
    // Undo / Redo `count` steps (>= 1). Returns the steps done, or -1 if a
    // step was refused (the document was not in the recorded state: nothing
    // was changed and the history was cleared). Clear returns 0.
    int historyOp(HistoryOp op, int32_t count);
    uint64_t historyCounts() const { return doc_.historyCounts(); }
    std::vector<HistoryEntry> historyEntries() { return doc_.historyEntries(); }
    HistoryStats historyStats() { return doc_.historyStats(); }
    void setHistoryBudget(uint64_t bytes, size_t maxSteps) { doc_.setHistoryBudget(bytes, maxSteps); }

    // Viewport: applied immediately (thread-safe), drawn at the next frame.
    ViewportInfo viewportOp(ViewOp op, const double* args, size_t argCount);
    canvas::Vec2 screenToCanvas(canvas::Vec2 screen) const;
    // Eyedropper at a screen point (view pixels): see CanvasDocument::sampleColor.
    bool sampleColor(double screenX, double screenY, uint32_t& argb) const;
    canvas::Vec2 canvasToScreen(canvas::Vec2 canvasPoint) const;

    // --- Misc --------------------------------------------------------------
    void requestRender();
    void runSelfTest();  // triangle self-test (diagnostic scene)

    EngineStatus status() const;
    std::string statusText() const;
    std::string statusJson() const;
    std::string capabilityReport() const;

    // Tests / tooling: captures the next presented frame (backbuffer, RGBA8
    // top-left). False if the surface cannot be read back or on timeout.
    bool captureFrame(gfx::CaptureResult& out, std::chrono::milliseconds timeout);

    // Blocks until `predicate(status)` is true or timeout (tests / tooling).
    bool waitForStatus(const std::function<bool(const EngineStatus&)>& predicate,
                       std::chrono::milliseconds timeout) const;

private:
    enum class ThreadState { NotStarted, Running, Exited };

    bool onRenderThread() const;

    // Render-thread only ------------------------------------------------------
    void threadMain();
    void teardown();
    bool canRender() const;
    void renderFrame();
    void handleFrameStatus(gfx::FrameStatus status);
    bool selectDevice(std::vector<gfx::BackendType> excluded);
    void destroyDevice();
    void recoverDevice(const char* reason);
    void recordBackendFailure(gfx::BackendType failed);
    void onSurfaceAvailable();
    // Detaches the surface from the device, then releases the window.
    void detachAndReleaseWindow();
    void releaseSurfaceForUi();
    void onRenderThreadException(const char* what);
    void logPerfIfDue();
    void publishStatus(bool force);

    EngineConfig config_;
    TaskQueue queue_;
    std::thread thread_;
    std::atomic<bool> quit_{false};
    std::atomic<bool> surfaceRevoked_{false};
    std::atomic<std::thread::id> renderThreadId_{};  // written by the render thread, read by callers

    // Thread lifecycle (guarded by lifecycleMutex_)
    mutable std::mutex lifecycleMutex_;
    std::condition_variable lifecycleCv_;
    ThreadState threadState_ = ThreadState::NotStarted;

    // Render-thread state
    gfx::BackendSelection selection_;
    std::unique_ptr<PlatformWindow> window_;
    bool surfaceAttached_ = false;
    bool paused_ = false;
    TestScene scene_;
    SceneMode sceneMode_ = SceneMode::Diagnostic;  // render-thread copy
    CanvasDocument doc_;  // shared, self-synchronised (declared before canvas_, which refers to it)
    CanvasController canvas_;
    bool canvasCaptureInFlight_ = false;
    bool debugCaptureRequested_ = false;  // render thread
    bool debugCaptureInFlight_ = false;
    std::mutex debugCaptureMutex_;
    std::condition_variable debugCaptureCv_;
    uint64_t debugCaptureSerial_ = 0;     // guarded by debugCaptureMutex_
    bool debugCaptureOk_ = false;
    gfx::CaptureResult debugCapture_;
    gfx::FrameScheduler scheduler_;
    std::vector<gfx::BackendType> failedBackends_;
    uint32_t backendFailures_[3] = {0, 0, 0};      // indexed by BackendType, decays over time
    int64_t lastBackendFailureNs_[3] = {0, 0, 0};
    std::vector<int64_t> recentRecoveriesNs_;         // burst detection
    std::vector<gfx::BackendAttempt> lastAttempts_;
    uint32_t consecutiveErrors_ = 0;
    uint32_t consecutiveOutOfDate_ = 0;
    uint32_t consecutiveExceptions_ = 0;
    uint32_t deviceRecoveries_ = 0;
    bool renderingDisabled_ = false;
    std::string pendingRecovery_;
    bool selfTestRequested_ = false;
    bool selfTestCaptureInFlight_ = false;
    SelfTestResult selfTest_;
    std::string state_ = "Created";
    std::string capabilityReport_;
    int64_t lastPublishNs_ = 0;
    bool statusDirty_ = false;
    int64_t lastPerfLogNs_ = 0;
    uint64_t lastPerfLogFrames_ = 0;

    // Viewport (any thread, guarded)
    mutable std::mutex viewMutex_;
    canvas::Viewport view_;

    // Published snapshot
    mutable std::mutex statusMutex_;
    mutable std::condition_variable statusCv_;
    EngineStatus status_;
    std::string publishedReport_;
};

}  // namespace pe
