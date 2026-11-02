package com.paintengine.app

import android.app.Activity
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.content.res.Configuration
import android.graphics.Color
import android.graphics.Typeface
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.view.WindowInsets
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import com.paintengine.BuildConfig
import com.paintengine.engine.EngineSurfaceView
import com.paintengine.engine.NativeEngine
import org.json.JSONObject
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Engine diagnostics (Block 1 validation tools): a full-screen engine surface,
 * a diagnostics overlay and the on-device validation run. Opened from PxPaint
 * Settings -> Engine diagnostics.
 *
 * Runs in its own process (":diagnostics", see AndroidManifest.xml): the
 * engine's Vulkan entry points are process-wide, so this activity's engine
 * must never share a process with PxPaint's own engine.
 *
 * Buttons
 *   MODE        continuous <-> on-demand rendering
 *   DIAG        diagnostics overlay/logging on/off (render mode untouched)
 *   SELF-TEST   pixel + geometry check of the next frame
 *   BACKEND     auto -> vulkan -> gles (full engine restart on the same surface)
 *   DEVICE TEST automated lifecycle/stability validation (~1-2 minutes)
 *   REPORT      shareable text report (status, timeline, native log, GPU caps)
 *
 * Optional launch extras (adb): --es backend gles|vulkan|auto, --ez fallback,
 * --ez validation, --es render continuous|ondemand, --ez diagnostics.
 */
class DiagnosticsActivity : Activity(), DeviceValidation.Host {

    // Host interface for DeviceValidation (written only on the main thread).
    @Volatile override var engine: NativeEngine? = null
    override val surfaceView: EngineSurfaceView by lazy { EngineSurfaceView(this) }
    override var backendPreference: String = "auto"
    @Volatile override var isInForeground: Boolean = false

    private var allowFallback = true
    private var validation = BuildConfig.DEBUG
    private var diagnosticsOn = true
    private var renderMode = NativeEngine.RenderMode.ON_DEMAND

    // Not named "overlay": inside View.apply {} that would resolve to View.getOverlay().
    private lateinit var overlayText: TextView
    private lateinit var overlayScroll: ScrollView
    private lateinit var bannerText: TextView
    private lateinit var bottomPanel: LinearLayout
    private lateinit var backendButton: Button
    private lateinit var reportPanel: LinearLayout
    private lateinit var reportText: TextView
    private val actionButtons = mutableListOf<Button>()
    private var testRunning = false
    private var lastValidation: String? = null
    private var lastSeenSelfTestRuns = -1
    private var lastSeenState = ""

    private val handler = Handler(Looper.getMainLooper())
    private val overlayUpdater = object : Runnable {
        override fun run() {
            engine?.let { e ->
                overlayText.text = e.statusText()
                trackEngineEvents(e)
            }
            handler.postDelayed(this, OVERLAY_PERIOD_MS)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        EventLog.add("Activity onCreate (${Build.MANUFACTURER} ${Build.MODEL}, Android ${Build.VERSION.RELEASE})")
        val root = FrameLayout(this)

        NativeEngine.loadError?.let { error ->
            Log.e(TAG, "Native library failed to load", error)
            root.addView(TextView(this).apply {
                text = "libpaintengine.so failed to load:\n$error"
                setTextColor(Color.RED)
                setPadding(32, 160, 32, 32)
            })
            setContentView(root)
            return
        }

        backendPreference = intent.getStringExtra("backend") ?: "auto"
        allowFallback = intent.getBooleanExtra("fallback", true)
        validation = intent.getBooleanExtra("validation", BuildConfig.DEBUG)
        diagnosticsOn = intent.getBooleanExtra("diagnostics", true)
        renderMode = if (intent.getStringExtra("render") == "continuous") NativeEngine.RenderMode.CONTINUOUS
        else NativeEngine.RenderMode.ON_DEMAND

        surfaceView.eventListener = EngineSurfaceView.EventListener { EventLog.add(it) }
        root.addView(surfaceView, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))

        overlayText = TextView(this).apply {
            typeface = Typeface.MONOSPACE
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 9.5f)
            setTextColor(Color.WHITE)
            setBackgroundColor(0xAA000000.toInt())
            setPadding(16, 12, 16, 12)
            text = "starting..."
        }
        overlayScroll = ScrollView(this).apply {
            addView(overlayText)
            visibility = if (diagnosticsOn) View.VISIBLE else View.GONE
        }
        root.addView(overlayScroll, FrameLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT, Gravity.TOP))

        bannerText = TextView(this).apply {
            setTextColor(Color.YELLOW)
            setBackgroundColor(0xCC000000.toInt())
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
            setPadding(16, 8, 16, 8)
            visibility = View.GONE
        }
        backendButton = actionButton(backendLabel()) { cycleBackend() }
        bottomPanel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(bannerText)
            addView(buttonRow(
                actionButton("MODE") { toggleRenderMode() },
                actionButton("DIAG") { toggleDiagnostics() },
                actionButton("SELF-TEST") { engine?.runSelfTest() },
            ))
            addView(buttonRow(
                backendButton,
                actionButton("DEVICE TEST") { startDeviceTest() },
                actionButton("REPORT") { showReport(buildReport(lastValidation)) },
            ))
        }
        root.addView(bottomPanel, FrameLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT, Gravity.BOTTOM))

        reportPanel = buildReportPanel()
        root.addView(reportPanel, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))

        applySystemBarInsets(root)
        setContentView(root)

        engine = createEngine()
        surfaceView.engine = engine
        if (engine == null) showFatal(root, "Native engine creation failed (see REPORT)")
    }

    // ---------------------------------------------------------------- engine
    private fun createEngine(): NativeEngine? {
        val config = NativeEngine.Config(
            backend = backendPreference,
            allowFallback = allowFallback,
            validation = validation,
            renderMode = renderMode,
            diagnostics = diagnosticsOn,
        )
        EventLog.add("Engine create: $config")
        val e = NativeEngine(config)
        if (!e.isAlive) {
            EventLog.add("Engine creation FAILED")
            return null
        }
        lastSeenSelfTestRuns = -1
        lastSeenState = ""
        return e
    }

    /** Full native teardown, then a new engine on the same Android Surface. */
    override fun switchBackend(preference: String) {
        val hadSurface = surfaceView.hasSurface
        EventLog.add("Backend switch: '$backendPreference' -> '$preference' (surface present: $hadSurface)")
        engine?.let { old ->
            if (hadSurface) old.surfaceDestroyed()  // blocking: swapchain/EGLSurface gone, window released
            surfaceView.engine = null
            engine = null
            old.close()                             // blocking: device + backend destroyed
        }
        EventLog.add("Previous device shutdown: " + JSONObject(NativeEngine.lastShutdownReport()).optString("text"))
        backendPreference = preference
        val e = createEngine()
        engine = e
        surfaceView.engine = e
        if (e != null) {
            if (!isInForeground) e.pause()
            if (hadSurface && surfaceView.holder.surface.isValid) {
                e.surfaceCreated(surfaceView.holder.surface)
                e.surfaceChanged(surfaceView.surfaceWidth, surfaceView.surfaceHeight)
            }
        }
        backendButton.text = backendLabel()
    }

    private fun cycleBackend() {
        if (testRunning) return
        val next = when (backendPreference) {
            "auto" -> "vulkan"
            "vulkan" -> "gles"
            else -> "auto"
        }
        switchBackend(next)
    }

    private fun backendLabel() = "BACKEND: " + backendPreference.uppercase(Locale.US)

    override fun setRenderMode(mode: NativeEngine.RenderMode) {
        renderMode = mode
        engine?.setRenderMode(mode)
        EventLog.add("Render mode: $mode")
    }

    override fun setDiagnostics(enabled: Boolean) {
        diagnosticsOn = enabled
        engine?.setDiagnosticsEnabled(enabled)
        overlayScroll.visibility = if (enabled) View.VISIBLE else View.GONE
        EventLog.add("Diagnostics: $enabled")
    }

    private fun toggleRenderMode() = setRenderMode(
        if (renderMode == NativeEngine.RenderMode.ON_DEMAND) NativeEngine.RenderMode.CONTINUOUS
        else NativeEngine.RenderMode.ON_DEMAND,
    )

    private fun toggleDiagnostics() = setDiagnostics(!diagnosticsOn)

    /** Logs self-test results and state changes into the timeline. */
    private fun trackEngineEvents(e: NativeEngine) {
        val s = try { JSONObject(e.statusJson()) } catch (ex: Exception) { return }
        val state = s.optString("state")
        if (state != lastSeenState) {
            EventLog.add("Engine state: $state (backend ${s.optString("backend")})")
            lastSeenState = state
        }
        val runs = s.optInt("selfTestRuns")
        if (runs != lastSeenSelfTestRuns && s.optString("selfTest") != "pending") {
            if (runs > 0) {
                EventLog.add("Self-test #$runs ${s.optString("selfTest")} [${s.optString("backend")}, " +
                    "${s.optInt("surfaceWidth")}x${s.optInt("surfaceHeight")}, display ${s.optString("currentTransform")}]: " +
                    s.optString("selfTestDetail"))
            }
            lastSeenSelfTestRuns = runs
        }
    }

    // ------------------------------------------------------------ validation
    private fun startDeviceTest() {
        if (testRunning || engine == null) return
        testRunning = true
        reportPanel.visibility = View.GONE
        actionButtons.forEach { it.isEnabled = false }
        onProgress("Device test starting - keep the app open (about 1-2 minutes)")
        EventLog.add("Device validation started")
        Thread({
            val result = DeviceValidation(this).run()
            runOnUiThread {
                testRunning = false
                lastValidation = result
                actionButtons.forEach { it.isEnabled = true }
                bannerText.visibility = View.GONE
                backendButton.text = backendLabel()
                showReport(buildReport(result))
            }
        }, "PE.DeviceValidation").start()
    }

    override fun runOnMain(action: Runnable) = runOnUiThread(action)

    override fun onProgress(message: String) {
        runOnUiThread {
            bannerText.text = message
            bannerText.visibility = View.VISIBLE
        }
    }

    // ---------------------------------------------------------------- report
    private fun buildReport(validationResult: String?): String {
        val e = engine
        val sb = StringBuilder()
        sb.append("=== PAINT ENGINE DEVICE REPORT ===\n")
        sb.append("time: ").append(SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(Date())).append('\n')
        sb.append("app: ").append(BuildConfig.VERSION_NAME).append(" (").append(BuildConfig.BUILD_TYPE).append(")\n")
        sb.append("device: ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL)
            .append(" (").append(Build.DEVICE).append("), Android ").append(Build.VERSION.RELEASE)
            .append(" / API ").append(Build.VERSION.SDK_INT)
        if (Build.VERSION.SDK_INT >= 31) sb.append(", SoC ").append(Build.SOC_MANUFACTURER).append(' ').append(Build.SOC_MODEL)
        sb.append('\n')
        sb.append("backend preference: ").append(backendPreference).append(", fallback: ").append(allowFallback)
            .append(", validation requested: ").append(validation).append('\n')
        sb.append("surface callbacks: created ").append(surfaceView.createdCount).append(", destroyed ")
            .append(surfaceView.destroyedCount).append(", max surfaceDestroyed ").append(surfaceView.maxDestroyMs).append(" ms\n")
        if (validationResult != null) sb.append('\n').append(validationResult)
        sb.append("\n--- ENGINE STATUS ---\n").append(e?.statusText() ?: "no engine").append('\n')
        sb.append("--- LAST DEVICE SHUTDOWN ---\n")
            .append(JSONObject(NativeEngine.lastShutdownReport()).optString("text")).append('\n')
        sb.append("\n--- APP TIMELINE ---\n").append(EventLog.dump()).append('\n')
        sb.append("\n--- NATIVE LOG (last 250 lines) ---\n").append(NativeEngine.recentLog(250))
        sb.append("\n--- GPU CAPABILITIES ---\n").append(e?.capabilityReport() ?: "").append('\n')
        return if (sb.length > MAX_REPORT_CHARS) sb.substring(0, MAX_REPORT_CHARS) + "\n[truncated]" else sb.toString()
    }

    private fun buildReportPanel(): LinearLayout {
        reportText = TextView(this).apply {
            typeface = Typeface.MONOSPACE
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 9f)
            setTextColor(Color.WHITE)
            setPadding(16, 16, 16, 16)
            setTextIsSelectable(true)
        }
        val scroll = ScrollView(this).apply { addView(reportText) }
        return LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(0xF0101014.toInt())
            visibility = View.GONE
            addView(scroll, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))
            addView(buttonRow(
                plainButton("SHARE") { shareReport(reportText.text.toString()) },
                plainButton("COPY") { copyReport(reportText.text.toString()) },
                plainButton("CLOSE") { visibility = View.GONE },
            ))
        }
    }

    private fun showReport(text: String) {
        reportText.text = text
        reportPanel.visibility = View.VISIBLE
        EventLog.add("Report shown (${text.length} chars)")
    }

    private fun shareReport(text: String) {
        val send = Intent(Intent.ACTION_SEND).apply {
            type = "text/plain"
            putExtra(Intent.EXTRA_SUBJECT, "Paint Engine device report")
            putExtra(Intent.EXTRA_TEXT, text)
        }
        startActivity(Intent.createChooser(send, "Share report"))
    }

    private fun copyReport(text: String) {
        val clipboard = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        clipboard.setPrimaryClip(ClipData.newPlainText("Paint Engine report", text))
        Toast.makeText(this, "Report copied (${text.length} chars)", Toast.LENGTH_SHORT).show()
    }

    // -------------------------------------------------------------- UI utils
    private fun actionButton(label: String, onClick: () -> Unit): Button =
        plainButton(label, onClick).also { actionButtons.add(it) }

    private fun plainButton(label: String, onClick: () -> Unit) = Button(this).apply {
        text = label
        setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f)
        setAllCaps(false)
        setOnClickListener { onClick() }
    }

    private fun buttonRow(vararg buttons: Button) = LinearLayout(this).apply {
        orientation = LinearLayout.HORIZONTAL
        for (b in buttons) addView(b, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
    }

    /** Keeps overlay and buttons out from under the status / navigation bars (edge-to-edge on Android 15+). */
    private fun applySystemBarInsets(root: FrameLayout) {
        root.setOnApplyWindowInsetsListener { _, insets ->
            val top: Int
            val bottom: Int
            if (Build.VERSION.SDK_INT >= 30) {
                val bars = insets.getInsets(WindowInsets.Type.systemBars() or WindowInsets.Type.displayCutout())
                top = bars.top
                bottom = bars.bottom
            } else {
                @Suppress("DEPRECATION")
                top = insets.systemWindowInsetTop
                @Suppress("DEPRECATION")
                bottom = insets.systemWindowInsetBottom
            }
            overlayScroll.setPadding(0, top, 0, 0)
            bottomPanel.setPadding(0, 0, 0, bottom)
            reportPanel.setPadding(0, top, 0, bottom)
            insets
        }
    }

    private fun showFatal(root: FrameLayout, message: String) {
        root.addView(TextView(this).apply {
            text = message
            setTextColor(Color.RED)
            setPadding(32, 200, 32, 32)
        })
    }

    // ------------------------------------------------------------- lifecycle
    override fun onStart() {
        super.onStart()
        EventLog.add("Activity onStart")
    }

    override fun onResume() {
        super.onResume()
        isInForeground = true
        EventLog.add("Activity onResume")
        engine?.resume()
        handler.removeCallbacks(overlayUpdater)
        handler.post(overlayUpdater)
    }

    override fun onPause() {
        isInForeground = false
        EventLog.add("Activity onPause")
        handler.removeCallbacks(overlayUpdater)
        engine?.pause()
        super.onPause()
    }

    override fun onStop() {
        EventLog.add("Activity onStop")
        super.onStop()
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        val orientation = when (newConfig.orientation) {
            Configuration.ORIENTATION_LANDSCAPE -> "landscape"
            Configuration.ORIENTATION_PORTRAIT -> "portrait"
            else -> "undefined"
        }
        val rotation = if (Build.VERSION.SDK_INT >= 30) {
            display?.rotation ?: -1
        } else {
            @Suppress("DEPRECATION")
            windowManager.defaultDisplay.rotation
        }
        EventLog.add("Configuration changed: $orientation, ${newConfig.screenWidthDp}x${newConfig.screenHeightDp} dp " +
            "(display rotation ${rotation * 90} deg)")
    }

    override fun onDestroy() {
        EventLog.add("Activity onDestroy (finishing: $isFinishing)")
        handler.removeCallbacks(overlayUpdater)
        // Normally the SurfaceView surface is destroyed before onDestroy. If
        // not, close() still detaches the swapchain/EGLSurface before the
        // window reference is released (ordered native teardown), and a later
        // surfaceDestroyed callback is a no-op.
        surfaceView.engine = null
        engine?.close()
        engine = null
        super.onDestroy()
    }

    private companion object {
        const val TAG = "PE.Diagnostics"
        const val OVERLAY_PERIOD_MS = 500L
        const val MAX_REPORT_CHARS = 150_000
    }
}
