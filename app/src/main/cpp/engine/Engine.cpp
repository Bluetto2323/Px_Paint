#include "engine/Engine.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <system_error>

#include "core/Log.h"
#include "core/TestHooks.h"
#include "core/Time.h"

namespace pe {

namespace {
constexpr const char* kTag = "Engine";
constexpr uint32_t kMaxConsecutiveErrors = 5;
constexpr uint32_t kMaxConsecutiveExceptions = 3;
constexpr int64_t kPerfLogPeriodNs = 5'000'000'000;
// Two failures of the same backend within this window exclude it (if another
// backend remains). Failures further apart are treated as unrelated.
constexpr int64_t kFailureWindowNs = 60'000'000'000;
// More recoveries than this within kFailureWindowNs: stop retrying.
constexpr size_t kMaxRecoveriesPerWindow = 6;
}  // namespace

Engine::Engine(const EngineConfig& config) : config_(config) { scheduler_.setMode(config.renderMode); }

Engine::~Engine() { shutdown(); }

bool Engine::onRenderThread() const { return std::this_thread::get_id() == renderThreadId_.load(); }

void Engine::start() {
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        if (threadState_ != ThreadState::NotStarted) return;
        threadState_ = ThreadState::Running;
    }
    quit_ = false;
    PE_LOGI(kTag, "Starting (backend=%s, fallback=%s, validation=%s, render=%s, diagnostics=%s)",
            gfx::toString(config_.backend), config_.allowFallback ? "on" : "off",
            config_.enableValidation ? "on" : "off", gfx::toString(config_.renderMode),
            config_.diagnostics ? "on" : "off");
    queue_.setExceptionHandler([this](const char* what) {
        // Runs on the render thread right after the failing task.
        pendingRecovery_ = std::string("exception in lifecycle task: ") + what;
    });
    try {
        thread_ = std::thread([this] { threadMain(); });
    } catch (const std::system_error& e) {
        PE_LOGE(kTag, "Could not start the render thread: %s", e.what());
        queue_.close();
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        threadState_ = ThreadState::Exited;
        lifecycleCv_.notify_all();
    }
}

void Engine::shutdown() {
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        if (threadState_ == ThreadState::NotStarted) {
            threadState_ = ThreadState::Exited;
            return;
        }
    }
    if (onRenderThread()) {
        PE_LOGE(kTag, "shutdown() called from the render thread: ignored (would self-join)");
        return;
    }
    quit_ = true;
    queue_.close();  // later lifecycle calls are refused
    queue_.wake();
    if (thread_.joinable()) {
        thread_.join();
        PE_LOGI(kTag, "Shut down");
    }
    queue_.clear();  // tasks refused/unrun: their windows are released here, never used
}

// ------------------------------------------------------------ public (UI)
void Engine::surfaceCreated(std::unique_ptr<PlatformWindow> window) {
    if (!window) return;
    // std::function must be copyable: move the window into a shared holder.
    auto holder = std::make_shared<std::unique_ptr<PlatformWindow>>(std::move(window));
    const bool posted = queue_.post([this, holder] {
        if (window_) {
            // Android always destroys before re-creating; be defensive anyway:
            // the old window must be detached BEFORE its reference is dropped.
            PE_LOGW(kTag, "surfaceCreated while a window is still attached: releasing the old one first");
            detachAndReleaseWindow();
        }
        // surfaceRevoked_ is only cleared by the release task: a destroy that
        // raced with this create must keep blocking new frames.
        window_ = std::move(*holder);
        PE_LOGI(kTag, "surfaceCreated: %s %ux%u", toString(window_->kind()), window_->width(), window_->height());
        if (renderingDisabled_) {
            PE_LOGW(kTag, "Rendering disabled after repeated failures: window kept, not attached");
        } else if (!selection_) {
            selectDevice(failedBackends_);
        } else if (selection_.device->attachSurface(*window_)) {
            surfaceAttached_ = true;
            onSurfaceAvailable();
        } else {
            recoverDevice("attachSurface failed on existing device");
        }
        publishStatus(true);
    });
    if (!posted) PE_LOGW(kTag, "surfaceCreated after shutdown: window released without use");
}

void Engine::surfaceChanged(uint32_t width, uint32_t height) {
    queue_.post([this, width, height] {
        PE_LOGI(kTag, "surfaceChanged: %ux%u", width, height);
        if (window_ && window_->kind() == WindowKind::Headless)
            static_cast<HeadlessWindow*>(window_.get())->resize(width, height);
        if (selection_ && surfaceAttached_) selection_.device->notifySurfaceResized({width, height});
        scheduler_.markWarmup();  // the rebuild frame must not count as steady state
        scheduler_.requestFrame();
        publishStatus(true);
    });
}

bool Engine::surfaceDestroyed() {
    if (onRenderThread()) {  // defensive: never wait for ourselves
        releaseSurfaceForUi();
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        if (threadState_ != ThreadState::Running) return false;  // no thread can use the window
    }

    // (1) Stop starting new frames on this surface immediately.
    surfaceRevoked_ = true;

    // (2) Post the release; completion is signalled even if it throws.
    auto done = std::make_shared<bool>(false);
    struct CompletionSignal {
        Engine* engine;
        std::shared_ptr<bool> done;
        ~CompletionSignal() {
            std::lock_guard<std::mutex> lock(engine->lifecycleMutex_);
            *done = true;
            engine->lifecycleCv_.notify_all();
        }
    };
    const bool posted = queue_.post([this, done] {
        CompletionSignal signal{this, done};
        releaseSurfaceForUi();
    });
    queue_.wake();
    if (!posted) PE_LOGI(kTag, "surfaceDestroyed during shutdown: waiting for render thread teardown");

    // (4) Wait without timeout: for the release, or for the thread to exit
    // (teardown releases the window in the same order).
    const int64_t start = nowNs();
    std::unique_lock<std::mutex> lock(lifecycleMutex_);
    while (!*done && threadState_ != ThreadState::Exited) {
        if (lifecycleCv_.wait_for(lock, std::chrono::seconds(1)) == std::cv_status::timeout && !*done &&
            threadState_ != ThreadState::Exited) {
            lock.unlock();
            PE_LOGW(kTag, "surfaceDestroyed: still waiting for the render thread (%.1f s) - GPU call blocked?",
                    nsToMs(nowNs() - start) / 1000.0);
            lock.lock();
        }
    }
    const bool releasedByTask = *done;
    lock.unlock();
    const double waitedMs = nsToMs(nowNs() - start);
    if (waitedMs > 100.0) PE_LOGW(kTag, "surfaceDestroyed took %.1f ms", waitedMs);
    if (!releasedByTask) PE_LOGI(kTag, "surfaceDestroyed: window released by render thread teardown");
    return true;
}

void Engine::pause() {
    queue_.post([this] {
        paused_ = true;
        PE_LOGI(kTag, "Paused");
        publishStatus(true);
    });
}

void Engine::resume() {
    queue_.post([this] {
        paused_ = false;
        scheduler_.resetIntervalTracking();
        scheduler_.requestFrame();
        PE_LOGI(kTag, "Resumed");
        publishStatus(true);
    });
}

void Engine::setRenderMode(gfx::RenderMode mode) {
    queue_.post([this, mode] {
        config_.renderMode = mode;
        scheduler_.setMode(mode);
        scheduler_.resetIntervalTracking();
        scheduler_.requestFrame();
        PE_LOGI(kTag, "Render mode: %s", gfx::toString(mode));
        publishStatus(true);
    });
}

void Engine::setDiagnosticsEnabled(bool enabled) {
    queue_.post([this, enabled] {
        config_.diagnostics = enabled;
        lastPerfLogNs_ = 0;
        PE_LOGI(kTag, "Diagnostics: %s (render mode unchanged: %s)", enabled ? "on" : "off",
                gfx::toString(scheduler_.mode()));
        publishStatus(true);
    });
}

void Engine::requestRender() {
    queue_.post([this] { scheduler_.requestFrame(); });
}

void Engine::runSelfTest() {
    queue_.post([this] {
        selfTestRequested_ = true;
        scheduler_.requestFrame();
    });
}

EngineStatus Engine::status() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return status_;
}

std::string Engine::statusText() const { return diagnostics::statusSummary(status()); }

std::string Engine::statusJson() const { return diagnostics::statusJson(status()); }

std::string Engine::capabilityReport() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return publishedReport_;
}

bool Engine::waitForStatus(const std::function<bool(const EngineStatus&)>& predicate,
                           std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(statusMutex_);
    return statusCv_.wait_for(lock, timeout, [&] { return predicate(status_); });
}

// ------------------------------------------------------------ render thread
void Engine::threadMain() {
    renderThreadId_.store(std::this_thread::get_id());
    PE_LOGI(kTag, "Render thread started");
    state_ = "Waiting for surface";
    publishStatus(true);

    while (!quit_) {
        try {
            const bool block = !canRender() && pendingRecovery_.empty();
            if (block && statusDirty_) publishStatus(true);  // never go idle with a stale snapshot
            queue_.waitAndRun(block);
            if (quit_) break;
            if (!pendingRecovery_.empty()) {
                const std::string reason = std::move(pendingRecovery_);
                pendingRecovery_.clear();
                recoverDevice(reason.c_str());
            }
            if (canRender()) renderFrame();
        } catch (const std::exception& e) {
            onRenderThreadException(e.what());
        } catch (...) {
            onRenderThreadException("non-standard exception");
        }
    }

    teardown();
    // Only now may UI-thread waiters assume the window is no longer used.
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    threadState_ = ThreadState::Exited;
    lifecycleCv_.notify_all();
}

void Engine::teardown() {
    // Orderly teardown on the thread that owns the GPU objects:
    // swapchain/EGLSurface -> window reference -> scene resources -> device
    // -> backend (instance / EGLDisplay).
    try {
        detachAndReleaseWindow();
        scene_.destroy();
        destroyDevice();
    } catch (const std::exception& e) {
        PE_LOGE(kTag, "Exception during teardown: %s", e.what());
    } catch (...) {
        PE_LOGE(kTag, "Unknown exception during teardown");
    }
    state_ = "Shut down";
    publishStatus(true);
    PE_LOGI(kTag, "Render thread exiting");
}

bool Engine::canRender() const {
    return selection_ && surfaceAttached_ && !surfaceRevoked_.load() && !paused_ && !renderingDisabled_ &&
           scheduler_.wantsFrame();
}

void Engine::detachAndReleaseWindow() {
    bool detached = true;
    if (selection_) {
        try {
            selection_.device->detachSurface();  // waits for the GPU, destroys swapchain / EGLSurface
        } catch (const std::exception& e) {
            PE_LOGE(kTag, "detachSurface threw: %s", e.what());
            detached = false;
        }
        if (!detached) {
            // Last resort: destroying the device also destroys the surface.
            try {
                destroyDevice();
                detached = true;
            } catch (...) {
                PE_LOGE(kTag, "destroyDevice threw while recovering from a failed detach");
            }
        }
        if (detached && selection_ && selection_.device->surfaceState().attached) {
            PE_LOGE(kTag, "Invariant violated: device still reports an attached surface after detach");
            detached = false;
        }
    }
    surfaceAttached_ = false;
    if (!window_) return;
    if (detached) {
        window_.reset();  // ANativeWindow_release happens here, after the surface is gone
    } else {
        PE_LOGE(kTag, "Window reference deliberately leaked: its surface could not be destroyed safely");
        (void)window_.release();
    }
}

void Engine::releaseSurfaceForUi() {
    detachAndReleaseWindow();
    surfaceRevoked_ = false;
    PE_LOGI(kTag, "surfaceDestroyed: surface detached, window released");
    publishStatus(true);
}

void Engine::onRenderThreadException(const char* what) {
    consecutiveExceptions_++;
    PE_LOGE(kTag, "Render thread exception #%u: %s", consecutiveExceptions_, what);
    if (consecutiveExceptions_ >= kMaxConsecutiveExceptions) {
        PE_LOGE(kTag, "Too many consecutive exceptions: graphics disabled for this session");
        try {
            scene_.destroy();
            destroyDevice();
        } catch (...) {
            PE_LOGE(kTag, "Exception while disabling graphics");
        }
        renderingDisabled_ = true;
        state_ = "Error: graphics disabled after repeated exceptions";
        publishStatus(true);
        return;
    }
    pendingRecovery_ = std::string("render thread exception: ") + what;
}

bool Engine::selectDevice(std::vector<gfx::BackendType> excluded) {
    if (!window_) return false;
    gfx::SelectionOptions options;
    options.preference = config_.backend;
    options.allowFallback = config_.allowFallback;
    options.enableValidation = config_.enableValidation;
    options.excluded = std::move(excluded);

    selection_ = gfx::BackendSelector::select(options, *window_);
    lastAttempts_ = selection_.attempts;
    if (!selection_) {
        state_ = "Error: no usable graphics backend";
        surfaceAttached_ = false;
        std::string report = "No usable backend.\n";
        for (const auto& a : selection_.attempts)
            report += std::string(gfx::toString(a.type)) + " failed at '" + a.failedStage + "': " + a.reason + "\n";
        capabilityReport_ = report;
        diagnostics::logMultiline("Diagnostics", report);
        return false;
    }
    surfaceAttached_ = true;
    state_ = "Starting";
    capabilityReport_ = diagnostics::capabilityReport(selection_.device->capabilities(),
                                                      selection_.backend->describe(), selection_.attempts);
    diagnostics::logMultiline("Diagnostics", capabilityReport_);
    onSurfaceAvailable();
    return true;
}

void Engine::destroyDevice() {
    if (!selection_) return;
    scene_.destroy();  // release Refs before the device's ResourceManager shuts down
    selection_.device->waitIdle();
    selection_.reset();  // device (swapchain/EGLSurface first) then backend
    surfaceAttached_ = false;
}

void Engine::recordBackendFailure(gfx::BackendType failed) {
    const size_t i = static_cast<size_t>(failed);
    const int64_t now = nowNs();
    if (now - lastBackendFailureNs_[i] > kFailureWindowNs) backendFailures_[i] = 0;
    lastBackendFailureNs_[i] = now;
    if (++backendFailures_[i] < 2) return;  // first failure: retry the same backend
    if (std::find(failedBackends_.begin(), failedBackends_.end(), failed) != failedBackends_.end()) return;

    // Exclude it only if something else remains to fall back to: excluding
    // the last candidate would leave the app with no graphics at all.
    gfx::SelectionOptions probe;
    probe.preference = config_.backend;
    probe.allowFallback = config_.allowFallback;
    probe.excluded = failedBackends_;
    probe.excluded.push_back(failed);
    if (gfx::BackendSelector::candidateOrder(probe).empty()) {
        PE_LOGW(kTag, "Backend %s failed repeatedly but is the only one left: retrying it", gfx::toString(failed));
        return;
    }
    failedBackends_.push_back(failed);
    PE_LOGW(kTag, "Backend %s excluded for this session (falling back)", gfx::toString(failed));
}

void Engine::recoverDevice(const char* reason) {
    const gfx::BackendType failed = selection_ ? selection_.device->backendType() : gfx::BackendType::None;
    PE_LOGE(kTag, "Recovering graphics device (%s, backend %s)", reason, gfx::toString(failed));
    deviceRecoveries_++;
    selfTestCaptureInFlight_ = false;
    destroyDevice();

    const int64_t now = nowNs();
    recentRecoveriesNs_.push_back(now);
    recentRecoveriesNs_.erase(std::remove_if(recentRecoveriesNs_.begin(), recentRecoveriesNs_.end(),
                                             [&](int64_t t) { return now - t > kFailureWindowNs; }),
                              recentRecoveriesNs_.end());
    if (recentRecoveriesNs_.size() > kMaxRecoveriesPerWindow) {
        PE_LOGE(kTag, "%zu device recoveries within %lld s: graphics disabled for this session",
                recentRecoveriesNs_.size(), static_cast<long long>(kFailureWindowNs / 1'000'000'000));
        renderingDisabled_ = true;
        state_ = "Error: graphics disabled after repeated device failures";
        publishStatus(true);
        return;
    }

    if (failed != gfx::BackendType::None) recordBackendFailure(failed);
    consecutiveErrors_ = 0;
    if (window_ && !surfaceRevoked_) selectDevice(failedBackends_);
    publishStatus(true);
}

void Engine::onSurfaceAvailable() {
    scheduler_.markAttach(nowNs());
    scheduler_.requestFrame();
    consecutiveOutOfDate_ = 0;
    if (config_.diagnostics && config_.autoSelfTest) selfTestRequested_ = true;
}

void Engine::renderFrame() {
    gfx::GraphicsDevice& device = *selection_.device;
    const uint64_t serial = scheduler_.beginFrame(nowNs());
    gfx::FrameContext frame;
    gfx::FrameStatus status = device.beginFrame(serial, frame);
    if (status != gfx::FrameStatus::Ok) {
        scheduler_.endFrameSkipped(status);
        handleFrameStatus(status);
        publishStatus(false);
        return;
    }
    consecutiveOutOfDate_ = 0;

    const gfx::TextureFormat fmt = frame.backbuffer->colorFormat();
    bool sceneOk = scene_.isReadyFor(fmt) || scene_.create(device, fmt);
    if (sceneOk) {
        scene_.render(*frame.context, *frame.backbuffer);
    } else {
        // Still close the frame properly (acquired image must be presented).
        gfx::RenderPassDesc pass;
        pass.target = frame.backbuffer;
        pass.clearColor = {0.5f, 0.0f, 0.0f, 1.0f};
        frame.context->beginRenderPass(pass);
        frame.context->endRenderPass();
    }

    if (selfTestRequested_ && sceneOk) {
        selfTestRequested_ = false;
        selfTest_.runs++;
        if (device.requestCapture()) {
            selfTestCaptureInFlight_ = true;
            selfTest_.status = SelfTestStatus::Pending;
            selfTest_.detail.clear();
        } else {
            selfTest_.status = SelfTestStatus::Unsupported;
            selfTest_.detail = "backbuffer readback not supported by this surface";
        }
    }

    (void)PE_TEST_HOOK("engine.frame.recorded");  // timing hook only
    if (PE_TEST_HOOK("engine.frame.throw")) throw std::runtime_error("injected render-thread exception");

    status = device.endFrame();
    if (status == gfx::FrameStatus::Ok && PE_TEST_HOOK("engine.frame.device_lost")) {
        PE_LOGE(kTag, "Injected device loss (test hook)");
        status = gfx::FrameStatus::DeviceLost;
    }
    if (status == gfx::FrameStatus::Ok) {
        scheduler_.endFramePresented(nowNs());
        consecutiveErrors_ = 0;
        consecutiveExceptions_ = 0;
        state_ = sceneOk ? "Running" : "Error: test scene creation failed";
        if (selfTestCaptureInFlight_) {
            selfTestCaptureInFlight_ = false;
            gfx::CaptureResult capture;
            const uint32_t runs = selfTest_.runs;
            if (device.takeCapture(capture)) {
                selfTest_ = TestScene::verify(capture);
            } else {
                selfTest_.status = SelfTestStatus::Failed;
                selfTest_.detail = "capture was not produced";
            }
            selfTest_.runs = runs;
            if (selfTest_.status == SelfTestStatus::Passed)
                PE_LOGI(kTag, "Self-test PASSED: %s", selfTest_.detail.c_str());
            else
                PE_LOGE(kTag, "Self-test %s: %s", toString(selfTest_.status), selfTest_.detail.c_str());
            publishStatus(true);
        }
        logPerfIfDue();
    } else {
        selfTestCaptureInFlight_ = false;
        scheduler_.endFrameSkipped(status);
        handleFrameStatus(status);
    }
    publishStatus(false);
}

void Engine::handleFrameStatus(gfx::FrameStatus status) {
    switch (status) {
        case gfx::FrameStatus::Ok:
        case gfx::FrameStatus::NoSurface: return;
        case gfx::FrameStatus::SurfaceOutOfDate:
            // Rebuild happens at the next beginFrame. A zero-sized or busy
            // window must not turn the render loop into a busy spin.
            if (++consecutiveOutOfDate_ > 2) std::this_thread::sleep_for(std::chrono::milliseconds(8));
            return;
        case gfx::FrameStatus::SurfaceLost:
            PE_LOGW(kTag, "Surface lost: detaching and trying to re-attach once");
            selection_.device->detachSurface();
            surfaceAttached_ = window_ && !surfaceRevoked_ && selection_.device->attachSurface(*window_);
            if (surfaceAttached_)
                onSurfaceAvailable();
            else
                state_ = "Surface lost (waiting for a new one)";
            publishStatus(true);
            return;
        case gfx::FrameStatus::DeviceLost: recoverDevice("device lost"); return;
        case gfx::FrameStatus::Error:
            if (++consecutiveErrors_ >= kMaxConsecutiveErrors) recoverDevice("repeated frame errors");
            return;
    }
}

void Engine::logPerfIfDue() {
    if (!config_.diagnostics) return;
    const int64_t now = nowNs();
    if (lastPerfLogNs_ == 0) {
        lastPerfLogNs_ = now;
        lastPerfLogFrames_ = scheduler_.stats().framesPresented;
        return;
    }
    if (now - lastPerfLogNs_ < kPerfLogPeriodNs) return;
    const auto& st = scheduler_.stats();
    const double seconds = nsToMs(now - lastPerfLogNs_) / 1000.0;
    const uint64_t rendered = st.framesPresented - lastPerfLogFrames_;
    char rate[64];
    if (scheduler_.mode() == gfx::RenderMode::Continuous && st.rateValid)
        std::snprintf(rate, sizeof(rate), "%.1f fps", st.fps);
    else
        std::snprintf(rate, sizeof(rate), "rate n/a (%s)", gfx::toString(scheduler_.mode()));
    PE_LOGI("Perf", "%s: %llu frames rendered in %.1fs, steady cpu avg %.2f ms / max %.2f ms, total rendered %llu, skipped %llu",
            rate, static_cast<unsigned long long>(rendered), seconds, st.steadyAvgCpuMs, st.steadyMaxCpuMs,
            static_cast<unsigned long long>(st.framesPresented), static_cast<unsigned long long>(st.framesSkipped));
    lastPerfLogNs_ = now;
    lastPerfLogFrames_ = st.framesPresented;
}

void Engine::publishStatus(bool force) {
    const int64_t now = nowNs();
    if (!force && now - lastPublishNs_ < 250'000'000) {
        statusDirty_ = true;
        return;
    }
    lastPublishNs_ = now;
    statusDirty_ = false;

    EngineStatus s;
    const bool terminal = state_.rfind("Error", 0) == 0 || state_ == "Shut down";
    if (terminal)
        s.state = state_;
    else if (!selection_)
        s.state = window_ ? "Initializing" : "Waiting for surface";
    else if (!surfaceAttached_)
        s.state = "No surface (device kept)";
    else if (paused_)
        s.state = "Paused";
    else
        s.state = state_;
    s.paused = paused_;
    if (selection_) {
        const auto& caps = selection_.device->capabilities();
        s.backend = selection_.device->backendType();
        s.backendDescription = selection_.backend->describe();
        s.gpu = caps.deviceName + " (" + caps.vendorName + ")";
        s.api = caps.apiVersion();
        s.driver = caps.driverVersion;
        s.surface = selection_.device->surfaceState();
        s.resources = selection_.device->resources().stats();
        s.validationLayerActive = caps.featureUsable("Validation layer");
        for (const auto& a : lastAttempts_) s.backendInitMs += a.durationMs;
    }
    s.attempts = lastAttempts_;
    s.windowKind = window_ ? toString(window_->kind()) : "none";
    s.frames = scheduler_.stats();
    s.renderMode = scheduler_.mode();
    s.diagnostics = config_.diagnostics;
    s.selfTest = selfTest_;
    s.deviceRecoveries = deviceRecoveries_;
    s.warnings = log::warningCount(LogSource::Engine);
    s.errors = log::errorCount(LogSource::Engine);
    s.validationErrors = log::errorCount(LogSource::ValidationLayer);
    s.validationWarnings = log::warningCount(LogSource::ValidationLayer);
    s.driverDebugMessages =
        log::errorCount(LogSource::DriverDebug) + log::warningCount(LogSource::DriverDebug);
    s.recentProblems = log::recentProblems(4);
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        status_ = std::move(s);
        publishedReport_ = capabilityReport_;
    }
    statusCv_.notify_all();
}

}  // namespace pe
