package com.paintengine.pxpaint.core

/**
 * PxPaint design tokens (pure values, unit-tested for contrast and touch
 * sizes). The view theme (ui/Theme.kt) turns them into pixels and drawables;
 * no screen hard-codes a colour or a size of its own.
 *
 * Block 11: three complete themes (Deep Blue, Pure Black, Warm Beige) of
 * translucent "glass" surfaces floating over the canvas. One accent per
 * theme marks the active tool, the selection and the primary action.
 */
data class ColorTokens(
    val bg: Int,
    val surface: Int,
    /** Floating panels and controls (the "glass"), opaque value; see [panel] and [chrome]. */
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
    /** Around the canvas (passed to the engine; never the paper or the artwork). */
    val workspace: Int,
    val ripple: Int,
) {
    val isDark: Boolean get() = ColorMath.luminance(bg) < 0.2
    /** Selected background: accent at low alpha. */
    val accentSoft: Int get() = (accent and 0x00FFFFFF) or (if (isDark) 0x33000000 else 0x2E000000)
    /** Floating controls over the canvas: the glass colour at 90 % (text >= 7:1 over white or black artwork). */
    val chrome: Int get() = (raised and 0x00FFFFFF) or (CHROME_ALPHA shl 24)
    /** Floating panels: the glass colour at 95 % (long text, lists). */
    val panel: Int get() = (raised and 0x00FFFFFF) or (PANEL_ALPHA shl 24)
    /** Hairline edge of glass surfaces: a light rim on dark themes, the border colour on the light one. */
    val edge: Int get() = if (isDark) 0x24FFFFFF else (outline and 0x00FFFFFF) or 0xE6000000.toInt()
    /** Soft shadow tint. */
    val shadow: Int get() = if (isDark) 0xFF000000.toInt() else 0xFF5A4630.toInt()

    companion object {
        const val CHROME_ALPHA = 0xCC
        const val PANEL_ALPHA = 0xE0
    }
}

object DesignTokens {
    // --- colours: three complete themes (Block 11) ------------------------
    fun colors(theme: ThemeMode, highContrast: Boolean): ColorTokens {
        val base = when (theme) {
            ThemeMode.DEEP_BLUE -> DEEP_BLUE
            ThemeMode.PURE_BLACK -> PURE_BLACK
            ThemeMode.WARM_BEIGE -> WARM_BEIGE
        }
        if (!highContrast) return base
        return if (base.isDark) base.copy(
            outline = 0xFF9AA4B2.toInt(), textSecondary = 0xFFD0D7E0.toInt(), textTertiary = 0xFFAAB3BF.toInt(),
        ) else base.copy(
            outline = 0xFF6B5A44.toInt(), textSecondary = 0xFF3E3428.toInt(), textTertiary = 0xFF55483A.toInt(),
        )
    }

    /** Dark navy, midnight blue, cool blue-grey; soft silver text; calm blue accent. */
    val DEEP_BLUE = ColorTokens(
        bg = 0xFF101A2B.toInt(), surface = 0xFF17263B.toInt(), raised = 0xFF20344E.toInt(), sunken = 0xFF0B1320.toInt(),
        outline = 0xFF354A65.toInt(), text = 0xFFE6EDF7.toInt(), textSecondary = 0xFFA9BAD0.toInt(),
        textTertiary = 0xFF8899B2.toInt(), accent = 0xFF6FA8E8.toInt(), onAccent = 0xFF0B1626.toInt(),
        danger = 0xFFFF7A88.toInt(), scrim = 0xB3050A12.toInt(), workspace = 0xFF0C1524.toInt(), ripple = 0x33DCE8FF,
    )

    /** Deep black, graphite, neutral grey; one cool, quiet accent. */
    val PURE_BLACK = ColorTokens(
        bg = 0xFF08090B.toInt(), surface = 0xFF111317.toInt(), raised = 0xFF1B1E23.toInt(), sunken = 0xFF040506.toInt(),
        outline = 0xFF30343B.toInt(), text = 0xFFF3F4F6.toInt(), textSecondary = 0xFFA2A7B0.toInt(),
        textTertiary = 0xFF80868F.toInt(), accent = 0xFFA6B7D4.toInt(), onAccent = 0xFF0B0D10.toInt(),
        danger = 0xFFFF6B78.toInt(), scrim = 0xCC000000.toInt(), workspace = 0xFF050607.toInt(), ripple = 0x2EFFFFFF,
    )

    /**
     * Warm paper, sand, cream, muted brown, graphite text. The suggested
     * accent (#9B7850) and secondary text (#766957) are darkened (#6F4F2C,
     * #5C4F40) so text keeps WCAG AA on every surface, beige background included.
     */
    val WARM_BEIGE = ColorTokens(
        bg = 0xFFD8C8A8.toInt(), surface = 0xFFE6D9BF.toInt(), raised = 0xFFF0E6D3.toInt(), sunken = 0xFFCDBC9A.toInt(),
        outline = 0xFFC6B697.toInt(), text = 0xFF30291F.toInt(), textSecondary = 0xFF5C4F40.toInt(),
        textTertiary = 0xFF6E604B.toInt(), accent = 0xFF6F4F2C.toInt(), onAccent = 0xFFFBF6EC.toInt(),
        danger = 0xFF922530.toInt(), scrim = 0x80241C12.toInt(), workspace = 0xFFC8B793.toInt(), ripple = 0x26302010,
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
    const val RADIUS_L = 22f
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
