package com.paintengine.pxpaint.core

import kotlin.math.hypot

/**
 * Block 12.5: a finger held still on the canvas becomes the eyedropper
 * (pure, unit-tested). The stroke starts at once as always; if the finger
 * has not moved more than [slopPx] after [HOLD_MS], the workspace cancels
 * that stroke (the canvas is restored exactly) and picks colours until the
 * finger lifts. A second finger, a stylus or any real movement disarms it.
 */
class LongPressPick(private val slopPx: Float) {
    var armed = false
        private set
    private var downX = 0f
    private var downY = 0f
    private var downTime = 0L

    fun down(timeMs: Long, x: Float, y: Float) {
        armed = true
        downTime = timeMs
        downX = x
        downY = y
    }

    fun move(x: Float, y: Float) {
        if (armed && hypot(x - downX, y - downY) > slopPx) armed = false
    }

    fun cancel() { armed = false }

    /** True once (then disarmed) when the hold is long enough at [nowMs]. */
    fun fire(nowMs: Long): Boolean {
        if (!armed || nowMs - downTime < HOLD_MS) return false
        armed = false
        return true
    }

    companion object {
        /** Long enough not to catch a dot or the start of a slow stroke. */
        const val HOLD_MS = 450L
    }
}
