package com.paintengine.pxpaint.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class HistoryTest {
    // 3 steps done (stroke on "Layer 1", new layer, opacity on "Ink\tA"), 2 undone (next redo first).
    private val table = "history\t3\t2\t123456\t134217728\t100\t4\t0\n" +
        "0\t0\tLayer 1\n4\t0\tLayer 2\n11\t0\tInk\\tA\n13\t1\tInk\n1\t1\tLayer 1\n"

    @Test
    fun engineTableParsing() {
        val h = HistoryState.parse(table)!!
        assertEquals(3, h.undo)
        assertEquals(2, h.redo)
        assertEquals(123456L, h.bytes)
        assertEquals(134217728L, h.budget)
        assertEquals(100, h.maxSteps)
        assertEquals(4L, h.dropped)
        assertEquals(0L, h.refused)
        assertTrue(h.canUndo)
        assertTrue(h.canRedo)
        assertEquals(listOf(HistoryKind.STROKE, HistoryKind.ADD_LAYER, HistoryKind.OPACITY, HistoryKind.MERGE_DOWN, HistoryKind.ERASE),
            h.steps.map { it.kind })
        assertEquals("Ink\tA", h.steps[2].label)
        assertEquals(listOf(false, false, false, true, true), h.steps.map { it.undone })
    }

    @Test
    fun everyEngineKindHasAName() {
        // Engine HistoryKind 0..14 (engine/History.h); unknown codes fall back to a generic name.
        for (code in 0..14) assertTrue("kind $code", HistoryKind.of(code) != HistoryKind.OTHER)
        assertEquals(HistoryKind.OTHER, HistoryKind.of(99))
        assertEquals(HistoryKind.OTHER, HistoryKind.of(-3))
    }

    @Test
    fun malformedTables() {
        assertNull(HistoryState.parse(null))
        assertNull(HistoryState.parse(""))
        assertNull(HistoryState.parse("active\t1\n"))
        assertNull(HistoryState.parse("history\t1\t0\n"))
        assertNull(HistoryState.parse("history\tx\t0\t0\t0\t0\t0\t0\n"))
        assertNull(HistoryState.parse("history\t-1\t0\t0\t0\t0\t0\t0\n"))
        // A list that disagrees with the counts is dropped (the counts still drive the buttons).
        val h = HistoryState.parse("history\t2\t0\t0\t0\t0\t0\t0\n0\t0\tA\n")!!
        assertEquals(2, h.undo)
        assertTrue(h.steps.isEmpty())
        val g = HistoryState.parse("history\t1\t1\t0\t0\t0\t0\t0\n0\t1\tA\n1\t0\tB\n")!!   // undone flags in the wrong place
        assertTrue(g.steps.isEmpty())
        // Short lines are skipped, not fatal.
        val k = HistoryState.parse("history\t1\t0\t0\t0\t0\t0\t0\n0\n0\t0\tA\n")!!
        assertEquals(1, k.steps.size)
    }

    @Test
    fun emptyHistory() {
        val h = HistoryState.parse("history\t0\t0\t0\t67108864\t100\t0\t0\n")!!
        assertFalse(h.canUndo)
        assertFalse(h.canRedo)
        assertTrue(h.steps.isEmpty())
        assertEquals(HistoryState.EMPTY.undo, h.undo)
    }

    @Test
    fun movingToAStep() {
        val h = HistoryState.parse(table)!!       // 3 done, 2 undone
        assertEquals(0, h.moveTo(2))               // the current step
        assertEquals(-1, h.moveTo(1))
        assertEquals(-3, h.moveTo(-1))             // Start: undo everything
        assertEquals(1, h.moveTo(3))               // redo one
        assertEquals(2, h.moveTo(4))
        assertEquals(2, h.moveTo(40))              // clamped to what exists
        assertEquals(-3, h.moveTo(-9))
    }

    @Test
    fun counts() {
        val c = HistoryState.counts(5, 0)
        assertTrue(c.canUndo)
        assertFalse(c.canRedo)
        assertEquals(0, HistoryState.counts(-1, -2).undo)
    }

    @Test
    fun memoryBudgetFollowsTheDevice() {
        val mib = 1024L * 1024L
        val gib = 1024L * mib
        assertEquals(64 * mib, HistoryState.budgetForRam(2 * gib))
        assertEquals(128 * mib, HistoryState.budgetForRam(4 * gib))
        assertEquals(192 * mib, HistoryState.budgetForRam(6 * gib))
        assertEquals(192 * mib, HistoryState.budgetForRam(16 * gib))
        assertEquals(32 * mib, HistoryState.budgetForRam(512 * mib))
        assertEquals(64 * mib, HistoryState.budgetForRam(0))
        assertEquals(64 * mib, HistoryState.budgetForRam(-1))
    }

    @Test
    fun everyHistoryStringIsTranslated() {
        for (k in HistoryKind.entries) {
            assertTrue(k.label.en.isNotBlank())
            assertTrue(k.label.it.isNotBlank())
        }
        assertEquals("Undone: Stroke", String.format(S.UNDONE.en, "Stroke"))
        assertEquals("3 passaggi · 1 MB di 64 MB", String.format(S.HISTORY_MEMORY.it, 3, "1 MB", "64 MB"))
    }
}
