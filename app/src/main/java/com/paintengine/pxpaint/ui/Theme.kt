package com.paintengine.pxpaint.ui

import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.Drawable
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.RippleDrawable
import android.text.TextUtils
import android.util.TypedValue
import android.view.View
import android.widget.TextView
import com.paintengine.pxpaint.core.AppSettings
import com.paintengine.pxpaint.core.LayoutPolicy
import com.paintengine.pxpaint.core.ThemeMode
import kotlin.math.roundToInt

/**
 * PxPaint design tokens. Built once per settings change; every view is
 * built in code from these values, so a theme or size change is a UI rebuild.
 *
 * Identity: a quiet graphite interface ("ink") that steps back from the
 * artwork, with one warm accent ("ember") reserved for the active tool, the
 * primary action and focus. The light theme is a warm paper tone.
 */
class PxTheme(context: Context, val settings: AppSettings) {
    /** One of the three interface themes (core/DesignTokens): every colour below comes from it. */
    val mode: ThemeMode = settings.theme
    val scale: Float = settings.uiSize.scale
    private val density: Float = context.resources.displayMetrics.density
    val reduceMotion: Boolean = settings.reduceMotion
    /** Motion system (Block 9): tokens, level (user setting + system animator scale), helpers. */
    val motion = Motion(settings.motionLevel, context.resources.displayMetrics.density * settings.uiSize.scale)
    val touchTarget: Int get() = dp(
        (if (settings.largeTouchTargets) LayoutPolicy.LARGE_TOUCH_TARGET_DP else LayoutPolicy.TOUCH_TARGET_DP).toFloat(),
    )

    // --- colours (core/DesignTokens) -------------------------------------------
    val colors = com.paintengine.pxpaint.core.DesignTokens.colors(mode, settings.highContrast)
    val dark: Boolean = colors.isDark
    val bg = colors.bg
    val surface = colors.surface
    val raised = colors.raised
    val sunken = colors.sunken
    val outline = colors.outline
    val text = colors.text
    val textSecondary = colors.textSecondary
    val textTertiary = colors.textTertiary
    val accent = colors.accent
    val onAccent = colors.onAccent
    val accentSoft = colors.accentSoft
    val danger = colors.danger
    val scrim = colors.scrim
    /** Around the canvas in the workspace (passed to the engine). */
    val workspace = colors.workspace
    /** Floating chrome over the canvas: the panel colour, translucent (text keeps >= 7:1 over any artwork). */
    val chrome = colors.chrome
    /** Floating panels (brushes, layers, colour...): translucent, a little more opaque than the chrome. */
    val panel = colors.panel
    /** Hairline edge of floating glass surfaces. */
    val edge = colors.edge
    /** Shadow colour of floating surfaces (Android 9+ tints elevation shadows with it). */
    val shadow = colors.shadow
    val ripple = colors.ripple

    // --- metrics --------------------------------------------------------------
    fun dp(v: Float): Int = (v * density * scale).roundToInt()
    fun dp(v: Int): Int = dp(v.toFloat())
    fun dpF(v: Float): Float = v * density * scale
    val radiusS get() = dpF(com.paintengine.pxpaint.core.DesignTokens.RADIUS_S)
    val radiusM get() = dpF(com.paintengine.pxpaint.core.DesignTokens.RADIUS_M)
    val radiusL get() = dpF(com.paintengine.pxpaint.core.DesignTokens.RADIUS_L)
    val radiusPill get() = dpF(com.paintengine.pxpaint.core.DesignTokens.RADIUS_PILL)
    val gap get() = dp(com.paintengine.pxpaint.core.DesignTokens.SPACE_S)
    val pad get() = dp(com.paintengine.pxpaint.core.DesignTokens.SPACE_L)
    val disabledAlpha get() = com.paintengine.pxpaint.core.DesignTokens.DISABLED_ALPHA
    /** Animation length (0 with "reduce motion"). */
    fun motion(ms: Long): Long = motion.d(ms)

    // --- typography (sp, scaled by the interface size; user font scale applies) -------
    enum class Type(val sp: Float, val medium: Boolean) {
        DISPLAY(26f, true), TITLE(20f, true), HEADLINE(16f, true), BODY(15f, false),
        LABEL(13f, true), CAPTION(12f, false), MICRO(10.5f, true)
    }

    fun style(tv: TextView, type: Type, color: Int = text): TextView {
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, type.sp * scale)
        tv.typeface = if (type.medium) Typeface.create("sans-serif-medium", Typeface.NORMAL) else Typeface.DEFAULT
        tv.setTextColor(color)
        if (type == Type.DISPLAY || type == Type.TITLE) tv.letterSpacing = -0.01f
        if (type == Type.MICRO) tv.letterSpacing = 0.06f
        return tv
    }

    fun text(context: Context, value: CharSequence, type: Type, color: Int = text, lines: Int = 0): TextView =
        style(TextView(context), type, color).apply {
            this.text = value
            if (lines > 0) {
                maxLines = lines
                ellipsize = TextUtils.TruncateAt.END
            }
            includeFontPadding = false
        }

    // --- shapes -----------------------------------------------------------------
    fun rounded(fill: Int, radius: Float = radiusM, stroke: Int = 0, strokeWidthDp: Float = 1f): GradientDrawable =
        GradientDrawable().apply {
            shape = GradientDrawable.RECTANGLE
            cornerRadius = radius
            setColor(fill)
            if (stroke != 0) setStroke(maxOf(1, dp(strokeWidthDp)), stroke)
        }

    fun oval(fill: Int, stroke: Int = 0): GradientDrawable = GradientDrawable().apply {
        shape = GradientDrawable.OVAL
        setColor(fill)
        if (stroke != 0) setStroke(maxOf(1, dp(1.5f)), stroke)
    }

    /** The brand gradient (the logo's sweep of paint): the active tool, the colour well's ring. */
    fun brand(radius: Float = radiusPill): GradientDrawable =
        GradientDrawable(GradientDrawable.Orientation.TL_BR, intArrayOf(BRAND_START, BRAND_END)).apply {
            shape = GradientDrawable.RECTANGLE
            cornerRadius = radius
        }

    /** Touch feedback over [content] (bounded to its shape). */
    fun pressable(content: Drawable?, radius: Float = radiusM): Drawable =
        RippleDrawable(ColorStateList.valueOf(ripple), content, rounded(Color.WHITE, radius))

    /** Selected/unselected tint list for icons and text. */
    fun selectable(selected: Int = accent, normal: Int = text): ColorStateList = ColorStateList(
        arrayOf(intArrayOf(android.R.attr.state_selected), intArrayOf(-android.R.attr.state_enabled), intArrayOf()),
        intArrayOf(selected, textTertiary, normal),
    )

    /** Translucent "glass" surface: [fill] (chrome or panel colour) with a hairline edge. */
    fun glass(radius: Float = radiusL, fill: Int = chrome): GradientDrawable = rounded(fill, radius, edge, 1f)

    /**
     * Makes [v] float over the canvas: soft shadow (tinted on Android 9+),
     * glass background. No blur: Android cannot blur what is under a view
     * when it is the GPU canvas (a SurfaceView), and blurring the canvas
     * itself would cost GPU time while painting (Block 11 brief).
     */
    fun float(v: View, radius: Float = radiusL, fill: Int = chrome, elevationDp: Float = 6f) {
        v.background = glass(radius, fill)
        v.elevation = dpF(elevationDp)
        if (android.os.Build.VERSION.SDK_INT >= 28) {
            v.outlineAmbientShadowColor = shadow
            v.outlineSpotShadowColor = shadow
        }
    }

    fun applyCardOutline(v: View, radius: Float = radiusM, fill: Int = surface) {
        v.background = pressable(rounded(fill, radius, outline), radius)
    }

    companion object {
        const val BRAND_START = 0xFFFFB25C.toInt()
        const val BRAND_END = 0xFFFF4F7B.toInt()
        /** Ink on the brand gradient (contrast 7:1 or more on both ends). */
        const val ON_BRAND = 0xFF1B1024.toInt()
    }
}
