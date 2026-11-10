package com.paintengine.pxpaint.core

/**
 * Per-stroke colour dynamics (Block 15): the stroke's colour is shifted in
 * hue (up to +-60 degrees at 100 %) and lightness (up to +-35 %) by a value
 * derived from [seed], so every stroke of a "leaves" or "confetti" brush is a
 * little different while one stroke stays one colour. Alpha is kept.
 */
object ColorJitter {
    fun apply(argb: Int, hueJitter: Float, lightJitter: Float, seed: Int): Int {
        if (hueJitter <= 0f && lightJitter <= 0f) return argb
        val r = (argb shr 16 and 0xFF) / 255f
        val g = (argb shr 8 and 0xFF) / 255f
        val b = (argb and 0xFF) / 255f
        val mx = maxOf(r, g, b)
        val mn = minOf(r, g, b)
        val d = mx - mn
        var h = when {
            d < 1e-6f -> 0f
            mx == r -> ((g - b) / d).mod(6f)
            mx == g -> (b - r) / d + 2f
            else -> (r - g) / d + 4f
        } * 60f
        val s = if (mx <= 0f) 0f else d / mx
        var v = mx
        h = (h + 60f * hueJitter.coerceIn(0f, 1f) * signed(seed, 1)).mod(360f)
        v = (v * (1f + 0.35f * lightJitter.coerceIn(0f, 1f) * signed(seed, 2))).coerceIn(0f, 1f)
        val c = v * s
        val x = c * (1f - kotlin.math.abs((h / 60f).mod(2f) - 1f))
        val m = v - c
        val (r1, g1, b1) = when ((h / 60f).toInt().coerceIn(0, 5)) {
            0 -> Triple(c, x, 0f)
            1 -> Triple(x, c, 0f)
            2 -> Triple(0f, c, x)
            3 -> Triple(0f, x, c)
            4 -> Triple(x, 0f, c)
            else -> Triple(c, 0f, x)
        }
        fun ch(f: Float) = ((f + m) * 255f + 0.5f).toInt().coerceIn(0, 255)
        return (argb and 0xFF000000.toInt()) or (ch(r1) shl 16) or (ch(g1) shl 8) or ch(b1)
    }

    /** Deterministic value in -1..1 from a seed and a channel. */
    private fun signed(seed: Int, channel: Int): Float {
        var x = seed * 0x9E3779B1.toInt() + channel * 0x85EBCA6B.toInt()
        x = x xor (x ushr 15); x *= 0x2C1B3C6D; x = x xor (x ushr 12); x *= 0x297A2D39; x = x xor (x ushr 15)
        return (x and 0xFFFF) / 32767.5f - 1f
    }
}
