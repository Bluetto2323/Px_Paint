package com.paintengine.pxpaint.core

import kotlin.math.abs
import kotlin.math.floor
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

/**
 * Colour conversions of the colour workspace (pure, unit-tested). Colours
 * are straight (non-premultiplied) sRGB ARGB ints, exactly what the engine's
 * brush receives, so what the picker shows is what is painted.
 */
object ColorMath {
    /** h in [0, 360), s and v in [0, 1] -> opaque ARGB. */
    fun hsvToArgb(h: Float, s: Float, v: Float): Int {
        val hh = ((h % 360f) + 360f) % 360f / 60f
        val ss = s.coerceIn(0f, 1f)
        val vv = v.coerceIn(0f, 1f)
        val i = floor(hh).toInt()
        val f = hh - i
        val p = vv * (1 - ss)
        val q = vv * (1 - ss * f)
        val t = vv * (1 - ss * (1 - f))
        val (r, g, b) = when (i) {
            0 -> Triple(vv, t, p)
            1 -> Triple(q, vv, p)
            2 -> Triple(p, vv, t)
            3 -> Triple(p, q, vv)
            4 -> Triple(t, p, vv)
            else -> Triple(vv, p, q)
        }
        return rgb((r * 255).roundToInt(), (g * 255).roundToInt(), (b * 255).roundToInt())
    }

    /** ARGB -> [h 0..360, s 0..1, v 0..1] (alpha ignored). Grey keeps h = 0. */
    fun argbToHsv(c: Int): FloatArray {
        val r = red(c) / 255f
        val g = green(c) / 255f
        val b = blue(c) / 255f
        val mx = max(r, max(g, b))
        val mn = min(r, min(g, b))
        val d = mx - mn
        val h = when {
            d == 0f -> 0f
            mx == r -> 60f * (((g - b) / d) % 6f)
            mx == g -> 60f * ((b - r) / d + 2f)
            else -> 60f * ((r - g) / d + 4f)
        }.let { if (it < 0) it + 360f else it }
        return floatArrayOf(h, if (mx == 0f) 0f else d / mx, mx)
    }

    fun rgb(r: Int, g: Int, b: Int): Int =
        (0xFF shl 24) or (r.coerceIn(0, 255) shl 16) or (g.coerceIn(0, 255) shl 8) or b.coerceIn(0, 255)

    fun red(c: Int) = (c shr 16) and 0xFF
    fun green(c: Int) = (c shr 8) and 0xFF
    fun blue(c: Int) = c and 0xFF

    /** "#RRGGBB" (upper case). */
    fun toHex(c: Int): String = String.format("#%06X", c and 0xFFFFFF)

    /**
     * Parses "RRGGBB", "#RRGGBB", "RGB" or "#RGB" (case and spaces ignored)
     * into an opaque colour; null if it is not one of those.
     */
    fun parseHex(text: String): Int? {
        var t = text.trim().removePrefix("#").trim()
        if (t.length == 3) t = t.map { "$it$it" }.joinToString("")
        if (t.length != 6 || t.any { it.digitToIntOrNull(16) == null }) return null
        return (0xFF shl 24) or t.toInt(16)
    }

    /** Straight ARGB from a premultiplied RGBA8 pixel (engine composite), opaque. */
    fun unpremultiply(r: Int, g: Int, b: Int, a: Int): Int {
        if (a <= 0) return 0
        if (a >= 255) return rgb(r, g, b)
        fun u(v: Int) = ((v * 255 + a / 2) / a).coerceIn(0, 255)
        return rgb(u(r), u(g), u(b))
    }

    /** WCAG 2.x relative luminance of an sRGB colour. */
    fun luminance(c: Int): Double {
        fun ch(v: Int): Double {
            val s = v / 255.0
            return if (s <= 0.04045) s / 12.92 else Math.pow((s + 0.055) / 1.055, 2.4)
        }
        return 0.2126 * ch(red(c)) + 0.7152 * ch(green(c)) + 0.0722 * ch(blue(c))
    }

    /** WCAG contrast ratio (1..21) of two opaque colours. */
    fun contrast(a: Int, b: Int): Double {
        val la = luminance(a)
        val lb = luminance(b)
        return (max(la, lb) + 0.05) / (min(la, lb) + 0.05)
    }

    /** [over] (ARGB with alpha) composited over the opaque [under]. */
    fun over(over: Int, under: Int): Int {
        val a = ((over ushr 24) and 0xFF) / 255f
        fun mix(f: Int, b: Int) = (f * a + b * (1 - a)).roundToInt()
        return rgb(mix(red(over), red(under)), mix(green(over), green(under)), mix(blue(over), blue(under)))
    }

    /** Hue distance in degrees (0..180). */
    fun hueDistance(a: Float, b: Float): Float {
        val d = abs(a - b) % 360f
        return if (d > 180f) 360f - d else d
    }
}
