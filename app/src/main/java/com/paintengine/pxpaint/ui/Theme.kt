package com.paintengine.pxpaint.ui

import android.content.Context
import android.content.res.ColorStateList
import android.content.res.Configuration
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
    val dark: Boolean = when (settings.theme) {
        ThemeMode.DARK -> true
        ThemeMode.LIGHT -> false
        ThemeMode.SYSTEM ->
            (context.resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES
    }
    val scale: Float = settings.uiSize.scale
    private val density: Float = context.resources.displayMetrics.density
    val reduceMotion: Boolean = settings.reduceMotion
    val touchTarget: Int get() = dp(
        (if (settings.largeTouchTargets) LayoutPolicy.LARGE_TOUCH_TARGET_DP else LayoutPolicy.TOUCH_TARGET_DP).toFloat(),
    )

    // --- colours ------------------------------------------------------------
    val bg = if (dark) 0xFF0F1012.toInt() else 0xFFF4F2EE.toInt()
    val surface = if (dark) 0xFF17191C.toInt() else 0xFFFFFFFF.toInt()
    val raised = if (dark) 0xFF1F2226.toInt() else 0xFFFBFAF8.toInt()
    val sunken = if (dark) 0xFF0B0C0E.toInt() else 0xFFE9E6E0.toInt()
    val outline = when {
        settings.highContrast -> if (dark) 0xFF9AA1A9.toInt() else 0xFF5A5E64.toInt()
        dark -> 0xFF2B2F35.toInt()
        else -> 0xFFDDD9D2.toInt()
    }
    val text = if (dark) 0xFFEEF0F2.toInt() else 0xFF18191B.toInt()
    val textSecondary = if (dark) 0xFFA3A9B0.toInt() else 0xFF5E636A.toInt()
    val textTertiary = if (dark) 0xFF6E747C.toInt() else 0xFF8E939A.toInt()
    val accent = if (dark) 0xFFFF7A59.toInt() else 0xFFE5532F.toInt()
    val onAccent = if (dark) 0xFF1A0D08.toInt() else 0xFFFFFFFF.toInt()
    val accentSoft = (accent and 0x00FFFFFF) or (if (dark) 0x2E000000 else 0x24000000)
    val danger = if (dark) 0xFFFF5C6C.toInt() else 0xFFD93848.toInt()
    val scrim = if (dark) 0xB3000000.toInt() else 0x80101012.toInt()
    /** Around the canvas in the workspace (passed to the engine). */
    val workspace = if (dark) 0xFF121316.toInt() else 0xFFE4E1DB.toInt()
    /** Floating chrome over the canvas: slightly translucent surface. */
    val chrome = (raised and 0x00FFFFFF) or 0xF2000000.toInt()
    val ripple = if (dark) 0x33FFFFFF else 0x22000000

    // --- metrics --------------------------------------------------------------
    fun dp(v: Float): Int = (v * density * scale).roundToInt()
    fun dp(v: Int): Int = dp(v.toFloat())
    fun dpF(v: Float): Float = v * density * scale
    val radiusS get() = dpF(10f)
    val radiusM get() = dpF(16f)
    val radiusL get() = dpF(24f)
    val gap get() = dp(8)
    val pad get() = dp(16)

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

    /** Touch feedback over [content] (bounded to its shape). */
    fun pressable(content: Drawable?, radius: Float = radiusM): Drawable =
        RippleDrawable(ColorStateList.valueOf(ripple), content, rounded(Color.WHITE, radius))

    /** Selected/unselected tint list for icons and text. */
    fun selectable(selected: Int = accent, normal: Int = text): ColorStateList = ColorStateList(
        arrayOf(intArrayOf(android.R.attr.state_selected), intArrayOf(-android.R.attr.state_enabled), intArrayOf()),
        intArrayOf(selected, textTertiary, normal),
    )

    fun applyCardOutline(v: View, radius: Float = radiusM, fill: Int = surface) {
        v.background = pressable(rounded(fill, radius, outline), radius)
    }
}
