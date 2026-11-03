package com.paintengine.pxpaint.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class BrushStateTest {
    @Test
    fun encodeDecodeRoundTrip() {
        val s = BrushState(ToolBrush(7.5f, 0.4f, 0.12f), ToolBrush(120f, 0.8f, 0.5f), 0x80FF0000.toInt(), listOf(1, 2, 3))
        assertEquals(s, BrushState.decode(s.encode()))
    }

    @Test
    fun invalidValuesFallBackOrClamp() {
        val d = BrushState()
        val s = BrushState.decode(mapOf("marker" to "9999;7;0", "eraser" to "x;y;z", "color" to "nope", "recent" to "1,a,3"))
        assertEquals(BrushState.MAX_SIZE, s.marker.size, 0f)
        assertEquals(1f, s.marker.opacity, 0f)
        assertEquals(BrushState.MIN_SPACING, s.marker.spacing, 0f)
        assertEquals(d.eraser, s.eraser)
        assertEquals(d.color, s.color)
        assertEquals(listOf(1, 3), s.recentColors)
        assertEquals(d, BrushState.decode(emptyMap()))
    }

    @Test
    fun recentColoursAreUniqueNewestFirstAndBounded() {
        var s = BrushState()
        for (c in 1..12) s = s.withRecent(c)
        s = s.withRecent(5)
        assertEquals(BrushState.MAX_RECENT, s.recentColors.size)
        assertEquals(5, s.recentColors[0])
        assertEquals(1, s.recentColors.count { it == 5 })
    }

    @Test
    fun sliderMappingsCoverTheRangeAndRoundTrip() {
        assertEquals(BrushState.MIN_SIZE, BrushState.sliderToSize(0f), 0f)
        assertEquals(BrushState.MAX_SIZE, BrushState.sliderToSize(1f), 0f)
        for (size in listOf(1f, 2.5f, 12f, 64f, 300f)) {
            val back = BrushState.sliderToSize(BrushState.sizeToSlider(size))
            assertTrue("size $size -> $back", kotlin.math.abs(back - size) <= maxOf(0.25f, size * 0.01f))
        }
        assertEquals(0.02f, BrushState.sliderToSpacing(0f), 1e-6f)
        assertEquals(1.0f, BrushState.sliderToSpacing(1f), 1e-6f)
        assertEquals(0.01f, BrushState.sliderToOpacity(0f), 1e-6f)
        assertEquals(0.5f, BrushState.sliderToOpacity(0.5f), 1e-6f)
        assertEquals("24 px", BrushState.sizeLabel(24f))
        assertEquals("2.5 px", BrushState.sizeLabel(2.5f))
        assertEquals("1 px", BrushState.sizeLabel(1f))
    }
}
