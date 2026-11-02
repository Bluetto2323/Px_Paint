// Paint Engine - host verification harness.
//
// Runs the REAL engine (same sources as the Android build, minus JNI and
// ANativeWindow) against a headless window on a desktop driver:
//   * OpenGL ES : Mesa llvmpipe through EGL pbuffers
//   * Vulkan    : any ICD exposing VK_EXT_headless_surface (e.g. SwiftShader)
// Built with PE_ENABLE_TEST_HOOKS so timing windows can be widened and faults
// injected at precise points (see core/TestHooks.h).
//
// usage: pe_host_test <auto|vulkan|gles> <vulkan|gles|none> [--no-fallback]
//                     [--suite basic|lifecycle|faults|canvas]
//   basic     : selection, self-test, resize, pause/resume, surface loss,
//               render-mode / diagnostics separation. Expects 0 logged errors.
//   lifecycle : surfaceDestroyed mid-frame / mid-rebuild, shutdown during a
//               render request, rapid recreation, pause/resume churn.
//               Expects 0 logged errors.
//   faults    : injected render-thread exceptions and device losses,
//               recovery and backend fallback. Errors are expected here.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/Log.h"
#include "core/ShutdownReport.h"
#include "core/TestHooks.h"
#include "core/Time.h"
#include "diagnostics/Diagnostics.h"
#include "engine/Engine.h"
#include "platform/PlatformWindow.h"

using namespace std::chrono_literals;
using pe::EngineStatus;
using pe::gfx::BackendType;
using pe::gfx::RenderMode;

namespace {

int gFailures = 0;
int gChecks = 0;

void check(bool ok, const char* what) {
    gChecks++;
    if (!ok) gFailures++;
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    std::fflush(stdout);
}

void section(const char* name) {
    std::printf("-- %s\n", name);
    std::fflush(stdout);
}

// ------------------------------------------------------------------ window
// Records when the engine drops its reference (== ANativeWindow_release on
// Android).
struct WindowProbe {
    std::atomic<bool> released{false};
    std::atomic<int64_t> releasedAtNs{0};
};

// Engine::surfaceChanged resizes the window via HeadlessWindow when kind() is
// Headless, so the probe-owning window derives from HeadlessWindow.
class TrackedHeadless final : public pe::HeadlessWindow {
public:
    TrackedHeadless(uint32_t w, uint32_t h, std::shared_ptr<WindowProbe> probe)
        : HeadlessWindow(w, h), probe_(std::move(probe)) {}
    ~TrackedHeadless() override {
        probe_->releasedAtNs = pe::nowNs();
        probe_->released = true;
    }

private:
    std::shared_ptr<WindowProbe> probe_;
};

std::unique_ptr<pe::PlatformWindow> trackedWindow(uint32_t w, uint32_t h, std::shared_ptr<WindowProbe>& probe) {
    probe = std::make_shared<WindowProbe>();
    return std::make_unique<TrackedHeadless>(w, h, probe);
}

// ------------------------------------------------------------------- hooks
class HookController {
public:
    bool operator()(const char* point) {
        bool delay = false;
        int ms = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (delayShots_ > 0 && delayPoint_ == point) {
                delayShots_--;
                delay = true;
                ms = delayMs_;
            }
        }
        if (delay) {
            entered = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            exitedAtNs = pe::nowNs();
            // Make THIS frame throw at the next hook point (same thread).
            if (throwAfterDelay.exchange(false)) throwShots = 1;
            return false;
        }
        if (std::strcmp(point, "engine.frame.throw") == 0) return take(throwShots);
        if (std::strcmp(point, "engine.frame.device_lost") == 0) return take(deviceLostShots);
        return false;
    }

    void armDelay(const char* point, int ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        delayPoint_ = point;
        delayMs_ = ms;
        delayShots_ = 1;
        entered = false;
        exitedAtNs = 0;
    }

    bool waitEntered(std::chrono::milliseconds timeout) {
        const auto end = std::chrono::steady_clock::now() + timeout;
        while (!entered && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(1ms);
        return entered;
    }

    std::atomic<bool> entered{false};
    std::atomic<int64_t> exitedAtNs{0};
    std::atomic<int> throwShots{0};
    std::atomic<int> deviceLostShots{0};
    std::atomic<bool> throwAfterDelay{false};

private:
    static bool take(std::atomic<int>& shots) {
        int v = shots.load();
        while (v > 0)
            if (shots.compare_exchange_weak(v, v - 1)) return true;
        return false;
    }
    std::mutex mutex_;
    std::string delayPoint_;
    int delayMs_ = 0;
    int delayShots_ = 0;
};

HookController gHooks;

// ----------------------------------------------------------------- helpers
uint64_t frames(const pe::Engine& e) { return e.status().frames.framesPresented; }

// Engine errors plus validation-layer errors: both must stay at zero in the
// clean suites (driver debug output is informational and not counted).
uint64_t totalErrors() {
    return pe::log::errorCount(pe::LogSource::Engine) + pe::log::errorCount(pe::LogSource::ValidationLayer);
}

bool framesAdvance(const pe::Engine& e, uint64_t delta, std::chrono::milliseconds t = 5s) {
    const uint64_t start = frames(e);
    return e.waitForStatus([&](const EngineStatus& s) { return s.frames.framesPresented >= start + delta; }, t);
}

bool framesStable(const pe::Engine& e, std::chrono::milliseconds settle = 150ms,
                  std::chrono::milliseconds window = 300ms) {
    std::this_thread::sleep_for(settle);
    const uint64_t before = frames(e);
    std::this_thread::sleep_for(window);
    return frames(e) == before;
}

bool selfTestPassedAfter(const pe::Engine& e, uint32_t runs, std::chrono::milliseconds t = 15s) {
    return e.waitForStatus(
               [&](const EngineStatus& s) {
                   return s.selfTest.runs > runs && s.selfTest.status != pe::SelfTestStatus::Pending &&
                          s.selfTest.status != pe::SelfTestStatus::NotRun;
               },
               t) &&
           e.status().selfTest.status == pe::SelfTestStatus::Passed;
}

bool waitRunning(const pe::Engine& e, std::chrono::milliseconds t = 20s) {
    return e.waitForStatus([](const EngineStatus& s) { return s.state == "Running" || s.state.rfind("Error", 0) == 0; },
                           t) &&
           e.status().state == "Running";
}

double msSince(int64_t t0) { return pe::nsToMs(pe::nowNs() - t0); }

pe::EngineConfig makeConfig(const std::string& pref, bool noFallback, RenderMode mode) {
    pe::EngineConfig c;
    c.backend = pe::gfx::backendPreferenceFromString(pref);
    c.allowFallback = !noFallback;
    c.enableValidation = true;  // exercises debug paths; missing layers -> one warning
    c.renderMode = mode;
    c.diagnostics = true;
    return c;
}

// Shared check for "surfaceDestroyed returned only after the window was
// released, and the release happened after the in-flight GPU work".
void checkReleaseOrdering(bool returned, const std::shared_ptr<WindowProbe>& probe, int64_t returnedAtNs,
                          int64_t hookExitNs, const char* label) {
    std::string base(label);
    check(returned, (base + ": surfaceDestroyed returned true (released by render thread)").c_str());
    check(probe->released.load(), (base + ": window already released when surfaceDestroyed returned").c_str());
    check(probe->releasedAtNs.load() <= returnedAtNs, (base + ": release happened before the callback returned").c_str());
    check(hookExitNs != 0 && probe->releasedAtNs.load() >= hookExitNs,
          (base + ": release happened only after the in-flight GPU work finished").c_str());
}

// =============================================================== suite: basic
int suiteBasic(const std::string& pref, const std::string& expected, bool noFallback) {
    const uint64_t errorsAtStart = totalErrors();
    {
        pe::Engine engine(makeConfig(pref, noFallback, RenderMode::Continuous));
        engine.start();

        section("selection + first frame");
        engine.surfaceCreated(std::make_unique<pe::HeadlessWindow>(320, 240));
        const bool settled = engine.waitForStatus(
            [](const EngineStatus& s) { return s.state == "Running" || s.state.rfind("Error", 0) == 0; }, 20s);
        EngineStatus s = engine.status();
        std::printf("  state=%s backend=%s gpu=%s api=%s\n", s.state.c_str(), pe::gfx::toString(s.backend),
                    s.gpu.c_str(), s.api.c_str());
        for (const auto& a : s.attempts)
            std::printf("  attempt %s: %s %s\n", pe::gfx::toString(a.type), a.success ? "OK" : a.failedStage.c_str(),
                        a.reason.c_str());

        if (expected == "none") {
            check(settled && s.state.rfind("Error", 0) == 0, "no backend available -> clean error state (no crash)");
            check(s.backend == BackendType::None, "no device created");
            const int64_t t0 = pe::nowNs();
            engine.surfaceDestroyed();
            check(msSince(t0) < 1000.0, "surfaceDestroyed returns promptly with no device");
            engine.shutdown();
            return 0;
        }

        const BackendType want = expected == "vulkan" ? BackendType::Vulkan : BackendType::OpenGLES;
        check(settled && s.state == "Running", "engine reaches Running");
        check(s.backend == want, "expected backend selected");
        check(selfTestPassedAfter(engine, 0), "self-test: triangle pixels verified");
        std::printf("    self-test: %s\n", engine.status().selfTest.detail.c_str());
        check(engine.status().selfTest.detail.find("geometry OK") != std::string::npos,
              "self-test includes the geometry (aspect) check");
        check(framesAdvance(engine, 30), "continuous mode produces frames");

        section("diagnostics: startup vs steady state, rate, skip reasons");
        s = engine.status();
        std::printf("    backend init %.1f ms, first frame %.1f ms, steady avg %.3f ms (%llu steady frames)\n",
                    s.backendInitMs, s.frames.firstFrameMs, s.frames.steadyAvgCpuMs,
                    static_cast<unsigned long long>(s.frames.steadyFrames));
        check(s.backendInitMs > 0.0 && s.frames.firstFrameMs > 0.0, "startup timings reported");
        check(s.frames.steadyFrames + pe::gfx::FrameScheduler::kWarmupFrames <= s.frames.framesPresented,
              "warm-up frames excluded from steady-state statistics");
        check(engine.waitForStatus([](const EngineStatus& st) { return st.frames.rateValid && st.frames.fps > 0.0; }, 5s),
              "continuous: fps measured");
        s = engine.status();
        const auto& fr = s.frames;
        check(fr.framesSkipped == fr.skippedNoSurface + fr.skippedOutOfDate + fr.skippedSurfaceLost +
                                      fr.skippedDeviceLost + fr.skippedError,
              "skipped frames fully attributed to reasons");
        check(pe::diagnostics::statusJson(s).find("\"framesRendered\":") != std::string::npos, "status JSON available");

        section("resize");
        const uint32_t rebuildsBefore = engine.status().surface.recreateCount;
        engine.surfaceChanged(400, 300);
        check(engine.waitForStatus(
                  [&](const EngineStatus& st) { return st.surface.recreateCount > rebuildsBefore; }, 5s),
              "resize: surface/swapchain rebuilt");
        if (want == BackendType::OpenGLES) {
            const auto e = engine.status().surface.extent;
            check(e.width == 400 && e.height == 300, "resize: new extent 400x300");
            check(engine.status().surface.lastRebuildMs > 0.0, "resize: rebuild time reported");
        } else {
            const auto e = engine.status().surface.extent;
            std::printf("    swapchain extent after resize: %ux%u (driver currentExtent)\n", e.width, e.height);
        }
        uint32_t runs = engine.status().selfTest.runs;
        engine.runSelfTest();
        check(selfTestPassedAfter(engine, runs), "self-test after resize");

        section("pause / resume");
        engine.pause();
        check(framesStable(engine), "no frames while paused");
        engine.resume();
        check(framesAdvance(engine, 10), "rendering restarts after resume");

        section("surface destroyed / recreated");
        const auto liveBefore = engine.status().resources.totalLive();
        std::shared_ptr<WindowProbe> probe;
        engine.surfaceDestroyed();  // releases the initial plain window
        s = engine.status();
        check(!s.surface.attached && s.backend == want, "device kept without surface");
        check(framesStable(engine, 50ms), "no frames without surface");
        runs = engine.status().selfTest.runs;
        engine.surfaceCreated(trackedWindow(256, 192, probe));
        check(selfTestPassedAfter(engine, runs), "surface recreated: self-test passes again");
        check(engine.status().resources.totalLive() == liveBefore, "GPU resources reused, not reallocated");

        section("render mode vs diagnostics (independent settings)");
        engine.setDiagnosticsEnabled(false);
        check(engine.waitForStatus([](const EngineStatus& st) { return !st.diagnostics; }, 2s) &&
                  engine.status().renderMode == RenderMode::Continuous,
              "diagnostics off keeps render mode continuous");
        check(framesAdvance(engine, 10), "continuous rendering with diagnostics off");
        engine.setRenderMode(RenderMode::OnDemand);
        check(engine.waitForStatus([](const EngineStatus& st) { return st.renderMode == RenderMode::OnDemand; }, 2s),
              "render mode switched to on-demand");
        check(framesStable(engine), "on-demand: idle when nothing is requested");
        check(!engine.status().frames.rateValid, "on-demand: idle time is not reported as fps");
        engine.setDiagnosticsEnabled(true);
        check(engine.waitForStatus([](const EngineStatus& st) { return st.diagnostics; }, 2s) &&
                  engine.status().renderMode == RenderMode::OnDemand,
              "diagnostics on does NOT switch back to continuous");
        check(framesStable(engine), "on-demand + diagnostics: still idle");
        uint64_t before = frames(engine);
        engine.requestRender();
        check(framesAdvance(engine, 1, 2s), "on-demand: requestRender produces a frame");
        check(framesStable(engine, 100ms) && frames(engine) <= before + 2, "on-demand: returns to idle afterwards");
        engine.setRenderMode(RenderMode::Continuous);
        check(framesAdvance(engine, 10), "back to continuous");

        s = engine.status();
        std::printf("  frames=%llu skipped=%llu avgCpu=%.2fms live=%u\n",
                    static_cast<unsigned long long>(s.frames.framesPresented),
                    static_cast<unsigned long long>(s.frames.framesSkipped), s.frames.steadyAvgCpuMs,
                    s.resources.totalLive());
        std::printf("--- overlay text ---\n%s--------------------\n", engine.statusText().c_str());
        engine.shutdown();
        check(probe && probe->released, "shutdown released the attached window");
        const auto report = pe::shutdown::last();
        std::printf("    last shutdown: %s\n", pe::shutdown::describe(report).c_str());
        check(report.valid && report.clean() && report.resourcesCreated == report.resourcesDestroyed,
              "shutdown report: device destroyed with no leaked resources");
    }
    const uint64_t newErrors = totalErrors() - errorsAtStart;
    check(newErrors == 0, "no errors logged (incl. leak check at shutdown)");
    if (newErrors)
        for (const auto& p : pe::log::recentProblems(10)) std::printf("    %s\n", p.c_str());
    return 0;
}

// ========================================================== suite: lifecycle
int suiteLifecycle(const std::string& pref, const std::string& expected, bool noFallback) {
    const BackendType want = expected == "vulkan" ? BackendType::Vulkan : BackendType::OpenGLES;
    const uint64_t errorsAtStart = totalErrors();
    {
        pe::Engine engine(makeConfig(pref, noFallback, RenderMode::Continuous));
        engine.start();
        std::shared_ptr<WindowProbe> probe;
        engine.surfaceCreated(trackedWindow(320, 240, probe));
        check(waitRunning(engine) && engine.status().backend == want, "engine running on expected backend");
        check(selfTestPassedAfter(engine, 0), "initial self-test");

        // L1 ----------------------------------------------------------------
        section("L1 surfaceDestroyed during a frame");
        gHooks.armDelay("engine.frame.recorded", 300);  // frame is recorded, not yet submitted/presented
        check(gHooks.waitEntered(5s), "render thread is inside a frame");
        int64_t t0 = pe::nowNs();
        bool returned = engine.surfaceDestroyed();
        int64_t tRet = pe::nowNs();
        std::printf("    surfaceDestroyed waited %.1f ms for the in-flight frame\n", pe::nsToMs(tRet - t0));
        checkReleaseOrdering(returned, probe, tRet, gHooks.exitedAtNs.load(), "L1");
        check(!engine.status().surface.attached, "L1: surface detached");
        check(framesStable(engine, 50ms), "L1: no frame after the surface was revoked");

        uint32_t runs = engine.status().selfTest.runs;
        engine.surfaceCreated(trackedWindow(300, 220, probe));
        check(selfTestPassedAfter(engine, runs), "L1: new surface renders correctly");

        // L1b ---------------------------------------------------------------
        section("L1b GPU stall longer than the old 3 s timeout");
        gHooks.armDelay("engine.frame.recorded", 3500);
        check(gHooks.waitEntered(5s), "L1b: render thread stalled inside a frame");
        t0 = pe::nowNs();
        returned = engine.surfaceDestroyed();
        tRet = pe::nowNs();
        std::printf("    surfaceDestroyed waited %.1f ms (old protocol gave up after 3000 ms)\n", pe::nsToMs(tRet - t0));
        check(pe::nsToMs(tRet - t0) > 3000.0, "L1b: no early return after 3 s");
        checkReleaseOrdering(returned, probe, tRet, gHooks.exitedAtNs.load(), "L1b");
        runs = engine.status().selfTest.runs;
        engine.surfaceCreated(trackedWindow(320, 240, probe));
        check(selfTestPassedAfter(engine, runs), "L1b: new surface renders correctly");

        // L2 ----------------------------------------------------------------
        section("L2 surfaceDestroyed during swapchain / EGL surface recreation");
        gHooks.armDelay("surface.rebuild", 300);
        engine.surfaceChanged(640, 480);  // != current extent -> rebuild in next beginFrame
        check(gHooks.waitEntered(5s), "render thread is inside the surface rebuild");
        t0 = pe::nowNs();
        returned = engine.surfaceDestroyed();
        tRet = pe::nowNs();
        std::printf("    surfaceDestroyed waited %.1f ms for the rebuild\n", pe::nsToMs(tRet - t0));
        checkReleaseOrdering(returned, probe, tRet, gHooks.exitedAtNs.load(), "L2");
        runs = engine.status().selfTest.runs;
        engine.surfaceCreated(trackedWindow(320, 240, probe));
        check(selfTestPassedAfter(engine, runs), "L2: new surface renders correctly");

        // L2b ---------------------------------------------------------------
        section("L2b extreme aspect ratios: triangle must stay undistorted");
        for (auto size : {std::pair<uint32_t, uint32_t>{640, 160}, {160, 640}}) {
            const uint32_t rebuilds = engine.status().surface.recreateCount;
            engine.surfaceChanged(size.first, size.second);
            engine.waitForStatus([&](const EngineStatus& st) { return st.surface.recreateCount > rebuilds; }, 5s);
            runs = engine.status().selfTest.runs;
            engine.runSelfTest();
            const bool ok = selfTestPassedAfter(engine, runs);
            const auto e = engine.status().surface.extent;
            std::printf("    requested %ux%u, surface %ux%u: %s\n", size.first, size.second, e.width, e.height,
                        engine.status().selfTest.detail.c_str());
            check(ok, "L2b: self-test incl. geometry passes on a non-square surface");
        }

        // L4 ----------------------------------------------------------------
        section("L4 rapid surface recreation (50x, no waiting in between)");
        const auto liveBefore = engine.status().resources.totalLive();
        engine.surfaceDestroyed();
        bool allReleased = true;
        int64_t worstMs = 0;
        for (int i = 0; i < 50; ++i) {
            std::shared_ptr<WindowProbe> p;
            engine.surfaceCreated(trackedWindow(200 + i, 150 + i, p));
            if (i % 3 == 0) engine.surfaceChanged(220 + i, 160 + i);
            const int64_t a = pe::nowNs();
            engine.surfaceDestroyed();
            worstMs = std::max<int64_t>(worstMs, static_cast<int64_t>(msSince(a)));
            allReleased &= p->released.load();
        }
        std::printf("    worst surfaceDestroyed latency: %lld ms\n", static_cast<long long>(worstMs));
        check(allReleased, "L4: every window was released before its surfaceDestroyed returned");
        runs = engine.status().selfTest.runs;
        engine.surfaceCreated(trackedWindow(320, 240, probe));
        check(selfTestPassedAfter(engine, runs), "L4: rendering correct after the churn");
        check(engine.status().resources.totalLive() == liveBefore, "L4: no GPU resource growth");

        // L5 ----------------------------------------------------------------
        section("L5 pause / resume repeated (200x)");
        for (int i = 0; i < 200; ++i) {
            engine.pause();
            if (i % 2 == 0) engine.requestRender();
            engine.resume();
        }
        engine.pause();
        check(engine.waitForStatus([](const EngineStatus& st) { return st.paused; }, 2s), "L5: final state paused");
        check(framesStable(engine), "L5: no frames while paused (requestRender ignored)");
        engine.resume();
        check(framesAdvance(engine, 10), "L5: rendering resumes");
        // pause + surface loss + resume interleaving
        for (int i = 0; i < 10; ++i) {
            engine.pause();
            engine.surfaceDestroyed();
            engine.resume();  // resumed without surface: must not render
            engine.surfaceCreated(trackedWindow(320, 240, probe));
        }
        check(framesAdvance(engine, 10), "L5: pause/destroy/resume/create interleaving survives");
        engine.shutdown();
        check(probe->released, "L5: shutdown released the window");
    }

    // L3 --------------------------------------------------------------------
    section("L3 shutdown during a render request (on-demand engine)");
    {
        pe::Engine engine(makeConfig(pref, noFallback, RenderMode::OnDemand));
        engine.start();
        std::shared_ptr<WindowProbe> probe;
        engine.surfaceCreated(trackedWindow(320, 240, probe));
        check(waitRunning(engine) && selfTestPassedAfter(engine, 0), "L3: on-demand engine running");
        check(framesStable(engine), "L3: on-demand engine idles by default (no forced continuous)");
        gHooks.armDelay("engine.frame.recorded", 300);
        engine.requestRender();
        check(gHooks.waitEntered(5s), "L3: requested frame in progress");
        for (int i = 0; i < 50; ++i) engine.requestRender();  // more pending requests
        const int64_t t0 = pe::nowNs();
        engine.shutdown();  // "app closed" with no surfaceDestroyed first
        const int64_t tRet = pe::nowNs();
        std::printf("    shutdown took %.1f ms\n", pe::nsToMs(tRet - t0));
        check(probe->released, "L3: window released by shutdown");
        check(probe->releasedAtNs >= gHooks.exitedAtNs.load(), "L3: window released after the in-flight frame");
        const int64_t t1 = pe::nowNs();
        check(!engine.surfaceDestroyed() && msSince(t1) < 50.0, "L3: late surfaceDestroyed returns immediately");
        engine.requestRender();  // must be a harmless no-op
        std::shared_ptr<WindowProbe> late;
        engine.surfaceCreated(trackedWindow(10, 10, late));
        check(late->released, "L3: surfaceCreated after shutdown releases the window without using it");
    }
    {
        // Destroy the Engine object while lifecycle work is still queued.
        std::shared_ptr<WindowProbe> probe;
        auto engine = std::make_unique<pe::Engine>(makeConfig(pref, noFallback, RenderMode::OnDemand));
        engine->start();
        engine->surfaceCreated(trackedWindow(320, 240, probe));
        for (int i = 0; i < 100; ++i) engine->requestRender();
        engine->surfaceChanged(330, 250);
        engine.reset();  // ~Engine: blocks, ordered teardown
        check(probe->released, "L3: Engine destroyed with queued work: window released, no crash");
    }

    const uint64_t newErrors = totalErrors() - errorsAtStart;
    check(newErrors == 0, "lifecycle suite: no errors logged (incl. leak checks)");
    if (newErrors)
        for (const auto& p : pe::log::recentProblems(10)) std::printf("    %s\n", p.c_str());
    return 0;
}

// ============================================================= suite: faults
int suiteFaults(const std::string& pref, const std::string& expected, bool noFallback) {
    const BackendType want = expected == "vulkan" ? BackendType::Vulkan : BackendType::OpenGLES;
    std::shared_ptr<WindowProbe> probe;
    {
        pe::Engine engine(makeConfig(pref, noFallback, RenderMode::Continuous));
        engine.start();
        engine.surfaceCreated(trackedWindow(320, 240, probe));
        check(waitRunning(engine) && engine.status().backend == want && selfTestPassedAfter(engine, 0),
              "engine running on expected backend");

        section("F1 render-thread exception mid-frame -> device recreated");
        uint32_t recoveries = engine.status().deviceRecoveries;
        uint32_t runs = engine.status().selfTest.runs;
        gHooks.throwShots = 1;
        check(engine.waitForStatus([&](const EngineStatus& s) { return s.deviceRecoveries > recoveries; }, 10s),
              "F1: exception caught, recovery triggered");
        check(selfTestPassedAfter(engine, runs) && engine.status().backend == want,
              "F1: same backend recreated, scene resources rebuilt, pixels correct");
        int64_t t0 = pe::nowNs();
        engine.surfaceDestroyed();
        check(probe->released && msSince(t0) < 2000.0, "F1: surfaceDestroyed still works after the exception");
        engine.surfaceCreated(trackedWindow(320, 240, probe));
        check(waitRunning(engine), "F1: running again on a new surface");

        section("F2 device lost (simulated) -> resources recreated");
        recoveries = engine.status().deviceRecoveries;
        runs = engine.status().selfTest.runs;
        gHooks.deviceLostShots = 1;
        check(engine.waitForStatus([&](const EngineStatus& s) { return s.deviceRecoveries > recoveries; }, 10s),
              "F2: device loss handled");
        // F1 already counted one failure for this backend, so a second loss
        // excludes it: fallback if another backend exists and is allowed.
        const bool canFallBack = pref == "auto" && want == BackendType::Vulkan && !noFallback;
        const BackendType afterLoss = canFallBack ? BackendType::OpenGLES : want;
        if (canFallBack) {
            check(engine.waitForStatus([&](const EngineStatus& s) { return s.backend == BackendType::OpenGLES; }, 15s),
                  "F2: second failure of Vulkan -> fallback to OpenGL ES");
        }
        check(selfTestPassedAfter(engine, runs) && engine.status().backend == afterLoss,
              canFallBack ? "F2: OpenGL ES renders the scene correctly after fallback"
                          : "F2: backend kept (no alternative) and scene rebuilt");
        std::printf("    backend after device loss: %s, recoveries %u\n", pe::gfx::toString(engine.status().backend),
                    engine.status().deviceRecoveries);

        section("F4 surfaceDestroyed racing with an exception mid-frame");
        recoveries = engine.status().deviceRecoveries;
        runs = engine.status().selfTest.runs;
        gHooks.throwAfterDelay = true;
        gHooks.armDelay("engine.frame.recorded", 300);
        check(gHooks.waitEntered(5s), "F4: frame in progress (will throw after the delay)");
        t0 = pe::nowNs();
        engine.surfaceDestroyed();  // release task runs BEFORE the recovery
        const int64_t tRet = pe::nowNs();
        check(probe->released && probe->releasedAtNs <= tRet, "F4: window released before surfaceDestroyed returned");
        check(probe->releasedAtNs >= gHooks.exitedAtNs.load(), "F4: release happened after the aborted frame");
        check(engine.waitForStatus([&](const EngineStatus& s) { return s.deviceRecoveries > recoveries; }, 10s),
              "F4: exception still handled (device recreated)");
        engine.surfaceCreated(trackedWindow(320, 240, probe));
        check(selfTestPassedAfter(engine, runs), "F4: new surface renders correctly");

        section("F3 repeated exceptions -> graphics disabled safely");
        gHooks.throwShots = 3;
        check(engine.waitForStatus(
                  [](const EngineStatus& s) { return s.state.rfind("Error: graphics disabled", 0) == 0; }, 15s),
              "F3: engine enters a clean disabled state");
        t0 = pe::nowNs();
        engine.surfaceDestroyed();
        check(probe->released && msSince(t0) < 2000.0, "F3: surfaceDestroyed works in the disabled state");
        std::shared_ptr<WindowProbe> p2;
        engine.surfaceCreated(trackedWindow(64, 64, p2));
        engine.shutdown();
        check(p2->released, "F3: shutdown releases the window kept in the disabled state");
    }
    gHooks.throwShots = 0;
    gHooks.deviceLostShots = 0;
    return 0;
}


// ============================================================== suite: canvas
pe::CanvasStatus canvasOf(const pe::Engine& e) { return e.status().canvas; }

bool canvasSelfTestPassedAfter(const pe::Engine& e, uint32_t runs, std::chrono::milliseconds t = 30s) {
    return e.waitForStatus(
               [&](const EngineStatus& s) {
                   return s.canvas.selfTest.runs > runs && s.canvas.selfTest.status != pe::SelfTestStatus::Pending &&
                          s.canvas.selfTest.status != pe::SelfTestStatus::NotRun;
               },
               t) &&
           e.status().canvas.selfTest.status == pe::SelfTestStatus::Passed;
}

int suiteCanvas(const std::string& pref, const std::string& expected, bool noFallback) {
    const BackendType want = expected == "vulkan" ? BackendType::Vulkan : BackendType::OpenGLES;
    const uint64_t errorsAtStart = totalErrors();
    uint64_t injectedErrors = 0;
    std::shared_ptr<WindowProbe> probe;
    {
        pe::EngineConfig cfg = makeConfig(pref, noFallback, RenderMode::OnDemand);
        cfg.scene = pe::SceneMode::Canvas;
        pe::Engine engine(cfg);
        engine.start();
        constexpr uint32_t kW = 900, kH = 600;
        engine.surfaceCreated(trackedWindow(kW, kH, probe));
        engine.surfaceChanged(kW, kH);
        check(waitRunning(engine) && engine.status().backend == want, "engine running on expected backend");
        check(engine.status().selfTest.status == pe::SelfTestStatus::NotRun,
              "canvas scene: no automatic triangle self-test (would flash on the user's screen)");
        check(framesStable(engine), "on-demand: idle without a canvas");
        const uint32_t baseLive = engine.status().resources.totalLive();

        section("C1 canvas creation, validation and dimensions");
        check(!engine.createCanvas({0, 100}).empty(), "C1: 0 px width rejected");
        check(!engine.createCanvas({20000, 100}).empty(), "C1: side above 16384 rejected");
        check(engine.createCanvas({1920, 1080, 0xFFF5F0E6u}).empty(), "C1: 1920x1080 accepted");
        check(engine.waitForStatus([](const EngineStatus& s) { return s.canvas.open && s.canvas.gpuReady; }, 10s),
              "C1: GPU storage created");
        auto c = canvasOf(engine);
        std::printf("    storage: %s\n", c.storage.c_str());
        check(c.width == 1920 && c.height == 1080, "C1: dimensions reported");
        check(c.tilesTotal == 1 && c.tilesAllocated == 1 && c.bytesAllocated == 1920ull * 1080 * 4,
              "C1: 1920x1080 uses ONE texture (single-texture strategy), exact size");
        auto info = engine.viewportOp(pe::ViewOp::Query, nullptr, 0);
        check(info.state.fitted && info.canvasWidth == 1920 && info.screenWidth == kW, "C1: viewport fitted to the screen");
        const auto centre = engine.canvasToScreen({960, 540});
        check(std::abs(centre.x - kW / 2.0) < 1e-6 && std::abs(centre.y - kH / 2.0) < 1e-6,
              "C1: canvas centre at screen centre");
        const double fitZoom = std::min((kW - 48.0) / 1920.0, (kH - 48.0) / 1080.0);
        check(std::abs(info.state.zoom - fitZoom) < 1e-9, "C1: fit zoom keeps the aspect ratio (min of both axes)");

        section("C2 canvas GPU self-test (tiles, seams, partial updates, rotation)");
        uint32_t runs = canvasOf(engine).selfTest.runs;
        engine.runCanvasSelfTest();
        const bool passed = canvasSelfTestPassedAfter(engine, runs);
        std::printf("    %s\n", canvasOf(engine).selfTest.detail.c_str());
        check(passed, "C2: canvas self-test PASSED (4 steps, pixel-exact against the CPU model)");
        check(canvasOf(engine).width == 1920 && framesStable(engine), "C2: user canvas back, idle again");

        section("C3 viewport commands (thread-safe, exact transforms)");
        const pe::canvas::Vec2 focal{300, 200};
        const auto before = engine.screenToCanvas(focal);
        double zargs[] = {2.5, focal.x, focal.y};
        info = engine.viewportOp(pe::ViewOp::ZoomAt, zargs, 3);
        const auto after = engine.screenToCanvas(focal);
        check(std::abs(before.x - after.x) < 1e-6 && std::abs(before.y - after.y) < 1e-6,
              "C3: zoom keeps the canvas point under the focal point");
        double rargs[] = {0.7, focal.x, focal.y};
        engine.viewportOp(pe::ViewOp::RotateAt, rargs, 3);
        const auto afterRot = engine.screenToCanvas(focal);
        check(std::abs(before.x - afterRot.x) < 1e-6 && std::abs(before.y - afterRot.y) < 1e-6,
              "C3: rotation keeps the canvas point under the focal point");
        double gargs[] = {focal.x, focal.y, focal.x + 40, focal.y - 25, 1.3, -0.2};
        engine.viewportOp(pe::ViewOp::Gesture, gargs, 6);
        const auto afterGesture = engine.screenToCanvas({focal.x + 40, focal.y - 25});
        check(std::abs(before.x - afterGesture.x) < 1e-6 && std::abs(before.y - afterGesture.y) < 1e-6,
              "C3: two-finger gesture keeps the canvas point under the fingers");
        const auto rt = engine.canvasToScreen(engine.screenToCanvas({123.25, 456.5}));
        check(std::abs(rt.x - 123.25) < 1e-9 && std::abs(rt.y - 456.5) < 1e-9, "C3: screen->canvas->screen round trip");
        check(framesAdvance(engine, 1), "C3: viewport change renders a frame (on-demand)");
        info = engine.viewportOp(pe::ViewOp::Fit, nullptr, 0);
        check(info.state.fitted && std::abs(info.state.rotation - (0.7 - 0.2)) < 1e-9,
              "C3: fit keeps the rotation and refits");
        double sr[] = {0.0, kW / 2.0, kH / 2.0};
        engine.viewportOp(pe::ViewOp::SetRotationAt, sr, 3);
        info = engine.viewportOp(pe::ViewOp::Fit, nullptr, 0);
        check(std::abs(info.state.zoom - fitZoom) < 1e-9, "C3: reset rotation + fit = initial view");

        section("C4 large canvas: tiles allocated on demand, partial updates");
        check(engine.createCanvas({8000, 6000}).empty(), "C4: 8000x6000 accepted");
        check(engine.waitForStatus(
                  [](const EngineStatus& s) { return s.canvas.width == 8000 && s.canvas.gpuReady; }, 10s),
              "C4: tiled storage created");
        c = canvasOf(engine);
        std::printf("    storage: %s\n", c.storage.c_str());
        check(c.tileSize == 256 && c.tilesTotal == 32u * 24u, "C4: 32 x 24 tiles of 256 px");
        check(c.tilesAllocated == 0 && c.bytesAllocated == 0, "C4: no tile allocated before any content");
        pe::canvas::Vec2 at;
        check(engine.stampAt(kW / 2.0, kH / 2.0, 40, 0xFFE03030u, &at), "C4: stamp at the screen centre accepted");
        check(std::abs(at.x - 4000) < 1e-6 && std::abs(at.y - 3000) < 1e-6, "C4: screen centre maps to canvas centre");
        check(engine.waitForStatus([](const EngineStatus& s) { return s.canvas.writes >= 1; }, 5s),
              "C4: stamp written");
        c = canvasOf(engine);
        const uint64_t tileBytes = 258ull * 258 * 4;
        check(c.tilesAllocated == 1 && c.bytesAllocated == tileBytes && c.textureUploads == 1 &&
                  c.bytesUploaded == 40ull * 40 * 4,
              "C4: one tile allocated, only 40x40 texels uploaded (partial update)");
        const auto corner = engine.canvasToScreen({4096, 3072});  // shared by 4 tiles
        check(engine.stampAt(corner.x, corner.y, 20, 0xFF3060E0u, nullptr), "C4: stamp on a 4-tile corner");
        check(engine.waitForStatus([](const EngineStatus& s) { return s.canvas.writes >= 2; }, 5s),
              "C4: corner stamp written");
        c = canvasOf(engine);
        check(c.tilesAllocated == 4 && c.textureUploads == 5,
              "C4: corner stamp touched exactly 4 tiles (3 new), one upload each");
        check(!engine.stampAt(2, 2, 10, 0xFF000000u, nullptr), "C4: stamp outside the canvas rejected");
        std::printf("    tiles %u/%u, %.2f MiB, uploads %llu (%llu bytes), last write %.2f ms, max %.2f ms\n",
                    c.tilesAllocated, c.tilesTotal, c.bytesAllocated / 1048576.0,
                    static_cast<unsigned long long>(c.textureUploads), static_cast<unsigned long long>(c.bytesUploaded),
                    c.lastWriteMs, c.maxWriteMs);

        section("C5 culling: only visible tiles are drawn");
        check(framesAdvance(engine, 1) || framesStable(engine), "C5: frame rendered");
        c = canvasOf(engine);
        check(c.tilesVisible == c.tilesTotal && c.piecesDrawn == 4, "C5: fitted view: all tiles visible, 4 drawn");
        double z[] = {8.0, 0, 0};
        const auto tl = engine.canvasToScreen({0, 0});
        z[1] = tl.x;
        z[2] = tl.y;
        engine.viewportOp(pe::ViewOp::SetZoomAt, z, 3);
        check(engine.waitForStatus([](const EngineStatus& s) { return s.canvas.tilesVisible < 30; }, 5s),
              "C5: zoomed into a corner: few tiles visible");
        c = canvasOf(engine);
        std::printf("    zoomed: %u visible tiles of %u, %u pieces drawn, %u draw calls\n", c.tilesVisible,
                    c.tilesTotal, c.piecesDrawn, c.drawCalls);
        check(c.piecesDrawn == 0, "C5: content tiles off-screen are culled");

        section("C6 device loss -> content rebuilt from the journal");
        const uint32_t recoveries = engine.status().deviceRecoveries;
        const uint64_t errorsBeforeLoss = totalErrors();
        gHooks.deviceLostShots = 1;
        engine.requestRender();
        check(engine.waitForStatus([&](const EngineStatus& s) { return s.deviceRecoveries > recoveries; }, 10s),
              "C6: device loss handled");
        check(engine.waitForStatus(
                  [](const EngineStatus& s) { return s.canvas.gpuReady && s.canvas.restores >= 1; }, 10s),
              "C6: canvas storage recreated");
        c = canvasOf(engine);
        check(c.tilesAllocated == 4 && c.journalOps == 2, "C6: same 4 tiles restored from 2 journal operations");
        injectedErrors = totalErrors() - errorsBeforeLoss;  // "Injected device loss" + "Recovering ..."
        runs = c.selfTest.runs;
        engine.runCanvasSelfTest();
        check(canvasSelfTestPassedAfter(engine, runs), "C6: canvas self-test passes on the recreated device");

        section("C6b content export / reload (project persistence)");
        {
            const auto ops = engine.canvasContent();
            check(ops.size() == 2, "C6b: content mirror holds the 2 stamps, in order");
            const bool exact = ops.size() == 2 && ops[0].x == 3980 && ops[0].y == 2980 && ops[0].w == 40 &&
                               ops[0].h == 40 && ops[0].argb == 0xFFE03030u && ops[1].w == 20 && ops[1].h == 20 &&
                               ops[1].argb == 0xFF3060E0u;
            check(exact, "C6b: exported rectangles/colours match the stamps exactly");
            const uint64_t rev0 = engine.canvasContentRevision();
            // Reopen the same canvas and reload: storage must end up identical.
            check(engine.createCanvas({8000, 6000}).empty(), "C6b: canvas re-created");
            std::vector<pe::ContentOp> withJunk = ops;
            withJunk.push_back({-100, -100, 50, 50, 0xFF00FF00u});   // fully outside: dropped
            withJunk.push_back({7990, 5990, 40, 40, 0xFF00FF00u});   // clipped to 10x10
            const size_t kept = engine.loadCanvasContent(withJunk);
            check(kept == 3, "C6b: load keeps in-canvas ops (outside dropped, edge clipped)");
            check(engine.canvasContentRevision() > rev0, "C6b: content revision advanced");
            check(engine.waitForStatus(
                      [](const EngineStatus& s) { return s.canvas.width == 8000 && s.canvas.gpuReady && s.canvas.journalOps == 3; },
                      10s),
                  "C6b: reloaded content applied on the render thread (3 journal ops)");
            c = canvasOf(engine);
            check(c.tilesAllocated == 5, "C6b: same 4 tiles + the clipped edge tile allocated after reload");
            const auto back = engine.canvasContent();
            check(back.size() == 3 && back[2].w == 10 && back[2].h == 10, "C6b: clipped op stored clipped");
            engine.clearCanvasContent();
            check(engine.canvasContent().empty(), "C6b: clear empties the content mirror");
        }

        section("C7 maximum canvas and memory budget");
        const std::string err = engine.createCanvas({16384, 16384});
        check(err.empty(), "C7: 16384x16384 passes size validation");
        check(engine.waitForStatus(
                  [](const EngineStatus& s) { return s.canvas.width == 16384 && (s.canvas.gpuReady || !s.canvas.error.empty()); },
                  10s),
              "C7: storage decided for 16384x16384");
        c = canvasOf(engine);
        std::printf("    16384x16384: %s%s\n", c.gpuReady ? "ready - " : "rejected - ",
                    c.gpuReady ? c.storage.c_str() : c.error.c_str());
        check(c.gpuReady ? (c.tilesTotal == 64u * 64u && c.tilesAllocated == 0) : c.error.find("budget") != std::string::npos,
              "C7: either tiled with 0 tiles allocated, or rejected with an explicit budget reason");

        section("C8 close canvas -> GPU memory released");
        engine.clearCanvasContent();
        engine.closeCanvas();
        check(engine.waitForStatus([](const EngineStatus& s) { return !s.canvas.open; }, 5s), "C8: canvas closed");
        check(engine.waitForStatus([&](const EngineStatus& s) { return s.resources.totalLive() <= baseLive; }, 5s),
              "C8: live GPU resources back to the pre-canvas baseline");
        std::printf("    live resources: %u (baseline %u)\n", engine.status().resources.totalLive(), baseLive);
        std::printf("--- overlay text ---\n%s--------------------\n", engine.statusText().c_str());
        engine.shutdown();
        const auto report = pe::shutdown::last();
        std::printf("    last shutdown: %s\n", pe::shutdown::describe(report).c_str());
        check(report.valid && report.clean() && report.resourcesCreated == report.resourcesDestroyed,
              "C8: shutdown report: no leaked resources");
    }
    gHooks.deviceLostShots = 0;
    const uint64_t newErrors = totalErrors() - errorsAtStart - injectedErrors;
    check(injectedErrors <= 2, "canvas suite: the injected device loss logged only its own 2 errors");
    check(newErrors == 0, "canvas suite: no other engine or validation errors logged");
    if (newErrors)
        for (const auto& p : pe::log::recentProblems(10)) std::printf("    %s\n", p.c_str());
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: %s <auto|vulkan|gles> <vulkan|gles|none> [--no-fallback] [--suite basic|lifecycle|faults|canvas]\n",
                     argv[0]);
        return 2;
    }
    const std::string pref = argv[1];
    const std::string expected = argv[2];
    bool noFallback = false;
    std::string suite = "basic";
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-fallback") == 0) noFallback = true;
        if (std::strcmp(argv[i], "--suite") == 0 && i + 1 < argc) suite = argv[++i];
    }
    pe::log::setMinLevel(pe::LogLevel::Info);
    pe::testhooks::install([](const char* point) { return gHooks(point); });

    std::printf("=== host test [%s]: preference=%s expected=%s fallback=%s ===\n", suite.c_str(), pref.c_str(),
                expected.c_str(), noFallback ? "off" : "on");
    if (suite == "basic")
        suiteBasic(pref, expected, noFallback);
    else if (suite == "lifecycle")
        suiteLifecycle(pref, expected, noFallback);
    else if (suite == "faults")
        suiteFaults(pref, expected, noFallback);
    else if (suite == "canvas")
        suiteCanvas(pref, expected, noFallback);
    else {
        std::fprintf(stderr, "unknown suite %s\n", suite.c_str());
        return 2;
    }
    pe::testhooks::install(nullptr);
    std::printf("=== %d/%d checks passed ===\n", gChecks - gFailures, gChecks);
    return gFailures == 0 ? 0 : 1;
}
