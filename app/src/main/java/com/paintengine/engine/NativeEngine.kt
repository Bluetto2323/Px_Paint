package com.paintengine.engine

import android.view.Surface

/**
 * Kotlin facade over the native engine (libpaintengine.so).
 *
 * Threading: lifecycle and control methods must be called from the main (UI)
 * thread. Read-only queries ([statusText], [statusJson], [capabilityReport])
 * may be called from any thread: they share a lock with [close], so the native
 * engine can never be destroyed while a query is running. The native side owns
 * its own render thread; every method only posts a command and returns
 * quickly, except [surfaceDestroyed], which blocks until the native side has
 * destroyed its swapchain / EGLSurface and released the ANativeWindow
 * (Android requires that before SurfaceHolder.Callback.surfaceDestroyed
 * returns). That wait has no timeout by design; the native render thread
 * never waits for the UI thread, so it cannot deadlock.
 */
class NativeEngine(config: Config) : AutoCloseable {

    enum class RenderMode(val nativeName: String) {
        /** A frame every vsync. */
        CONTINUOUS("continuous"),
        /** A frame only when requested (requestRender, lifecycle events). */
        ON_DEMAND("ondemand"),
    }

    /** What the engine draws. */
    enum class Scene(val code: Int) {
        /** Block 1 triangle: backend validation and pixel self-test. */
        DIAGNOSTIC(0),
        /** PxPaint canvas. */
        CANVAS(1),
    }

    /** Viewport operations; arguments in screen pixels and radians (see Engine.h ViewOp). */
    enum class ViewOp(val code: Int) {
        QUERY(0), FIT(1), CENTER(2), PAN_BY(3), ZOOM_AT(4), SET_ZOOM_AT(5), ROTATE_AT(6), SET_ROTATION_AT(7),
        GESTURE(8),
    }

    /** Snapshot of the canvas viewport. */
    data class ViewportInfo(
        val zoom: Double, val rotation: Double, val centerX: Double, val centerY: Double, val fitted: Boolean,
        val minZoom: Double, val maxZoom: Double, val canvasWidth: Int, val canvasHeight: Int,
        val screenWidth: Int, val screenHeight: Int,
    ) {
        val hasCanvas: Boolean get() = canvasWidth > 0 && canvasHeight > 0

        companion object {
            val EMPTY = ViewportInfo(1.0, 0.0, 0.0, 0.0, false, 0.0, 0.0, 0, 0, 0, 0)

            fun from(a: DoubleArray?): ViewportInfo = if (a == null || a.size < 11) EMPTY else ViewportInfo(
                a[0], a[1], a[2], a[3], a[4] != 0.0, a[5], a[6], a[7].toInt(), a[8].toInt(), a[9].toInt(), a[10].toInt(),
            )
        }
    }

    data class Config(
        /** "auto", "vulkan" or "gles". */
        val backend: String = "auto",
        val allowFallback: Boolean = true,
        val validation: Boolean = false,
        /** Independent of [diagnostics]. */
        val renderMode: RenderMode = RenderMode.ON_DEMAND,
        /** Periodic perf log + automatic pixel self-test; does not force continuous rendering. */
        val diagnostics: Boolean = true,
        val scene: Scene = Scene.DIAGNOSTIC,
        /** Canvas pixel budget in bytes; 0 = let the engine derive it from the GPU report. */
        val canvasMemoryBudgetBytes: Long = 0L,
    )

    @Volatile
    private var handle: Long = nativeCreate(
        config.backend, config.allowFallback, config.validation, config.renderMode.nativeName, config.diagnostics,
        config.scene.code, config.canvasMemoryBudgetBytes,
    )

    /** False if native creation failed (see logcat) or after [close]. */
    val isAlive: Boolean get() = handle != 0L

    fun surfaceCreated(surface: Surface) = withHandle { nativeSurfaceCreated(it, surface) }
    fun surfaceChanged(width: Int, height: Int) = withHandle { nativeSurfaceChanged(it, width, height) }

    /**
     * Blocks until the native engine no longer uses the surface. Safe after
     * [close] (the native teardown already released the window).
     * @return true if the render thread released it, false if no render
     * thread was running (nothing could be using the surface).
     */
    fun surfaceDestroyed(): Boolean {
        val h = handle
        return if (h != 0L) nativeSurfaceDestroyed(h) else false
    }

    fun pause() = withHandle { nativePause(it) }
    fun resume() = withHandle { nativeResume(it) }
    fun requestRender() = withHandle { nativeRequestRender(it) }
    fun setRenderMode(mode: RenderMode) = withHandle { nativeSetRenderMode(it, mode.nativeName) }
    fun setDiagnosticsEnabled(enabled: Boolean) = withHandle { nativeSetDiagnosticsEnabled(it, enabled) }
    fun runSelfTest() = withHandle { nativeRunSelfTest(it) }

    // --- Canvas (UI thread) ----------------------------------------------
    fun setScene(scene: Scene) = withHandle { nativeSetSceneMode(it, scene.code) }

    /** Returns null if accepted (GPU storage is created asynchronously, see statusJson), else the reason. */
    fun createCanvas(width: Int, height: Int, paperArgb: Int, transparentBackground: Boolean): String? {
        val h = handle
        if (h == 0L) return "engine not running"
        return nativeCreateCanvas(h, width, height, paperArgb, transparentBackground).ifEmpty { null }
    }

    /** JSON {ok, tiled, maxBytes, reason}: storage decision on this device, nothing is allocated. */
    fun checkCanvasSize(width: Int, height: Int): String =
        handle.let { if (it != 0L) nativeCheckCanvasSize(it, width, height) else "{\"ok\":false,\"reason\":\"engine not running\"}" }

    fun closeCanvas() = withHandle { nativeCloseCanvas(it) }
    fun setCanvasColors(workspaceArgb: Int, paperArgb: Int, transparentBackground: Boolean) =
        withHandle { nativeSetCanvasColors(it, workspaceArgb, paperArgb, transparentBackground) }
    fun clearCanvas() = withHandle { nativeClearCanvas(it) }
    fun runCanvasSelfTest() = withHandle { nativeRunCanvasSelfTest(it) }

    /** Applies a viewport operation immediately (the frame follows) and returns the new state. */
    fun viewport(op: ViewOp, vararg args: Double): ViewportInfo {
        val h = handle
        return if (h == 0L) ViewportInfo.EMPTY else ViewportInfo.from(nativeViewportOp(h, op.code, args))
    }

    /** Exact screen (view pixels) -> canvas pixels mapping. */
    fun screenToCanvas(x: Double, y: Double): DoubleArray =
        handle.let { if (it != 0L) nativeMapPoint(it, true, x, y) ?: doubleArrayOf(0.0, 0.0) else doubleArrayOf(0.0, 0.0) }

    fun canvasToScreen(x: Double, y: Double): DoubleArray =
        handle.let { if (it != 0L) nativeMapPoint(it, false, x, y) ?: doubleArrayOf(0.0, 0.0) else doubleArrayOf(0.0, 0.0) }

    /** Diagnostic square stamp under a screen point (no brush engine yet). False if off the canvas. */
    fun stamp(x: Double, y: Double, sizePx: Int, argb: Int): Boolean =
        handle.let { it != 0L && nativeStamp(it, x, y, sizePx, argb) }
    @Synchronized
    fun statusText(): String = handle.let { if (it != 0L) nativeStatusText(it) else "engine not running" }

    /** Machine-readable status (see diagnostics::statusJson). "{}" after [close]. */
    @Synchronized
    fun statusJson(): String = handle.let { if (it != 0L) nativeStatusJson(it) else "{}" }

    @Synchronized
    fun capabilityReport(): String = handle.let { if (it != 0L) nativeCapabilityReport(it) else "" }

    fun logReport() = withHandle { nativeLogReport(it) }

    /** Stops the render thread and frees every GPU resource (blocking). Idempotent. */
    @Synchronized
    override fun close() {
        val h = handle
        handle = 0L
        if (h != 0L) nativeDestroy(h)
    }

    private inline fun withHandle(block: (Long) -> Unit) {
        val h = handle
        if (h != 0L) block(h)
    }

    companion object {
        /** Null when the library loaded, otherwise the load error. */
        val loadError: Throwable? = try {
            System.loadLibrary("paintengine")
            null
        } catch (t: UnsatisfiedLinkError) {
            t
        }

        @JvmStatic private external fun nativeCreate(
            backend: String, allowFallback: Boolean, validation: Boolean, renderMode: String, diagnostics: Boolean,
            scene: Int, canvasBudgetBytes: Long,
        ): Long
        @JvmStatic private external fun nativeDestroy(handle: Long)
        @JvmStatic private external fun nativeSurfaceCreated(handle: Long, surface: Surface)
        @JvmStatic private external fun nativeSurfaceChanged(handle: Long, width: Int, height: Int)
        @JvmStatic private external fun nativeSurfaceDestroyed(handle: Long): Boolean
        @JvmStatic private external fun nativePause(handle: Long)
        @JvmStatic private external fun nativeResume(handle: Long)
        @JvmStatic private external fun nativeRequestRender(handle: Long)
        @JvmStatic private external fun nativeSetRenderMode(handle: Long, mode: String)
        @JvmStatic private external fun nativeSetDiagnosticsEnabled(handle: Long, enabled: Boolean)
        @JvmStatic private external fun nativeRunSelfTest(handle: Long)
        @JvmStatic private external fun nativeStatusText(handle: Long): String
        @JvmStatic private external fun nativeCapabilityReport(handle: Long): String
        @JvmStatic private external fun nativeLogReport(handle: Long)
        @JvmStatic private external fun nativeStatusJson(handle: Long): String
        @JvmStatic private external fun nativeRecentLog(maxLines: Int): String
        @JvmStatic private external fun nativeLastShutdownReport(): String
        @JvmStatic private external fun nativeSetSceneMode(handle: Long, scene: Int)
        @JvmStatic private external fun nativeCreateCanvas(
            handle: Long, width: Int, height: Int, paperArgb: Int, transparent: Boolean,
        ): String
        @JvmStatic private external fun nativeCheckCanvasSize(handle: Long, width: Int, height: Int): String
        @JvmStatic private external fun nativeCloseCanvas(handle: Long)
        @JvmStatic private external fun nativeSetCanvasColors(handle: Long, workspace: Int, paper: Int, transparent: Boolean)
        @JvmStatic private external fun nativeClearCanvas(handle: Long)
        @JvmStatic private external fun nativeViewportOp(handle: Long, op: Int, args: DoubleArray): DoubleArray?
        @JvmStatic private external fun nativeMapPoint(handle: Long, toCanvas: Boolean, x: Double, y: Double): DoubleArray?
        @JvmStatic private external fun nativeStamp(handle: Long, x: Double, y: Double, size: Int, argb: Int): Boolean
        @JvmStatic private external fun nativeRunCanvasSelfTest(handle: Long)

        /** Last [maxLines] native log lines (all sources), works without an engine. */
        fun recentLog(maxLines: Int): String = if (loadError == null) nativeRecentLog(maxLines) else ""

        /** JSON bookkeeping of the most recently destroyed graphics device. */
        fun lastShutdownReport(): String = if (loadError == null) nativeLastShutdownReport() else "{}"
    }
}
