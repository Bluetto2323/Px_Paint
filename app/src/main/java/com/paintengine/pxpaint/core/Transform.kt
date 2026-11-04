package com.paintengine.pxpaint.core

import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.hypot
import kotlin.math.roundToInt
import kotlin.math.sin

/** Where a touch lands on the transform box. */
enum class TransformHandle(val hx: Int, val hy: Int) {
    NONE(0, 0), MOVE(0, 0), ROTATE(0, 0),
    N(0, -1), S(0, 1), E(1, 0), W(-1, 0), NE(1, -1), NW(-1, -1), SE(1, 1), SW(-1, 1);

    val corner: Boolean get() = hx != 0 && hy != 0
    val edge: Boolean get() = (hx == 0) != (hy == 0)
}

/**
 * The transform of lifted selection content (Block 12), pure Kotlin
 * (unit-tested). The box is the selection's bounds when it was lifted, in
 * canvas pixels; the transform is
 *
 *     canvas = c + t + R(rotation) · K(skewX, skewY) · S(scaleX, scaleY) · (p − c)
 *
 * with c the box centre. Handles: corners scale (keeping the opposite corner
 * in place; proportional when [proportional]), edges scale one axis (or skew
 * in [skewMode]), the handle above the top edge rotates (15° steps with
 * [snap]), anywhere else moves. Flips negate a scale. A pure translation is
 * rounded to whole pixels so moved pixels stay sharp.
 */
class TransformState(val left: Double, val top: Double, val right: Double, val bottom: Double) {
    var tx = 0.0; var ty = 0.0
    var sx = 1.0; var sy = 1.0
    var rotation = 0.0
    var kx = 0.0; var ky = 0.0
    var proportional = true
    var snap = false
    var skewMode = false

    val cx get() = (left + right) / 2
    val cy get() = (top + bottom) / 2
    val w get() = right - left
    val h get() = bottom - top

    private var handle = TransformHandle.NONE
    private var px0 = 0.0; private var py0 = 0.0
    private var tx0 = 0.0; private var ty0 = 0.0
    private var sx0 = 1.0; private var sy0 = 1.0
    private var rot0 = 0.0; private var kx0 = 0.0; private var ky0 = 0.0

    val isTranslation: Boolean get() = rotation == 0.0 && kx == 0.0 && ky == 0.0 && sx == 1.0 && sy == 1.0
    val isIdentity: Boolean get() = isTranslation && tx.roundToInt() == 0 && ty.roundToInt() == 0

    // L = R · K · S
    private fun l00() = cos(rotation) * sx - sin(rotation) * ky * sx
    private fun l01() = cos(rotation) * kx * sy - sin(rotation) * sy
    private fun l10() = sin(rotation) * sx + cos(rotation) * ky * sx
    private fun l11() = sin(rotation) * kx * sy + cos(rotation) * sy

    /** The engine's matrix (a, b, c, d, tx, ty), floating -> canvas. */
    fun matrix(out: DoubleArray) {
        if (isTranslation) {
            out[0] = 1.0; out[1] = 0.0; out[2] = 0.0; out[3] = 1.0
            out[4] = tx.roundToInt().toDouble(); out[5] = ty.roundToInt().toDouble()
            return
        }
        val a = l00(); val c = l01(); val b = l10(); val d = l11()
        out[0] = a; out[1] = b; out[2] = c; out[3] = d
        out[4] = cx + tx - (a * cx + c * cy)
        out[5] = cy + ty - (b * cx + d * cy)
    }

    /** Canvas position of the box point at (u, v) relative to its centre (unscaled box units). */
    fun mapX(u: Double, v: Double): Double = if (isTranslation) cx + u + tx.roundToInt() else cx + tx + l00() * u + l01() * v
    fun mapY(u: Double, v: Double): Double = if (isTranslation) cy + v + ty.roundToInt() else cy + ty + l10() * u + l11() * v

    /** Canvas position of a handle; ROTATE is [rotateDistance] beyond the top edge, along the box's "up". */
    fun handleX(hd: TransformHandle, rotateDistance: Double): Double = when (hd) {
        TransformHandle.ROTATE -> mapX(0.0, -h / 2) + upX() * rotateDistance
        TransformHandle.MOVE, TransformHandle.NONE -> mapX(0.0, 0.0)
        else -> mapX(hd.hx * w / 2, hd.hy * h / 2)
    }
    fun handleY(hd: TransformHandle, rotateDistance: Double): Double = when (hd) {
        TransformHandle.ROTATE -> mapY(0.0, -h / 2) + upY() * rotateDistance
        TransformHandle.MOVE, TransformHandle.NONE -> mapY(0.0, 0.0)
        else -> mapY(hd.hx * w / 2, hd.hy * h / 2)
    }

    // Unit vector from the centre towards the top edge (screen "up" of the box).
    private fun upX(): Double { val x = mapX(0.0, -h / 2) - mapX(0.0, 0.0); val y = mapY(0.0, -h / 2) - mapY(0.0, 0.0); val n = hypot(x, y); return if (n > 0) x / n else 0.0 }
    private fun upY(): Double { val x = mapX(0.0, -h / 2) - mapX(0.0, 0.0); val y = mapY(0.0, -h / 2) - mapY(0.0, 0.0); val n = hypot(x, y); return if (n > 0) y / n else -1.0 }

    /** The handle at canvas point (x, y) within [radius] (canvas px); inside the box = MOVE, else NONE. */
    fun hit(x: Double, y: Double, radius: Double, rotateDistance: Double): TransformHandle {
        var best = TransformHandle.NONE
        var bestD = radius
        for (hd in TransformHandle.entries) {
            if (hd == TransformHandle.NONE || hd == TransformHandle.MOVE) continue
            val d = hypot(x - handleX(hd, rotateDistance), y - handleY(hd, rotateDistance))
            if (d <= bestD) { best = hd; bestD = d }
        }
        if (best != TransformHandle.NONE) return best
        return if (inside(x, y)) TransformHandle.MOVE else TransformHandle.NONE
    }

    /** Inside the transformed box. */
    fun inside(x: Double, y: Double): Boolean {
        val u = toLocalX(x, y); val v = toLocalY(x, y)
        return abs(u) <= abs(w / 2) && abs(v) <= abs(h / 2)
    }

    // Canvas -> box units (inverse of L around the moved centre).
    private fun det() = l00() * l11() - l01() * l10()
    private fun toLocalX(x: Double, y: Double): Double {
        val dx = x - (cx + tx); val dy = y - (cy + ty); val dt = det()
        return if (dt == 0.0) 0.0 else (l11() * dx - l01() * dy) / dt
    }
    private fun toLocalY(x: Double, y: Double): Double {
        val dx = x - (cx + tx); val dy = y - (cy + ty); val dt = det()
        return if (dt == 0.0) 0.0 else (-l10() * dx + l00() * dy) / dt
    }

    /** A drag starts at canvas point (x, y) on [hd] (MOVE anywhere that is not a handle). */
    fun begin(hd: TransformHandle, x: Double, y: Double) {
        handle = if (hd == TransformHandle.NONE) TransformHandle.MOVE else hd
        px0 = x; py0 = y
        tx0 = tx; ty0 = ty; sx0 = sx; sy0 = sy; rot0 = rotation; kx0 = kx; ky0 = ky
    }

    /** The pointer is now at canvas point (x, y). */
    fun drag(x: Double, y: Double) {
        val hd = handle
        when {
            hd == TransformHandle.MOVE -> { tx = tx0 + (x - px0); ty = ty0 + (y - py0) }
            hd == TransformHandle.ROTATE -> {
                val ccx = cx + tx0; val ccy = cy + ty0
                var r = rot0 + atan2(y - ccy, x - ccx) - atan2(py0 - ccy, px0 - ccx)
                if (snap) r = Math.round(r / SNAP_STEP) * SNAP_STEP
                rotation = normalize(r)
            }
            skewMode && hd.edge -> skew(hd, x, y)
            hd.corner || hd.edge -> scale(hd, x, y)
            else -> Unit
        }
    }

    fun end() { handle = TransformHandle.NONE }

    fun flipHorizontal() { sx = -sx }
    fun flipVertical() { sy = -sy }

    private fun scale(hd: TransformHandle, x: Double, y: Double) {
        // Pointer in "scaled box" space: R·K removed, relative to the centre at the drag start.
        val rx = x - (cx + tx0); val ry = y - (cy + ty0)
        val c = cos(-rot0); val s = sin(-rot0)
        val ux = c * rx - s * ry; val uy = s * rx + c * ry          // R⁻¹
        val kd = 1 - kx0 * ky0
        if (abs(kd) < 1e-9) return
        val qx = (ux - kx0 * uy) / kd; val qy = (uy - ky0 * ux) / kd    // K⁻¹
        val ax = -hd.hx * w / 2 * sx0; val ay = -hd.hy * h / 2 * sy0   // the opposite side stays
        var nsx = sx0; var nsy = sy0
        if (proportional && hd.corner) {
            val dx = hd.hx * w * sx0; val dy = hd.hy * h * sy0       // handle - anchor
            val f = ((qx - ax) * dx + (qy - ay) * dy) / (dx * dx + dy * dy)
            nsx = sx0 * f; nsy = sy0 * f
        } else {
            if (hd.hx != 0) nsx = (qx - ax) / (hd.hx * w)
            if (hd.hy != 0) nsy = (qy - ay) / (hd.hy * h)
        }
        // Never thinner than one pixel (keeps the transform invertible).
        if (abs(nsx * w) < 1) nsx = (if (nsx < 0) -1.0 else 1.0) / w
        if (abs(nsy * h) < 1) nsy = (if (nsy < 0) -1.0 else 1.0) / h
        sx = nsx; sy = nsy
        // New centre: half way from the anchor along each scaled axis (unchanged on a free axis).
        val dcx = if (hd.hx != 0) ax + hd.hx * w * nsx / 2 else 0.0
        val dcy = if (hd.hy != 0) ay + hd.hy * h * nsy / 2 else 0.0
        // Back through R·K to canvas.
        val kxv = dcx + kx0 * dcy; val kyv = ky0 * dcx + dcy
        tx = tx0 + cos(rot0) * kxv - sin(rot0) * kyv
        ty = ty0 + sin(rot0) * kxv + cos(rot0) * kyv
    }

    private fun skew(hd: TransformHandle, x: Double, y: Double) {
        // Pointer movement with the rotation removed.
        val c = cos(-rot0); val s = sin(-rot0)
        val dx = x - px0; val dy = y - py0
        val ux = c * dx - s * dy; val uy = s * dx + c * dy
        if (hd.hy != 0) kx = kx0 + ux / (hd.hy * h / 2 * sy0)    // top / bottom edge: horizontal inclination
        else ky = ky0 + uy / (hd.hx * w / 2 * sx0)               // left / right edge: vertical inclination
        kx = kx.coerceIn(-MAX_SKEW, MAX_SKEW)
        ky = ky.coerceIn(-MAX_SKEW, MAX_SKEW)
    }

    private fun normalize(r: Double): Double {
        var v = r % (2 * PI)
        if (v > PI) v -= 2 * PI
        if (v < -PI) v += 2 * PI
        return if (abs(v) < 1e-12) 0.0 else v
    }

    companion object {
        /** Rotation snap step: 15°. */
        const val SNAP_STEP = PI / 12
        /** Skew limit (tan of ~71°): beyond, the box would be nearly flat. */
        const val MAX_SKEW = 3.0
    }
}
