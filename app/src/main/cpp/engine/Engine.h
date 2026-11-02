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
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/TaskQueue.h"
#include "diagnostics/Diagnostics.h"
#include "engine/TestScene.h"
#include "graphics/BackendSelector.h"
#include "graphics/abstraction/FrameScheduler.h"
#include "platform/PlatformWindow.h"

namespace pe {

struct EngineConfig {
    gfx::BackendPreference backend = gfx::BackendPreference::Auto;
    bool allowFallback = true;
    bool enableValidation = false;
    gfx::RenderMode renderMode = gfx::RenderMode::OnDemand;
    bool diagnostics = true;    // periodic perf log + automatic self-test
    bool autoSelfTest = true;   // (with diagnostics) verify the first frame after each attach
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

    // --- Misc --------------------------------------------------------------
    void requestRender();
    void runSelfTest();

    EngineStatus status() const;
    std::string statusText() const;
    std::string statusJson() const;
    std::string capabilityReport() const;

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

    // Published snapshot
    mutable std::mutex statusMutex_;
    mutable std::condition_variable statusCv_;
    EngineStatus status_;
    std::string publishedReport_;
};

}  // namespace pe
