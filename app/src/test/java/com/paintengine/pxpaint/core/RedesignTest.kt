package com.paintengine.pxpaint.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** Block 11: layer drag arithmetic, the minimal selection bar, brush selector data. */
class RedesignTest {
    // Five rows, 60 px tall, 4 px apart (list order: top layer first).
    private val tops = IntArray(5) { it * 64 }
    private val heights = IntArray(5) { 60 }

    @Test
    fun dragTargetFollowsTheRowsMiddle() {
        // Not moved: stays.
        assertEquals(2, LayerDrag.target(tops, heights, 2, 2 * 64 + 30f))
        // Dragged up past the middle of row 1: lands at 1; past row 0's middle: 0.
        assertEquals(1, LayerDrag.target(tops, heights, 2, 64 + 20f))
        assertEquals(0, LayerDrag.target(tops, heights, 2, 10f))
        // Down past row 3 / row 4.
        assertEquals(3, LayerDrag.target(tops, heights, 2, 3 * 64 + 40f))
        assertEquals(4, LayerDrag.target(tops, heights, 2, 9999f))
        assertEquals(0, LayerDrag.target(tops, heights, 2, -9999f))
    }

    @Test
    fun rowsOpenAGapWhereTheLayerLands() {
        val step = 64
        // Row 1 dragged down to position 3: rows 2 and 3 move up by one step, the others stay.
        assertEquals(listOf(0, 0, -step, -step, 0), (0 until 5).map { LayerDrag.shift(it, 1, 3, step) })
        // Row 3 dragged up to 0: rows 0..2 move down.
        assertEquals(listOf(step, step, step, 0, 0), (0 until 5).map { LayerDrag.shift(it, 3, 0, step) })
        assertEquals(List(5) { 0 }, (0 until 5).map { LayerDrag.shift(it, 2, 2, step) })
        // The landing slot is where the row will be laid out after the move.
        assertEquals(0, LayerDrag.landingTop(tops, heights, 3, 0))
        assertEquals(3 * 64, LayerDrag.landingTop(tops, heights, 1, 3))
        assertEquals(4 * 64, LayerDrag.landingTop(tops, heights, 0, 4))
    }

    @Test
    fun listPositionToEngineIndex() {
        // 5 layers: the top row (0) is engine index 4, the bottom row (4) index 0.
        assertEquals(4, LayerDrag.engineIndex(0, 5))
        assertEquals(0, LayerDrag.engineIndex(4, 5))
        assertEquals(2, LayerDrag.engineIndex(2, 5))
    }

    @Test
    fun autoScrollOnlyNearTheEdges() {
        assertEquals(0f, LayerDrag.autoScroll(500f, 0f, 1000f, 60f, 20f))
        assertEquals(-20f, LayerDrag.autoScroll(0f, 0f, 1000f, 60f, 20f))
        assertEquals(20f, LayerDrag.autoScroll(1000f, 0f, 1000f, 60f, 20f))
        assertEquals(-10f, LayerDrag.autoScroll(30f, 0f, 1000f, 60f, 20f), 1e-4f)
        assertEquals(20f, LayerDrag.autoScroll(1300f, 0f, 1000f, 60f, 20f))   // beyond the edge: full speed
        assertEquals(0f, LayerDrag.autoScroll(10f, 0f, 100f, 60f, 20f))       // too short to scroll by edge zones
        assertTrue(LayerDrag.LONG_PRESS_MS in 200..400)
    }

    @Test
    fun selectionBarIsMinimal() {
        val all = SelectionAction.available(active = true, clipboard = true, moving = false, editable = true, polygonPoints = 0)
        val (bar, overflow) = SelectionAction.split(all)
        assertEquals(listOf(SelectionAction.MOVE, SelectionAction.COPY, SelectionAction.PASTE, SelectionAction.CLEAR, SelectionAction.DESELECT), bar)
        assertEquals(listOf(SelectionAction.COPY_MERGED, SelectionAction.CUT, SelectionAction.DUPLICATE, SelectionAction.INVERT), overflow)
        // Nothing is lost: every available action is in one of the two.
        assertEquals(all.toSet(), (bar + overflow).toSet())
        val moving = SelectionAction.split(SelectionAction.available(true, true, true, true, 0))
        assertTrue(moving.second.isEmpty())
    }

    @Test
    fun brushSelectorDataComesFromTheLibrary() {
        // The selector shows the library's own presets and favourites: no duplicate definitions.
        var lib = BrushLibrary.migrate(BrushPresetsTest.builtIns(), null)
        val ids = lib.list(BrushFilter.ALL, "").map { it.id }
        assertEquals(ids.size, ids.toSet().size)
        val first = ids.first()
        lib = lib.toggleFavorite(first)
        assertEquals(listOf(first), lib.list(BrushFilter.FAVORITES, "").map { it.id })
        lib = lib.select(BrushTool.PAINT, ids[1])
        assertEquals(ids[1], lib.recents.first())
        assertFalse(lib.list(BrushFilter.ALL, "zzzz-no-such-brush").any())
    }
}
