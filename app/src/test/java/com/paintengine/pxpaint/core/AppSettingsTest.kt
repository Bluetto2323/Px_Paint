package com.paintengine.pxpaint.core

import org.junit.Assert.assertEquals
import org.junit.Test

class AppSettingsTest {
    @Test
    fun defaultsRoundTrip() {
        val d = AppSettings()
        assertEquals(d, AppSettings.decode(d.encode()))
        assertEquals(d, AppSettings.decode(emptyMap()))
    }

    @Test
    fun everyFieldRoundTrips() {
        val s = AppSettings(
            theme = ThemeMode.LIGHT, uiSize = UiSize.EXTRA_LARGE, language = LanguagePref.ITALIAN,
            dockSide = DockSide.RIGHT, showToolLabels = true, twoFingerRotate = false, rotationSnap = false,
            fingerMode = FingerMode.GESTURES_ONLY, pressureCurve = PressureCurve.SOFT, pressureSensitivity = -0.5f,
            pressureSmoothing = 0.8f, render = RenderPreference.SMOOTH, backend = BackendChoice.GLES,
            performanceOverlay = true, largeTouchTargets = true, reduceMotion = true, highContrast = true,
        )
        assertEquals(s, AppSettings.decode(s.encode()))
    }

    @Test
    fun invalidValuesFallBackToDefaults() {
        val map = AppSettings(theme = ThemeMode.LIGHT).encode().toMutableMap()
        map[AppSettings.K_UI_SIZE] = "GIGANTIC"
        map[AppSettings.K_ROTATE] = "maybe"
        map["some_future_key"] = "x"
        val s = AppSettings.decode(map)
        assertEquals(ThemeMode.LIGHT, s.theme)
        assertEquals(UiSize.DEFAULT, s.uiSize)
        assertEquals(true, s.twoFingerRotate)
    }

    @Test
    fun blockThreeFingerSettingIsMigrated() {
        assertEquals(FingerMode.PAN, AppSettings.decode(mapOf("finger_drawing" to "false")).fingerMode)
        assertEquals(FingerMode.DRAW, AppSettings.decode(mapOf("finger_drawing" to "true")).fingerMode)
        // The new key wins over the old one.
        assertEquals(FingerMode.GESTURES_ONLY,
            AppSettings.decode(mapOf("finger_drawing" to "true", "finger_mode" to "GESTURES_ONLY")).fingerMode)
    }

    @Test
    fun pressureValuesAreClamped() {
        val s = AppSettings.decode(mapOf("pressure_sensitivity" to "7", "pressure_smoothing" to "NaN", "pressure_curve" to "x;1;2"))
        assertEquals(1.0, s.pressureSensitivity.toDouble(), 0.0)
        assertEquals(0.35, s.pressureSmoothing.toDouble(), 1e-6)
        assertEquals(PressureCurve.LINEAR, s.pressureCurve)
    }

    @Test
    fun encodedKeysAreStable() {
        // Changing a key would silently reset users' settings.
        assertEquals(
            setOf("theme", "ui_size", "language", "dock_side", "tool_labels", "two_finger_rotate", "rotation_snap",
                "finger_mode", "pressure_curve", "pressure_sensitivity", "pressure_smoothing", "render", "backend", "performance_overlay", "large_touch_targets", "reduce_motion",
                "high_contrast"),
            AppSettings().encode().keys,
        )
    }
}
