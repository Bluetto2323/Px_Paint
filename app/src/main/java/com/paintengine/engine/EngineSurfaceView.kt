package com.paintengine.engine

import android.content.Context
import android.os.SystemClock
import android.util.Log
import android.view.SurfaceHolder
import android.view.SurfaceView

/**
 * SurfaceView that forwards its surface lifecycle to the native engine.
 *
 * The view never renders anything itself: the native render thread draws
 * into the ANativeWindow obtained from [SurfaceHolder.getSurface].
 *
 * [engine] can be replaced at runtime (backend switch). The surface state is
 * tracked here so a new engine can be attached to a surface that already
 * exists, and so diagnostics can report every lifecycle callback.
 */
class EngineSurfaceView(context: Context) : SurfaceView(context), SurfaceHolder.Callback2 {

    /** Receives lifecycle events (timestamped by the caller). */
    fun interface EventListener {
        fun onSurfaceEvent(message: String)
    }

    @Volatile var engine: NativeEngine? = null
    var eventListener: EventListener? = null

    @Volatile var hasSurface = false
        private set
    @Volatile var surfaceWidth = 0
        private set
    @Volatile var surfaceHeight = 0
        private set
    @Volatile var createdCount = 0
        private set
    @Volatile var destroyedCount = 0
        private set
    /** Duration of the last surfaceDestroyed callback (native release included). */
    @Volatile var lastDestroyMs = -1L
        private set
    @Volatile var maxDestroyMs = 0L
        private set

    init {
        holder.addCallback(this)
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        hasSurface = true
        createdCount++
        event("surfaceCreated #$createdCount")
        engine?.surfaceCreated(holder.surface)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        surfaceWidth = width
        surfaceHeight = height
        event("surfaceChanged ${width}x$height format=$format")
        engine?.surfaceChanged(width, height)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        // Must not return before the native side stopped using the window:
        // nativeSurfaceDestroyed blocks (no timeout) until the swapchain /
        // EGLSurface is destroyed and the ANativeWindow reference released.
        val start = SystemClock.elapsedRealtime()
        val releasedByRenderThread = engine?.surfaceDestroyed() ?: false
        val ms = SystemClock.elapsedRealtime() - start
        hasSurface = false
        destroyedCount++
        lastDestroyMs = ms
        if (ms > maxDestroyMs) maxDestroyMs = ms
        event("surfaceDestroyed #$destroyedCount: native surface released in $ms ms (render thread: $releasedByRenderThread)")
    }

    override fun surfaceRedrawNeeded(holder: SurfaceHolder) {
        engine?.requestRender()
    }

    private fun event(message: String) {
        Log.i(TAG, message)
        eventListener?.onSurfaceEvent(message)
    }

    private companion object {
        const val TAG = "PE.SurfaceView"
    }
}
