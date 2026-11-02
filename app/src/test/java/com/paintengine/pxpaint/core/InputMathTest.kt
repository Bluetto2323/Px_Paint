package com.paintengine.pxpaint.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.PI
import kotlin.math.hypot

class InputMathTest {
    private val eps = 1e-9

    @Test
    fun strokeSpacingIsConstantAcrossSegments() {
        val s = StrokeSampler(10.0)
        val pts = ArrayList<P>()
        pts += s.begin(P(0.0, 0.0))
        // Irregular event spacing: 3, 7, 25, 1, 64 px along x.
        for (x in listOf(3.0, 10.0, 35.0, 36.0, 100.0)) pts += s.moveTo(P(x, 0.0))
        assertEquals(11, pts.size)                       // 0, 10, ..., 100
        for (i in pts.indices) assertEquals(i * 10.0, pts[i].x, eps)
    }

    @Test
    fun strokeFollowsDiagonalsAndIgnoresZeroMoves() {
        val s = StrokeSampler(5.0)
        s.begin(P(0.0, 0.0))
        assertTrue(s.moveTo(P(0.0, 0.0)).isEmpty())
        val pts = s.moveTo(P(30.0, 40.0))                // length 50
        assertEquals(10, pts.size)
        assertEquals(50.0, hypot(pts.last().x, pts.last().y), eps)
    }

    @Test
    fun twoFingerScaleRotationAndFocal() {
        val st = TwoFinger.step(P(100.0, 100.0), P(200.0, 100.0), P(100.0, 100.0), P(100.0, 300.0), true)
        assertEquals(2.0, st.scale, eps)
        assertEquals(PI / 2, st.rotation, eps)
        assertEquals(P(150.0, 100.0), st.prevFocal)
        assertEquals(P(100.0, 200.0), st.newFocal)
        val noRot = TwoFinger.step(P(100.0, 100.0), P(200.0, 100.0), P(100.0, 100.0), P(100.0, 300.0), false)
        assertEquals(0.0, noRot.rotation, eps)
    }

    @Test
    fun twoFingerIgnoresTinySpans() {
        val st = TwoFinger.step(P(0.0, 0.0), P(1.0, 0.0), P(0.0, 0.0), P(0.0, 50.0), true)
        assertEquals(1.0, st.scale, eps)
        assertEquals(0.0, st.rotation, eps)
    }

    @Test
    fun rotationWrapsAndSnaps() {
        assertEquals(-PI + 0.1, TwoFinger.normalize(PI + 0.1), eps)
        assertEquals(PI / 2, RotationSnap.target(Math.toRadians(87.0))!!, eps)
        assertEquals(0.0, RotationSnap.target(Math.toRadians(-3.0))!!, eps)
        assertNull(RotationSnap.target(Math.toRadians(20.0)))
        assertEquals(PI, kotlin.math.abs(RotationSnap.target(Math.toRadians(-178.0))!!), eps)
    }

    @Test
    fun drawGateTapDrawsOneDab() {
        val g = DrawGate()
        g.down(0, P(5.0, 5.0))
        assertEquals(listOf(P(5.0, 5.0)), g.up())
    }

    @Test
    fun drawGateOpensOnSlopOrDelay() {
        val g = DrawGate(slopPx = 10.0, delayMs = 90)
        g.down(0, P(0.0, 0.0))
        assertTrue(g.move(10, P(3.0, 0.0)).isEmpty())
        val opened = g.move(20, P(12.0, 0.0))
        assertEquals(3, opened.size)                     // buffered points flushed in order
        assertEquals(DrawGate.State.DRAWING, g.state)
        assertEquals(listOf(P(20.0, 0.0)), g.move(30, P(20.0, 0.0)))

        val still = DrawGate(slopPx = 10.0, delayMs = 90)
        still.down(0, P(0.0, 0.0))
        assertTrue(still.tick(50).isEmpty())
        assertEquals(1, still.tick(95).size)
    }

    @Test
    fun secondFingerCancelsAPendingTouch() {
        val g = DrawGate()
        g.down(0, P(0.0, 0.0))
        g.move(10, P(2.0, 0.0))
        g.secondPointer()
        assertEquals(DrawGate.State.CANCELLED, g.state)
        assertTrue(g.move(200, P(50.0, 0.0)).isEmpty())
        assertTrue(g.up().isEmpty())
    }
}
