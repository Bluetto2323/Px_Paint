package com.paintengine.pxpaint.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.sin

/** Block 10: selection gestures, geometry, outline format, contextual actions, routing. */
class SelectionTest {
    private fun gesture(shape: SelectionShape) = SelectionGesture(tapSlopPx = 10f, closeRadiusPx = 20f).also { it.shape = shape }

    @Test
    fun rectangleAndEllipseDrag() {
        for (shape in listOf(SelectionShape.RECTANGLE, SelectionShape.ELLIPSE)) {
            val g = gesture(shape)
            g.down(100f, 100f)
            g.move(150f, 160f)
            g.move(300f, 250f)
            assertTrue(g.busy)
            assertEquals(SelectionGesture.Outcome.SELECT, g.up(320f, 260f))
            assertEquals(2, g.points.count)
            assertEquals(100f, g.points.x(0)); assertEquals(100f, g.points.y(0))
            assertEquals(320f, g.points.x(1)); assertEquals(260f, g.points.y(1))
            assertFalse(g.busy)
        }
    }

    @Test
    fun tapsAndDegenerateShapes() {
        val g = gesture(SelectionShape.RECTANGLE)
        g.down(100f, 100f)
        g.move(104f, 103f)
        assertEquals("a tap (within the slop) deselects", SelectionGesture.Outcome.TAP, g.up(104f, 103f))
        // A long but flat drag selects nothing (zero height).
        g.down(100f, 100f)
        assertEquals(SelectionGesture.Outcome.TAP, g.up(400f, 100.5f))
        val l = gesture(SelectionShape.LASSO)
        l.down(10f, 10f)
        l.move(200f, 10f)
        assertEquals("a lasso along a line encloses nothing", SelectionGesture.Outcome.TAP, l.up(400f, 10f))
    }

    @Test
    fun lassoSkipsTinyStepsAndKeepsEverySample() {
        val g = gesture(SelectionShape.LASSO)
        g.down(0f, 0f)
        for (i in 1..1000) g.move(i * 0.5f, 0f)   // 0.5 px steps: most are skipped (1.5 px minimum)
        assertTrue(g.points.count in 300..340)
        // A circle drawn with 6000 samples: all kept (steps ~ 2 px), no reallocation per sample beyond doubling.
        g.cancel()
        g.down(500f, 300f)
        for (i in 0..6000) {
            val a = 2 * Math.PI * i / 6000
            g.move((300 + 200 * cos(a)).toFloat() + 0f, (300 + 200 * sin(a)).toFloat())
        }
        assertEquals(SelectionGesture.Outcome.SELECT, g.up(500f, 300f))
        val area = g.points.area()
        assertTrue("area $area", abs(area - Math.PI * 200 * 200) / (Math.PI * 200 * 200) < 0.02)
    }

    @Test
    fun polygonTapsCloseAndUndo() {
        val g = gesture(SelectionShape.POLYGON)
        g.down(100f, 100f)
        assertEquals(SelectionGesture.Outcome.NONE, g.up(100f, 100f))
        assertEquals(SelectionGesture.Phase.POLYGON, g.phase)
        assertEquals(1, g.points.count)
        // Press, drag (rubber band), lift: the vertex lands where the pointer is lifted.
        g.down(300f, 100f)
        g.move(310f, 120f)
        assertEquals(310f, g.cursorX)
        assertEquals(SelectionGesture.Outcome.NONE, g.up(300f, 300f))
        assertEquals(2, g.points.count)
        assertTrue(g.cursorX.isNaN())
        // Tapping the first vertex before 3 corners does not close; it adds nothing (same place as the first? no: far from the last).
        g.down(100f, 300f)
        assertEquals(SelectionGesture.Outcome.NONE, g.up(100f, 300f))
        assertEquals(3, g.points.count)
        g.undoVertex()
        assertEquals(2, g.points.count)
        g.down(100f, 300f); g.up(100f, 300f)
        // Tapping the first vertex (within the close radius) closes it.
        g.down(105f, 95f)
        assertEquals(SelectionGesture.Outcome.SELECT, g.up(105f, 95f))
        assertEquals(3, g.points.count)
        assertFalse(g.busy)
        // "Close shape" from the bar.
        g.down(0f, 0f); g.up(0f, 0f)
        g.down(50f, 0f); g.up(50f, 0f)
        assertFalse(g.closePolygon())
        g.down(50f, 50f); g.up(50f, 50f)
        assertTrue(g.closePolygon())
        // Undoing every vertex cancels.
        g.down(0f, 0f); g.up(0f, 0f)
        g.undoVertex()
        assertFalse(g.busy)
    }

    @Test
    fun affineInverseAndOffsets() {
        // Zoom 2, rotated 30 degrees, panned: what the engine's view matrix looks like.
        val r = Math.toRadians(30.0)
        val m = floatArrayOf((2 * cos(r)).toFloat(), (2 * sin(r)).toFloat(), (-2 * sin(r)).toFloat(), (2 * cos(r)).toFloat(), 140f, -60f)
        val s = Affine().also { it.set(m) }
        val c = Affine()
        assertTrue(s.invertInto(c))
        val x = s.x(123.0, 456.0); val y = s.y(123.0, 456.0)
        assertEquals(123.0, c.x(x, y), 1e-3)
        assertEquals(456.0, c.y(x, y), 1e-3)
        assertEquals(2.0, s.scale, 1e-6)
        // A 100 px screen drag to the right at zoom 2 moves the content 50 canvas px along the rotated axis.
        val d = IntArray(2)
        SelectionGeometry.canvasOffset(c, 100.0, 0.0, d)
        assertEquals(Math.round(50 * cos(r)).toInt(), d[0])
        assertEquals(Math.round(-50 * sin(r)).toInt(), d[1])
        assertFalse(Affine().also { it.set(floatArrayOf(0f, 0f, 0f, 0f, 1f, 1f)) }.invertInto(c))
    }

    @Test
    fun previewMatchesWhatTheEngineSelects() {
        // Unrotated, zoom 0.5, canvas origin at (20, 30) on screen.
        val s = Affine().also { it.set(floatArrayOf(0.5f, 0f, 0f, 0.5f, 20f, 30f)) }
        val c = Affine().also { s.invertInto(it) }
        val box = DoubleArray(4)
        SelectionGeometry.canvasBox(c, 70.3f, 80.1f, 20.2f, 130.9f, snap = true, out = box)
        // Screen 20.2..70.3 -> canvas 0.4..100.6 -> rounded 0..101 (whole pixels, like the engine's rectangle).
        assertEquals(listOf(0.0, 100.0, 101.0, 202.0), box.toList())
        val out = FloatArray(8)
        assertEquals(4, SelectionGeometry.outline(s, box, false, 4, out))
        assertEquals(20f, out[0]); assertEquals(80f, out[1]); assertEquals(70.5f, out[2]); assertEquals(131f, out[5])
        val ring = FloatArray(64 * 2)
        SelectionGeometry.canvasBox(c, 20f, 30f, 120f, 130f, snap = false, out = box)
        assertEquals(64, SelectionGeometry.outline(s, box, true, 64, ring))
        for (i in 0 until 64) {
            val dx = ring[i * 2] - 70f; val dy = ring[i * 2 + 1] - 80f
            assertEquals(50.0, Math.hypot(dx.toDouble(), dy.toDouble()), 1e-3)
        }
    }

    @Test
    fun outlineFormat() {
        val two = floatArrayOf(2f, 4f, 0f, 0f, 10f, 0f, 10f, 10f, 0f, 10f, 3f, 20f, 20f, 30f, 20f, 25f, 30f)
        assertEquals(2, SelectionOutline.loops(two))
        assertEquals(7, SelectionOutline.points(two))
        val starts = mutableListOf<Pair<Int, Int>>()
        SelectionOutline.forEachLoop(two) { start, n -> starts += start to n }
        assertEquals(listOf(2 to 4, 11 to 3), starts)
        assertEquals(0, SelectionOutline.loops(floatArrayOf(0f)))
        assertEquals(-1, SelectionOutline.loops(floatArrayOf(1f, 4f, 0f, 0f)))       // truncated
        assertEquals(-1, SelectionOutline.loops(floatArrayOf(1f, 1f, 0f, 0f, 9f)))   // trailing data
        assertEquals(-1, SelectionOutline.loops(null))
    }

    @Test
    fun contextualActionsOnlyWhatCanAct() {
        val none = SelectionAction.available(active = false, clipboard = false, moving = false, editable = true, polygonPoints = 0)
        assertEquals(listOf(SelectionAction.SELECT_ALL, SelectionAction.FROM_CONTENT), none)
        assertTrue(SelectionAction.PASTE in SelectionAction.available(false, true, false, true, 0))
        val sel = SelectionAction.available(active = true, clipboard = false, moving = false, editable = true, polygonPoints = 0)
        assertTrue(sel.containsAll(listOf(SelectionAction.MOVE, SelectionAction.COPY, SelectionAction.COPY_MERGED, SelectionAction.CUT,
            SelectionAction.DUPLICATE, SelectionAction.CLEAR, SelectionAction.INVERT, SelectionAction.DESELECT)))
        assertFalse(SelectionAction.PASTE in sel)
        // A locked / hidden layer: no pixel edits offered (copying still works).
        val locked = SelectionAction.available(true, true, false, editable = false, polygonPoints = 0)
        for (a in listOf(SelectionAction.MOVE, SelectionAction.CUT, SelectionAction.CLEAR)) assertFalse(a in locked)
        assertTrue(SelectionAction.COPY in locked && SelectionAction.PASTE in locked)
        assertEquals(listOf(SelectionAction.MOVE_DONE, SelectionAction.MOVE_CANCEL, SelectionAction.FLIP_H, SelectionAction.FLIP_V,
            SelectionAction.SNAP, SelectionAction.KEEP_RATIO, SelectionAction.SKEW), SelectionAction.available(true, true, true, true, 0))
        assertEquals(listOf(SelectionAction.POLYGON_UNDO, SelectionAction.POLYGON_CANCEL), SelectionAction.available(true, true, false, true, 2))
        assertEquals(SelectionAction.POLYGON_CLOSE, SelectionAction.available(false, false, false, true, 3).first())
    }

    @Test
    fun errorsAreExplained() {
        assertEquals(null, SelectionErrors.message(SelectionAction.COPY, 0))
        assertEquals(S.SEL_ERR_LOCKED, SelectionErrors.message(SelectionAction.CUT, 5))
        assertEquals(S.SEL_ERR_HIDDEN, SelectionErrors.message(SelectionAction.CLEAR, 6))
        assertEquals(S.SEL_ERR_NOTHING, SelectionErrors.message(SelectionAction.COPY, 9))
        assertEquals(S.SEL_ERR_NO_CONTENT, SelectionErrors.message(SelectionAction.FROM_CONTENT, 9))
        assertEquals(S.SEL_ERR_TOO_MANY, SelectionErrors.message(SelectionAction.PASTE, 7))
    }

    @Test
    fun modeCodesMatchTheEngine() {
        assertEquals(listOf(0, 1, 2, 3), SelectionMode.entries.map { it.code })
        assertEquals(SelectionShape.LASSO, SelectionShape.of("LASSO"))
        assertEquals(SelectionShape.RECTANGLE, SelectionShape.of("nonsense"))
        // History kinds 15..20 have names (not "Edit").
        for (code in 15..20) assertTrue(HistoryKind.of(code) != HistoryKind.OTHER)
    }

    @Test
    fun selectToolRoutesEveryPointerToTheSelection() {
        val log = mutableListOf<String>()
        val r = TouchRouter(object : TouchRouter.Actions {
            override fun beginStroke(pointerId: Int, kind: ToolKind, eraser: Boolean) { log += "begin $pointerId" }
            override fun strokeSamples(pointerId: Int) { log += "move" }
            override fun endStroke(pointerId: Int) { log += "end" }
            override fun cancelStroke() { log += "cancel" }
            override fun panStart(pointerId: Int) { log += "pan" }
            override fun panMove(pointerId: Int) {}
            override fun gestureStart(a: Int, b: Int) { log += "gesture" }
            override fun gestureMove(a: Int, b: Int) {}
            override fun gestureEnd(a: Int, b: Int) {}
        })
        r.selectTool = true
        r.fingerMode = FingerMode.PAN          // even with "paint with a finger" off, a finger selects
        r.down(0, ToolKind.FINGER, 0, 10f, 10f)
        r.move()
        r.up(0, false)
        assertEquals(listOf("begin 0", "move", "end"), log)
        // A second finger right away: a pinch, the shape is cancelled and the view moves.
        log.clear()
        r.down(0, ToolKind.FINGER, 0, 10f, 10f)
        r.pointerDown(1, ToolKind.FINGER, 30, 200f, 200f)
        assertEquals(listOf("begin 0", "cancel", "gesture"), log)
    }
}
