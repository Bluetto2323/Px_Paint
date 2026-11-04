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
import com.paintengine.BuildConfig
import com.paintengine.R
import com.paintengine.app.DiagnosticsActivity
import com.paintengine.engine.NativeEngine
import com.paintengine.pxpaint.core.MotionTokens
import com.paintengine.pxpaint.core.AppSettings
import com.paintengine.pxpaint.core.BrushLibrary
import com.paintengine.pxpaint.core.BrushPresetFormat
import com.paintengine.pxpaint.core.BrushState
import com.paintengine.pxpaint.core.CanvasSizeRules
import com.paintengine.pxpaint.core.PressureCurve
import com.paintengine.pxpaint.input.StylusMonitor
import com.paintengine.pxpaint.core.LanguagePref
import com.paintengine.pxpaint.core.ProjectStore
import com.paintengine.pxpaint.core.RenderPreference
import com.paintengine.pxpaint.core.S
import com.paintengine.pxpaint.core.ThemeMode
import com.paintengine.pxpaint.screens.DebugDemo
import com.paintengine.pxpaint.screens.GalleryScreen
import com.paintengine.pxpaint.screens.PxHost
import com.paintengine.pxpaint.screens.Screen
import com.paintengine.pxpaint.ui.Motion
import com.paintengine.pxpaint.ui.BrushPreviews
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
    override lateinit var stylus: StylusMonitor
        private set
    override lateinit var brushes: BrushLibrary
        private set
    override val previews = BrushPreviews()
    private var brushesDirty = false
    private val saveBrushes = Runnable {
        brushesDirty = false
        BrushLibraryStore.saveAsync(this, brushes, io)
    }
    private var tableKey: Pair<PressureCurve, Float>? = null
    private var table = FloatArray(0)
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
    /** Between the screens and the overlays: the project-opening container transform. */
    private lateinit var transitionLayer: FrameLayout
    private var transitionImage: View? = null
    private var transitionFor: Screen? = null
    private var transitionExpanded = false
    private var transitionScreenReady = false
    private var intro: com.paintengine.pxpaint.ui.BrandIntro? = null
    override var debugInitDelayMs = 0L
        private set
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
        brushes = BrushLibraryStore.load(this)
        stylus = StylusMonitor(this)
        store = ProjectStore(File(filesDir, "projects"))
        readConfiguration(resources.configuration)
        theme = makeTheme()

        root = FrameLayout(this)
        content = FrameLayout(this)
        overlays = OverlayHost(this, theme)
        transitionLayer = FrameLayout(this).apply { importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO_HIDE_DESCENDANTS }
        root.addView(content, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
        root.addView(transitionLayer, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
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
        val demo = DebugDemo.from(intent)
        if (BuildConfig.DEBUG) debugInitDelayMs = intent?.getLongExtra("pxpaint.slowinit", 0L) ?: 0L
        // Startup animation over the gallery (cold start only; scripted debug states skip it).
        if (savedInstanceState == null && (demo == null || intent?.hasExtra("pxpaint.intro") == true)) {
            intro = com.paintengine.pxpaint.ui.BrandIntro(this, theme) { intro = null }.also {
                root.addView(it, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
            }
        }
        rememberSplashTheme()
        pushWith(GalleryScreen(this), Enter.NONE)
        demo?.let { DebugDemo.start(this, it) }
    }

    override fun onResume() {
        super.onResume()
        resumed = true
        engine?.resume()
        top()?.onResume()
    }

    override fun onPause() {
        if (brushesDirty) {
            handler.removeCallbacks(saveBrushes)
            saveBrushes.run()
        }
        resumed = false
        top()?.onPause()
        engine?.pause()
        super.onPause()
    }

    override fun onDestroy() {
        // Screens close their canvas (queueing the last save) first; the disk
        // work finishes (it reads the engine) before the engine goes.
        for (s in stack.asReversed()) runCatching { s.onRemoved() }
        if (brushesDirty) {
            handler.removeCallbacks(saveBrushes)
            saveBrushes.run()
        }
        stack.clear()
        views.clear()
        io.shutdown()
        runCatching { io.awaitTermination(10, TimeUnit.SECONDS) }
        engine?.close()
        engine = null
        stylus.close()
        previews.close()
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

    // ------------------------------------------------------------- navigation
    // Shared-axis transitions (Block 9): the incoming screen fades in moving a
    // little along x (forward from the right, back from the left) over the
    // outgoing one, which is removed when the incoming has entered. The
    // outgoing screen is never animated itself (a workspace holds a SurfaceView).
    private enum class Enter { NONE, FORWARD, BACK }

    override fun push(screen: Screen) = pushWith(screen, Enter.FORWARD)

    private fun pushWith(screen: Screen, enter: Enter) {
        overlays.dismissAll()
        val outgoing = top()?.let { views[it] }
        stack += screen
        show(screen, enter, outgoing)
    }

    override fun pop() {
        if (stack.size <= 1) {
            finish()
            return
        }
        overlays.dismissAll()
        clearTransition()
        val s = stack.removeAt(stack.lastIndex)
        val outgoing = views.remove(s)
        s.onRemoved()
        show(stack.last(), Enter.BACK, outgoing)
    }

    override fun replaceTop(screen: Screen) {
        overlays.dismissAll()
        var outgoing: View? = null
        stack.removeLastOrNull()?.let {
            outgoing = views.remove(it)
            it.onRemoved()
        }
        stack += screen
        show(screen, Enter.FORWARD, outgoing)
    }

    private fun show(screen: Screen, enter: Enter = Enter.NONE, outgoing: View? = null) {
        val v = views.getOrPut(screen) { screen.createView() }
        (v.parent as? FrameLayout)?.removeView(v)
        // Leftovers of an interrupted transition (rapid back / forward): only the outgoing view may stay.
        for (i in content.childCount - 1 downTo 0) content.getChildAt(i).let { if (it !== outgoing) content.removeView(it) }
        v.animate().cancel()
        v.alpha = 1f; v.translationX = 0f; v.translationY = 0f; v.scaleX = 1f; v.scaleY = 1f
        content.addView(v, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
        screen.onShown()
        if (resumed) screen.onResume()
        val m = theme.motion
        val dropOutgoing = Runnable { if (outgoing != null && outgoing !== v) content.removeView(outgoing) }
        if (enter == Enter.NONE || screen.ownEntrance || !m.enabled) {
            dropOutgoing.run()
            return
        }
        m.enter(v, dyDp = 0f, dxDp = if (enter == Enter.FORWARD) MotionTokens.SLIDE else -MotionTokens.SLIDE) { dropOutgoing.run() }
    }

    override fun pushFrom(screen: Screen, origin: View, image: android.graphics.Bitmap?) {
        val m = theme.motion
        if (!m.movement || image == null || !origin.isAttachedToWindow || root.width == 0) {
            push(screen)
            return
        }
        clearTransition()
        val r = android.graphics.Rect()
        origin.getGlobalVisibleRect(r)
        val rootPos = IntArray(2).also { root.getLocationInWindow(it) }
        r.offset(-rootPos[0], -rootPos[1])
        val iv = android.widget.ImageView(this).apply {
            setImageBitmap(image)
            scaleType = android.widget.ImageView.ScaleType.FIT_CENTER
            pivotX = 0f
            pivotY = 0f
            x = r.left.toFloat()
            y = r.top.toFloat()
        }
        transitionLayer.addView(iv, FrameLayout.LayoutParams(r.width().coerceAtLeast(1), r.height().coerceAtLeast(1)))
        transitionImage = iv
        transitionFor = screen
        transitionExpanded = false
        transitionScreenReady = false
        pushWith(screen, Enter.FORWARD)
        // Grow to the canvas area (the workspace fits the canvas to the screen).
        val w = root.width.toFloat()
        val h = root.height.toFloat()
        val s = minOf(w * 0.94f / r.width(), h * 0.8f / r.height())
        iv.animate().x((w - r.width() * s) / 2f).y((h - r.height() * s) / 2f).scaleX(s).scaleY(s)
            .setDuration(m.d(MotionTokens.LONG)).setInterpolator(Motion.decelerate)
            .withEndAction {
                transitionExpanded = true
                if (transitionScreenReady) fadeTransition()
                // Still loading: reveal the screen's own loading indicator shortly after.
                else handler.postDelayed({ if (transitionImage === iv) fadeTransition() }, MotionTokens.LOADING_DELAY)
            }.start()
    }

    override fun screenReady(screen: Screen) {
        if (screen !== transitionFor) return
        transitionScreenReady = true
        if (transitionExpanded) fadeTransition()
    }

    private fun fadeTransition() {
        val iv = transitionImage ?: return
        transitionImage = null
        transitionFor = null
        theme.motion.exit(iv, dyDp = 0f, duration = MotionTokens.SHORT) { transitionLayer.removeView(iv) }
    }

    private fun clearTransition() {
        transitionImage?.let { it.animate().cancel(); transitionLayer.removeView(it) }
        transitionImage = null
        transitionFor = null
    }

    override fun appReady() {
        val i = intro ?: return
        // After the gallery's first frame with its data, never before.
        root.post { i.ready(); (top() as? GalleryScreen)?.animateIn() }
    }

    /** Android 13+: the system splash of the next start follows the app theme (dark / light). */
    private fun rememberSplashTheme() {
        if (Build.VERSION.SDK_INT >= 33) {
            runCatching { splashScreen.setSplashScreenTheme(if (theme.dark) R.style.Theme_PxPaint else R.style.Theme_PxPaint_Light) }
        }
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
    override fun updateSettings(newSettings: AppSettings, rebuild: Boolean) {
        val old = settings
        if (newSettings == old) return
        settings = newSettings
        SettingsStore.save(this, newSettings)
        if (old.render != newSettings.render) engine?.setRenderMode(renderMode(newSettings))
        val visual = old.theme != newSettings.theme || old.uiSize != newSettings.uiSize ||
            old.language != newSettings.language || old.highContrast != newSettings.highContrast ||
            old.largeTouchTargets != newSettings.largeTouchTargets || old.motionLevel != newSettings.motionLevel
        if (visual) retheme() else {
            // Toolbar / gesture / overlay changes: rebuild the visible screen.
            if (rebuild) top()?.let { rebuild(it) }
            for (other in stack) if (other !== top()) views.remove(other)
        }
    }

    private fun retheme() {
        theme = makeTheme()
        rememberSplashTheme()
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

    override fun pressureTable(): FloatArray {
        val key = settings.pressureCurve to settings.pressureSensitivity
        if (key != tableKey) {
            table = settings.pressureCurve.table(settings.pressureSensitivity, NativeEngine.PRESSURE_CURVE_SIZE)
            tableKey = key
        }
        return table
    }

    override fun updateBrush(state: BrushState) {
        if (state == brush) return
        brush = state
        SettingsStore.saveBrush(this, state)
    }

    override fun updateBrushes(library: BrushLibrary) {
        if (library == brushes) return
        brushes = library
        brushesDirty = true
        // Debounced: sliders send many small changes.
        handler.removeCallbacks(saveBrushes)
        handler.postDelayed(saveBrushes, 400)
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
        // Undo history: memory budget from the device RAM (on top of the canvas budget).
        e.setHistoryBudget(historyBudget(), com.paintengine.pxpaint.core.HistoryState.MAX_STEPS)
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

    /** 1/32 of device RAM, clamped to 32 … 192 MiB (HistoryState.budgetForRam). */
    fun historyBudget(): Long {
        val am = getSystemService(ACTIVITY_SERVICE) as ActivityManager
        val mi = ActivityManager.MemoryInfo().also { am.getMemoryInfo(it) }
        return com.paintengine.pxpaint.core.HistoryState.budgetForRam(mi.totalMem)
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

    override fun setImmersive(on: Boolean) {
        val w = window
        if (Build.VERSION.SDK_INT >= 30) {
            val c = w.insetsController ?: return
            if (on) {
                c.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
                c.hide(android.view.WindowInsets.Type.systemBars())
            } else {
                c.show(android.view.WindowInsets.Type.systemBars())
            }
        } else {
            @Suppress("DEPRECATION")
            val immersive = View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_FULLSCREEN
            @Suppress("DEPRECATION")
            w.decorView.systemUiVisibility = if (on) w.decorView.systemUiVisibility or immersive else w.decorView.systemUiVisibility and immersive.inv()
        }
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

/**
 * Brush library on disk: filesDir/brushes/library.pxb (BrushLibrary.encode),
 * written atomically (temp file + rename) on the io thread. Built-in presets
 * come from assets/brushes/builtin.pxb. First start with Block 5: migrated
 * from the Block 3/4 brush settings (BrushLibrary.migrate).
 */
object BrushLibraryStore {
    private const val TAG = "PxPaint"

    fun builtIns(context: Context): List<com.paintengine.pxpaint.core.BrushPreset> =
        runCatching { context.assets.open("brushes/builtin.pxb").bufferedReader().use { BrushPresetFormat.parsePresets(it.readText()) } }
            .onFailure { Log.e(TAG, "Built-in brushes unreadable", it) }
            .getOrDefault(emptyList())

    private fun file(context: Context) = File(File(context.filesDir, "brushes"), "library.pxb")

    fun load(context: Context): BrushLibrary {
        val builtIns = builtIns(context)
        val f = file(context)
        if (f.exists()) {
            runCatching { BrushLibrary.decode(f.readText(), builtIns) }.getOrNull()?.let { return it }
            Log.w(TAG, "Brush library unreadable, keeping a copy and starting fresh")
            runCatching { f.copyTo(File(f.parentFile, "library.damaged.pxb"), overwrite = true) }
        }
        val prefs = context.getSharedPreferences("pxpaint.brush", Context.MODE_PRIVATE)
        val legacy = if (prefs.contains("marker")) SettingsStore.loadBrush(context) else null
        return BrushLibrary.migrate(builtIns, legacy)
    }

    fun saveAsync(context: Context, library: BrushLibrary, io: ExecutorService) {
        val text = library.encode()
        val f = file(context)
        runCatching {
            io.execute {
                runCatching {
                    f.parentFile?.mkdirs()
                    val tmp = File(f.parentFile, "library.pxb.tmp")
                    tmp.writeText(text)
                    if (!tmp.renameTo(f)) {
                        f.delete()
                        tmp.renameTo(f)
                    }
                }.onFailure { Log.e(TAG, "Brush library not saved", it) }
            }
        }
    }
}
