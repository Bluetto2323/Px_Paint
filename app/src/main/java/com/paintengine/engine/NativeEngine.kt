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

    data class Config(
        /** "auto", "vulkan" or "gles". */
        val backend: String = "auto",
        val allowFallback: Boolean = true,
        val validation: Boolean = false,
        /** Independent of [diagnostics]. */
        val renderMode: RenderMode = RenderMode.ON_DEMAND,
        /** Periodic perf log + automatic pixel self-test; does not force continuous rendering. */
        val diagnostics: Boolean = true,
    )

    @Volatile
    private var handle: Long = nativeCreate(
        config.backend, config.allowFallback, config.validation, config.renderMode.nativeName, config.diagnostics,
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

        /** Last [maxLines] native log lines (all sources), works without an engine. */
        fun recentLog(maxLines: Int): String = if (loadError == null) nativeRecentLog(maxLines) else ""

        /** JSON bookkeeping of the most recently destroyed graphics device. */
        fun lastShutdownReport(): String = if (loadError == null) nativeLastShutdownReport() else "{}"
    }
}
