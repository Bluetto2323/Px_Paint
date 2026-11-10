package com.paintengine.pxpaint.core

/**
 * User preferences. Pure Kotlin (no Android types) so the encoding and the
 * defaults are unit-tested; SettingsStore persists the encoded map in
 * SharedPreferences.
 *
 * Unknown keys are ignored and invalid values fall back to the default, so
 * settings written by a newer version never break an older one (and vice versa).
 */
/**
 * The three interface themes (Block 11). They change the whole interface,
 * never the artwork. Names of Blocks 2-10 migrate: dark / system -> Deep
 * Blue, light -> Warm Beige.
 */
enum class ThemeMode(val label: S) {
    DEEP_BLUE(S.THEME_DEEP_BLUE), PURE_BLACK(S.THEME_PURE_BLACK), WARM_BEIGE(S.THEME_WARM_BEIGE);

    companion object {
        fun parse(v: String?): ThemeMode? = when (v) {
            "DARK", "SYSTEM" -> DEEP_BLUE
            "LIGHT" -> WARM_BEIGE
            else -> entries.firstOrNull { it.name == v }
        }
    }
}

enum class UiSize(val scale: Float) { COMPACT(0.9f), DEFAULT(1.0f), LARGE(1.15f), EXTRA_LARGE(1.3f) }

/** Where the tool dock goes when the layout uses a side rail (landscape phone, tablet). */
enum class DockSide { AUTO, LEFT, RIGHT }

/** BATTERY = on-demand rendering (a frame only when something changes); SMOOTH = every vsync. */
enum class RenderPreference { BATTERY, SMOOTH }

enum class BackendChoice(val engineName: String) { AUTO("auto"), VULKAN("vulkan"), GLES("gles") }

enum class LanguagePref { SYSTEM, ENGLISH, ITALIAN }

data class AppSettings(
    val theme: ThemeMode = ThemeMode.DEEP_BLUE,
    val uiSize: UiSize = UiSize.DEFAULT,
    val language: LanguagePref = LanguagePref.SYSTEM,
    // Toolbar
    val dockSide: DockSide = DockSide.AUTO,
    val showToolLabels: Boolean = false,
    // Gestures
    val twoFingerRotate: Boolean = true,
    val rotationSnap: Boolean = true,
    val fingerMode: FingerMode = FingerMode.DRAW,
    // Gestures (Block 8): finger taps anywhere on the canvas.
    val tapUndo: Boolean = true,            // two-finger tap
    val tapRedo: Boolean = true,            // three-finger tap
    val quickPinchFit: Boolean = true,      // fast pinch-in: fit the canvas
    val tapFullscreen: Boolean = true,      // four-finger tap: interface on / off
    val stylusButtonPick: Boolean = true,   // stylus side button held: eyedropper
    val stylusButtonErase: Boolean = false, // ... or: erases while held (takes precedence)
    val longPressPick: Boolean = true,      // Block 12.5: a finger held still on the canvas: eyedropper
    /** Block 12.5: the controls setup offered at the first start has been seen. */
    val controlsSetupDone: Boolean = false,
    // Workspace (Block 8)
    val sliderSide: Side = Side.LEFT,       // quick size / opacity sliders
    val pinnedPanel: PinnedPanel = PinnedPanel.NONE,   // tablets
    val panelWidthDp: Int = LayoutPolicy.PANEL_DEFAULT_DP,
    // Stylus & pressure (Block 4): device calibration, shared by every brush.
    val pressureCurve: PressureCurve = PressureCurve.LINEAR,
    val pressureSensitivity: Float = 0f,   // -1 (needs force) .. +1 (light touch)
    val pressureSmoothing: Float = 0.35f,  // 0 = raw .. 1 = strongest
    // Performance
    val render: RenderPreference = RenderPreference.BATTERY,
    val backend: BackendChoice = BackendChoice.AUTO,
    val performanceOverlay: Boolean = false,
    // Accessibility
    val largeTouchTargets: Boolean = false,
    /** Block 9: full / reduced / no animation (replaces the Block 2 "reduce motion" switch). */
    val motionLevel: MotionLevel = MotionLevel.FULL,
    val highContrast: Boolean = false,
) {
    /** Anything but full motion (kept for the views that only need a yes / no). */
    val reduceMotion: Boolean get() = motionLevel != MotionLevel.FULL

    fun encode(): Map<String, String> = mapOf(
        K_THEME to theme.name,
        K_UI_SIZE to uiSize.name,
        K_LANGUAGE to language.name,
        K_DOCK_SIDE to dockSide.name,
        K_TOOL_LABELS to showToolLabels.toString(),
        K_ROTATE to twoFingerRotate.toString(),
        K_SNAP to rotationSnap.toString(),
        K_FINGER_MODE to fingerMode.name,
        K_TAP_UNDO to tapUndo.toString(),
        K_TAP_REDO to tapRedo.toString(),
        K_PINCH_FIT to quickPinchFit.toString(),
        K_TAP_FULLSCREEN to tapFullscreen.toString(),
        K_STYLUS_PICK to stylusButtonPick.toString(),
        K_STYLUS_ERASE to stylusButtonErase.toString(),
        K_LONG_PRESS_PICK to longPressPick.toString(),
        K_CONTROLS_SETUP to controlsSetupDone.toString(),
        K_SLIDER_SIDE to sliderSide.name,
        K_PINNED to pinnedPanel.name,
        K_PANEL_WIDTH to panelWidthDp.toString(),
        K_PRESSURE_CURVE to pressureCurve.encode(),
        K_PRESSURE_SENSITIVITY to pressureSensitivity.toString(),
        K_PRESSURE_SMOOTHING to pressureSmoothing.toString(),
        K_RENDER to render.name,
        K_BACKEND to backend.name,
        K_OVERLAY to performanceOverlay.toString(),
        K_LARGE_TARGETS to largeTouchTargets.toString(),
        K_MOTION to motionLevel.name,
        K_HIGH_CONTRAST to highContrast.toString(),
    )

    companion object {
        const val VERSION = 1
        const val K_THEME = "theme"
        const val K_UI_SIZE = "ui_size"
        const val K_LANGUAGE = "language"
        const val K_DOCK_SIDE = "dock_side"
        const val K_TOOL_LABELS = "tool_labels"
        const val K_ROTATE = "two_finger_rotate"
        const val K_SNAP = "rotation_snap"
        /** Block 2/3 boolean ("paint with a finger"); read only to migrate to [K_FINGER_MODE]. */
        const val K_FINGER = "finger_drawing"
        const val K_FINGER_MODE = "finger_mode"
        const val K_PRESSURE_CURVE = "pressure_curve"
        const val K_TAP_UNDO = "tap_undo"
        const val K_TAP_REDO = "tap_redo"
        const val K_PINCH_FIT = "quick_pinch_fit"
        const val K_TAP_FULLSCREEN = "tap_fullscreen"
        const val K_STYLUS_PICK = "stylus_button_pick"
        const val K_STYLUS_ERASE = "stylus_button_erase"
        const val K_LONG_PRESS_PICK = "long_press_pick"
        const val K_CONTROLS_SETUP = "controls_setup_done"
        const val K_SLIDER_SIDE = "slider_side"
        const val K_PINNED = "pinned_panel"
        const val K_PANEL_WIDTH = "panel_width_dp"
        const val K_PRESSURE_SENSITIVITY = "pressure_sensitivity"
        const val K_PRESSURE_SMOOTHING = "pressure_smoothing"
        const val K_RENDER = "render"
        const val K_BACKEND = "backend"
        const val K_OVERLAY = "performance_overlay"
        const val K_LARGE_TARGETS = "large_touch_targets"
        /** Block 2-8 boolean; read only to migrate to [K_MOTION]. */
        const val K_REDUCE_MOTION = "reduce_motion"
        const val K_MOTION = "motion_level"
        const val K_HIGH_CONTRAST = "high_contrast"

        fun decode(map: Map<String, String>): AppSettings {
            val d = AppSettings()
            return AppSettings(
                theme = ThemeMode.parse(map[K_THEME]) ?: d.theme,
                uiSize = enumOr(map[K_UI_SIZE], d.uiSize),
                language = enumOr(map[K_LANGUAGE], d.language),
                dockSide = enumOr(map[K_DOCK_SIDE], d.dockSide),
                showToolLabels = boolOr(map[K_TOOL_LABELS], d.showToolLabels),
                twoFingerRotate = boolOr(map[K_ROTATE], d.twoFingerRotate),
                rotationSnap = boolOr(map[K_SNAP], d.rotationSnap),
                fingerMode = enumValues<FingerMode>().firstOrNull { it.name == map[K_FINGER_MODE] }
                    ?: when (map[K_FINGER]) {
                        "false" -> FingerMode.PAN
                        else -> d.fingerMode
                    },
                tapUndo = boolOr(map[K_TAP_UNDO], d.tapUndo),
                tapRedo = boolOr(map[K_TAP_REDO], d.tapRedo),
                quickPinchFit = boolOr(map[K_PINCH_FIT], d.quickPinchFit),
                tapFullscreen = boolOr(map[K_TAP_FULLSCREEN], d.tapFullscreen),
                stylusButtonPick = boolOr(map[K_STYLUS_PICK], d.stylusButtonPick),
                stylusButtonErase = boolOr(map[K_STYLUS_ERASE], d.stylusButtonErase),
                longPressPick = boolOr(map[K_LONG_PRESS_PICK], d.longPressPick),
                controlsSetupDone = boolOr(map[K_CONTROLS_SETUP], d.controlsSetupDone),
                sliderSide = enumOr(map[K_SLIDER_SIDE], d.sliderSide),
                pinnedPanel = enumOr(map[K_PINNED], d.pinnedPanel),
                panelWidthDp = map[K_PANEL_WIDTH]?.toIntOrNull()?.coerceIn(LayoutPolicy.PANEL_MIN_DP, LayoutPolicy.PANEL_MAX_DP) ?: d.panelWidthDp,
                pressureCurve = PressureCurve.decode(map[K_PRESSURE_CURVE]) ?: d.pressureCurve,
                pressureSensitivity = floatIn(map[K_PRESSURE_SENSITIVITY], -1f, 1f, d.pressureSensitivity),
                pressureSmoothing = floatIn(map[K_PRESSURE_SMOOTHING], 0f, 1f, d.pressureSmoothing),
                render = enumOr(map[K_RENDER], d.render),
                backend = enumOr(map[K_BACKEND], d.backend),
                performanceOverlay = boolOr(map[K_OVERLAY], d.performanceOverlay),
                largeTouchTargets = boolOr(map[K_LARGE_TARGETS], d.largeTouchTargets),
                motionLevel = enumValues<MotionLevel>().firstOrNull { it.name == map[K_MOTION] }
                    ?: if (map[K_REDUCE_MOTION] == "true") MotionLevel.REDUCED else d.motionLevel,
                highContrast = boolOr(map[K_HIGH_CONTRAST], d.highContrast),
            )
        }

        private inline fun <reified E : Enum<E>> enumOr(value: String?, default: E): E =
            enumValues<E>().firstOrNull { it.name == value } ?: default

        private fun floatIn(value: String?, lo: Float, hi: Float, default: Float): Float =
            value?.toFloatOrNull()?.takeIf { it.isFinite() }?.coerceIn(lo, hi) ?: default

        private fun boolOr(value: String?, default: Boolean): Boolean = when (value) {
            "true" -> true
            "false" -> false
            else -> default
        }
    }
}
