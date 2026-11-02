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
 */
class EngineSurfaceView(context: Context, private val engine: NativeEngine) :
    SurfaceView(context), SurfaceHolder.Callback2 {

    init {
        holder.addCallback(this)
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        Log.i(TAG, "surfaceCreated")
        engine.surfaceCreated(holder.surface)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        Log.i(TAG, "surfaceChanged ${width}x$height format=$format")
        engine.surfaceChanged(width, height)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        // Must not return before the native side stopped using the window:
        // nativeSurfaceDestroyed blocks (no timeout) until the swapchain /
        // EGLSurface is destroyed and the ANativeWindow reference released.
        val start = SystemClock.elapsedRealtime()
        val releasedByRenderThread = engine.surfaceDestroyed()
        Log.i(TAG, "surfaceDestroyed: native surface released in ${SystemClock.elapsedRealtime() - start} ms " +
            "(render thread: $releasedByRenderThread)")
    }

    override fun surfaceRedrawNeeded(holder: SurfaceHolder) {
        engine.requestRender()
    }

    private companion object {
        const val TAG = "PE.SurfaceView"
    }
}
