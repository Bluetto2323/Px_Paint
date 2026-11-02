package com.paintengine.pxpaint.core

import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.hypot
import kotlin.math.roundToLong

/** A point in view pixels. */
data class P(val x: Double, val y: Double)

/**
 * Evenly spaced dabs along a drag: the distance left over from one segment
 * carries into the next, so the spacing is constant whatever the touch
 * event rate.
 */
class StrokeSampler(spacing: Double) {
    var spacing: Double = spacing.coerceAtLeast(0.5)
        set(value) {
            field = value.coerceAtLeast(0.5)
        }
    private var last: P? = null
    private var carry = 0.0

    fun begin(p: P): List<P> {
        last = p
        carry = 0.0
        return listOf(p)
    }

    fun moveTo(p: P): List<P> {
        val from = last ?: return begin(p)
        val dx = p.x - from.x
        val dy = p.y - from.y
        val len = hypot(dx, dy)
        if (len == 0.0) return emptyList()
        val out = ArrayList<P>()
        var d = spacing - carry
        while (d <= len) {
            val t = d / len
            out += P(from.x + dx * t, from.y + dy * t)
            d += spacing
        }
        carry = len - (d - spacing)
        last = p
        return out
    }

    fun end() {
        last = null
        carry = 0.0
    }
}

/** One step of a two-finger gesture, as NativeEngine.ViewOp.GESTURE expects it. */
data class GestureStep(val prevFocal: P, val newFocal: P, val scale: Double, val rotation: Double)

object TwoFinger {
    /** Pinch distance below which scale/rotation are ignored (fingers almost touching). */
    const val MIN_SPAN_PX = 24.0

    fun step(prevA: P, prevB: P, newA: P, newB: P, allowRotation: Boolean): GestureStep {
        val pf = P((prevA.x + prevB.x) / 2, (prevA.y + prevB.y) / 2)
        val nf = P((newA.x + newB.x) / 2, (newA.y + newB.y) / 2)
        val d0 = hypot(prevB.x - prevA.x, prevB.y - prevA.y)
        val d1 = hypot(newB.x - newA.x, newB.y - newA.y)
        if (d0 < MIN_SPAN_PX || d1 < MIN_SPAN_PX) return GestureStep(pf, nf, 1.0, 0.0)
        val scale = d1 / d0
        val rot = if (allowRotation) {
            normalize(atan2(newB.y - newA.y, newB.x - newA.x) - atan2(prevB.y - prevA.y, prevB.x - prevA.x))
        } else 0.0
        return GestureStep(pf, nf, scale, rot)
    }

    fun normalize(a: Double): Double {
        var r = a % (2 * PI)
        if (r <= -PI) r += 2 * PI
        if (r > PI) r -= 2 * PI
        return r
    }
}

/** Snaps the canvas rotation to the nearest right angle when close enough. */
object RotationSnap {
    val THRESHOLD_RAD = Math.toRadians(5.0)

    /** Target angle to snap to, or null if [rotation] is not close to a multiple of 90°. */
    fun target(rotation: Double): Double? {
        val quarter = PI / 2
        val nearest = (rotation / quarter).roundToLong() * quarter
        return if (abs(rotation - nearest) <= THRESHOLD_RAD) TwoFinger.normalize(nearest) else null
    }
}

/**
 * Decides when a one-finger touch becomes a stroke. Nothing is drawn while
 * PENDING: if a second finger lands in that window the touch was the start
 * of a pan/zoom, and no stray dab must appear (there is no undo yet).
 * A touch becomes a stroke when it moves past [slopPx], when it lasts
 * longer than [delayMs], or when it ends as a tap.
 */
class DrawGate(private val slopPx: Double = 10.0, private val delayMs: Long = 90) {
    enum class State { IDLE, PENDING, DRAWING, CANCELLED }

    var state = State.IDLE
        private set
    private var downX = 0.0
    private var downY = 0.0
    private var downTime = 0L
    private val buffered = ArrayList<P>()

    fun down(timeMs: Long, p: P) {
        state = State.PENDING
        downX = p.x
        downY = p.y
        downTime = timeMs
        buffered.clear()
        buffered += p
    }

    /** Returns the points to draw now (buffered ones when the gate opens). */
    fun move(timeMs: Long, p: P): List<P> = when (state) {
        State.PENDING -> {
            buffered += p
            if (hypot(p.x - downX, p.y - downY) > slopPx || timeMs - downTime >= delayMs) open() else emptyList()
        }
        State.DRAWING -> listOf(p)
        else -> emptyList()
    }

    /** Re-check on a timer tick without a move (a still finger becomes a stroke). */
    fun tick(timeMs: Long): List<P> =
        if (state == State.PENDING && timeMs - downTime >= delayMs) open() else emptyList()

    /** Finger lifted: a pending touch is a tap (draws its dab). */
    fun up(): List<P> {
        val out = if (state == State.PENDING) open() else emptyList()
        state = State.IDLE
        return out
    }

    /** Another finger arrived: a pending touch is discarded; a stroke simply stops. */
    fun secondPointer() {
        state = if (state == State.PENDING || state == State.DRAWING) State.CANCELLED else state
        buffered.clear()
    }

    fun reset() {
        state = State.IDLE
        buffered.clear()
    }

    private fun open(): List<P> {
        state = State.DRAWING
        val out = ArrayList(buffered)
        buffered.clear()
        return out
    }
}
