package com.paintengine.pxpaint.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ColorDropTest {
    private fun drop() = ColorDrop(slopPx = 10f, holdSlopPx = 8f, rangePx = 400f)

    @Test
    fun tapOpensTheColourPanel() {
        val d = drop()
        d.down(100f, 100f, 0.2f)
        assertFalse(d.move(104f, 103f))            // within the slop: still a tap
        assertEquals(ColorDrop.Release.TAP, d.up())
        assertEquals(ColorDrop.State.IDLE, d.state)
    }

    @Test
    fun dragAndReleaseDropsWithTheCurrentThreshold() {
        val d = drop()
        d.down(100f, 100f, 0.2f)
        assertTrue(d.move(100f, 60f))              // drag begins: the hold timer starts
        assertEquals(ColorDrop.State.DRAGGING, d.state)
        assertTrue(d.move(100f, 20f))              // moved on: the hold timer restarts
        assertFalse(d.move(102f, 18f))             // resting
        assertEquals(ColorDrop.Release.DROP, d.up())
        assertEquals(0.2f, d.threshold, 0f)
    }

    @Test
    fun holdingTurnsTheDragIntoAThresholdSlider() {
        val d = drop()
        d.down(100f, 500f, 0.25f)
        d.move(300f, 300f)
        assertTrue(d.hold())
        assertEquals(ColorDrop.State.THRESHOLD, d.state)
        assertFalse(d.hold())                      // only once
        d.move(400f, 310f)                         // +100 px of 400: +25 %
        assertEquals(0.5f, d.threshold, 1e-4f)
        d.move(0f, 300f)
        assertEquals(0f, d.threshold, 0f)          // clamped
        d.move(2000f, 300f)
        assertEquals(1f, d.threshold, 0f)
        assertEquals(ColorDrop.Release.DONE, d.up())
        // A hold before any drag is nothing.
        d.down(0f, 0f, 0.1f)
        assertFalse(d.hold())
    }
}
