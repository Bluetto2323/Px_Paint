package com.paintengine.pxpaint.core

import kotlin.math.hypot

/**
 * ColorDrop (Procreate's bucket): the colour chip is dragged onto the canvas
 * and dropped to fill. Holding the finger still over the canvas fills at
 * once and turns the rest of the drag into a threshold slider (left = less,
 * right = more), re-filling live. Pure state, no Android: the screen feeds
 * pointer positions and a hold timer and performs the fills.
 */
class ColorDrop(private val slopPx: Float, private val holdSlopPx: Float, private val rangePx: Float) {
    enum class State { IDLE, PRESSED, DRAGGING, THRESHOLD }

    var state = State.IDLE
        private set
    var x = 0f
        private set
    var y = 0f
        private set
    /** Fill threshold 0..1 (the tolerance), live while in [State.THRESHOLD]. */
    var threshold = 0f
        private set
    /** Where the hold must be re-timed from: moves when the finger leaves [holdSlopPx]. */
    var anchorX = 0f
        private set
    var anchorY = 0f
        private set
    private var downX = 0f
    private var downY = 0f
    private var startX = 0f
    private var startThreshold = 0f

    fun down(px: Float, py: Float, currentThreshold: Float) {
        state = State.PRESSED
        x = px; y = py; downX = px; downY = py; anchorX = px; anchorY = py
        threshold = currentThreshold.coerceIn(0f, 1f)
    }

    /** Returns true when the hold timer must be restarted (drag began, or the finger moved on). */
    fun move(px: Float, py: Float): Boolean {
        x = px; y = py
        when (state) {
            State.PRESSED -> if (hypot(px - downX, py - downY) > slopPx) {
                state = State.DRAGGING
                anchorX = px; anchorY = py
                return true
            }
            State.DRAGGING -> if (hypot(px - anchorX, py - anchorY) > holdSlopPx) {
                anchorX = px; anchorY = py
                return true
            }
            State.THRESHOLD -> threshold = (startThreshold + (px - startX) / rangePx).coerceIn(0f, 1f)
            State.IDLE -> Unit
        }
        return false
    }

    /** The finger stayed at the anchor long enough: fill there, then slide for the threshold. */
    fun hold(): Boolean {
        if (state != State.DRAGGING) return false
        state = State.THRESHOLD
        startX = x
        startThreshold = threshold
        return true
    }

    /** What lifting the finger means; the state returns to idle. */
    enum class Release { TAP, DROP, DONE, NONE }

    fun up(): Release {
        val r = when (state) {
            State.PRESSED -> Release.TAP
            State.DRAGGING -> Release.DROP
            State.THRESHOLD -> Release.DONE
            State.IDLE -> Release.NONE
        }
        state = State.IDLE
        return r
    }

    fun cancel() { state = State.IDLE }

    companion object {
        const val HOLD_MS = 450L
    }
}
