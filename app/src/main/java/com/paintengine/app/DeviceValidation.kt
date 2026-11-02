package com.paintengine.app

import android.os.SystemClock
import android.view.Gravity
import android.view.View
import android.widget.FrameLayout
import com.paintengine.engine.NativeEngine
import org.json.JSONObject
import java.util.Locale
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * On-device stability / lifecycle validation, driven through the REAL Android
 * machinery (no fake callbacks):
 *  - SurfaceView GONE/VISIBLE  -> Android destroys / recreates the surface;
 *  - SurfaceView size changes  -> real surfaceChanged + swapchain rebuild;
 *  - backend switch            -> full native teardown + new device on the
 *                                 same Android Surface, with leak bookkeeping.
 * Each step is verified from engine state and the pixel self-test (which also
 * checks the triangle is not distorted).
 *
 * Runs on its own thread; every UI / engine-control action is executed on the
 * main thread and waited for. Engine state is read through the thread-safe
 * NativeEngine.statusJson().
 */
class DeviceValidation(private val host: Host) {

    /** What the runner needs from the Activity (all called on the main thread). */
    interface Host {
        val engine: NativeEngine?
        val surfaceView: com.paintengine.engine.EngineSurfaceView
        val isInForeground: Boolean
        fun setRenderMode(mode: NativeEngine.RenderMode)
        fun setDiagnostics(enabled: Boolean)
        fun switchBackend(preference: String)
        val backendPreference: String
        fun runOnMain(action: Runnable)
        fun onProgress(message: String)
    }

    private class Aborted(message: String) : Exception(message)

    private val out = StringBuilder()
    private var passed = 0
    private var failed = 0
    private var skipped = 0

    fun run(): String {
        val t0 = SystemClock.elapsedRealtime()
        line("=== DEVICE VALIDATION (automated, on-device) ===")
        try {
            steps()
        } catch (e: Aborted) {
            line("ABORTED: ${e.message}")
            failed++
        } catch (t: Throwable) {
            line("ABORTED by exception: $t")
            failed++
        }
        line(String.format(Locale.US, "=== RESULT: %d passed, %d failed, %d skipped (%.1f s) ===",
            passed, failed, skipped, (SystemClock.elapsedRealtime() - t0) / 1000.0))
        EventLog.add("Device validation finished: $passed passed, $failed failed, $skipped skipped")
        return out.toString()
    }

    // ------------------------------------------------------------------ steps
    private fun steps() {
        val originalPref = host.backendPreference
        ui { host.setDiagnostics(true); host.setRenderMode(NativeEngine.RenderMode.CONTINUOUS) }

        // A ---------------------------------------------------------------
        section("A. Initial state")
        check("engine running with a self-test result", waitFor(20_000) {
            it.optString("state") == "Running" && it.optInt("selfTestRuns") > 0 && it.optString("selfTest") != "pending"
        })
        val base = status()
        line("    backend=${base.optString("backend")} gpu=${base.optString("gpu")}")
        line("    api=${base.optString("api")} driver=${base.optString("driver")}")
        line(fmt("    backend init %.1f ms, first frame %.1f ms after attach",
            base.optDouble("backendInitMs"), base.optDouble("firstFrameMs")))
        check("self-test passed (${base.optString("selfTestDetail")})", base.optString("selfTest") == "PASSED")
        val baseErrors = base.optLong("engineErrors")
        val baseWarnings = base.optLong("engineWarnings")
        val baseLive = base.optInt("liveResources")

        // B ---------------------------------------------------------------
        section("B. Stable continuous rendering (5 s) + repeated self-tests")
        ensureForeground()
        var s0 = status()
        SystemClock.sleep(5_000)
        var s1 = status()
        val rendered = s1.optLong("framesRendered") - s0.optLong("framesRendered")
        val badSkips = s1.optLong("skippedError") - s0.optLong("skippedError")
        line(fmt("    %d frames rendered in 5 s, steady CPU avg %.2f ms / max %.2f ms, %s",
            rendered, s1.optDouble("steadyAvgCpuMs"), s1.optDouble("steadyMaxCpuMs"),
            if (s1.optBoolean("rateValid")) fmt("%.1f fps", s1.optDouble("fps")) else "rate not yet valid"))
        check("at least 100 frames rendered in 5 s (>= 20 fps)", rendered >= 100)
        check("no frames lost to errors", badSkips == 0L)
        for (i in 1..3) {
            check("self-test #$i during continuous rendering", runSelfTest())
            SystemClock.sleep(300)
        }

        // C ---------------------------------------------------------------
        section("C. Surface destroy / recreate x10 (SurfaceView GONE -> VISIBLE)")
        var allOk = true
        for (i in 1..10) {
            ensureForeground()
            val destroyedBefore = host.surfaceView.destroyedCount
            val runsBefore = status().optInt("selfTestRuns")
            ui { host.surfaceView.visibility = View.GONE }
            val gone = waitFor(5_000) { host.surfaceView.destroyedCount > destroyedBefore && !it.optBoolean("surfaceAttached") }
            ui { host.surfaceView.visibility = View.VISIBLE }
            val back = waitFor(15_000) {
                it.optBoolean("surfaceAttached") && it.optInt("selfTestRuns") > runsBefore && it.optString("selfTest") != "pending"
            }
            val st = status()
            val ok = gone && back && st.optString("selfTest") == "PASSED"
            if (!ok) {
                allOk = false
                line("    cycle $i FAILED: gone=$gone back=$back selfTest=${st.optString("selfTest")} ${st.optString("selfTestDetail")}")
            }
        }
        check("10/10 cycles: surface released, re-attached, pixels correct", allOk)
        line("    surfaceDestroyed callback: last ${host.surfaceView.lastDestroyMs} ms, max ${host.surfaceView.maxDestroyMs} ms")
        check("surfaceDestroyed never blocked the UI thread > 500 ms", host.surfaceView.maxDestroyMs <= 500)
        waitFor(2_000) { it.optInt("pendingDestruction") == 0 }
        s1 = status()
        check("GPU resources unchanged (${s1.optInt("liveResources")} live, was $baseLive), nothing pending",
            s1.optInt("liveResources") == baseLive && s1.optInt("pendingDestruction") == 0)

        // D ---------------------------------------------------------------
        section("D. Surface resize (real surfaceChanged) - triangle must stay undistorted")
        val fullW = host.surfaceView.width
        val fullH = host.surfaceView.height
        val sizes = listOf(Pair(fullW, fullH * 6 / 10), Pair(fullW * 7 / 10, fullH * 7 / 10), Pair(fullW / 2, fullH))
        var resizeOk = true
        for ((w, h) in sizes) {
            ensureForeground()
            ui { host.surfaceView.layoutParams = FrameLayout.LayoutParams(w, h, Gravity.CENTER) }
            val sized = waitFor(5_000) {
                it.optBoolean("surfaceAttached") &&
                    ((it.optInt("surfaceWidth") == w && it.optInt("surfaceHeight") == h) ||
                        (it.optInt("surfaceWidth") == h && it.optInt("surfaceHeight") == w))
            }
            val st = status()
            val test = sized && runSelfTest()
            line("    ${w}x$h -> surface ${st.optInt("surfaceWidth")}x${st.optInt("surfaceHeight")} " +
                fmt("(rebuild %.1f ms) ", st.optDouble("lastRebuildMs")) + if (test) "OK" else "FAILED: ${status().optString("selfTestDetail")}")
            resizeOk = resizeOk && test
        }
        ui {
            host.surfaceView.layoutParams =
                FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT)
        }
        val restored = waitFor(5_000) { it.optInt("surfaceWidth") * it.optInt("surfaceHeight") == fullW * fullH }
        check("3 sizes: surface follows the view, self-test (incl. geometry) passes", resizeOk)
        check("full size restored and self-test passes", restored && runSelfTest())

        // E ---------------------------------------------------------------
        section("E. Pause / resume x20 (engine level)")
        ensureForeground()
        ui { repeat(20) { host.engine?.pause(); host.engine?.resume() } }
        ui { host.engine?.pause() }
        check("engine reports paused", waitFor(2_000) { it.optBoolean("paused") })
        SystemClock.sleep(200)
        s0 = status()
        SystemClock.sleep(600)
        s1 = status()
        check("no frames while paused", s1.optLong("framesRendered") == s0.optLong("framesRendered"))
        ui { host.engine?.resume() }
        check("rendering resumes", framesAdvance(30, 3_000))

        // F ---------------------------------------------------------------
        section("F. Render modes")
        ui { host.setRenderMode(NativeEngine.RenderMode.ON_DEMAND) }
        SystemClock.sleep(400)
        s0 = status()
        SystemClock.sleep(1_000)
        s1 = status()
        check("on-demand: idle (<= 1 frame in 1 s)", s1.optLong("framesRendered") - s0.optLong("framesRendered") <= 1)
        check("on-demand: no FPS reported for idle time", !s1.optBoolean("rateValid"))
        ui { host.engine?.requestRender() }
        check("on-demand: requestRender produces a frame", framesAdvance(1, 2_000))
        ui { host.setRenderMode(NativeEngine.RenderMode.CONTINUOUS) }
        check("continuous: rendering every vsync again", framesAdvance(30, 3_000))
        check("continuous: FPS measured", waitFor(3_000) { it.optBoolean("rateValid") })

        // G ---------------------------------------------------------------
        section("G. Backend switching (full teardown + new device on the same Surface)")
        val sequence = listOf("gles" to "OpenGL ES", "vulkan" to "Vulkan")
        for ((pref, expected) in sequence) {
            ensureForeground()
            ui { host.switchBackend(pref) }
            val prev = JSONObject(NativeEngine.lastShutdownReport())
            check("previous device shut down cleanly: ${prev.optString("text")}",
                prev.optBoolean("valid") && prev.optBoolean("clean") && prev.optLong("created") == prev.optLong("destroyed"))
            val up = waitFor(20_000) {
                it.optString("state") == "Running" && it.optInt("selfTestRuns") > 0 && it.optString("selfTest") != "pending"
            }
            val st = status()
            if (expected == "Vulkan" && st.optString("backend") != "Vulkan" && up) {
                line("    Vulkan not available on this device: got ${st.optString("backend")}")
                skipped++
                continue
            }
            check("$expected selected and running (init ${fmt("%.1f", st.optDouble("backendInitMs"))} ms), diagnostics report ${st.optString("backend")}",
                up && st.optString("backend") == expected)
            check("$expected self-test: ${st.optString("selfTestDetail")}", st.optString("selfTest") == "PASSED")
            check("$expected: ${st.optInt("liveResources")} live resources (same scene as before: $baseLive)",
                st.optInt("liveResources") == baseLive)
        }
        if (originalPref != "vulkan") {
            ui { host.switchBackend(originalPref) }
            check("restored original backend preference '$originalPref'", waitFor(20_000) { it.optString("state") == "Running" })
        }

        // H ---------------------------------------------------------------
        section("H. Error accounting")
        val end = status()
        val newErrors = end.optLong("engineErrors") - baseErrors
        val newWarnings = end.optLong("engineWarnings") - baseWarnings
        check("no engine errors during the whole run", newErrors == 0L)
        line("    engine warnings during run: $newWarnings")
        line("    validation layer: ${if (end.optBoolean("validationLayerActive")) "active" else "not installed (optional)"}, " +
            "${end.optLong("validationErrors")} errors / ${end.optLong("validationWarnings")} warnings (not engine errors)")
        line("    driver debug messages: ${end.optLong("driverDebugMessages")} (informational)")
        line("    device recoveries: ${end.optInt("deviceRecoveries")}, skipped frames: ${end.optLong("framesSkipped")} " +
            "(out-of-date ${end.optLong("skippedOutOfDate")}, errors ${end.optLong("skippedError")})")
    }

    // ---------------------------------------------------------------- helpers
    private fun status(): JSONObject = try {
        JSONObject(host.engine?.statusJson() ?: "{}")
    } catch (e: Exception) {
        JSONObject()
    }

    private fun waitFor(timeoutMs: Long, predicate: (JSONObject) -> Boolean): Boolean {
        val end = SystemClock.elapsedRealtime() + timeoutMs
        while (SystemClock.elapsedRealtime() < end) {
            if (predicate(status())) return true
            SystemClock.sleep(50)
        }
        return predicate(status())
    }

    private fun framesAdvance(delta: Long, timeoutMs: Long): Boolean {
        val start = status().optLong("framesRendered")
        return waitFor(timeoutMs) { it.optLong("framesRendered") >= start + delta }
    }

    private fun runSelfTest(): Boolean {
        val runs = status().optInt("selfTestRuns")
        ui { host.engine?.runSelfTest() }
        val done = waitFor(10_000) { it.optInt("selfTestRuns") > runs && it.optString("selfTest") != "pending" }
        return done && status().optString("selfTest") == "PASSED"
    }

    private fun ensureForeground() {
        if (!ui { host.isInForeground }) throw Aborted("app left the foreground - keep the app open during the test")
    }

    private fun <T> ui(block: () -> T): T {
        val latch = CountDownLatch(1)
        var result: T? = null
        var error: Throwable? = null
        host.runOnMain {
            try {
                result = block()
            } catch (t: Throwable) {
                error = t
            } finally {
                latch.countDown()
            }
        }
        if (!latch.await(60, TimeUnit.SECONDS)) throw Aborted("main thread did not respond within 60 s")
        error?.let { throw it }
        @Suppress("UNCHECKED_CAST")
        return result as T
    }

    private fun check(name: String, ok: Boolean) {
        if (ok) passed++ else failed++
        line("  [${if (ok) "PASS" else "FAIL"}] $name")
    }

    private fun section(name: String) {
        line("-- $name")
        host.onProgress("Device test: $name")
    }

    private fun line(text: String) {
        out.append(text).append('\n')
        EventLog.add("test: $text")
    }

    private fun fmt(format: String, vararg args: Any): String = String.format(Locale.US, format, *args)
}
