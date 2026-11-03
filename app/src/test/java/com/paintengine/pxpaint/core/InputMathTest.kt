package com.paintengine.pxpaint.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.PI
import kotlin.math.hypot

class InputMathTest {
    private val eps = 1e-9

    @Test
    fun sampleBufferKeepsEverySampleInOrderAndIsReused() {
        val b = SampleBuffer(2)
        for (i in 0 until 100) b.add(i.toFloat(), -i.toFloat(), i * 4f)
        assertEquals(100, b.count)
        for (i in 0 until 100) {
            assertEquals(i.toDouble(), b.data[i * 3].toDouble(), 0.0)
            assertEquals(-i.toDouble(), b.data[i * 3 + 1].toDouble(), 0.0)
            assertEquals(i * 4.0, b.data[i * 3 + 2].toDouble(), 0.0)
        }
        val grown = b.data
        b.clear()
        b.add(1f, 2f, 3f)
        assertEquals(1, b.count)
        assertTrue("buffer reused, no new allocation", grown === b.data)
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
    fun quickSecondFingerCancelsTheStroke() {
        val g = StrokeGate(cancelWindowMs = 250, slopPx = 24.0)
        g.down(1000, P(100.0, 100.0))
        g.move(P(160.0, 100.0))
        assertEquals(StrokeGate.Decision.CANCEL, g.onSecondPointer(1200))  // within 250 ms: a pinch
        assertFalse(g.active)
    }

    @Test
    fun drawnStrokeIsKeptWhenAGestureFollows() {
        val g = StrokeGate(cancelWindowMs = 250, slopPx = 24.0)
        g.down(1000, P(100.0, 100.0))
        g.move(P(400.0, 120.0))
        assertEquals(StrokeGate.Decision.COMMIT, g.onSecondPointer(1800))
    }

    @Test
    fun restingFingerThenPinchCancels() {
        val g = StrokeGate(cancelWindowMs = 250, slopPx = 24.0)
        g.down(1000, P(100.0, 100.0))
        g.move(P(110.0, 104.0))
        assertEquals(StrokeGate.Decision.CANCEL, g.onSecondPointer(3000))  // never really moved
    }
}
