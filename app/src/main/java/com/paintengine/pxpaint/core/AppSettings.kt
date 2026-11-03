package com.paintengine.pxpaint.core

/**
 * User preferences. Pure Kotlin (no Android types) so the encoding and the
 * defaults are unit-tested; SettingsStore persists the encoded map in
 * SharedPreferences.
 *
 * Unknown keys are ignored and invalid values fall back to the default, so
 * settings written by a newer version never break an older one (and vice versa).
 */
enum class ThemeMode { SYSTEM, DARK, LIGHT }

enum class UiSize(val scale: Float) { COMPACT(0.9f), DEFAULT(1.0f), LARGE(1.15f), EXTRA_LARGE(1.3f) }

/** Where the tool dock goes when the layout uses a side rail (landscape phone, tablet). */
enum class DockSide { AUTO, LEFT, RIGHT }

/** BATTERY = on-demand rendering (a frame only when something changes); SMOOTH = every vsync. */
enum class RenderPreference { BATTERY, SMOOTH }

enum class BackendChoice(val engineName: String) { AUTO("auto"), VULKAN("vulkan"), GLES("gles") }

enum class LanguagePref { SYSTEM, ENGLISH, ITALIAN }

data class AppSettings(
    val theme: ThemeMode = ThemeMode.DARK,
    val uiSize: UiSize = UiSize.DEFAULT,
    val language: LanguagePref = LanguagePref.SYSTEM,
    // Toolbar
    val dockSide: DockSide = DockSide.AUTO,
    val showToolLabels: Boolean = false,
    // Gestures
    val twoFingerRotate: Boolean = true,
    val rotationSnap: Boolean = true,
    val fingerMode: FingerMode = FingerMode.DRAW,
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
    val reduceMotion: Boolean = false,
    val highContrast: Boolean = false,
) {
    fun encode(): Map<String, String> = mapOf(
        K_THEME to theme.name,
        K_UI_SIZE to uiSize.name,
        K_LANGUAGE to language.name,
        K_DOCK_SIDE to dockSide.name,
        K_TOOL_LABELS to showToolLabels.toString(),
        K_ROTATE to twoFingerRotate.toString(),
        K_SNAP to rotationSnap.toString(),
        K_FINGER_MODE to fingerMode.name,
        K_PRESSURE_CURVE to pressureCurve.encode(),
        K_PRESSURE_SENSITIVITY to pressureSensitivity.toString(),
        K_PRESSURE_SMOOTHING to pressureSmoothing.toString(),
        K_RENDER to render.name,
        K_BACKEND to backend.name,
        K_OVERLAY to performanceOverlay.toString(),
        K_LARGE_TARGETS to largeTouchTargets.toString(),
        K_REDUCE_MOTION to reduceMotion.toString(),
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
        const val K_PRESSURE_SENSITIVITY = "pressure_sensitivity"
        const val K_PRESSURE_SMOOTHING = "pressure_smoothing"
        const val K_RENDER = "render"
        const val K_BACKEND = "backend"
        const val K_OVERLAY = "performance_overlay"
        const val K_LARGE_TARGETS = "large_touch_targets"
        const val K_REDUCE_MOTION = "reduce_motion"
        const val K_HIGH_CONTRAST = "high_contrast"

        fun decode(map: Map<String, String>): AppSettings {
            val d = AppSettings()
            return AppSettings(
                theme = enumOr(map[K_THEME], d.theme),
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
                pressureCurve = PressureCurve.decode(map[K_PRESSURE_CURVE]) ?: d.pressureCurve,
                pressureSensitivity = floatIn(map[K_PRESSURE_SENSITIVITY], -1f, 1f, d.pressureSensitivity),
                pressureSmoothing = floatIn(map[K_PRESSURE_SMOOTHING], 0f, 1f, d.pressureSmoothing),
                render = enumOr(map[K_RENDER], d.render),
                backend = enumOr(map[K_BACKEND], d.backend),
                performanceOverlay = boolOr(map[K_OVERLAY], d.performanceOverlay),
                largeTouchTargets = boolOr(map[K_LARGE_TARGETS], d.largeTouchTargets),
                reduceMotion = boolOr(map[K_REDUCE_MOTION], d.reduceMotion),
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
