package com.paintengine.pxpaint.core

import kotlin.math.exp
import kotlin.math.ln
import kotlin.math.roundToInt

/** Size / opacity / spacing of one tool. Size is the diameter in CANVAS pixels. */
data class ToolBrush(val size: Float, val opacity: Float, val spacing: Float) {
    fun sanitized() = ToolBrush(
        size = size.takeIf { it.isFinite() }?.coerceIn(BrushState.MIN_SIZE, BrushState.MAX_SIZE) ?: 24f,
        opacity = opacity.takeIf { it.isFinite() }?.coerceIn(BrushState.MIN_OPACITY, 1f) ?: 1f,
        spacing = spacing.takeIf { it.isFinite() }?.coerceIn(BrushState.MIN_SPACING, BrushState.MAX_SPACING) ?: BrushState.DEFAULT_SPACING,
    )
}

/**
 * The user's brush setup, remembered across sessions (SharedPreferences via
 * the activity). Pure Kotlin, unit-tested: encoding, clamping, slider maps.
 */
data class BrushState(
    val marker: ToolBrush = ToolBrush(24f, 1f, DEFAULT_SPACING),
    val eraser: ToolBrush = ToolBrush(64f, 1f, DEFAULT_SPACING),
    val color: Int = 0xFF2B3A67.toInt(),
    val recentColors: List<Int> = emptyList(),
) {
    fun encode(): Map<String, String> = mapOf(
        "marker" to enc(marker),
        "eraser" to enc(eraser),
        "color" to color.toString(),
        "recent" to recentColors.take(MAX_RECENT).joinToString(","),
    )

    fun withRecent(c: Int): BrushState =
        copy(recentColors = (listOf(c) + recentColors.filter { it != c }).take(MAX_RECENT))

    companion object {
        const val MIN_SIZE = 1f
        const val MAX_SIZE = 500f
        const val MIN_OPACITY = 0.01f
        const val MIN_SPACING = 0.02f
        const val MAX_SPACING = 1.0f
        /** Continuous stroke (the engine also keeps big brushes' edges straight). */
        const val DEFAULT_SPACING = 0.08f
        const val MAX_RECENT = 8

        fun decode(map: Map<String, String>): BrushState {
            val d = BrushState()
            return BrushState(
                marker = dec(map["marker"]) ?: d.marker,
                eraser = dec(map["eraser"]) ?: d.eraser,
                color = map["color"]?.toIntOrNull() ?: d.color,
                recentColors = map["recent"]?.split(',')?.mapNotNull { it.trim().toIntOrNull() }?.take(MAX_RECENT) ?: emptyList(),
            )
        }

        private fun enc(t: ToolBrush) = "${t.size};${t.opacity};${t.spacing}"
        private fun dec(v: String?): ToolBrush? {
            val p = v?.split(';') ?: return null
            if (p.size != 3) return null
            val nums = p.map { it.toFloatOrNull() ?: return null }
            return ToolBrush(nums[0], nums[1], nums[2]).sanitized()
        }

        // Slider mappings: size is logarithmic (fine control of small brushes).
        fun sizeToSlider(size: Float): Float =
            (ln(size.coerceIn(MIN_SIZE, MAX_SIZE).toDouble()) / ln(MAX_SIZE.toDouble())).toFloat()

        fun sliderToSize(v: Float): Float {
            val raw = exp(v.coerceIn(0f, 1f) * ln(MAX_SIZE.toDouble())).toFloat()
            // Whole pixels above 10 px, quarter pixels below.
            return (if (raw >= 10f) raw.roundToInt().toFloat() else (raw * 4f).roundToInt() / 4f).coerceIn(MIN_SIZE, MAX_SIZE)
        }

        fun spacingToSlider(spacing: Float): Float = ((spacing - MIN_SPACING) / (MAX_SPACING - MIN_SPACING)).coerceIn(0f, 1f)
        fun sliderToSpacing(v: Float): Float =
            ((MIN_SPACING + v.coerceIn(0f, 1f) * (MAX_SPACING - MIN_SPACING)) * 100f).roundToInt() / 100f
        fun sliderToOpacity(v: Float): Float = ((v.coerceIn(0f, 1f) * 100f).roundToInt().coerceAtLeast(1)) / 100f

        fun sizeLabel(size: Float): String =
            if (size >= 10f || size == size.roundToInt().toFloat()) "${size.roundToInt()} px"
            else String.format(java.util.Locale.US, "%.2f px", size).replace(Regex("0+ px$"), " px").replace(". px", " px")
    }
}
