package com.paintengine.pxpaint.core

import java.util.Locale
import kotlin.math.PI
import kotlin.math.roundToInt
import kotlin.math.roundToLong

/** Display formatting (pure, unit-tested). */
object Format {
    fun dimensions(w: Int, h: Int): String = "$w × $h px"

    fun bytes(n: Long): String {
        val mib = n / (1024.0 * 1024.0)
        return when {
            n < 1024 -> "$n B"
            mib < 1.0 -> String.format(Locale.US, "%.0f KB", n / 1024.0)
            mib < 10.0 -> String.format(Locale.US, "%.1f MB", mib)
            mib < 1024.0 -> String.format(Locale.US, "%.0f MB", mib)
            else -> String.format(Locale.US, "%.1f GB", mib / 1024.0)
        }
    }

    fun zoomPercent(zoom: Double): String {
        val pct = zoom * 100.0
        return if (pct < 10.0) String.format(Locale.US, "%.1f%%", pct) else "${pct.roundToLong()}%"
    }

    /** -180..180, rounded; "0°" for tiny angles. */
    fun degrees(radians: Double): String {
        val deg = (TwoFinger.normalize(radians) * 180.0 / PI).roundToInt()
        return "${if (deg == -180) 180 else deg}°"
    }

    enum class Ago { JUST_NOW, MINUTES, HOURS, YESTERDAY, DAYS, DATE }

    /** Relative age bucket and its number (minutes, hours or days). */
    fun ago(nowMs: Long, thenMs: Long): Pair<Ago, Long> {
        val s = ((nowMs - thenMs) / 1000).coerceAtLeast(0)
        return when {
            s < 60 -> Ago.JUST_NOW to 0
            s < 3600 -> Ago.MINUTES to s / 60
            s < 86_400 -> Ago.HOURS to s / 3600
            s < 2 * 86_400 -> Ago.YESTERDAY to 1
            s < 7 * 86_400 -> Ago.DAYS to s / 86_400
            else -> Ago.DATE to 0
        }
    }
}
