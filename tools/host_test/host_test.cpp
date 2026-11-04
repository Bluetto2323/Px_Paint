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
#include <cstdlib>
#include <cstring>
#include <functional>
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
#include "PresetFile.h"
#include "platform/PlatformWindow.h"

using namespace std::chrono_literals;
using pe::EngineStatus;
using pe::gfx::BackendType;
using pe::gfx::RenderMode;

namespace {

int gFailures = 0;
int gChecks = 0;

void check(bool ok, const char* what);
void check(bool ok, const std::string& what) { check(ok, what.c_str()); }
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
                  c.bytesUploaded == tileBytes,
              "C4: one tile allocated, filled once (own pixels + gutter)");
        const auto corner = engine.canvasToScreen({4096, 3072});  // shared by 4 tiles
        check(engine.stampAt(corner.x, corner.y, 20, 0xFF3060E0u, nullptr), "C4: stamp on a 4-tile corner");
        check(engine.waitForStatus([](const EngineStatus& s) { return s.canvas.writes >= 2; }, 5s),
              "C4: corner stamp written");
        c = canvasOf(engine);
        check(c.tilesAllocated == 4 && c.textureUploads == 5 && c.bytesUploaded < 5 * tileBytes,
              "C4: corner stamp touched exactly 4 tiles (3 new), one upload each, partial for the existing tile");
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

        section("C6 device loss -> GPU copy rebuilt from the CPU raster");
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
        check(c.tilesAllocated == 4 && c.rasterTiles == 4, "C6: the same 4 tiles re-uploaded from the 4 raster tiles");
        injectedErrors = totalErrors() - errorsBeforeLoss;  // "Injected device loss" + "Recovering ..."
        runs = c.selfTest.runs;
        engine.runCanvasSelfTest();
        check(canvasSelfTestPassedAfter(engine, runs), "C6: canvas self-test passes on the recreated device");

        section("C6b content export / import (project persistence) and legacy import");
        {
            std::vector<uint8_t> all;
            const size_t n = engine.exportCanvasTiles(false, 1000, all);
            check(n == 4 && all.size() == 4 + 4 * (12 + pe::canvas::RasterStore::kTileBytes),
                  "C6b: export of every tile: 4 records with pixels");
            // Stamp centre (4000, 3000) lies in tile (15, 11) at (160, 184).
            bool found = false;
            for (size_t off = 4, i = 0; i < n; ++i) {
                int32_t tx, ty, flags;
                std::memcpy(&tx, &all[off], 4);
                std::memcpy(&ty, &all[off + 4], 4);
                std::memcpy(&flags, &all[off + 8], 4);
                off += 12;
                if (tx == 15 && ty == 11 && flags == 0) {
                    const uint8_t* px = &all[off + (184 * 256 + 160) * 4];
                    found = px[0] == 0xE0 && px[1] == 0x30 && px[2] == 0x30 && px[3] == 0xFF;
                }
                if (!(flags & 1)) off += pe::canvas::RasterStore::kTileBytes;
            }
            check(found, "C6b: exported pixels hold the stamp colour exactly");
            std::vector<uint8_t> changed;
            check(engine.exportCanvasTiles(true, 1000, changed) == 0, "C6b: nothing changed since that export");
            const uint64_t rev0 = engine.canvasContentRevision();
            check(engine.createCanvas({8000, 6000}).empty(), "C6b: canvas re-created (empty)");
            check(engine.document().stats().tilesAllocated == 0, "C6b: new document has no tiles");
            check(engine.importCanvasTiles(all.data(), all.size()) == 4, "C6b: 4 tiles imported");
            check(engine.document().pixel(4000, 3000).toArgb() == 0xFFE03030u &&
                      engine.document().pixel(4096, 3072).toArgb() == 0xFF3060E0u,
                  "C6b: imported content identical (both stamps)");
            check(engine.canvasContentRevision() > rev0, "C6b: content revision advanced");
            check(engine.waitForStatus(
                      [](const EngineStatus& s) { return s.canvas.width == 8000 && s.canvas.gpuReady && s.canvas.tilesAllocated == 4; },
                      10s),
                  "C6b: imported tiles uploaded to the GPU");
            std::vector<uint8_t> bad(all.begin(), all.begin() + 100);
            check(engine.importCanvasTiles(bad.data(), bad.size()) == -1, "C6b: truncated data rejected");
            uint32_t tw = 0, th = 0;
            std::vector<uint32_t> thumb;
            check(engine.canvasThumbnail(480, tw, th, thumb) && tw == 480 && th == 360 && thumb.size() == 480u * 360u,
                  "C6b: thumbnail 480x360 keeps the 4:3 aspect ratio");
            check((thumb[180 * 480 + 240] >> 24) > 0 && (thumb[10 * 480 + 10] >> 24) == 0,
                  "C6b: thumbnail shows the stamp, transparent elsewhere");
            // Block 2 projects: rectangle ops become raster content.
            std::vector<pe::ContentOp> legacy = {{3980, 2980, 40, 40, 0xFFE03030u}, {4086, 3062, 20, 20, 0xFF3060E0u},
                                                 {-100, -100, 50, 50, 0xFF00FF00u}, {7990, 5990, 40, 40, 0xFF00FF00u}};
            check(engine.loadCanvasContent(legacy) == 3, "C6b: legacy load keeps in-canvas ops (outside dropped)");
            check(engine.document().pixel(7999, 5999).toArgb() == 0xFF00FF00u &&
                      engine.document().pixel(4000, 3000).toArgb() == 0xFFE03030u,
                  "C6b: legacy ops rasterised (edge op clipped)");
            std::vector<uint8_t> legacyExport;
            check(engine.exportCanvasTiles(true, 1000, legacyExport) == 5,
                  "C6b: imported legacy content is unsaved in the new format (5 tiles)");
            check(engine.waitForStatus(
                      [](const EngineStatus& s) { return s.canvas.gpuReady && s.canvas.tilesAllocated == 5; }, 10s),
                  "C6b: legacy content on the GPU (5 tiles)");
            engine.clearCanvasContent();
            check(engine.document().stats().tilesAllocated == 0, "C6b: clear empties the raster");
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

// =============================================================== suite: brush
// Brush strokes through the real engine: screen samples -> viewport ->
// CPU raster -> partial uploads -> GPU display, checked on the backbuffer.
struct Cap {
    pe::gfx::CaptureResult c;
    const uint8_t* at(int x, int y) const { return &c.rgba[(static_cast<size_t>(y) * c.width + x) * 4]; }
    int grey(int x, int y) const { return at(x, y)[0]; }
};

// Waits until the render thread consumed every queued stroke event (read
// from the document directly: the published status may lag), then for the
// status of a frame rendered after that.
bool brushIdle(pe::Engine& e, std::chrono::milliseconds t = 10s) {
    const auto end = std::chrono::steady_clock::now() + t;
    for (;;) {
        const auto b = e.document().stats().brush;
        if (b.pendingEvents == 0 && b.samplesProcessed == b.samplesReceived) break;
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(1ms);
    }
    const uint64_t processed = e.document().stats().brush.samplesProcessed;
    return e.waitForStatus([&](const EngineStatus& s) { return s.canvas.brushSamplesProcessed >= processed; }, t);
}

// Engine stroke sample: x, y, t, pressure, tilt, orientation (NaN = absent).
struct Sample6 {
    float v[pe::Engine::kSampleFloats];
};
Sample6 sample6(double x, double y, double t, float pressure = NAN, float tilt = NAN, float orientation = NAN) {
    return {{static_cast<float>(x), static_cast<float>(y), static_cast<float>(t), pressure, tilt, orientation}};
}

// Stroke along screen points; `perBatch` samples per call (like MotionEvent
// history). `pressure(i)` (optional) gives sample i's pressure.
void screenStroke(pe::Engine& e, const pe::brush::BrushSettings& b, const std::vector<pe::canvas::Vec2>& pts,
                  size_t perBatch = 4, bool finish = true, const std::function<float(size_t)>& pressure = nullptr,
                  float tilt = NAN, float orientation = NAN) {
    auto at = [&](size_t k) {
        return sample6(pts[k].x, pts[k].y, static_cast<double>(k * 4), pressure ? pressure(k) : NAN, tilt, orientation);
    };
    const Sample6 first = at(0);
    e.strokeBegin(b, first.v);
    std::vector<float> buf;
    for (size_t i = 1; i < pts.size(); i += perBatch) {
        buf.clear();
        for (size_t k = i; k < std::min(pts.size(), i + perBatch); ++k) {
            const Sample6 sm = at(k);
            buf.insert(buf.end(), sm.v, sm.v + pe::Engine::kSampleFloats);
        }
        e.strokeSamples(buf.data(), buf.size() / pe::Engine::kSampleFloats);
    }
    if (finish) e.strokeEnd();
}

std::vector<pe::canvas::Vec2> screenLine(double x0, double y0, double x1, double y1, int n) {
    std::vector<pe::canvas::Vec2> v;
    for (int i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / (n - 1);
        v.push_back({x0 + (x1 - x0) * t, y0 + (y1 - y0) * t});
    }
    return v;
}

pe::brush::BrushSettings brushOf(float d, float opacity, uint32_t argb, bool erase = false) {
    pe::brush::BrushSettings b;
    b.diameter = d;
    b.opacity = opacity;
    b.argb = argb;
    b.erase = erase;
    return b;
}

bool capture(pe::Engine& e, Cap& out) { return e.captureFrame(out.c, 10s) && out.c.width > 0; }

int suiteBrush(const std::string& pref, const std::string& expected, bool noFallback) {
    const BackendType want = expected == "vulkan" ? BackendType::Vulkan : BackendType::OpenGLES;
    const uint64_t errorsAtStart = totalErrors();
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
        const uint32_t baseLive = engine.status().resources.totalLive();
        const uint32_t kPaper = 0xFFFFFFFFu;

        section("S1 stroke through the engine: screen samples -> canvas raster -> GPU display");
        check(engine.createCanvas({1200, 800, kPaper}).empty(), "S1: 1200x800 canvas (single texture)");
        check(engine.waitForStatus([](const EngineStatus& s) { return s.canvas.gpuReady; }, 10s), "S1: storage ready");
        const double zoom = engine.viewportOp(pe::ViewOp::Query, nullptr, 0).state.zoom;
        screenStroke(engine, brushOf(40, 1.0f, 0xFF000000u), screenLine(200, 300, 700, 300, 60));
        check(brushIdle(engine), "S1: every queued sample processed");
        const auto bs1 = engine.document().stats().brush;
        check(bs1.samplesReceived == 60 && bs1.samplesProcessed == 60 && bs1.strokesCommitted == 1,
              "S1: 60 samples received = processed, 1 stroke committed");
        const auto mid = engine.screenToCanvas({450, 300});
        check(engine.document().pixel(static_cast<int>(mid.x), static_cast<int>(mid.y)).a == 255 &&
                  engine.document().pixel(static_cast<int>(mid.x), static_cast<int>(mid.y) + 22).a == 0,
              "S1: canvas raster: 40 px (canvas) stroke at the mapped position");
        Cap cap;
        check(capture(engine, cap), "S1: backbuffer captured");
        if (cap.c.width) {
            check(cap.grey(450, 300) <= 4 && cap.grey(250, 300) <= 4 && cap.grey(650, 300) <= 4,
                  "S1: GPU shows the stroke (black along the centre line)");
            const int edge = static_cast<int>(std::lround(20 * zoom));
            check(cap.grey(450, 300 - edge - 3) >= 250 && cap.grey(450, 300 + edge + 3) >= 250,
                  "S1: paper beyond the stroke edge (stroke width scaled by the zoom)");
            int intermediate = 0;
            for (int y = 300 - edge - 2; y <= 300 - edge + 2; ++y)
                if (cap.grey(450, y) > 8 && cap.grey(450, y) < 247) intermediate++;
            check(intermediate >= 1, "S1: antialiased edge on screen (intermediate values)");
            std::printf("    zoom %.3f; edge profile:", zoom);
            for (int y = 300 - edge - 3; y <= 300 - edge + 3; ++y) std::printf(" %d", cap.grey(450, y));
            std::printf("\n");
        }

        section("S2 opacity on the GPU: a self-crossing 50% stroke stays 50%");
        std::vector<pe::canvas::Vec2> loop;
        for (int i = 0; i <= 80; ++i) {
            const double a = 2 * pe::canvas::kPi * 1.5 * i / 80;
            loop.push_back({450 + 120 * std::cos(a), 420 + 60 * std::sin(2 * a)});  // figure-eight, crosses itself
        }
        screenStroke(engine, brushOf(30, 0.5f, 0xFF000000u), loop, 3);
        check(brushIdle(engine), "S2: processed");
        if (capture(engine, cap)) {
            int lo = 255, hi = 0;
            for (int y = 340; y < 500; ++y)
                for (int x = 300; x < 600; ++x) {
                    const int g = cap.grey(x, y);
                    if (g < 250) lo = std::min(lo, g), hi = std::max(hi, g);
                }
            check(std::abs(lo - 128) <= 3, "S2: darkest pixel ~128 (50% over white): no darkening where the stroke crosses itself");
            std::printf("    darkest %d (expected ~128)\n", lo);
        }

        section("S3 tiled canvas, strokes across tile boundaries, zoomed in");
        check(engine.createCanvas({3000, 2000, kPaper}).empty(), "S3: 3000x2000 canvas (tiled)");
        check(engine.waitForStatus([](const EngineStatus& s) { return s.canvas.gpuReady && s.canvas.tileSize == 256; }, 10s),
              "S3: tiled storage ready");
        double z[] = {3.0, 0, 0};
        auto anchor = engine.canvasToScreen({512, 512});
        z[1] = anchor.x;
        z[2] = anchor.y;
        engine.viewportOp(pe::ViewOp::SetZoomAt, z, 3);
        double pan[] = {450 - anchor.x, 300 - anchor.y};
        engine.viewportOp(pe::ViewOp::PanBy, pan, 2);
        const auto a0 = engine.canvasToScreen({400, 512}), a1 = engine.canvasToScreen({640, 512});
        screenStroke(engine, brushOf(24, 1.0f, 0xFF2040A0u), screenLine(a0.x, a0.y, a1.x, a1.y, 25));
        const auto b0 = engine.canvasToScreen({512, 420}), b1 = engine.canvasToScreen({512, 600});
        screenStroke(engine, brushOf(24, 1.0f, 0xFF2040A0u), screenLine(b0.x, b0.y, b1.x, b1.y, 25));
        check(brushIdle(engine), "S3: processed");
        check(engine.waitForStatus([](const EngineStatus& s) { return s.canvas.rasterTiles == 4 && s.canvas.tilesAllocated == 4; }, 5s),
              "S3: strokes crossing the 4-tile corner touched exactly 4 tiles (CPU and GPU)");
        if (capture(engine, cap)) {
            // Along the horizontal stroke's centre (canvas y = 512 = a tile row edge) and
            // across the column edge at canvas x = 512: uniform colour, no seam.
            bool uniform = true;
            for (int x = 300; x <= 380; ++x)
                uniform &= std::abs(cap.at(x, 300)[2] - 0xA0) <= 3 && std::abs(cap.at(x, 300)[0] - 0x20) <= 3;
            for (int y = 220; y <= 290; ++y)
                uniform &= std::abs(cap.at(450, y)[2] - 0xA0) <= 3;
            check(uniform, "S3: stroke colour uniform across tile edges on screen (no seams)");
            // Edge profile symmetric around the tile row edge (canvas y = 512 -> screen y = 300).
            bool symmetric = true;
            for (int d = 0; d < 45; ++d) symmetric &= std::abs(cap.grey(330, 300 - d) - cap.grey(330, 299 + d)) <= 6;
            check(symmetric, "S3: stroke profile symmetric across the tile row boundary");
        }

        section("S4 rotated + zoomed view: brush size in canvas pixels, straight screen line");
        check(engine.createCanvas({1600, 1200, kPaper}).empty(), "S4: canvas");
        check(engine.waitForStatus([](const EngineStatus& s) { return s.canvas.gpuReady && s.canvas.width == 1600; }, 10s),
              "S4: ready");
        double r[] = {0.65, 450, 300};  // ~37 degrees
        engine.viewportOp(pe::ViewOp::SetRotationAt, r, 3);
        double zz[] = {2.0, 450, 300};
        engine.viewportOp(pe::ViewOp::SetZoomAt, zz, 3);
        const double zoomS4 = engine.viewportOp(pe::ViewOp::Query, nullptr, 0).state.zoom;
        screenStroke(engine, brushOf(10, 1.0f, 0xFF000000u), screenLine(250, 300, 650, 300, 33));
        check(brushIdle(engine), "S4: processed");
        // In canvas space the stroke is a rotated straight line of width 10.
        const auto c0 = engine.screenToCanvas({250, 300}), c1 = engine.screenToCanvas({650, 300});
        const pe::canvas::Vec2 dir = (c1 - c0) * (1.0 / (c1 - c0).length());
        const pe::canvas::Vec2 nrm{-dir.y, dir.x};
        bool centre = true, outside = true;
        for (int i = 1; i < 10; ++i) {
            const pe::canvas::Vec2 p = c0 + (c1 - c0) * (i / 10.0);
            centre &= engine.document().pixel(static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y))).a == 255;
            const pe::canvas::Vec2 q = p + nrm * 7.0, q2 = p - nrm * 7.0;
            outside &= engine.document().pixel(static_cast<int>(std::floor(q.x)), static_cast<int>(std::floor(q.y))).a == 0 &&
                       engine.document().pixel(static_cast<int>(std::floor(q2.x)), static_cast<int>(std::floor(q2.y))).a == 0;
        }
        check(centre && outside, "S4: canvas raster: rotated straight stroke, 10 canvas px wide");
        if (capture(engine, cap)) {
            const int half = static_cast<int>(std::lround(5 * zoomS4));
            bool onScreen = true;
            for (int x = 300; x <= 600; x += 10) onScreen &= cap.grey(x, 300) <= 4 && cap.grey(x, 300 - half - 3) >= 250 &&
                                                            cap.grey(x, 300 + half + 3) >= 250;
            check(onScreen, "S4: on screen: horizontal line, width = 10 x zoom, under the finger path");
        }

        section("S5 stroke cancellation (pinch after starting a stroke)");
        const uint64_t revBefore = engine.canvasContentRevision();
        std::vector<uint8_t> before;
        engine.exportCanvasTiles(false, 1000, before);
        screenStroke(engine, brushOf(60, 1.0f, 0xFFFF0000u), screenLine(300, 100, 600, 500, 20), 4, false);
        check(brushIdle(engine), "S5: in-progress stroke processed");
        check(engine.document().stats().brush.strokeActive, "S5: stroke active and visible");
        engine.strokeCancel();
        check(brushIdle(engine) && engine.waitForStatus([](const EngineStatus& s) { return s.canvas.brushCancelled >= 1; }, 5s),
              "S5: cancel processed");
        std::vector<uint8_t> after;
        engine.exportCanvasTiles(false, 1000, after);
        // Same set of non-empty tiles with identical pixels (tiles the
        // cancelled stroke created are exported as empty).
        auto nonEmpty = [](const std::vector<uint8_t>& d) {
            std::vector<std::vector<uint8_t>> out;
            int32_t n;
            std::memcpy(&n, d.data(), 4);
            size_t off = 4;
            for (int i = 0; i < n; ++i) {
                int32_t flags;
                std::memcpy(&flags, &d[off + 8], 4);
                const size_t start = off;
                off += 12;
                if (!(flags & 1)) {
                    out.emplace_back(d.begin() + start, d.begin() + off + pe::canvas::RasterStore::kTileBytes);
                    off += pe::canvas::RasterStore::kTileBytes;
                }
            }
            return out;
        };
        check(nonEmpty(before) == nonEmpty(after), "S5: canvas content identical to before the cancelled stroke");
        check(engine.canvasContentRevision() > revBefore, "S5: revision moved (the UI saves the restored state)");
        if (capture(engine, cap)) {
            const auto p = engine.canvasToScreen(engine.screenToCanvas({450, 300}));
            check(cap.at(static_cast<int>(p.x), static_cast<int>(p.y))[1] >= 200 || cap.grey(450, 300) <= 4,
                  "S5: no red left on screen");
        }

        section("S6 eraser on the GPU");
        screenStroke(engine, brushOf(30, 1.0f, 0xFF000000u, true), screenLine(400, 200, 400, 400, 10));
        check(brushIdle(engine), "S6: processed");
        if (capture(engine, cap)) check(cap.grey(400, 300) >= 250, "S6: erased area shows the paper again");

        section("S7 persistence round trip shows the same image");
        Cap before7, after7;
        // New canvases open fitted with no rotation: compare in that view.
        double r0[] = {0.0, 450, 300};
        engine.viewportOp(pe::ViewOp::SetRotationAt, r0, 3);
        engine.viewportOp(pe::ViewOp::Fit, nullptr, 0);
        const bool c1ok = capture(engine, before7);
        std::vector<uint8_t> all;
        engine.exportCanvasTiles(false, 1000, all);
        check(engine.createCanvas({1600, 1200, kPaper}).empty(), "S7: canvas re-created");
        check(engine.importCanvasTiles(all.data(), all.size()) >= 1, "S7: tiles imported");
        engine.waitForStatus([](const EngineStatus& s) { return s.canvas.gpuReady; }, 10s);
        const bool c2ok = capture(engine, after7);
        int worst = 0;
        if (c1ok && c2ok && before7.c.rgba.size() == after7.c.rgba.size())
            for (size_t i = 0; i < before7.c.rgba.size(); ++i) worst = std::max(worst, std::abs(before7.c.rgba[i] - after7.c.rgba[i]));
        check(c1ok && c2ok && worst <= 2, "S7: reloaded canvas renders identically (max diff " + std::to_string(worst) + ")");

        section("S8 sustained drawing: input independent of rendering, nothing lost");
        check(engine.createCanvas({4096, 4096, kPaper}).empty(), "S8: 4096x4096 canvas");
        check(engine.waitForStatus([](const EngineStatus& s) { return s.canvas.gpuReady && s.canvas.width == 4096; }, 10s),
              "S8: ready");
        double zf[] = {1.0, 450, 300};
        engine.viewportOp(pe::ViewOp::SetZoomAt, zf, 3);
        const uint64_t framesBefore = frames(engine);
        const auto rx0 = engine.status().canvas.brushSamplesReceived;
        const int64_t t0 = pe::nowNs();
        double worstLagMs = 0;
        int strokes = 0;
        // 40 fast strokes, samples every 4 ms in batches of 2 (like 240 Hz touch).
        for (int sidx = 0; sidx < 40; ++sidx) {
            const double y = 60 + (sidx % 12) * 40;
            std::vector<pe::canvas::Vec2> pts;
            for (int i = 0; i <= 60; ++i) pts.push_back({50 + i * 13.0, y + 25 * std::sin(i * 0.3)});
            engine.strokeBegin(brushOf(sidx % 2 ? 48.0f : 6.0f, 0.8f, 0xFF203060u), sample6(pts[0].x, pts[0].y, 0).v);
            for (size_t i = 1; i < pts.size(); i += 2) {
                Sample6 batch[2];
                size_t n = 0;
                for (size_t k = i; k < std::min(pts.size(), i + 2); ++k, ++n) batch[n] = sample6(pts[k].x, pts[k].y, k * 4.0);
                engine.strokeSamples(batch[0].v, n);
                std::this_thread::sleep_for(4ms);
            }
            engine.strokeEnd();
            const int64_t tEnd = pe::nowNs();
            brushIdle(engine, 5s);
            worstLagMs = std::max(worstLagMs, msSince(tEnd));
            strokes++;
        }
        const double totalMs = msSince(t0);
        const auto s8 = engine.status();
        const uint64_t received = s8.canvas.brushSamplesReceived - rx0;
        check(s8.canvas.brushSamplesProcessed == s8.canvas.brushSamplesReceived && received == 40u * 61u,
              "S8: all " + std::to_string(received) + " samples processed (none dropped)");
        std::printf("    %d strokes in %.0f ms, %llu frames, brush work last %.2f ms / max %.2f ms per frame,\n"
                    "    frame CPU work avg %.2f ms (record %.2f + submit %.2f), worst end-of-stroke -> processed %.1f ms\n",
                    strokes, totalMs, static_cast<unsigned long long>(frames(engine) - framesBefore),
                    s8.canvas.brushLastMs, s8.canvas.brushMaxMs, s8.frames.steadyAvgRecordMs + s8.frames.steadyAvgSubmitMs,
                    s8.frames.steadyAvgRecordMs, s8.frames.steadyAvgSubmitMs, worstLagMs);
        std::printf("    uploads %llu (%.1f MiB), raster tiles %llu, GPU tiles %u\n",
                    static_cast<unsigned long long>(s8.canvas.textureUploads), s8.canvas.bytesUploaded / 1048576.0,
                    static_cast<unsigned long long>(s8.canvas.rasterTiles), s8.canvas.tilesAllocated);
        check(s8.canvas.bytesUploaded < 4096ull * 4096 * 4, "S8: partial uploads only (less than one full canvas uploaded in total)");

        section("S10 stylus pressure and tilt on the GPU (Block 4)");
        check(engine.createCanvas({1200, 800, kPaper}).empty(), "S10: canvas");
        check(engine.waitForStatus([](const EngineStatus& s) { return s.canvas.gpuReady && s.canvas.width == 1200; }, 10s),
              "S10: ready");
        double z1[] = {1.0, 450, 300};
        engine.viewportOp(pe::ViewOp::SetZoomAt, z1, 3);
        pe::brush::BrushSettings pb = brushOf(40, 1.0f, 0xFF000000u);
        pb.pressureSize = true;
        pb.pressureSmoothing = 0.0f;
        const auto ramp = screenLine(150, 150, 750, 150, 61);
        screenStroke(engine, pb, ramp, 4, true, [](size_t k) { return static_cast<float>(k) / 60.0f; });
        // Finger stroke right after (no pressure axis): plain full-size stroke.
        screenStroke(engine, pb, screenLine(150, 300, 750, 300, 30));
        // Opacity from pressure.
        pe::brush::BrushSettings ob = brushOf(30, 1.0f, 0xFF000000u);
        ob.pressureOpacity = true;
        ob.pressureSmoothing = 0.0f;
        screenStroke(engine, ob, screenLine(150, 420, 750, 420, 30), 4, true, [](size_t) { return 0.5f; });
        check(brushIdle(engine), "S10: processed");
        Cap c10;
        if (capture(engine, c10)) {
            auto span = [&](int x, int yc) {
                int n = 0;
                for (int y = yc - 40; y <= yc + 40; ++y) n += c10.grey(x, y) < 128;
                return n;
            };
            const int lo = span(200, 150), mid = span(450, 150), hi = span(700, 150), finger = span(450, 300);
            std::printf("    pressure ramp widths on screen: %d / %d / %d px; finger stroke %d px\n", lo, mid, hi, finger);
            check(lo < hi - 20 && std::abs(mid - 24) <= 2, "S10: stroke width follows the pressure ramp on screen");
            check(std::abs(finger - 40) <= 2, "S10: finger stroke after a stylus stroke: full 40 px");
            check(std::abs(c10.grey(450, 420) - 102) <= 4, "S10: 50% pressure -> 60% opacity on screen (grey ~102)");
        }
        // Tilt: the stylus points right ON SCREEN; with the view rotated 90 degrees
        // the dab must still be long horizontally on screen (orientation mapped
        // to canvas space by the engine).
        check(engine.createCanvas({1200, 800, kPaper}).empty(), "S10: fresh canvas for the tilt check");
        engine.waitForStatus([](const EngineStatus& s) { return s.canvas.gpuReady && s.canvas.rasterTiles == 0; }, 10s);
        engine.viewportOp(pe::ViewOp::SetZoomAt, z1, 3);
        double r90[] = {pe::canvas::kPi / 2, 450, 300};
        engine.viewportOp(pe::ViewOp::SetRotationAt, r90, 3);
        pe::brush::BrushSettings tb = brushOf(20, 1.0f, 0xFF000000u);
        tb.tiltShape = true;
        const Sample6 tap = sample6(450, 560, 0, NAN, static_cast<float>(pe::canvas::kPi / 3), static_cast<float>(pe::canvas::kPi / 2));
        engine.strokeBegin(tb, tap.v);
        engine.strokeEnd();
        check(brushIdle(engine), "S10: tilted tap processed");
        if (capture(engine, c10)) {
            int w = 0, h = 0;
            for (int x = 400; x <= 500; ++x) w += c10.grey(x, 560) < 128;
            for (int y = 530; y <= 590; ++y) h += c10.grey(450, y) < 128;  // stays inside the canvas
            std::printf("    tilted dab on screen (view rotated 90 deg): %d x %d px\n", w, h);
            check(w > 40 && std::abs(h - 20) <= 2, "S10: tilt ellipse follows the stylus direction on screen, view rotated");
        }

        section("S11 brush library presets on the GPU (Block 5): display == CPU raster, every shipped preset");
        {
            bool header = false;
            const auto presets = pxtest::loadPresets(PE_BRUSH_ASSET, &header);
            check(header && presets.size() == 15, "S11: shipped preset file read (15 presets)");
            check(engine.createCanvas({1200, 800, kPaper}).empty(), "S11: canvas");
            engine.waitForStatus([](const EngineStatus& s) { return s.canvas.gpuReady && s.canvas.rasterTiles == 0; }, 10s);
            double one[] = {1.0, 450, 300};
            engine.viewportOp(pe::ViewOp::SetZoomAt, one, 3);
            double r0[] = {0.0, 450, 300};
            engine.viewportOp(pe::ViewOp::SetRotationAt, r0, 3);
            // 5 columns x 3 rows of short S-strokes, one per preset, 30 px.
            struct Box { int x0, y0, x1, y1; };
            std::vector<Box> boxes;
            for (size_t i = 0; i < presets.size(); ++i) {
                const int col = static_cast<int>(i % 5), row = static_cast<int>(i / 5);
                const double cx = 30 + col * 172, cy = 70 + row * 170;
                std::vector<pe::canvas::Vec2> pts;
                for (int k = 0; k < 40; ++k) {
                    const double t = k / 39.0;
                    pts.push_back({cx + 25 + 110 * t, cy + 30 * std::sin(t * 2 * pe::canvas::kPi)});
                }
                pe::brush::BrushSettings b = presets[i].s;
                b.diameter = 30;
                b.argb = 0xFF203060u;
                screenStroke(engine, b, pts, 4);
                boxes.push_back({static_cast<int>(cx), static_cast<int>(cy - 60), static_cast<int>(cx + 165), static_cast<int>(cy + 60)});
            }
            check(brushIdle(engine), "S11: all preset strokes processed");
            Cap c11;
            if (capture(engine, c11)) {
                bool inked = true, match = true;
                double worstRel = 0;
                for (size_t i = 0; i < boxes.size(); ++i) {
                    const Box& bx = boxes[i];
                    double gpu = 0, cpu = 0;
                    for (int y = bx.y0; y <= bx.y1; ++y)
                        for (int x = bx.x0; x <= bx.x1; ++x) {
                            gpu += 255 - c11.at(x, y)[2];  // blue channel: ink is dark on white
                            const pe::canvas::Vec2 cp = engine.screenToCanvas({x + 0.5, y + 0.5});
                            const auto px = engine.document().pixel(static_cast<int32_t>(std::floor(cp.x)), static_cast<int32_t>(std::floor(cp.y)));
                            cpu += 255 - (px.b + 255.0 * (1.0 - px.a / 255.0));
                        }
                    const double rel = std::abs(gpu - cpu) / std::max(1.0, cpu);
                    worstRel = std::max(worstRel, rel);
                    inked &= cpu > 255.0 * 300;
                    if (rel > 0.02) {
                        match = false;
                        std::printf("    %s: GPU ink %.0f vs CPU %.0f\n", presets[i].id.c_str(), gpu, cpu);
                    }
                }
                std::printf("    %zu preset strokes: worst GPU/CPU ink difference %.3f %%\n", boxes.size(), worstRel * 100);
                check(inked, "S11: every preset stroke is on the canvas");
                check(match, "S11: displayed ink == CPU raster ink (<= 2 %) for every preset (textures, buildup, wet edge, stamps)");
                // Texture survives the upload: the chalk stroke is speckled on screen.
                const Box& ch = boxes[9];
                int light = 0, dark = 0;
                for (int y = ch.y0; y <= ch.y1; ++y)
                    for (int x = ch.x0; x <= ch.x1; ++x) {
                        const int g = c11.grey(x, y);
                        light += g > 200 && g < 250;
                        dark += g < 120;
                    }
                check(presets[9].id == "builtin.chalk" && light > 200 && dark > 200,
                      "S11: chalk grain visible on screen (light and dark pixels inside the stroke)");
            }
        }

        section("S12 layers on the GPU (Block 6): blend modes, transparency, opacity, visibility, painting a layer");
        {
            check(engine.createCanvas({1200, 800, kPaper}).empty(), "S12: canvas");
            engine.waitForStatus([](const EngineStatus& s) { return s.canvas.gpuReady && s.canvas.rasterTiles == 0; }, 10s);
            engine.setCanvasColors(0xFF202020u, 0xFFE8D8B8u, false);  // cream paper: blend modes must use it
            double one[] = {1.0, 450, 300};
            engine.viewportOp(pe::ViewOp::SetZoomAt, one, 3);
            double r0[] = {0.0, 450, 300};
            engine.viewportOp(pe::ViewOp::SetRotationAt, r0, 3);
            const uint32_t liveBefore = engine.status().resources.totalLive();
            // Backdrop layer: a horizontal colour ramp over the left half, transparent on the right.
            auto c0 = engine.screenToCanvas({0.5, 0.5});
            const int ox = static_cast<int>(std::floor(c0.x)), oy = static_cast<int>(std::floor(c0.y));
            for (int i = 0; i < 8; ++i) {
                const uint32_t v = static_cast<uint32_t>(30 + i * 30);
                engine.document().fill({ox + 20 + i * 50, oy + 20, 50, 560}, 0xFF000000u | (v << 16) | ((255 - v) << 8) | 0x80u);
            }
            // One layer per blend mode: a column crossing ramp, paper and transparent parts.
            uint32_t id = 0;
            std::vector<uint32_t> ids;
            for (int m = 0; m < 8; ++m) {
                engine.layerOp(pe::Engine::LayerOp::Add, 0, 0, 0, "", id);
                ids.push_back(id);
                engine.document().fill({ox + 20, oy + 40 + m * 65, 860, 40}, 0xB4C06030u);
                engine.layerOp(pe::Engine::LayerOp::Blend, id, m, 0, "", id);
            }
            (void)engine.document().pixel(0, 0);  // finish the composite
            check(brushIdle(engine) && engine.waitForStatus([&](const EngineStatus&) { return !engine.document().hasPendingComposite(); }, 10s),
                  "S12: 9 layers composited");
            Cap c12;
            if (capture(engine, c12)) {
                // Display == CPU composite (paper included) at sample points of every band:
                // over the ramp, over plain paper, and where nothing is painted.
                int worst = 0;
                for (int m = 0; m < 8; ++m)
                    for (int sx : {60, 220, 380, 600, 850}) {
                        const int sy = 60 + m * 65;
                        const auto cp = engine.screenToCanvas({sx + 0.5, sy + 0.5});
                        const auto px = engine.document().pixel(static_cast<int32_t>(std::floor(cp.x)), static_cast<int32_t>(std::floor(cp.y)));
                        const uint8_t* g = c12.at(sx, sy);
                        worst = std::max({worst, std::abs(g[0] - px.r), std::abs(g[1] - px.g), std::abs(g[2] - px.b)});
                    }
                std::printf("    8 blend modes x 5 points (ramp / paper / beyond the canvas content): worst display-vs-composite diff %d\n", worst);
                check(worst <= 2, "S12: every blend mode shows on screen exactly as composited (paper as the backdrop)");
                // Known values: Multiply (mode 1) of the 70 % orange over cream paper is darker than both; Screen lighter.
                const uint8_t* mul = c12.at(850, 60 + 1 * 65);
                const uint8_t* scr = c12.at(850, 60 + 2 * 65);
                const uint8_t* paper = c12.at(850, 10);
                check(mul[2] < paper[2] && scr[0] >= paper[0], "S12: Multiply darkens, Screen lightens the paper");
            }
            // Opacity and visibility of one layer change the display (progressive recomposite).
            engine.layerOp(pe::Engine::LayerOp::Opacity, ids[0], 0, 0.0f, "", id);
            engine.layerOp(pe::Engine::LayerOp::Visible, ids[1], 0, 0, "", id);
            check(engine.waitForStatus([&](const EngineStatus&) { return !engine.document().hasPendingComposite(); }, 10s),
                  "S12: property changes recomposited");
            Cap c12b;
            if (capture(engine, c12b)) {
                const uint8_t* a = c12b.at(850, 60);
                const uint8_t* b = c12b.at(850, 125);
                const uint8_t* p = c12b.at(850, 10);
                check(std::abs(a[0] - p[0]) <= 1 && std::abs(b[0] - p[0]) <= 1 && std::abs(b[2] - p[2]) <= 1,
                      "S12: opacity 0 and hidden layers disappear from the display (paper shows)");
            }
            const uint32_t liveAfter = engine.status().resources.totalLive();
            std::printf("    GPU resources: %u before the layers, %u with 9 layers\n", liveBefore, liveAfter);
            check(liveAfter <= liveBefore + 4, "S12: layers allocate no GPU resources (the display stays one composite)");
            // Paint on the active (top) layer, zoomed and rotated, across a tile corner.
            double z2[] = {2.0, 450, 300};
            engine.viewportOp(pe::ViewOp::SetZoomAt, z2, 3);
            double r30[] = {0.5, 450, 300};
            engine.viewportOp(pe::ViewOp::SetRotationAt, r30, 3);
            const auto t0 = engine.canvasToScreen({480, 512}), t1 = engine.canvasToScreen({560, 512});
            screenStroke(engine, brushOf(20, 1.0f, 0xFF000000u), screenLine(t0.x, t0.y, t1.x, t1.y, 20));
            check(brushIdle(engine), "S12: stroke processed");
            const uint32_t top = ids.back();
            // Layer 1 has its ramp there (column 6: red 210): untouched by the stroke.
            const auto under = engine.document().layerPixel(1, 512, 512);
            check(engine.document().layerPixel(top, 512, 512).a == 255 && under.r == 210 && under.g == 45 &&
                      engine.document().layerPixel(ids[3], 512, 512).a == 0,
                  "S12: the stroke (zoom 2, rotated) is on the active layer only, across the 512 px tile corner");
            engine.layerOp(pe::Engine::LayerOp::Locked, top, 1, 0, "", id);
            float smp[pe::Engine::kSampleFloats] = {450, 300, 0, NAN, NAN, NAN};
            check(!engine.strokeBegin(brushOf(20, 1.0f, 0xFF000000u), smp), "S12: painting a locked layer is refused");
        }

        section("S13 undo / redo on the GPU (Block 7): the display returns exactly to the earlier frames");
        {
            auto settled = [&] {
                return brushIdle(engine) &&
                       engine.waitForStatus([&](const EngineStatus&) { return !engine.document().hasPendingComposite(); }, 10s);
            };
            auto diff = [](const Cap& a, const Cap& b) {
                if (a.c.width != b.c.width || a.c.height != b.c.height) return 999;
                int worst = 0;
                for (size_t i = 0; i < a.c.rgba.size(); ++i) worst = std::max(worst, std::abs(int(a.c.rgba[i]) - int(b.c.rgba[i])));
                return worst;
            };
            // Undo / redo of the given action, compared full-frame with the captures before / after it.
            auto roundTrip = [&](const char* what, const std::function<void()>& action, double* undoMs, double* redoMs) {
                Cap before, after, undone, redone;
                if (!settled() || !capture(engine, before)) return false;
                action();
                if (!settled() || !capture(engine, after)) return false;
                const bool changed = diff(before, after) > 0;
                auto timed = [&](pe::Engine::HistoryOp op, double* ms) {
                    const uint64_t f0 = frames(engine);
                    const auto t0 = std::chrono::steady_clock::now();
                    const int r = engine.historyOp(op, 1);
                    const bool shown = engine.waitForStatus([&](const EngineStatus& st) {
                        return st.frames.framesPresented > f0 && !engine.document().hasPendingComposite();
                    }, 10s);
                    if (ms) *ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                    return r == 1 && shown;
                };
                const bool u = timed(pe::Engine::HistoryOp::Undo, undoMs) && settled() && capture(engine, undone);
                const bool r = timed(pe::Engine::HistoryOp::Redo, redoMs) && settled() && capture(engine, redone);
                const int du = u ? diff(before, undone) : 999, dr = r ? diff(after, redone) : 999;
                std::printf("    %s: changed %s, undo vs before: max diff %d, redo vs after: max diff %d\n", what, changed ? "yes" : "NO",
                            du, dr);
                return changed && du == 0 && dr == 0;
            };
            for (const auto& size : {std::pair<uint32_t, uint32_t>{1200, 800}, std::pair<uint32_t, uint32_t>{4096, 4096}}) {
                const bool tiled = size.first > 2048;
                const std::string tag = tiled ? "S13 4096x4096 (tiled GPU storage): " : "S13 1200x800: ";
                check(engine.createCanvas({size.first, size.second, kPaper}).empty(), tag + "canvas");
                engine.waitForStatus([](const EngineStatus& st) { return st.canvas.gpuReady; }, 10s);
                engine.setCanvasColors(0xFF202020u, 0xFFE8D8B8u, false);
                double fit[] = {0.0};
                engine.viewportOp(pe::ViewOp::Fit, fit, 0);
                const uint32_t live0 = engine.status().resources.totalLive();
                double u1 = 0, r1 = 0, u2 = 0, r2 = 0;
                check(roundTrip("stroke", [&] { screenStroke(engine, brushOf(60, 0.8f, 0xFF2050C0u), screenLine(150, 200, 750, 420, 60)); }, &u1, &r1),
                      tag + "stroke: undo shows exactly the frame before it, redo exactly the frame after it");
                check(roundTrip("eraser", [&] {
                          auto b = brushOf(80, 1.0f, 0xFF000000u);
                          b.erase = true;
                          screenStroke(engine, b, screenLine(450, 100, 450, 500, 40));
                      }, &u2, &r2),
                      tag + "eraser: undo / redo exact on screen");
                uint32_t lid = 0;
                engine.layerOp(pe::Engine::LayerOp::Add, 0, 0, 0, "", lid);
                screenStroke(engine, brushOf(120, 0.7f, 0xFFE07020u), screenLine(150, 450, 750, 150, 40));
                engine.layerOp(pe::Engine::LayerOp::Blend, lid, static_cast<int32_t>(pe::canvas::BlendMode::Multiply), 0, "", lid);
                double u3 = 0, r3 = 0, u4 = 0, r4 = 0;
                check(roundTrip("delete a Multiply layer", [&] { engine.layerOp(pe::Engine::LayerOp::Delete, lid, 0, 0, "", lid); }, &u3, &r3),
                      tag + "delete layer: undo restores it on screen exactly (same id), redo removes it again");
                engine.historyOp(pe::Engine::HistoryOp::Undo, 1);  // the layer is back
                check(roundTrip("merge down", [&] { engine.layerOp(pe::Engine::LayerOp::MergeDown, lid, 0, 0, "", lid); }, &u4, &r4),
                      tag + "merge down: undo / redo exact on screen");
                std::printf("    undo -> frame presented: stroke %.1f ms, eraser %.1f ms, delete %.1f ms, merge %.1f ms | "
                            "redo: %.1f / %.1f / %.1f / %.1f ms\n", u1, u2, u3, u4, r1, r2, r3, r4);
                // Back and forth 20 times: nothing leaks, nothing released early (the frames stay correct).
                Cap end;
                bool ok = settled() && capture(engine, end);
                // (Tiled storage allocates GPU tiles where content is: compare with the state after the edits.)
                const uint32_t liveEnd = engine.status().resources.totalLive();
                for (int i = 0; i < 20 && ok; ++i) ok = engine.historyOp(pe::Engine::HistoryOp::Undo, 6) == 6 &&
                                                        engine.historyOp(pe::Engine::HistoryOp::Redo, 6) == 6;
                Cap again;
                ok = ok && settled() && capture(engine, again) && diff(end, again) == 0;
                const uint32_t live1 = engine.status().resources.totalLive();
                std::printf("    GPU resources: %u on the empty canvas, %u after the edits, %u after 20 x (undo 6 + redo 6)\n", live0,
                            liveEnd, live1);
                check(ok && live1 == liveEnd, tag + "20 undo / redo cycles: identical final frame, no GPU resources added or lost");
                engine.closeCanvas();
                engine.waitForStatus([](const EngineStatus& st) { return !st.canvas.open; }, 5s);
            }
        }

        section("S14 selection on the GPU (Block 10): clipped painting, moving content, undo / redo on screen");
        {
            auto settled = [&] {
                return brushIdle(engine) &&
                       engine.waitForStatus([&](const EngineStatus&) { return !engine.document().hasPendingComposite(); }, 10s);
            };
            auto diff = [](const Cap& a, const Cap& b) {
                if (a.c.width != b.c.width || a.c.height != b.c.height) return 999;
                int worst = 0;
                for (size_t i = 0; i < a.c.rgba.size(); ++i) worst = std::max(worst, std::abs(int(a.c.rgba[i]) - int(b.c.rgba[i])));
                return worst;
            };
            // Waits for a frame presented after now (the change is on screen); returns ms.
            auto shownMs = [&](const std::function<void()>& f) {
                const uint64_t f0 = frames(engine);
                const auto t0 = std::chrono::steady_clock::now();
                f();
                engine.waitForStatus([&](const EngineStatus& st) {
                    return st.frames.framesPresented > f0 && !engine.document().hasPendingComposite();
                }, 10s);
                return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            };
            check(engine.createCanvas({1200, 800, kPaper}).empty(), "S14: canvas");
            engine.waitForStatus([](const EngineStatus& st) { return st.canvas.gpuReady; }, 10s);
            engine.setCanvasColors(0xFF202020u, 0xFFE8D8B8u, false);
            double fit[] = {0.0};
            engine.viewportOp(pe::ViewOp::Fit, fit, 0);
            double rot[] = {0.4, 450, 300};
            engine.viewportOp(pe::ViewOp::SetRotationAt, rot, 3);
            const uint32_t live0 = engine.status().resources.totalLive();
            engine.document().fill({100, 100, 1000, 600}, 0xFF3070B0u);
            uint32_t lid = 0;
            engine.layerOp(pe::Engine::LayerOp::Add, 0, 0, 0, "", lid);
            engine.document().fill({500, 0, 200, 800}, 0x9020C040u);   // semi-transparent band on a second layer
            engine.layerOp(pe::Engine::LayerOp::Active, 1, 0, 0, "", lid);
            check(settled(), "S14: content settled");
            // Ellipse dragged on screen (rotated view: the shape is axis-aligned on the canvas).
            const float drag[4] = {250, 150, 650, 450};
            double selMs = shownMs([&] {
                check(engine.selectShape(pe::Engine::SelectShape::Ellipse, 0, true, drag, 2) == pe::LayerError::None &&
                          engine.selectionInfo().active,
                      "S14: ellipse selected from screen points");
            });
            Cap before, after;
            check(settled() && capture(engine, before), "S14: frame before painting");
            screenStroke(engine, brushOf(70, 1.0f, 0xFFD02020u), screenLine(80, 300, 820, 300, 60));
            check(settled() && capture(engine, after), "S14: frame after a stroke across the selection");
            // Screen pixels over canvas pixels whose 5x5 neighbourhood is entirely outside / inside.
            auto region = [&](int x, int y) {
                const auto c = engine.screenToCanvas({x + 0.5, y + 0.5});
                const int cx = static_cast<int>(std::floor(c.x)), cy = static_cast<int>(std::floor(c.y));
                int lo = 255, hi = 0;
                for (int j = -2; j <= 2; ++j)
                    for (int i = -2; i <= 2; ++i) {
                        const int v = engine.document().selectionAt(cx + i, cy + j);
                        lo = std::min(lo, v);
                        hi = std::max(hi, v);
                    }
                return hi == 0 ? 0 : lo == 255 ? 2 : 1;
            };
            size_t outChanged = 0, inChanged = 0, outPx = 0;
            for (int y = 0; y < static_cast<int>(after.c.height); ++y)
                for (int x = 0; x < static_cast<int>(after.c.width); ++x) {
                    const bool changed = std::memcmp(before.at(x, y), after.at(x, y), 4) != 0;
                    const int r = region(x, y);
                    if (r == 0) { outPx++; outChanged += changed; }
                    if (r == 2) inChanged += changed;
                }
            std::printf("    screen: %zu px outside the selection (all unchanged: %s), %zu px changed inside\n", outPx,
                        outChanged == 0 ? "yes" : "NO", inChanged);
            check(outChanged == 0 && inChanged > 2000 && outPx > 100000,
                  "S14: on screen, the stroke changed pixels inside the selection only (rotated, zoomed view)");
            // Moving the selected content: lifted into floating content, composited by the GPU while dragged.
            Cap m0, moving, moved;
            check(settled() && capture(engine, m0), "S14: frame before moving");
            double beginMs = shownMs([&] { uint32_t x; engine.selectionOp(pe::Engine::SelectionOp::MoveBegin, 0, x); });
            Cap lifted;
            check(settled() && capture(engine, lifted) && diff(m0, lifted) == 0, "S14: lifting the content does not change the picture");
            double stepMax = 0, stepSum = 0;
            for (int i = 1; i <= 12; ++i) {
                const double m = shownMs([&] { engine.selectionMoveScreen(10.0 * i, 5.0 * i); });
                stepMax = std::max(stepMax, m);
                stepSum += m;
            }
            check(settled() && capture(engine, moving) && diff(m0, moving) > 0, "S14: the moved content is displayed during the drag");
            uint32_t none = 0;
            double commitMs = shownMs([&] { engine.selectionOp(pe::Engine::SelectionOp::MoveCommit, 0, none); });
            check(settled() && capture(engine, moved) && diff(moving, moved) == 0, "S14: dropping it shows exactly what the drag showed");
            const uint64_t f0 = frames(engine);
            check(engine.historyOp(pe::Engine::HistoryOp::Undo, 1) == 1 &&
                      engine.waitForStatus([&](const EngineStatus& st) { return st.frames.framesPresented > f0; }, 10s) && settled(),
                  "S14: undo the move");
            Cap undone, redone;
            check(capture(engine, undone) && diff(m0, undone) == 0, "S14: undo: the frame before the move, exactly");
            engine.historyOp(pe::Engine::HistoryOp::Redo, 1);
            check(settled() && capture(engine, redone) && diff(moved, redone) == 0, "S14: redo: the moved frame, exactly");
            // Clear selected, cut + paste: undo / redo exact on screen.
            Cap c0, c1, c2, c3;
            check(settled() && capture(engine, c0), "S14: frame before clearing");
            double clearMs = shownMs([&] { engine.selectionOp(pe::Engine::SelectionOp::Clear, 0, none); });
            check(settled() && capture(engine, c1) && diff(c0, c1) > 0, "S14: clear selected shows the transparent area");
            engine.historyOp(pe::Engine::HistoryOp::Undo, 1);
            check(settled() && capture(engine, c2) && diff(c0, c2) == 0, "S14: undo clear: exact frame");
            engine.historyOp(pe::Engine::HistoryOp::Redo, 1);
            check(settled() && capture(engine, c3) && diff(c1, c3) == 0, "S14: redo clear: exact frame");
            uint32_t pid = 0;
            engine.historyOp(pe::Engine::HistoryOp::Undo, 1);
            engine.selectionOp(pe::Engine::SelectionOp::Copy, 0, none);
            double pasteMs = shownMs([&] { engine.selectionOp(pe::Engine::SelectionOp::Paste, 0, pid); });
            Cap p1;
            check(pid != 0 && settled() && capture(engine, p1), "S14: pasted as a new layer");
            Cap p2, p3;
            engine.historyOp(pe::Engine::HistoryOp::Undo, 1);
            check(settled() && capture(engine, p2) && diff(c0, p2) == 0, "S14: undo paste: exact frame without the pasted layer");
            engine.historyOp(pe::Engine::HistoryOp::Redo, 1);
            check(settled() && capture(engine, p3) && diff(p1, p3) == 0, "S14: redo paste: exact frame with it");
            std::printf("    select -> frame %.1f ms | move: begin %.1f ms, drag step avg %.1f / max %.1f ms, drop %.1f ms | "
                        "clear %.1f ms | paste %.1f ms\n", selMs, beginMs, stepSum / 12, stepMax, commitMs, clearMs, pasteMs);
            // Same final picture on every backend: unrotated, zoom 1, compared by run_host_tests.sh.
            double z1[] = {1.0, 450, 300};
            engine.viewportOp(pe::ViewOp::SetZoomAt, z1, 3);
            double r0[] = {0.0, 450, 300};
            engine.viewportOp(pe::ViewOp::SetRotationAt, r0, 3);
            engine.requestRender();
            Cap fin;
            if (settled() && engine.waitForStatus([&](const EngineStatus&) { return true; }, 1s) && capture(engine, fin)) {
                if (const char* dir = std::getenv("PE_CAPTURE_DIR")) {
                    const std::string path = std::string(dir) + "/s14_" + expected + ".rgba";
                    if (FILE* f = std::fopen(path.c_str(), "wb")) {
                        std::fwrite(fin.c.rgba.data(), 1, fin.c.rgba.size(), f);
                        std::fclose(f);
                    }
                }
                std::printf("    S14 composite checksum %016llx (CPU, identical on every backend)\n",
                            static_cast<unsigned long long>(engine.document().compositeChecksum()));
            }
            engine.closeCanvas();
            engine.waitForStatus([](const EngineStatus& st) { return !st.canvas.open; }, 5s);
            check(engine.waitForStatus([&](const EngineStatus& st) { return st.resources.totalLive() <= live0; }, 5s),
                  "S14: GPU resources back to the level before the selection work (floating content adds none)");
        }

        section("S9 close -> resources released");
        engine.closeCanvas();
        check(engine.waitForStatus([](const EngineStatus& s) { return !s.canvas.open; }, 5s), "S9: canvas closed");
        check(engine.waitForStatus([&](const EngineStatus& s) { return s.resources.totalLive() <= baseLive; }, 5s),
              "S9: live GPU resources back to the baseline");
        engine.shutdown();
        const auto report = pe::shutdown::last();
        check(report.valid && report.clean() && report.resourcesCreated == report.resourcesDestroyed,
              "S9: shutdown report: no leaked resources");
    }
    const uint64_t newErrors = totalErrors() - errorsAtStart;
    check(newErrors == 0, "brush suite: no engine or validation errors logged");
    if (newErrors)
        for (const auto& p : pe::log::recentProblems(10)) std::printf("    %s\n", p.c_str());
    return 0;
}

// ================================================================ suite: perf
// Same drawing scenario as the Block 2 baseline measurement (see
// docs/BLOCK3_REPORT.md): 40 fast strokes, a touch batch of 2 samples every
// 4 ms (240 Hz), 13 px per sample, brushes alternating 48 / 6 px, zoom 1.
int suitePerf(const std::string& pref, const std::string& expected, bool noFallback) {
    const BackendType want = expected == "vulkan" ? BackendType::Vulkan : BackendType::OpenGLES;
    std::shared_ptr<WindowProbe> probe;
    pe::EngineConfig cfg = makeConfig(pref, noFallback, RenderMode::OnDemand);
    cfg.scene = pe::SceneMode::Canvas;
    cfg.enableValidation = false;
    pe::Engine engine(cfg);
    engine.start();
    constexpr uint32_t kW = 1080, kH = 1600;  // phone-like surface
    engine.surfaceCreated(trackedWindow(kW, kH, probe));
    engine.surfaceChanged(kW, kH);
    check(waitRunning(engine) && engine.status().backend == want, "engine running on expected backend");
    check(engine.createCanvas({2480, 3508, 0xFFFFFFFFu}).empty(), "A4 canvas (tiled)");
    engine.waitForStatus([](const EngineStatus& s) { return s.canvas.gpuReady; }, 10s);
    double zf[] = {1.0, kW / 2.0, kH / 2.0};
    engine.viewportOp(pe::ViewOp::SetZoomAt, zf, 3);
    const auto s0 = engine.status();
    const int64_t t0 = pe::nowNs();
    std::vector<double> lags;
    for (int sidx = 0; sidx < 40; ++sidx) {
        const double y = 200 + (sidx % 24) * 50;
        std::vector<pe::canvas::Vec2> pts;
        for (int i = 0; i <= 60; ++i) pts.push_back({100 + i * 13.0, y + 25 * std::sin(i * 0.3)});
        const float size = sidx % 2 ? 48.0f : 6.0f;
        engine.strokeBegin(brushOf(size, 1.0f, 0xFF203060u), sample6(pts[0].x, pts[0].y, 0).v);
        for (size_t i = 1; i < pts.size(); i += 2) {
            Sample6 batch[2];
            size_t n = 0;
            for (size_t k = i; k < std::min(pts.size(), i + 2); ++k, ++n) batch[n] = sample6(pts[k].x, pts[k].y, k * 4.0);
            engine.strokeSamples(batch[0].v, n);
            std::this_thread::sleep_for(4ms);
        }
        engine.strokeEnd();
        const int64_t tEnd = pe::nowNs();
        const uint64_t framesAtEnd = frames(engine);
        brushIdle(engine, 10s);
        engine.waitForStatus([&](const EngineStatus& s) { return s.frames.framesPresented > framesAtEnd; }, 2s);
        lags.push_back(msSince(tEnd));
    }
    const double total = msSince(t0);
    const auto s1 = engine.status();
    std::sort(lags.begin(), lags.end());
    std::printf("    PERF %s: input->present avg %.2f ms, max %.2f ms (%llu frames with input); 40 strokes in %.0f ms "
                "(4.8 s of input pacing), %llu frames, frame CPU work avg %.2f ms; end of stroke -> idle: median %.1f ms, "
                "worst %.1f ms; %llu uploads, %.1f MiB uploaded\n",
                expected.c_str(), s1.canvas.inputLatencyAvgMs, s1.canvas.inputLatencyMaxMs,
                static_cast<unsigned long long>(s1.canvas.inputLatencyFrames), total,
                static_cast<unsigned long long>(s1.frames.framesPresented - s0.frames.framesPresented),
                s1.frames.steadyAvgRecordMs + s1.frames.steadyAvgSubmitMs, lags[lags.size() / 2], lags.back(),
                static_cast<unsigned long long>(s1.canvas.textureUploads - s0.canvas.textureUploads),
                (s1.canvas.bytesUploaded - s0.canvas.bytesUploaded) / 1048576.0);
    check(s1.canvas.brushSamplesProcessed == s1.canvas.brushSamplesReceived, "every sample processed");
    engine.shutdown();
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: %s <auto|vulkan|gles> <vulkan|gles|none> [--no-fallback] [--suite basic|lifecycle|faults|canvas|brush]\n",
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
    else if (suite == "brush")
        suiteBrush(pref, expected, noFallback);
    else if (suite == "perf")
        suitePerf(pref, expected, noFallback);
    else {
        std::fprintf(stderr, "unknown suite %s\n", suite.c_str());
        return 2;
    }
    pe::testhooks::install(nullptr);
    std::printf("=== %d/%d checks passed ===\n", gChecks - gFailures, gChecks);
    return gFailures == 0 ? 0 : 1;
}
