package com.paintengine.pxpaint.core

/**
 * PxPaint design tokens (pure values, unit-tested for contrast and touch
 * sizes). The view theme (ui/Theme.kt) turns them into pixels and drawables;
 * no screen hard-codes a colour or a size of its own.
 *
 * Identity: a quiet graphite interface ("ink") that steps back from the
 * artwork, one warm accent ("ember") for the active tool, the primary action
 * and focus. The light theme is a warm paper tone.
 */
data class ColorTokens(
    val bg: Int,
    val surface: Int,
    val raised: Int,
    val sunken: Int,
    val outline: Int,
    val text: Int,
    val textSecondary: Int,
    /** Hints and disabled content (WCAG: >= 3:1 on every surface). */
    val textTertiary: Int,
    val accent: Int,
    val onAccent: Int,
    val danger: Int,
    val scrim: Int,
    /** Around the canvas (passed to the engine). */
    val workspace: Int,
    val ripple: Int,
) {
    /** Selected background: accent at low alpha. */
    val accentSoft: Int get() = (accent and 0x00FFFFFF) or (if (isDark) 0x2E000000 else 0x24000000)
    /** Floating chrome over the canvas: the raised surface, slightly translucent. */
    val chrome: Int get() = (raised and 0x00FFFFFF) or 0xF2000000.toInt()
    val isDark: Boolean get() = ColorMath.luminance(bg) < 0.2
}

object DesignTokens {
    // --- colours ----------------------------------------------------------
    fun colors(dark: Boolean, highContrast: Boolean): ColorTokens {
        val base = if (dark) DARK else LIGHT
        return if (!highContrast) base else base.copy(
            outline = if (dark) 0xFF9AA1A9.toInt() else 0xFF5A5E64.toInt(),
            textSecondary = if (dark) 0xFFC9CED4.toInt() else 0xFF3E4248.toInt(),
            textTertiary = if (dark) 0xFFA3A9B0.toInt() else 0xFF5E636A.toInt(),
        )
    }

    val DARK = ColorTokens(
        bg = 0xFF0F1012.toInt(), surface = 0xFF17191C.toInt(), raised = 0xFF1F2226.toInt(), sunken = 0xFF0B0C0E.toInt(),
        outline = 0xFF2B2F35.toInt(), text = 0xFFEEF0F2.toInt(), textSecondary = 0xFFA3A9B0.toInt(),
        textTertiary = 0xFF80868E.toInt(), accent = 0xFFFF7A59.toInt(), onAccent = 0xFF1A0D08.toInt(),
        danger = 0xFFFF5C6C.toInt(), scrim = 0xB3000000.toInt(), workspace = 0xFF121316.toInt(), ripple = 0x33FFFFFF,
    )

    val LIGHT = ColorTokens(
        bg = 0xFFF4F2EE.toInt(), surface = 0xFFFFFFFF.toInt(), raised = 0xFFFBFAF8.toInt(), sunken = 0xFFE9E6E0.toInt(),
        outline = 0xFFDDD9D2.toInt(), text = 0xFF18191B.toInt(), textSecondary = 0xFF5E636A.toInt(),
        textTertiary = 0xFF7A7F86.toInt(), accent = 0xFFC24020.toInt(), onAccent = 0xFFFFFFFF.toInt(),
        danger = 0xFFC42D3D.toInt(), scrim = 0x80101012.toInt(), workspace = 0xFFE4E1DB.toInt(), ripple = 0x22000000,
    )

    // --- spacing (dp) -------------------------------------------------------
    const val SPACE_XS = 4
    const val SPACE_S = 8
    const val SPACE_M = 12
    const val SPACE_L = 16
    const val SPACE_XL = 24

    // --- corner radii (dp) --------------------------------------------------
    const val RADIUS_S = 10f
    const val RADIUS_M = 16f
    const val RADIUS_L = 24f
    const val RADIUS_PILL = 999f

    // --- icons (dp) ---------------------------------------------------------
    const val ICON_S = 18
    const val ICON_M = 22
    const val ICON_L = 28
    /** Icon button box (the touch target may be larger). */
    const val ICON_BUTTON = 40

    // --- touch targets (dp) -------------------------------------------------
    const val TOUCH = LayoutPolicy.TOUCH_TARGET_DP
    const val TOUCH_LARGE = LayoutPolicy.LARGE_TOUCH_TARGET_DP

    // --- panels (dp) --------------------------------------------------------
    const val PANEL_MIN = LayoutPolicy.PANEL_MIN_DP
    const val PANEL_DEFAULT = LayoutPolicy.PANEL_DEFAULT_DP
    const val PANEL_MAX = LayoutPolicy.PANEL_MAX_DP
    const val SHEET_MAX_WIDTH = 560
    /** Quick slider: track length and width. */
    const val SLIDER_LENGTH = 168
    const val SLIDER_WIDTH = 36
    const val SWATCH = 32

    // --- states ---------------------------------------------------------------
    const val DISABLED_ALPHA = 0.38f
    const val DIMMED_ALPHA = 0.55f
    const val PRESSED_SCALE = 0.96f

    // --- motion (ms; 0 with "reduce motion") ----------------------------------
    const val MOTION_SHORT = 140L
    const val MOTION_MEDIUM = 220L
}
