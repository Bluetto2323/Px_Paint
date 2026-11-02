package com.paintengine.app

import android.app.Activity
import android.graphics.Color
import android.graphics.Typeface
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.util.TypedValue
import android.view.Gravity
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import com.paintengine.BuildConfig
import com.paintengine.engine.EngineSurfaceView
import com.paintengine.engine.NativeEngine

/**
 * Block 1 host activity: a full-screen engine surface plus a diagnostics
 * overlay. Not the final UI.
 *
 * Launch options (adb):
 *   adb shell am start -n com.paintengine/.app.MainActivity --es backend gles
 *   adb shell am start -n com.paintengine/.app.MainActivity --es backend vulkan --ez fallback false
 *   adb shell am start -n com.paintengine/.app.MainActivity --ez validation true
 *   adb shell am start -n com.paintengine/.app.MainActivity --es render continuous --ez diagnostics false
 *
 * Render mode (continuous / on-demand) and diagnostics are independent
 * settings: the MODE and DIAG buttons toggle them separately.
 */
class MainActivity : Activity() {

    private var engine: NativeEngine? = null
    private lateinit var overlay: TextView
    private lateinit var overlayScroll: ScrollView
    private var diagnosticsOn = true
    private var renderMode = NativeEngine.RenderMode.ON_DEMAND
    private val handler = Handler(Looper.getMainLooper())
    private val overlayUpdater = object : Runnable {
        override fun run() {
            engine?.let { overlay.text = it.statusText() }
            handler.postDelayed(this, OVERLAY_PERIOD_MS)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val root = FrameLayout(this)

        NativeEngine.loadError?.let { error ->
            Log.e(TAG, "Native library failed to load", error)
            root.addView(TextView(this).apply {
                text = "libpaintengine.so failed to load:\n$error"
                setTextColor(Color.RED)
                setPadding(32, 96, 32, 32)
            })
            setContentView(root)
            return
        }

        diagnosticsOn = intent.getBooleanExtra("diagnostics", true)
        renderMode = if (intent.getStringExtra("render") == "continuous") NativeEngine.RenderMode.CONTINUOUS
        else NativeEngine.RenderMode.ON_DEMAND
        val config = NativeEngine.Config(
            backend = intent.getStringExtra("backend") ?: "auto",
            allowFallback = intent.getBooleanExtra("fallback", true),
            validation = intent.getBooleanExtra("validation", BuildConfig.DEBUG),
            renderMode = renderMode,
            diagnostics = diagnosticsOn,
        )
        Log.i(TAG, "Creating engine with $config")
        val engine = NativeEngine(config)
        if (!engine.isAlive) {
            root.addView(TextView(this).apply {
                text = "Native engine creation failed (see adb logcat -s 'PE.*')"
                setTextColor(Color.RED)
                setPadding(32, 96, 32, 32)
            })
            setContentView(root)
            return
        }
        this.engine = engine

        root.addView(EngineSurfaceView(this, engine), FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))

        overlay = TextView(this).apply {
            typeface = Typeface.MONOSPACE
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 10f)
            setTextColor(Color.WHITE)
            setBackgroundColor(0xAA000000.toInt())
            setPadding(16, 16, 16, 16)
            text = "starting..."
        }
        overlayScroll = ScrollView(this).apply {
            addView(overlay)
            visibility = if (diagnosticsOn) android.view.View.VISIBLE else android.view.View.GONE
        }
        root.addView(overlayScroll, FrameLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT, Gravity.TOP).apply {
            topMargin = 64
        })

        val buttons = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(button("MODE") { toggleRenderMode() })
            addView(button("DIAG") { toggleDiagnostics() })
            addView(button("SELF-TEST") { engine.runSelfTest() })
            addView(button("LOG") { engine.logReport() })
        }
        root.addView(buttons, FrameLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT, Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL).apply {
            bottomMargin = 48
        })
        setContentView(root)
    }

    private fun button(label: String, onClick: () -> Unit) = Button(this).apply {
        text = label
        setOnClickListener { onClick() }
    }

    private fun toggleRenderMode() {
        renderMode = if (renderMode == NativeEngine.RenderMode.ON_DEMAND) NativeEngine.RenderMode.CONTINUOUS
        else NativeEngine.RenderMode.ON_DEMAND
        engine?.setRenderMode(renderMode)
    }

    /** Diagnostics on/off; the render mode is left untouched. */
    private fun toggleDiagnostics() {
        diagnosticsOn = !diagnosticsOn
        engine?.setDiagnosticsEnabled(diagnosticsOn)
        overlayScroll.visibility = if (diagnosticsOn) android.view.View.VISIBLE else android.view.View.GONE
    }

    override fun onResume() {
        super.onResume()
        engine?.resume()
        if (engine != null) handler.post(overlayUpdater)
    }

    override fun onPause() {
        handler.removeCallbacks(overlayUpdater)
        engine?.pause()
        super.onPause()
    }

    override fun onDestroy() {
        handler.removeCallbacks(overlayUpdater)
        // Normally the SurfaceView surface is destroyed before onDestroy. If
        // not, close() still detaches the swapchain/EGLSurface before the
        // window reference is released (ordered native teardown), and a later
        // surfaceDestroyed callback is a no-op.
        engine?.close()
        engine = null
        super.onDestroy()
    }

    private companion object {
        const val TAG = "PE.MainActivity"
        const val OVERLAY_PERIOD_MS = 500L
    }
}
