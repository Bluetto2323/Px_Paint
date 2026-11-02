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
//                     [--suite basic|lifecycle|faults]
//   basic     : selection, self-test, resize, pause/resume, surface loss,
//               render-mode / diagnostics separation. Expects 0 logged errors.
//   lifecycle : surfaceDestroyed mid-frame / mid-rebuild, shutdown during a
//               render request, rapid recreation, pause/resume churn.
//               Expects 0 logged errors.
//   faults    : injected render-thread exceptions and device losses,
//               recovery and backend fallback. Errors are expected here.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/Log.h"
#include "core/TestHooks.h"
#include "core/Time.h"
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
    const uint64_t errorsAtStart = pe::log::errorCount();
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
        check(framesAdvance(engine, 30), "continuous mode produces frames");

        section("resize");
        const uint32_t rebuildsBefore = engine.status().surface.recreateCount;
        engine.surfaceChanged(400, 300);
        check(engine.waitForStatus(
                  [&](const EngineStatus& st) { return st.surface.recreateCount > rebuildsBefore; }, 5s),
              "resize: surface/swapchain rebuilt");
        if (want == BackendType::OpenGLES) {
            const auto e = engine.status().surface.extent;
            check(e.width == 400 && e.height == 300, "resize: new extent 400x300");
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
                    static_cast<unsigned long long>(s.frames.framesSkipped), s.frames.avgCpuMs,
                    s.resources.totalLive());
        std::printf("--- overlay text ---\n%s--------------------\n", engine.statusText().c_str());
        engine.shutdown();
        check(probe && probe->released, "shutdown released the attached window");
    }
    const uint64_t newErrors = pe::log::errorCount() - errorsAtStart;
    check(newErrors == 0, "no errors logged (incl. leak check at shutdown)");
    if (newErrors)
        for (const auto& p : pe::log::recentProblems(10)) std::printf("    %s\n", p.c_str());
    return 0;
}

// ========================================================== suite: lifecycle
int suiteLifecycle(const std::string& pref, const std::string& expected, bool noFallback) {
    const BackendType want = expected == "vulkan" ? BackendType::Vulkan : BackendType::OpenGLES;
    const uint64_t errorsAtStart = pe::log::errorCount();
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

    const uint64_t newErrors = pe::log::errorCount() - errorsAtStart;
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

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: %s <auto|vulkan|gles> <vulkan|gles|none> [--no-fallback] [--suite basic|lifecycle|faults]\n",
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
    else {
        std::fprintf(stderr, "unknown suite %s\n", suite.c_str());
        return 2;
    }
    pe::testhooks::install(nullptr);
    std::printf("=== %d/%d checks passed ===\n", gChecks - gFailures, gChecks);
    return gFailures == 0 ? 0 : 1;
}
