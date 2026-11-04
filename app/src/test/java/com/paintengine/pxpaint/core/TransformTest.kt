package com.paintengine.pxpaint.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.PI

/** Block 12: transform box gestures and the matrix sent to the engine. */
class TransformTest {
    private fun box() = TransformState(100.0, 100.0, 300.0, 200.0)   // 200 x 100, centre (200, 150)
    private val m = DoubleArray(6)
    private fun mx(t: TransformState, x: Double, y: Double): Pair<Double, Double> {
        t.matrix(m); return (m[0] * x + m[2] * y + m[4]) to (m[1] * x + m[3] * y + m[5])
    }

    @Test
    fun moveIsWholePixels() {
        val t = box()
        assertTrue(t.isIdentity)
        t.begin(t.hit(200.0, 150.0, 10.0, 30.0), 200.0, 150.0)
        t.drag(230.4, 139.6)
        t.matrix(m)
        assertEquals(listOf(1.0, 0.0, 0.0, 1.0, 30.0, -10.0), m.toList())
        assertFalse(t.isIdentity)
    }

    @Test
    fun cornerScalesKeepingTheOppositeCorner() {
        val t = box()
        assertEquals(TransformHandle.SE, t.hit(301.0, 199.0, 10.0, 30.0))
        t.proportional = false
        t.begin(TransformHandle.SE, 300.0, 200.0)
        t.drag(500.0, 300.0)                         // twice as wide and tall
        assertEquals(2.0, t.sx, 1e-9); assertEquals(2.0, t.sy, 1e-9)
        val (x0, y0) = mx(t, 100.0, 100.0)            // the opposite corner stays
        assertEquals(100.0, x0, 1e-9); assertEquals(100.0, y0, 1e-9)
        val (x1, y1) = mx(t, 300.0, 200.0)
        assertEquals(500.0, x1, 1e-9); assertEquals(300.0, y1, 1e-9)
        // Proportional: the larger of the two follows the diagonal projection.
        val p = box()
        p.begin(TransformHandle.SE, 300.0, 200.0)
        p.drag(500.0, 200.0)
        assertEquals(p.sx, p.sy, 1e-9)
        assertTrue(p.sx > 1.0)
        // An edge scales one axis only.
        val e = box()
        e.begin(TransformHandle.E, 300.0, 150.0)
        e.drag(400.0, 170.0)
        assertEquals(1.5, e.sx, 1e-9); assertEquals(1.0, e.sy, 1e-9)
        assertEquals(100.0, mx(e, 100.0, 150.0).first, 1e-9)
    }

    @Test
    fun rotateAboutTheCentreWithOptionalSnap() {
        val t = box()
        val rx = t.handleX(TransformHandle.ROTATE, 30.0); val ry = t.handleY(TransformHandle.ROTATE, 30.0)
        assertEquals(200.0, rx, 1e-9); assertEquals(70.0, ry, 1e-9)     // 30 px above the top edge
        assertEquals(TransformHandle.ROTATE, t.hit(rx, ry, 10.0, 30.0))
        t.begin(TransformHandle.ROTATE, rx, ry)
        t.drag(280.0, 150.0)                         // a quarter turn clockwise
        assertEquals(PI / 2, t.rotation, 1e-9)
        val (cx, cy) = mx(t, 200.0, 150.0)
        assertEquals(200.0, cx, 1e-9); assertEquals(150.0, cy, 1e-9)    // centre fixed
        val s = box()
        s.snap = true
        s.begin(TransformHandle.ROTATE, 200.0, 70.0)
        s.drag(230.0, 70.0)                          // ~20.6 degrees -> 15
        assertEquals(PI / 12, s.rotation, 1e-9)
    }

    @Test
    fun flipAndSkew() {
        val t = box()
        t.flipHorizontal()
        val (x, _) = mx(t, 100.0, 150.0)
        assertEquals(300.0, x, 1e-9)                 // left edge lands on the right
        t.flipVertical()
        assertEquals(200.0, mx(t, 200.0, 100.0).second, 1e-9)
        val k = box()
        k.skewMode = true
        k.begin(TransformHandle.S, 200.0, 200.0)
        k.drag(250.0, 200.0)                         // bottom edge 50 px right
        assertEquals(1.0, k.kx, 1e-9)
        val (bx, by) = mx(k, 200.0, 200.0)
        assertEquals(250.0, bx, 1e-9); assertEquals(200.0, by, 1e-9)
        assertEquals(150.0, mx(k, 200.0, 100.0).first, 1e-9)            // top edge 50 px left
    }

    @Test
    fun neverDegenerate() {
        val t = box()
        t.proportional = false
        t.begin(TransformHandle.E, 300.0, 150.0)
        t.drag(100.0, 150.0)                         // dragged onto the opposite edge
        t.matrix(m)
        assertTrue(Math.abs(m[0] * m[3] - m[1] * m[2]) > 1e-6)
        assertEquals(TransformHandle.MOVE, t.hit(150.0 + 0.2, 150.0, 1.0, 30.0).let { if (it == TransformHandle.NONE) TransformHandle.MOVE else it })
    }
}
