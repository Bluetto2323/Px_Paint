package com.paintengine.pxpaint.screens

import android.view.Gravity
import android.view.View
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import com.paintengine.BuildConfig
import com.paintengine.pxpaint.core.AppSettings
import com.paintengine.pxpaint.core.BackendChoice
import com.paintengine.pxpaint.core.DockSide
import com.paintengine.pxpaint.core.MotionLevel
import com.paintengine.pxpaint.core.PinnedPanel
import com.paintengine.pxpaint.core.Side
import com.paintengine.pxpaint.core.FingerMode
import com.paintengine.pxpaint.core.LanguagePref
import com.paintengine.pxpaint.core.RenderPreference
import com.paintengine.pxpaint.core.S
import com.paintengine.pxpaint.core.ThemeMode
import com.paintengine.pxpaint.core.UiSize
import com.paintengine.pxpaint.core.tr
import com.paintengine.pxpaint.ui.Icon
import com.paintengine.pxpaint.ui.IconButton
import com.paintengine.pxpaint.ui.IconDrawable
import com.paintengine.pxpaint.ui.PxTheme
import com.paintengine.pxpaint.ui.Widgets
import com.paintengine.pxpaint.ui.markHeading
import org.json.JSONObject

/**
 * Settings: appearance, toolbar, gestures, performance, accessibility,
 * about/diagnostics. Every change goes through [PxHost.updateSettings],
 * which persists it and re-themes the open screens; this screen is
 * rebuilt then and restores its scroll position.
 */
class SettingsScreen(host: PxHost) : Screen(host) {
    private var scrollY = 0
    private var scroll: ScrollView? = null

    override fun createView(): View {
        val root = FrameLayout(context).apply { setBackgroundColor(theme.bg) }
        val ins = host.insets
        val maxW = theme.dp(720)

        val page = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, 0, 0, ins.bottom + theme.dp(32))
        }
        val s = host.settings
        val set: (AppSettings) -> Unit = { n ->
            scrollY = scroll?.scrollY ?: 0
            host.updateSettings(n)
        }

        // --- Appearance
        page.addView(Widgets.sectionHeader(context, theme, tr(S.SEC_APPEARANCE)))
        // Three themes, each previewed with its own tokens; applied at once (no restart).
        page.addView(LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(theme.pad, theme.dp(8), theme.pad, theme.dp(8))
            addView(theme.text(context, tr(S.THEME), PxTheme.Type.BODY))
            addView(theme.text(context, tr(S.THEME_DESC), PxTheme.Type.CAPTION, theme.textSecondary).apply { setPadding(0, theme.dp(2), 0, theme.dp(8)) })
            addView(LinearLayout(context).apply {
                orientation = LinearLayout.HORIZONTAL
                for (m in ThemeMode.entries) addView(com.paintengine.pxpaint.ui.ThemePreview(context, theme, m, m == s.theme).apply {
                    setOnClickListener { if (m != host.settings.theme) set(host.settings.copy(theme = m)) }
                }, LinearLayout.LayoutParams(0, theme.dp(132), 1f).apply { marginEnd = if (m != ThemeMode.entries.last()) theme.dp(8) else 0 })
            })
        })
        page.addView(choice(tr(S.UI_SIZE), null, listOf(
            UiSize.COMPACT to "90%", UiSize.DEFAULT to "100%", UiSize.LARGE to "115%", UiSize.EXTRA_LARGE to "130%",
        ), s.uiSize) { set(s.copy(uiSize = it)) })
        page.addView(choice(tr(S.LANGUAGE), null, listOf(
            LanguagePref.SYSTEM to tr(S.LANG_SYSTEM), LanguagePref.ENGLISH to "English", LanguagePref.ITALIAN to "Italiano",
        ), s.language) { set(s.copy(language = it)) })

        // --- Toolbar
        page.addView(Widgets.sectionHeader(context, theme, tr(S.SEC_TOOLBAR)))
        page.addView(choice(tr(S.DOCK_SIDE), tr(S.DOCK_SIDE_DESC), listOf(
            DockSide.AUTO to tr(S.SIDE_AUTO), DockSide.LEFT to tr(S.SIDE_LEFT), DockSide.RIGHT to tr(S.SIDE_RIGHT),
        ), s.dockSide) { set(s.copy(dockSide = it)) })
        page.addView(Widgets.switchRow(context, theme, tr(S.TOOL_LABELS), null, s.showToolLabels) { set(s.copy(showToolLabels = it)) })
        page.addView(choice(tr(S.SLIDER_SIDE), tr(S.SLIDER_SIDE_DESC), listOf(
            Side.LEFT to tr(S.SIDE_LEFT), Side.RIGHT to tr(S.SIDE_RIGHT),
        ), s.sliderSide) { set(s.copy(sliderSide = it)) })
        page.addView(choice(tr(S.PINNED_PANEL), tr(S.PINNED_PANEL_DESC), listOf(
            PinnedPanel.NONE to tr(S.PINNED_NONE), PinnedPanel.BRUSHES to tr(S.BRUSH), PinnedPanel.COLOR to tr(S.COLOR),
            PinnedPanel.LAYERS to tr(S.LAYERS),
        ), s.pinnedPanel) { set(s.copy(pinnedPanel = it)) })

        // --- Gestures
        page.addView(Widgets.sectionHeader(context, theme, tr(S.SEC_GESTURES)))
        page.addView(choice(tr(S.FINGER_MODE), tr(S.FINGER_MODE_DESC), listOf(
            FingerMode.DRAW to tr(S.FINGER_DRAW), FingerMode.PAN to tr(S.FINGER_PAN), FingerMode.GESTURES_ONLY to tr(S.FINGER_GESTURES),
        ), s.fingerMode) { set(s.copy(fingerMode = it)) })
        page.addView(Widgets.switchRow(context, theme, tr(S.TWO_FINGER_ROTATE), null, s.twoFingerRotate) { set(s.copy(twoFingerRotate = it)) })
        page.addView(Widgets.switchRow(context, theme, tr(S.ROTATION_SNAP), null, s.rotationSnap) { set(s.copy(rotationSnap = it)) }.apply {
            isEnabled = s.twoFingerRotate
            alpha = if (s.twoFingerRotate) 1f else 0.45f
        })
        page.addView(Widgets.switchRow(context, theme, tr(S.GESTURE_TAP_UNDO), null, s.tapUndo) { set(s.copy(tapUndo = it)) })
        page.addView(Widgets.switchRow(context, theme, tr(S.GESTURE_TAP_REDO), null, s.tapRedo) { set(s.copy(tapRedo = it)) })
        page.addView(Widgets.switchRow(context, theme, tr(S.GESTURE_PINCH_FIT), tr(S.GESTURE_PINCH_FIT_DESC), s.quickPinchFit) { set(s.copy(quickPinchFit = it)) })
        page.addView(Widgets.switchRow(context, theme, tr(S.GESTURE_TAP_FULLSCREEN), null, s.tapFullscreen) { set(s.copy(tapFullscreen = it)) })
        page.addView(Widgets.switchRow(context, theme, tr(S.GESTURE_STYLUS_PICK), tr(S.GESTURE_STYLUS_PICK_DESC), s.stylusButtonPick) { set(s.copy(stylusButtonPick = it)) })

        // --- Stylus & pressure
        page.addView(Widgets.sectionHeader(context, theme, tr(S.SEC_STYLUS)))
        page.addView(Widgets.row(context, theme, tr(S.PRESSURE_SETTINGS),
            "${tr(S.PRESSURE_SETTINGS_DESC)}\n${stylusSummary()}",
            View(context).apply { background = IconDrawable(Icon.CHEVRON_RIGHT, theme.textSecondary) }.also {
                it.layoutParams = LinearLayout.LayoutParams(theme.dp(20), theme.dp(20))
            }).apply {
            isClickable = true
            setOnClickListener { host.push(PressureScreen(host)) }
        })

        // --- Performance
        page.addView(Widgets.sectionHeader(context, theme, tr(S.SEC_PERFORMANCE)))
        page.addView(choice(tr(S.RENDERING),
            if (s.render == RenderPreference.BATTERY) tr(S.RENDER_BATTERY_DESC) else tr(S.RENDER_SMOOTH_DESC),
            listOf(RenderPreference.BATTERY to tr(S.RENDER_BATTERY), RenderPreference.SMOOTH to tr(S.RENDER_SMOOTH)),
            s.render) { set(s.copy(render = it)) })
        page.addView(choice(tr(S.GRAPHICS_ENGINE), "${tr(S.BACKEND_AUTO_DESC)}\n${tr(S.BACKEND_APPLIES_LATER)}", listOf(
            BackendChoice.AUTO to tr(S.BACKEND_AUTO), BackendChoice.VULKAN to tr(S.BACKEND_VULKAN), BackendChoice.GLES to tr(S.BACKEND_GLES),
        ), s.backend) { set(s.copy(backend = it)) })
        page.addView(Widgets.switchRow(context, theme, tr(S.PERF_OVERLAY), tr(S.PERF_OVERLAY_DESC), s.performanceOverlay) { set(s.copy(performanceOverlay = it)) })

        // --- Accessibility
        page.addView(Widgets.sectionHeader(context, theme, tr(S.SEC_ACCESSIBILITY)))
        page.addView(Widgets.switchRow(context, theme, tr(S.LARGE_TARGETS), null, s.largeTouchTargets) { set(s.copy(largeTouchTargets = it)) })
        page.addView(Widgets.switchRow(context, theme, tr(S.HIGH_CONTRAST), null, s.highContrast) { set(s.copy(highContrast = it)) })
        page.addView(choice(tr(S.MOTION_LEVEL), tr(S.MOTION_LEVEL_DESC), listOf(
            MotionLevel.FULL to tr(S.MOTION_FULL), MotionLevel.REDUCED to tr(S.MOTION_REDUCED), MotionLevel.OFF to tr(S.MOTION_OFF),
        ), s.motionLevel) { set(s.copy(motionLevel = it)) })

        // --- About
        page.addView(Widgets.sectionHeader(context, theme, tr(S.SEC_ABOUT)))
        page.addView(Widgets.row(context, theme, tr(S.VERSION), "PxPaint ${BuildConfig.VERSION_NAME} (${BuildConfig.BUILD_TYPE})", null))
        page.addView(Widgets.row(context, theme, tr(S.GRAPHICS_IN_USE), graphicsInUse(), null))
        page.addView(Widgets.row(context, theme, tr(S.ENGINE_DIAGNOSTICS), tr(S.ENGINE_DIAGNOSTICS_DESC),
            View(context).apply { background = IconDrawable(Icon.CHEVRON_RIGHT, theme.textSecondary) }.also {
                it.layoutParams = LinearLayout.LayoutParams(theme.dp(20), theme.dp(20))
            }).apply {
            isClickable = true
            setOnClickListener { host.openDiagnostics() }
        })
        page.addView(Widgets.row(context, theme, tr(S.RESET_TIPS), null, null).apply {
            isClickable = true
            setOnClickListener {
                host.resetTips()
                host.overlays.message(tr(S.TIPS_RESET_DONE))
            }
        })

        // Centre a readable column on wide screens.
        val sv = ScrollView(context).apply {
            clipToPadding = false
            setPadding(ins.left, 0, ins.right, 0)
            addView(MaxWidth(context, maxW).apply { addView(page) })
        }
        scroll = sv
        val bar = topBar()
        root.addView(bar, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP))
        root.addView(sv, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT).apply {
            topMargin = ins.top + theme.touchTarget + theme.dp(16)
        })
        sv.post { sv.scrollTo(0, scrollY) }
        return root
    }

    private fun topBar(): View = LinearLayout(context).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        setBackgroundColor(theme.bg)
        setPadding(host.insets.left + theme.dp(4), host.insets.top + theme.dp(8), host.insets.right + theme.dp(8), theme.dp(8))
        addView(IconButton(context, theme, Icon.BACK, tr(S.ACTION_CLOSE)).apply { setOnClickListener { host.pop() } })
        addView(theme.text(context, tr(S.SETTINGS), PxTheme.Type.TITLE).apply {
            setPadding(theme.dp(8), 0, 0, 0)
            markHeading()
        })
    }

    /** Title (+ description) above a single-choice chip row. */
    private fun <T> choice(title: String, description: String?, options: List<Pair<T, String>>, selected: T, onSelect: (T) -> Unit): View =
        LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(theme.pad, theme.dp(10), theme.pad, theme.dp(12))
            addView(theme.text(context, title, PxTheme.Type.BODY))
            if (description != null) addView(theme.text(context, description, PxTheme.Type.CAPTION, theme.textSecondary).apply {
                setPadding(0, theme.dp(3), 0, 0)
            })
            addView(Widgets.segmented(context, theme, options, selected) { if (it != selected) onSelect(it) }.apply {
                setPadding(0, theme.dp(10), 0, 0)
            })
        }

    private fun stylusSummary(): String =
        if (host.stylus.pressureAvailable) tr(S.PRESSURE_STYLUS_READY) else tr(S.STYLUS_NONE)

    private fun graphicsInUse(): String {
        val e = host.engineIfStarted ?: return tr(S.NOT_STARTED_YET)
        val s = runCatching { JSONObject(e.statusJson()) }.getOrNull() ?: return tr(S.NOT_STARTED_YET)
        val backend = s.optString("backend")
        if (backend.isEmpty() || backend == "none") return tr(S.NOT_STARTED_YET)
        return "$backend · ${s.optString("gpu")} · ${s.optString("renderMode")}"
    }
}

/** Centres its single child and caps its width at [maxW] px. */
class MaxWidth(context: android.content.Context, private val maxW: Int) : FrameLayout(context) {
    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val w = MeasureSpec.getSize(widthMeasureSpec)
        val childW = minOf(w, maxW)
        var h = 0
        for (i in 0 until childCount) {
            val c = getChildAt(i)
            c.measure(MeasureSpec.makeMeasureSpec(childW, MeasureSpec.EXACTLY), getChildMeasureSpec(heightMeasureSpec, 0, LayoutParams.WRAP_CONTENT))
            h = maxOf(h, c.measuredHeight)
        }
        setMeasuredDimension(w, resolveSize(h, heightMeasureSpec))
    }

    override fun onLayout(changed: Boolean, l: Int, t: Int, r: Int, b: Int) {
        val w = r - l
        for (i in 0 until childCount) {
            val c = getChildAt(i)
            val x = (w - c.measuredWidth) / 2
            c.layout(x, 0, x + c.measuredWidth, c.measuredHeight)
        }
    }
}
