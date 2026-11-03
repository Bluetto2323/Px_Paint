package com.paintengine.pxpaint

import android.app.Activity
import android.app.ActivityManager
import android.content.Context
import android.content.Intent
import android.content.res.Configuration
import android.graphics.Color
import android.graphics.Rect
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.widget.FrameLayout
import com.paintengine.app.DiagnosticsActivity
import com.paintengine.engine.NativeEngine
import com.paintengine.pxpaint.core.AppSettings
import com.paintengine.pxpaint.core.BrushState
import com.paintengine.pxpaint.core.CanvasSizeRules
import com.paintengine.pxpaint.core.LanguagePref
import com.paintengine.pxpaint.core.ProjectStore
import com.paintengine.pxpaint.core.RenderPreference
import com.paintengine.pxpaint.core.S
import com.paintengine.pxpaint.core.ThemeMode
import com.paintengine.pxpaint.screens.GalleryScreen
import com.paintengine.pxpaint.screens.PxHost
import com.paintengine.pxpaint.screens.Screen
import com.paintengine.pxpaint.ui.OverlayHost
import com.paintengine.pxpaint.ui.PxTheme
import java.io.File
import java.util.Locale
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

/**
 * PxPaint's single activity: a small screen stack (gallery → workspace →
 * settings) over one [OverlayHost], and the process' only [NativeEngine].
 *
 * The engine is created on the first canvas open with the user's backend
 * and render-mode choice and a memory budget derived from device RAM, then
 * kept for the life of the activity (opening another canvas reuses the GPU
 * device). Rotation / resize do not recreate the activity (configChanges):
 * only the chrome is rebuilt and the SurfaceView gets surfaceChanged.
 *
 * The Block 1 diagnostics live in DiagnosticsActivity, in the ":diagnostics"
 * process, because the native Vulkan loader state is process-global.
 */
class PxPaintActivity : Activity(), PxHost {
    override val context: Context get() = this
    override lateinit var theme: PxTheme
        private set
    override var settings: AppSettings = AppSettings()
        private set
    override var brush: BrushState = BrushState()
        private set
    override lateinit var overlays: OverlayHost
        private set
    override val insets = Rect()
    override var windowWidthDp = 0
        private set
    override var windowHeightDp = 0
        private set
    override var smallestWidthDp = 0
        private set
    override lateinit var store: ProjectStore
        private set
    override val io: ExecutorService = Executors.newSingleThreadExecutor { r -> Thread(r, "PxPaint-io") }

    private val handler = Handler(Looper.getMainLooper())
    private lateinit var root: FrameLayout
    private lateinit var content: FrameLayout
    private val stack = ArrayList<Screen>()
    private val views = HashMap<Screen, View>()
    private var engine: NativeEngine? = null
    private var engineFailed = false
    private var resumed = false
    private var uiNightMode = 0
    private var locale = ""
    private var fontScale = 1f
    private var densityDpi = 0

    override val engineIfStarted: NativeEngine? get() = engine

    // ------------------------------------------------------------ lifecycle
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        settings = SettingsStore.load(this)
        brush = SettingsStore.loadBrush(this)
        store = ProjectStore(File(filesDir, "projects"))
        readConfiguration(resources.configuration)
        theme = makeTheme()

        root = FrameLayout(this)
        content = FrameLayout(this)
        overlays = OverlayHost(this, theme)
        root.addView(content, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
        root.addView(overlays, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
        applyWindowStyle()
        root.setOnApplyWindowInsetsListener { _, wi ->
            val r = systemInsets(wi)
            if (r != insets) {
                insets.set(r)
                overlays.insets.set(r)
                layoutChanged()
            }
            wi
        }
        setContentView(root)
        push(GalleryScreen(this))
    }

    override fun onResume() {
        super.onResume()
        resumed = true
        engine?.resume()
        top()?.onResume()
    }

    override fun onPause() {
        resumed = false
        top()?.onPause()
        engine?.pause()
        super.onPause()
    }

    override fun onDestroy() {
        // Screens close their canvas (queueing the last save) first; the disk
        // work finishes (it reads the engine) before the engine goes.
        for (s in stack.asReversed()) runCatching { s.onRemoved() }
        stack.clear()
        views.clear()
        io.shutdown()
        runCatching { io.awaitTermination(10, TimeUnit.SECONDS) }
        engine?.close()
        engine = null
        super.onDestroy()
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        val oldNight = uiNightMode
        val oldLocale = locale
        val oldFont = fontScale
        val oldDensity = densityDpi
        readConfiguration(newConfig)
        val themeFollowsSystem = settings.theme == ThemeMode.SYSTEM && oldNight != uiNightMode
        val languageFollowsSystem = settings.language == LanguagePref.SYSTEM && oldLocale != locale
        if (themeFollowsSystem || languageFollowsSystem || oldFont != fontScale || oldDensity != densityDpi) {
            retheme()
        } else {
            layoutChanged()
        }
    }

    @Deprecated("Deprecated in Java")
    override fun onBackPressed() {
        if (overlays.back()) return
        val s = top()
        if (s != null && s.onBack()) return
        if (stack.size > 1) pop() else @Suppress("DEPRECATION") super.onBackPressed()
    }

    // ----------------------------------------------------------- navigation
    private fun top(): Screen? = stack.lastOrNull()

    override fun push(screen: Screen) {
        overlays.dismissAll()
        top()?.let { detach(it) }
        stack += screen
        show(screen)
    }

    override fun pop() {
        if (stack.size <= 1) {
            finish()
            return
        }
        overlays.dismissAll()
        val s = stack.removeAt(stack.lastIndex)
        detach(s)
        views.remove(s)
        s.onRemoved()
        show(stack.last())
    }

    override fun replaceTop(screen: Screen) {
        overlays.dismissAll()
        stack.removeLastOrNull()?.let {
            detach(it)
            views.remove(it)
            it.onRemoved()
        }
        stack += screen
        show(screen)
    }

    private fun show(screen: Screen) {
        val v = views.getOrPut(screen) { screen.createView() }
        (v.parent as? FrameLayout)?.removeView(v)
        content.addView(v, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
        screen.onShown()
        if (resumed) screen.onResume()
    }

    private fun detach(screen: Screen) {
        views[screen]?.let { content.removeView(it) }
    }

    /** Window size / insets changed: the top screen adapts (or is rebuilt). */
    private fun layoutChanged() {
        val s = top() ?: return
        if (s.onLayoutChanged()) rebuild(s)
        // Screens below are rebuilt lazily when they come back.
        for (other in stack) if (other !== s) views.remove(other)
    }

    private fun rebuild(s: Screen) {
        detach(s)
        views.remove(s)
        val v = s.createView()
        views[s] = v
        content.addView(v, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
        s.onShown()
    }

    // ------------------------------------------------------------- settings
    override fun updateSettings(newSettings: AppSettings) {
        val old = settings
        if (newSettings == old) return
        settings = newSettings
        SettingsStore.save(this, newSettings)
        if (old.render != newSettings.render) engine?.setRenderMode(renderMode(newSettings))
        val visual = old.theme != newSettings.theme || old.uiSize != newSettings.uiSize ||
            old.language != newSettings.language || old.highContrast != newSettings.highContrast ||
            old.largeTouchTargets != newSettings.largeTouchTargets || old.reduceMotion != newSettings.reduceMotion
        if (visual) retheme() else {
            // Toolbar / gesture / overlay changes: rebuild the visible screen.
            top()?.let { rebuild(it) }
            for (other in stack) if (other !== top()) views.remove(other)
        }
    }

    private fun retheme() {
        theme = makeTheme()
        overlays.setTheme(theme)
        applyWindowStyle()
        val s = top() ?: return
        // Every screen is rebuilt with the new theme (lazily for hidden ones).
        for (other in stack) if (other !== s) views.remove(other)
        rebuild(s)
    }

    private fun makeTheme(): PxTheme {
        S.applyLanguage(settings.language, resources.configuration.locales[0] ?: Locale.getDefault())
        val effective = if (settings.theme == ThemeMode.SYSTEM) {
            settings.copy(theme = if (uiNightMode == Configuration.UI_MODE_NIGHT_NO) ThemeMode.LIGHT else ThemeMode.DARK)
        } else settings
        return PxTheme(this, effective)
    }

    override fun updateBrush(state: BrushState) {
        if (state == brush) return
        brush = state
        SettingsStore.saveBrush(this, state)
    }

    override fun tipsShown(id: String): Boolean = tipPrefs().getBoolean(id, false)
    override fun markTipShown(id: String) { tipPrefs().edit().putBoolean(id, true).apply() }
    override fun resetTips() { tipPrefs().edit().clear().apply() }
    private fun tipPrefs() = getSharedPreferences("pxpaint.tips", MODE_PRIVATE)

    override fun openDiagnostics() {
        startActivity(Intent(this, DiagnosticsActivity::class.java))
    }

    override fun runOnMain(action: () -> Unit) {
        if (isDestroyed) return
        handler.post { if (!isDestroyed) action() }
    }

    // --------------------------------------------------------------- engine
    override fun engine(): NativeEngine? {
        engine?.let { return it }
        if (engineFailed) return null
        NativeEngine.loadError?.let {
            Log.e(TAG, "Native library failed to load", it)
            engineFailed = true
            return null
        }
        val config = NativeEngine.Config(
            backend = settings.backend.engineName,
            allowFallback = true,
            validation = false,
            renderMode = renderMode(settings),
            diagnostics = true,
            scene = NativeEngine.Scene.CANVAS,
            canvasMemoryBudgetBytes = canvasBudget(),
        )
        Log.i(TAG, "Creating engine: $config")
        val e = NativeEngine(config)
        if (!e.isAlive) {
            Log.e(TAG, "Engine creation failed")
            engineFailed = true
            return null
        }
        if (!resumed) e.pause()
        engine = e
        return e
    }

    /** ¼ of device RAM, clamped to 256 MiB … 1.5 GiB (see docs/ENGINE_API.md). */
    fun canvasBudget(): Long {
        val am = getSystemService(ACTIVITY_SERVICE) as ActivityManager
        val mi = ActivityManager.MemoryInfo().also { am.getMemoryInfo(it) }
        return CanvasSizeRules.budgetForRam(mi.totalMem)
    }

    private fun renderMode(s: AppSettings) =
        if (s.render == RenderPreference.SMOOTH) NativeEngine.RenderMode.CONTINUOUS else NativeEngine.RenderMode.ON_DEMAND

    // --------------------------------------------------------------- window
    private fun readConfiguration(c: Configuration) {
        windowWidthDp = c.screenWidthDp
        windowHeightDp = c.screenHeightDp
        smallestWidthDp = c.smallestScreenWidthDp
        uiNightMode = c.uiMode and Configuration.UI_MODE_NIGHT_MASK
        locale = c.locales[0]?.toLanguageTag() ?: ""
        fontScale = c.fontScale
        densityDpi = c.densityDpi
    }

    private fun applyWindowStyle() {
        val w = window
        w.decorView.setBackgroundColor(theme.bg)
        @Suppress("DEPRECATION")
        run {
            w.statusBarColor = Color.TRANSPARENT
            w.navigationBarColor = Color.TRANSPARENT
        }
        if (Build.VERSION.SDK_INT >= 29) {
            w.isNavigationBarContrastEnforced = false
            w.isStatusBarContrastEnforced = false
        }
        if (Build.VERSION.SDK_INT >= 30) {
            w.setDecorFitsSystemWindows(false)
            val lightBars = !theme.dark
            val mask = WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS or WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS
            w.insetsController?.setSystemBarsAppearance(if (lightBars) mask else 0, mask)
        } else {
            @Suppress("DEPRECATION")
            var flags = View.SYSTEM_UI_FLAG_LAYOUT_STABLE or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
            @Suppress("DEPRECATION")
            if (!theme.dark) flags = flags or View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR or View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR
            @Suppress("DEPRECATION")
            w.decorView.systemUiVisibility = flags
        }
        if (Build.VERSION.SDK_INT >= 28) {
            w.attributes = w.attributes.apply {
                layoutInDisplayCutoutMode = android.view.WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
            }
        }
    }

    private fun systemInsets(wi: WindowInsets): Rect = if (Build.VERSION.SDK_INT >= 30) {
        val i = wi.getInsets(WindowInsets.Type.systemBars() or WindowInsets.Type.displayCutout())
        Rect(i.left, i.top, i.right, i.bottom)
    } else {
        @Suppress("DEPRECATION")
        Rect(wi.systemWindowInsetLeft, wi.systemWindowInsetTop, wi.systemWindowInsetRight, wi.systemWindowInsetBottom)
    }

    private companion object {
        const val TAG = "PxPaint"
    }
}

/** AppSettings ⇄ SharedPreferences (string map, see AppSettings.encode). */
object SettingsStore {
    private const val PREFS = "pxpaint.settings"

    fun load(context: Context): AppSettings {
        val all = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).all
        return AppSettings.decode(all.mapNotNull { (k, v) -> (v as? String)?.let { k to it } }.toMap())
    }

    private const val BRUSH_PREFS = "pxpaint.brush"

    fun loadBrush(context: Context): BrushState {
        val all = context.getSharedPreferences(BRUSH_PREFS, Context.MODE_PRIVATE).all
        return BrushState.decode(all.mapNotNull { (k, v) -> (v as? String)?.let { k to it } }.toMap())
    }

    fun saveBrush(context: Context, state: BrushState) {
        val e = context.getSharedPreferences(BRUSH_PREFS, Context.MODE_PRIVATE).edit()
        for ((k, v) in state.encode()) e.putString(k, v)
        e.apply()
    }

    fun save(context: Context, settings: AppSettings) {
        val e = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
        for ((k, v) in settings.encode()) e.putString(k, v)
        e.putString("version", AppSettings.VERSION.toString())
        e.apply()
    }
}
