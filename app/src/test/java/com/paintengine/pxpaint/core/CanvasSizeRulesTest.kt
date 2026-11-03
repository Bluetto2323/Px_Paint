package com.paintengine.pxpaint.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class CanvasSizeRulesTest {
    private val mib = 1024L * 1024L
    private val big = 16384

    @Test
    fun presetsAreValidAndScreenIsPortrait() {
        val presets = CanvasSizeRules.presets(2400, 1080)
        val screen = presets.first { it.kind == CanvasSizeRules.PresetKind.SCREEN }
        assertEquals(1080, screen.width)
        assertEquals(2400, screen.height)
        for (p in presets) {
            assertTrue("${p.kind}", CanvasSizeRules.estimate(p.width, p.height, big, 512 * mib).ok)
        }
        val a4 = presets.first { it.kind == CanvasSizeRules.PresetKind.A4 }
        assertEquals(2480 to 3508, a4.width to a4.height)
    }

    @Test
    fun singleTextureUpTo2048ThenTiles() {
        assertFalse(CanvasSizeRules.estimate(2048, 2048, big, 0).tiled)
        val e = CanvasSizeRules.estimate(2049, 1000, big, 0)
        assertTrue(e.tiled)
        assertEquals(9, e.tileColumns)
        assertEquals(4, e.tileRows)
    }

    @Test
    fun gpuLimitForcesTiles() {
        assertTrue(CanvasSizeRules.estimate(1500, 1500, 1024, 0).tiled)
        assertFalse(CanvasSizeRules.estimate(1024, 1024, 1024, 0).tiled)
    }

    @Test
    fun bytesMatchTheEngineStoragePolicy() {
        // Same numbers as pe_canvas_test: GPU copy (258^2 textures) + CPU raster (256^2 tiles).
        val e = CanvasSizeRules.estimate(4096, 4096, big, 0)
        assertEquals(16L * 16 * 258 * 258 * 4 + 16L * 16 * 256 * 256 * 4, e.maxBytes)
        assertEquals(1024L * 1024 * 4 * 2, CanvasSizeRules.estimate(1024, 1024, big, 0).maxBytes)
        assertEquals(1000L * 600 * 4 + 4L * 3 * 256 * 256 * 4, CanvasSizeRules.estimate(1000, 600, big, 0).maxBytes)
    }

    @Test
    fun limitsAndBudget() {
        assertEquals(CanvasSizeRules.SizeError.TOO_SMALL, CanvasSizeRules.estimate(0, 10, big, 0).error)
        assertEquals(CanvasSizeRules.SizeError.TOO_LARGE, CanvasSizeRules.estimate(16385, 10, big, 0).error)
        assertTrue(CanvasSizeRules.estimate(16384, 1, big, 0).ok)
        val over = CanvasSizeRules.estimate(8192, 8192, big, 64 * mib)
        assertFalse(over.ok)
        assertEquals(CanvasSizeRules.SizeError.OVER_BUDGET, over.error)
    }

    @Test
    fun ramBudgetIsClamped() {
        assertEquals(256 * mib, CanvasSizeRules.budgetForRam(512 * mib))
        assertEquals(1536 * mib, CanvasSizeRules.budgetForRam(64L * 1024 * mib))
        assertEquals(1024 * mib, CanvasSizeRules.budgetForRam(4096 * mib))
    }

    @Test
    fun largestSquareFitsAndNextDoesNot() {
        val budget = 256 * mib
        val side = CanvasSizeRules.largestSquareSide(big, budget)
        assertTrue(CanvasSizeRules.estimate(side, side, big, budget).ok)
        assertFalse(CanvasSizeRules.estimate(side + CanvasSizeRules.TILE_SIZE, side + CanvasSizeRules.TILE_SIZE, big, budget).ok)
    }
}
