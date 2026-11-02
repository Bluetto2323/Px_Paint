package com.paintengine.app

import android.os.SystemClock
import android.util.Log
import java.util.Locale

/**
 * App-side timeline (Activity lifecycle, surface callbacks, rotations,
 * self-test outcomes, backend switches). It goes into the on-device report
 * together with the native log, so lifecycle tests can be verified from the
 * phone alone, without adb/logcat.
 */
object EventLog {
    private const val MAX_LINES = 400
    private val start = SystemClock.elapsedRealtime()
    private val lines = ArrayDeque<String>()

    @Synchronized
    fun add(message: String) {
        val t = (SystemClock.elapsedRealtime() - start) / 1000.0
        lines.addLast(String.format(Locale.US, "+%.3fs %s", t, message))
        while (lines.size > MAX_LINES) lines.removeFirst()
        Log.i("PE.Event", message)
    }

    @Synchronized
    fun dump(): String = lines.joinToString("\n")
}
