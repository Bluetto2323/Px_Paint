package com.paintengine.pxpaint.core

import kotlin.math.PI
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sin

/**
 * The strokes drawn by brush previews, in the engine's preview format
 * (NativeEngine.renderBrushPreview): [n, then per stroke: count, diameter,
 * opacity, count x (x, y, t, pressure, tilt, orientation)], preview pixels.
 * The engine renders them with the real brush code, so a preview shows the
 * actual texture, edge, spacing and accumulation of the brush at 1:1 canvas
 * scale. Pure Kotlin (unit-tested).
 */
object BrushPreviewSpec {
    /** A text label the view draws over a detail preview (top-left corner, px). */
    data class Label(val text: S, val x: Float, val y: Float)

    class Builder {
        private val data = ArrayList<Float>()
        private var strokes = 0

        fun stroke(diameter: Float, opacity: Float, points: List<FloatArray>) {
            strokes++
            data += points.size.toFloat()
            data += diameter
            data += opacity
            for (p in points) for (v in p) data += v
        }

        fun build(): FloatArray = FloatArray(data.size + 1).also {
            it[0] = strokes.toFloat()
            for (i in data.indices) it[i + 1] = data[i]
        }
    }

    private const val NAN = Float.NaN

    private fun pt(x: Double, y: Double, t: Int, pressure: Float = NAN) =
        floatArrayOf(x.toFloat(), y.toFloat(), t * 8f, pressure, NAN, NAN)

    /** Diameter that fits a band of [band] px (texture stays at canvas scale). */
    fun fitDiameter(size: Float, band: Float): Float = min(size, band).coerceAtLeast(min(2f, band))

    /** One S-curve across the thumbnail. */
    fun thumbnail(p: BrushParams, w: Int, h: Int): FloatArray {
        val b = Builder()
        val d = fitDiameter(p.size, h * 0.5f)
        b.stroke(d, -1f, sCurve(d / 2.0 + h * 0.15, w - d / 2.0 - h * 0.15, h / 2.0, max(0.0, (h - d) / 2.0 * 0.75), 48))
        return b.build()
    }

    private fun sCurve(x0: Double, x1: Double, cy: Double, amp: Double, n: Int, pressure: ((Double) -> Float)? = null) =
        (0 until n).map { i ->
            val t = i / (n - 1.0)
            pt(x0 + (x1 - x0) * t, cy - amp * sin(t * 2 * PI), i, pressure?.invoke(t) ?: NAN)
        }

    private fun line(x0: Double, x1: Double, y: Double, n: Int, pressure: ((Double) -> Float)? = null) =
        (0 until n).map { i ->
            val t = i / (n - 1.0)
            pt(x0 + (x1 - x0) * t, y, i, pressure?.invoke(t) ?: NAN)
        }

    /**
     * Brush editor preview, four rows:
     *  1. an S-curve (finger: no pressure);
     *  2. a stroke with a stylus pressure ramp 0 -> 1 -> 0 (the brush's
     *     pressure response, drawn only if it has one: never shown as input);
     *  3. three sizes (40 %, 100 %, 180 % of the fitted size);
     *  4. three opacities (25 %, 50 %, 100 %).
     */
    fun detail(p: BrushParams, w: Int, h: Int): Pair<FloatArray, List<Label>> {
        val b = Builder()
        val labels = ArrayList<Label>()
        val pad = h * 0.04
        val usesPressure = p.pressureSize || p.pressureOpacity || p.grainPressure > 0f
        val rows = if (usesPressure) doubleArrayOf(0.34, 0.22, 0.22, 0.22) else doubleArrayOf(0.40, 0.0, 0.30, 0.30)
        var top = 0.0
        fun band(i: Int): Pair<Double, Double> = (top to h * rows[i]).also { top += h * rows[i] }

        val (t1, h1) = band(0)
        val d1 = fitDiameter(p.size, (h1 * 0.55).toFloat())
        labels += Label(S.PREVIEW_STROKE, pad.toFloat(), (t1 + pad * 0.5).toFloat())
        b.stroke(d1, -1f, sCurve(w * 0.08 + d1 / 2, w * 0.92 - d1 / 2, t1 + h1 / 2 + pad * 0.3, max(0.0, (h1 - d1) / 2 * 0.6), 64))

        if (usesPressure) {
            val (t2, h2) = band(1)
            val d2 = fitDiameter(p.size, (h2 * 0.62).toFloat())
            labels += Label(S.PREVIEW_PRESSURE, pad.toFloat(), (t2 + pad * 0.3).toFloat())
            b.stroke(d2, -1f, line(w * 0.08 + d2 / 2, w * 0.92 - d2 / 2, t2 + h2 * 0.58, 64) { t -> sin(t * PI).toFloat() })
        } else {
            band(1)
        }

        val (t3, h3) = band(2)
        labels += Label(S.PREVIEW_SIZES, pad.toFloat(), (t3 + pad * 0.3).toFloat())
        val d3 = fitDiameter(p.size, (h3 * 0.42).toFloat())
        val seg = w * 0.84 / 3
        for ((k, f) in listOf(0.4f, 1f, 1.8f).withIndex()) {
            val d = max(1f, d3 * f)
            val x0 = w * 0.08 + seg * k + d / 2 + seg * 0.08
            val x1 = w * 0.08 + seg * (k + 1) - d / 2 - seg * 0.08
            b.stroke(d, -1f, sCurve(x0, max(x0 + 1, x1), t3 + h3 * 0.6, h3 * 0.08, 24))
        }

        val (t4, h4) = band(3)
        labels += Label(S.PREVIEW_OPACITY, pad.toFloat(), (t4 + pad * 0.3).toFloat())
        val d4 = fitDiameter(p.size, (h4 * 0.5).toFloat())
        for ((k, o) in listOf(0.25f, 0.5f, 1f).withIndex()) {
            val x0 = w * 0.08 + seg * k + d4 / 2 + seg * 0.08
            val x1 = w * 0.08 + seg * (k + 1) - d4 / 2 - seg * 0.08
            b.stroke(d4, o, line(x0, max(x0 + 1, x1), t4 + h4 * 0.6, 24))
        }
        return b.build() to labels
    }

    /** Stable cache key of what a preview depends on. */
    fun key(p: BrushParams, kind: String, w: Int, h: Int, ink: Int, paper: Int, curveId: Int): String =
        buildString {
            append(kind).append(w).append('x').append(h).append(':').append(ink).append(':').append(paper).append(':').append(curveId)
            for ((k, v) in p.entries()) append(';').append(k).append('=').append(v)
        }
}
