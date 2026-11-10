package com.paintengine.pxpaint.core

import kotlin.math.pow

/** What touches the screen, from MotionEvent.getToolType (per pointer). */
enum class ToolKind {
    FINGER, STYLUS, ERASER, MOUSE, UNKNOWN;

    /** Only an active stylus (tip or eraser end) gives pressure / tilt worth using. */
    val isStylus: Boolean get() = this == STYLUS || this == ERASER

    companion object {
        // MotionEvent.TOOL_TYPE_* values (stable Android API constants).
        const val TOOL_TYPE_UNKNOWN = 0
        const val TOOL_TYPE_FINGER = 1
        const val TOOL_TYPE_STYLUS = 2
        const val TOOL_TYPE_MOUSE = 3
        const val TOOL_TYPE_ERASER = 4
        const val TOOL_TYPE_PALM = 5   // API 34

        fun fromToolType(toolType: Int): ToolKind = when (toolType) {
            TOOL_TYPE_FINGER -> FINGER
            TOOL_TYPE_STYLUS -> STYLUS
            TOOL_TYPE_ERASER -> ERASER
            TOOL_TYPE_MOUSE -> MOUSE
            TOOL_TYPE_PALM -> FINGER  // a palm is never a stylus; the router treats it as a finger
            else -> UNKNOWN
        }
    }
}

/**
 * Axes a device reports for a pointer (InputDevice.getMotionRange != null).
 * Nothing is assumed: an axis that is not reported is never read.
 */
data class AxisSupport(
    val pressure: Boolean = false,
    val tilt: Boolean = false,
    val orientation: Boolean = false,
    val distance: Boolean = false,
    /** Range of AXIS_PRESSURE as reported (MotionEvent values lie in it). */
    val pressureMin: Float = 0f,
    val pressureMax: Float = 1f,
) {
    companion object {
        val NONE = AxisSupport()
    }
}

object PressureNormalizer {
    /**
     * Maps a raw MotionEvent pressure to 0..1 with the device's reported
     * range; clamps out-of-range values (some drivers overshoot). A missing
     * or degenerate range falls back to the nominal 0..1. NaN stays NaN.
     */
    fun normalize(raw: Float, min: Float, max: Float): Float {
        if (raw.isNaN()) return Float.NaN
        val lo = if (min.isFinite()) min else 0f
        val hi = if (max.isFinite()) max else 1f
        val v = if (hi > lo + 1e-6f) (raw - lo) / (hi - lo) else raw
        return v.coerceIn(0f, 1f)
    }
}

/**
 * Pressure response: sensitivity (a gamma on the input) followed by a curve
 * through (0,0), (x1,y1), (x2,y2), (1,1). The curve is a monotone cubic
 * (Fritsch-Carlson), so it never goes backwards and never overshoots: more
 * pressure always gives at least as much size/opacity. 0 maps to 0 and 1 to 1;
 * the brush's minimum size / opacity keep strokes visible at zero.
 */
data class PressureCurve(val x1: Float, val y1: Float, val x2: Float, val y2: Float) {
    fun sanitized(): PressureCurve {
        val ax = x1.finiteOr(1f / 3).coerceIn(MIN_GAP, 1f - 2 * MIN_GAP)
        val bx = x2.finiteOr(2f / 3).coerceIn(ax + MIN_GAP, 1f - MIN_GAP)
        val ay = y1.finiteOr(1f / 3).coerceIn(0f, 1f)
        val by = y2.finiteOr(2f / 3).coerceIn(ay, 1f)
        return PressureCurve(ax, ay, bx, by)
    }

    /** Curve value at x in 0..1. */
    fun eval(x: Float): Float {
        val c = sanitized()
        val xs = floatArrayOf(0f, c.x1, c.x2, 1f)
        val ys = floatArrayOf(0f, c.y1, c.y2, 1f)
        val m = tangents(xs, ys)
        val t = x.coerceIn(0f, 1f)
        val i = when {
            t <= xs[1] -> 0
            t <= xs[2] -> 1
            else -> 2
        }
        val h = xs[i + 1] - xs[i]
        val s = (t - xs[i]) / h
        val s2 = s * s
        val s3 = s2 * s
        val v = (2 * s3 - 3 * s2 + 1) * ys[i] + (s3 - 2 * s2 + s) * h * m[i] +
            (-2 * s3 + 3 * s2) * ys[i + 1] + (s3 - s2) * h * m[i + 1]
        return v.coerceIn(0f, 1f)
    }

    /**
     * The engine's response table (NativeEngine.PRESSURE_CURVE_SIZE values):
     * table[i] = curve((i / (n-1)) ^ gamma(sensitivity)), monotonic, 0 .. 1.
     */
    fun table(sensitivity: Float, size: Int = TABLE_SIZE): FloatArray {
        val g = gamma(sensitivity)
        val out = FloatArray(size) { i -> eval((i.toFloat() / (size - 1)).pow(g)) }
        out[0] = 0f
        out[size - 1] = 1f
        for (i in 1 until size) if (out[i] < out[i - 1]) out[i] = out[i - 1]  // float noise only
        return out
    }

    /** Full response (sensitivity + curve) for one value; what the table samples. */
    fun response(p: Float, sensitivity: Float): Float = eval(p.coerceIn(0f, 1f).pow(gamma(sensitivity)))

    fun encode(): String = "$x1;$y1;$x2;$y2"

    companion object {
        const val TABLE_SIZE = 257
        private const val MIN_GAP = 0.05f
        val LINEAR = PressureCurve(1f / 3, 1f / 3, 2f / 3, 2f / 3)
        /** Light touch already gives a lot (soft pencil). */
        val SOFT = PressureCurve(0.25f, 0.5f, 0.6f, 0.85f)
        /** Needs a firmer press (ink pen). */
        val FIRM = PressureCurve(0.4f, 0.15f, 0.75f, 0.5f)
        /** Gentle start and end, steep middle. */
        val S_CURVE = PressureCurve(0.35f, 0.12f, 0.65f, 0.88f)

        /**
         * Sensitivity -1..1 -> input gamma: +1 = light touch counts more (0.25), -1 = needs force (1.98).
         * The neutral setting is 0.7, not 1: active pens are rarely pressed past ~60 % of their
         * range while drawing, and a linear response left normal strokes thin and faint.
         */
        fun gamma(sensitivity: Float): Float = NEUTRAL_GAMMA * 2f.pow(-1.5f * sensitivity.finiteOr(0f).coerceIn(-1f, 1f))
        const val NEUTRAL_GAMMA = 0.7f

        fun decode(v: String?): PressureCurve? {
            val p = v?.split(';') ?: return null
            if (p.size != 4) return null
            val n = p.map { it.toFloatOrNull() ?: return null }
            return PressureCurve(n[0], n[1], n[2], n[3]).sanitized()
        }

        private fun Float.finiteOr(d: Float) = if (isFinite()) this else d

        // Fritsch-Carlson monotone tangents.
        private fun tangents(x: FloatArray, y: FloatArray): FloatArray {
            val n = x.size
            val d = FloatArray(n - 1) { (y[it + 1] - y[it]) / (x[it + 1] - x[it]) }
            val m = FloatArray(n)
            m[0] = d[0]
            m[n - 1] = d[n - 2]
            for (i in 1 until n - 1) m[i] = if (d[i - 1] * d[i] <= 0f) 0f else (d[i - 1] + d[i]) / 2
            for (i in 0 until n - 1) {
                if (d[i] == 0f) {
                    m[i] = 0f
                    m[i + 1] = 0f
                } else {
                    val a = m[i] / d[i]
                    val b = m[i + 1] / d[i]
                    val s = a * a + b * b
                    if (s > 9f) {
                        val t = 3f / kotlin.math.sqrt(s)
                        m[i] = t * a * d[i]
                        m[i + 1] = t * b * d[i]
                    }
                }
            }
            return m
        }
    }
}
