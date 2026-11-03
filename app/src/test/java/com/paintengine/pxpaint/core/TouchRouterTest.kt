package com.paintengine.pxpaint.core

import org.junit.Assert.assertEquals
import org.junit.Test

class TouchRouterTest {
    /** Records the router's decisions as readable lines. */
    private class Log : TouchRouter.Actions {
        val lines = ArrayList<String>()
        override fun beginStroke(pointerId: Int, kind: ToolKind, eraser: Boolean) { lines += "begin $pointerId $kind${if (eraser) " erase" else ""}" }
        override fun strokeSamples(pointerId: Int) { lines += "samples $pointerId" }
        override fun endStroke(pointerId: Int) { lines += "end $pointerId" }
        override fun cancelStroke() { lines += "cancel" }
        override fun panStart(pointerId: Int) { lines += "pan $pointerId" }
        override fun panMove(pointerId: Int) { lines += "panmove $pointerId" }
        override fun gestureStart(a: Int, b: Int) { lines += "gesture $a $b" }
        override fun gestureMove(a: Int, b: Int) { lines += "gesturemove $a $b" }
        override fun gestureEnd(a: Int, b: Int) { lines += "gestureend" }
    }

    private fun router(log: Log, mode: FingerMode = FingerMode.DRAW) =
        TouchRouter(log, StrokeGate(cancelWindowMs = 250, slopPx = 24.0)).apply { fingerMode = mode }

    @Test
    fun fingerStrokeFollowsItsPointerAndEnds() {
        val log = Log()
        val r = router(log)
        r.down(3, ToolKind.FINGER, 0, 10f, 10f)
        r.move()
        r.up(3, canceled = false)
        assertEquals(listOf("begin 3 FINGER", "samples 3", "end 3"), log.lines)
    }

    @Test
    fun stylusPaintsInEveryFingerModeAndItsEraserEndErases() {
        for (mode in FingerMode.entries) {
            val log = Log()
            val r = router(log, mode)
            r.down(0, ToolKind.STYLUS, 0, 0f, 0f)
            r.up(0, false)
            r.down(1, ToolKind.ERASER, 100, 0f, 0f)
            r.up(1, false)
            assertEquals("$mode", listOf("begin 0 STYLUS", "end 0", "begin 1 ERASER erase", "end 1"), log.lines)
        }
    }

    @Test
    fun fingerModes() {
        val pan = Log()
        router(pan, FingerMode.PAN).apply { down(0, ToolKind.FINGER, 0, 0f, 0f); move(); up(0, false) }
        assertEquals(listOf("pan 0", "panmove 0"), pan.lines)
        val none = Log()
        router(none, FingerMode.GESTURES_ONLY).apply {
            down(0, ToolKind.FINGER, 0, 0f, 0f); move()
            pointerDown(1, ToolKind.FINGER, 50, 50f, 0f); move(); up(1, false)
        }
        assertEquals(listOf("gesture 0 1", "gesturemove 0 1", "gestureend"), none.lines)
    }

    @Test
    fun palmIsRejectedWhileTheStylusPaints() {
        val log = Log()
        val r = router(log)
        r.down(0, ToolKind.STYLUS, 0, 0f, 0f)
        r.pointerDown(1, ToolKind.FINGER, 30, 200f, 200f)   // palm lands
        r.move()
        r.pointerUp(1, false)
        r.move()
        r.up(0, false)
        assertEquals(listOf("begin 0 STYLUS", "samples 0", "samples 0", "end 0"), log.lines)
    }

    @Test
    fun palmFirstThenStylus() {
        // The palm touches first and starts a finger stroke; the stylus lands:
        // the palm stroke is cancelled (canvas restored) and the stylus paints.
        val log = Log()
        val r = router(log)
        r.down(0, ToolKind.FINGER, 0, 0f, 0f)
        r.move()
        r.pointerDown(1, ToolKind.STYLUS, 900, 10f, 10f)
        r.move()
        r.pointerUp(1, false)
        r.up(0, false)
        assertEquals(listOf("begin 0 FINGER", "samples 0", "cancel", "begin 1 STYLUS", "samples 1", "end 1"), log.lines)
    }

    @Test
    fun stylusThenFingerTransitions() {
        val log = Log()
        val r = router(log)
        r.down(0, ToolKind.STYLUS, 0, 0f, 0f)
        r.up(0, false)
        r.down(1, ToolKind.FINGER, 500, 0f, 0f)   // next touch is a finger: a plain finger stroke
        r.up(1, false)
        r.down(2, ToolKind.STYLUS, 900, 0f, 0f)
        r.up(2, false)
        assertEquals(listOf("begin 0 STYLUS", "end 0", "begin 1 FINGER", "end 1", "begin 2 STYLUS", "end 2"), log.lines)
    }

    @Test
    fun pinchRightAfterAFingerLandsCancelsTheStroke() {
        val log = Log()
        val r = router(log)
        r.down(0, ToolKind.FINGER, 0, 0f, 0f)
        r.pointerDown(1, ToolKind.FINGER, 80, 100f, 0f)
        r.move()
        r.pointerUp(0, false)
        r.move()
        r.up(1, false)
        assertEquals(listOf("begin 0 FINGER", "cancel", "gesture 0 1", "gesturemove 0 1", "gestureend"), log.lines)
    }

    @Test
    fun drawnFingerStrokeIsKeptWhenAGestureFollows() {
        val log = Log()
        val r = router(log)
        r.down(0, ToolKind.FINGER, 0, 0f, 0f)
        r.strokeMovedTo(300f, 0f)
        r.pointerDown(1, ToolKind.FINGER, 1500, 100f, 0f)
        assertEquals(listOf("begin 0 FINGER", "end 0", "gesture 0 1"), log.lines)
    }

    @Test
    fun systemCancelledPointerCancelsItsStroke() {
        val log = Log()
        val r = router(log)
        r.down(0, ToolKind.FINGER, 0, 0f, 0f)
        r.up(0, canceled = true)    // FLAG_CANCELED: the system decided it was a palm
        r.down(1, ToolKind.STYLUS, 100, 0f, 0f)
        r.cancel()                  // ACTION_CANCEL
        assertEquals(listOf("begin 0 FINGER", "cancel", "begin 1 STYLUS", "cancel"), log.lines)
    }

    @Test
    fun moveToolAndEraserTool() {
        val log = Log()
        val r = router(log).apply { moveTool = true }
        r.down(0, ToolKind.STYLUS, 0, 0f, 0f)
        r.move()
        r.up(0, false)
        r.moveTool = false
        r.eraserTool = true
        r.down(1, ToolKind.FINGER, 10, 0f, 0f)
        r.up(1, false)
        assertEquals(listOf("pan 0", "panmove 0", "begin 1 FINGER erase", "end 1"), log.lines)
    }

    @Test
    fun interruptKeepsTheStroke() {
        val log = Log()
        val r = router(log)
        r.down(4, ToolKind.STYLUS, 0, 0f, 0f)
        r.interrupt()
        r.move()   // nothing more until the next down
        assertEquals(listOf("begin 4 STYLUS", "end 4"), log.lines)
    }
}
