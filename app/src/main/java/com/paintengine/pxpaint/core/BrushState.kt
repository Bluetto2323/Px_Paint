package com.paintengine.pxpaint.core

import kotlin.math.exp
import kotlin.math.ln
import kotlin.math.roundToInt

/**
 * Size / opacity / spacing of one tool, plus its stylus dynamics. [size] is
 * the diameter in CANVAS pixels - the maximum size when pressure controls
 * size; [minSize] is the size at zero pressure as a fraction of it.
 * Dynamics only act on stylus strokes (fingers have no reliable pressure).
 */
data class ToolBrush(
    val size: Float,
    val opacity: Float,
    val spacing: Float,
    val pressureSize: Boolean = true,
    val minSize: Float = 0.25f,
    val pressureOpacity: Boolean = false,
    val minOpacity: Float = 0.3f,
    val tilt: Boolean = false,
) {
    fun sanitized() = copy(
        size = size.takeIf { it.isFinite() }?.coerceIn(BrushState.MIN_SIZE, BrushState.MAX_SIZE) ?: 24f,
        opacity = opacity.takeIf { it.isFinite() }?.coerceIn(BrushState.MIN_OPACITY, 1f) ?: 1f,
        spacing = spacing.takeIf { it.isFinite() }?.coerceIn(BrushState.MIN_SPACING, BrushState.MAX_SPACING) ?: BrushState.DEFAULT_SPACING,
        minSize = minSize.takeIf { it.isFinite() }?.coerceIn(BrushState.MIN_PRESSURE_FRACTION, 1f) ?: 0.25f,
        minOpacity = minOpacity.takeIf { it.isFinite() }?.coerceIn(BrushState.MIN_PRESSURE_FRACTION, 1f) ?: 0.3f,
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
    /** The user's saved colours (Block 8), in the order they were added. */
    val palette: List<Int> = emptyList(),
) {
    fun encode(): Map<String, String> = mapOf(
        "marker" to enc(marker),
        "eraser" to enc(eraser),
        "color" to color.toString(),
        "recent" to recentColors.take(MAX_RECENT).joinToString(","),
        "palette" to palette.take(MAX_PALETTE).joinToString(","),
    )

    /** Adds [c] (opaque) to the saved palette; already there or full: unchanged. */
    fun withSaved(c: Int): BrushState {
        val o = c or 0xFF000000.toInt()
        return if (o in palette || palette.size >= MAX_PALETTE) this else copy(palette = palette + o)
    }

    fun withoutSaved(c: Int): BrushState = copy(palette = palette - c)

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
        const val MAX_PALETTE = 30
        /** Lowest minimum size / opacity the UI offers (the engine floors are lower). */
        const val MIN_PRESSURE_FRACTION = 0.05f

        fun decode(map: Map<String, String>): BrushState {
            val d = BrushState()
            return BrushState(
                marker = dec(map["marker"]) ?: d.marker,
                eraser = dec(map["eraser"]) ?: d.eraser,
                color = map["color"]?.toIntOrNull() ?: d.color,
                recentColors = map["recent"]?.split(',')?.mapNotNull { it.trim().toIntOrNull() }?.take(MAX_RECENT) ?: emptyList(),
                palette = map["palette"]?.split(',')?.mapNotNull { it.trim().toIntOrNull() }?.distinct()?.take(MAX_PALETTE) ?: emptyList(),
            )
        }

        private fun enc(t: ToolBrush) =
            "${t.size};${t.opacity};${t.spacing};${t.pressureSize};${t.minSize};${t.pressureOpacity};${t.minOpacity};${t.tilt}"

        private fun dec(v: String?): ToolBrush? {
            val p = v?.split(';') ?: return null
            if (p.size != 3 && p.size != 8) return null   // 3 fields: Block 3
            val nums = p.take(3).map { it.toFloatOrNull() ?: return null }
            val base = ToolBrush(nums[0], nums[1], nums[2])
            if (p.size == 3) return base.sanitized()
            fun bool(s: String) = when (s) { "true" -> true; "false" -> false; else -> null }
            return base.copy(
                pressureSize = bool(p[3]) ?: base.pressureSize,
                minSize = p[4].toFloatOrNull() ?: base.minSize,
                pressureOpacity = bool(p[5]) ?: base.pressureOpacity,
                minOpacity = p[6].toFloatOrNull() ?: base.minOpacity,
                tilt = bool(p[7]) ?: base.tilt,
            ).sanitized()
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
