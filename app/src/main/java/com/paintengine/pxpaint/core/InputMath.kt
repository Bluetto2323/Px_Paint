package com.paintengine.pxpaint.core

import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.hypot
import kotlin.math.roundToLong

/** A point in view pixels. */
data class P(val x: Double, val y: Double)

/**
 * Reusable buffer of stroke samples handed to the engine once per touch
 * event: all historical samples of the MotionEvent in chronological order,
 * then the current one. [STRIDE] floats per sample: x, y, tMs, pressure,
 * tilt, orientation (NaN = axis not reported). It grows by doubling and is
 * reused, so the per-event path does not allocate once warmed up.
 */
class SampleBuffer(initialCapacity: Int = 64) {
    var data = FloatArray(maxOf(1, initialCapacity) * STRIDE)
        private set
    var count = 0
        private set

    fun clear() {
        count = 0
    }

    fun add(x: Float, y: Float, tMs: Float, pressure: Float = Float.NaN, tilt: Float = Float.NaN, orientation: Float = Float.NaN) {
        if ((count + 1) * STRIDE > data.size) data = data.copyOf(data.size * 2)
        val i = count * STRIDE
        data[i] = x
        data[i + 1] = y
        data[i + 2] = tMs
        data[i + 3] = pressure
        data[i + 4] = tilt
        data[i + 5] = orientation
        count++
    }

    /** The first sample as its own array (stroke begin). */
    fun first(): FloatArray = data.copyOfRange(0, STRIDE)

    companion object {
        const val STRIDE = 6   // NativeEngine.SAMPLE_FLOATS
    }
}

/**
 * Painting starts the moment a finger lands (no input delay). If a second
 * finger joins shortly after, the touch was the start of a pinch, not a
 * stroke: [onSecondPointer] then says CANCEL (the engine restores the canvas
 * exactly). A stroke that has clearly been drawn is kept (COMMIT) and the
 * gesture takes over from there.
 */
class StrokeGate(private val cancelWindowMs: Long = 250, private val slopPx: Double = 24.0) {
    enum class Decision { CANCEL, COMMIT }

    private var downTime = 0L
    private var startX = 0.0
    private var startY = 0.0
    private var travelled = 0.0
    var active = false
        private set

    fun down(timeMs: Long, p: P) {
        active = true
        downTime = timeMs
        startX = p.x
        startY = p.y
        travelled = 0.0
    }

    fun move(p: P) {
        if (active) travelled = maxOf(travelled, hypot(p.x - startX, p.y - startY))
    }

    fun onSecondPointer(timeMs: Long): Decision {
        val decision = if (timeMs - downTime <= cancelWindowMs || travelled <= slopPx) Decision.CANCEL else Decision.COMMIT
        active = false
        return decision
    }

    fun end() {
        active = false
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
