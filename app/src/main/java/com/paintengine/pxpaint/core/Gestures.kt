package com.paintengine.pxpaint.core

import kotlin.math.hypot

/** Shortcut recognised from a finger tap or a quick pinch. */
enum class GestureShortcut { UNDO, REDO, FULLSCREEN, FIT }

/**
 * Multi-finger taps on the canvas (pure, unit-tested), fed with the same
 * touch sequence as [TouchRouter]:
 *
 *  * 2 fingers -> undo, 3 -> redo, 4 -> interface on / off (each can be
 *    turned off in the settings);
 *  * every finger lands within [maxSpreadMs] of the first, none moves more
 *    than [slopPx], all are lifted within [maxDurationMs] of the first touch;
 *  * a stylus or mouse in the sequence, or a stroke that was committed
 *    (StrokeGate keeps a stroke that had really started), means it was not a
 *    tap: painting never triggers a shortcut;
 *  * a one-finger tap is a dot, never a shortcut.
 */
class TapGesture(
    private val slopPx: Float,
    private val maxDurationMs: Long = 320,
    private val maxSpreadMs: Long = 160,
) {
    private class Finger(val x: Float, val y: Float)

    private val down = HashMap<Int, Finger>()
    private var count = 0
    private var firstMs = 0L
    private var valid = false
    private var active = false

    var undoEnabled = true
    var redoEnabled = true
    var fullscreenEnabled = true

    fun pointerDown(id: Int, timeMs: Long, x: Float, y: Float, finger: Boolean) {
        if (!active) {
            active = true
            valid = true
            count = 0
            firstMs = timeMs
            down.clear()
        }
        if (!finger || timeMs - firstMs > maxSpreadMs) valid = false
        down[id] = Finger(x, y)
        count++
    }

    fun pointerMove(id: Int, x: Float, y: Float) {
        val f = down[id] ?: return
        if (hypot((x - f.x).toDouble(), (y - f.y).toDouble()) > slopPx) valid = false
    }

    /** A stroke of this sequence was committed (it was drawing, not tapping). */
    fun strokeCommitted() {
        valid = false
    }

    /** Pointer lifted; when it was the last one, the shortcut it formed (if any). */
    fun pointerUp(id: Int, timeMs: Long, canceled: Boolean = false): GestureShortcut? {
        if (!active) return null
        if (canceled) valid = false
        down.remove(id)
        if (down.isNotEmpty()) return null
        active = false
        if (!valid || timeMs - firstMs > maxDurationMs) return null
        return when (count) {
            2 -> GestureShortcut.UNDO.takeIf { undoEnabled }
            3 -> GestureShortcut.REDO.takeIf { redoEnabled }
            4 -> GestureShortcut.FULLSCREEN.takeIf { fullscreenEnabled }
            else -> null
        }
    }

    fun cancel() {
        active = false
        valid = false
        down.clear()
    }
}

/**
 * Quick pinch-in (fingers closing fast) = fit the canvas to the screen
 * (pure, unit-tested). A slow pinch is an ordinary zoom out.
 */
object QuickPinch {
    const val MAX_MS = 350L
    const val MAX_RATIO = 0.6

    fun isFit(startDistance: Double, endDistance: Double, durationMs: Long): Boolean =
        startDistance > 0 && durationMs in 1..MAX_MS && endDistance / startDistance <= MAX_RATIO
}
