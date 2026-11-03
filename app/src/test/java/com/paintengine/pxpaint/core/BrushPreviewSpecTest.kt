package com.paintengine.pxpaint.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class BrushPreviewSpecTest {
    /** Parsed preview stroke list: (diameter, opacity, samples of 6 floats). */
    private data class Stroke(val d: Float, val o: Float, val pts: List<FloatArray>)

    private fun parse(a: FloatArray): List<Stroke> {
        val n = a[0].toInt()
        var i = 1
        val out = ArrayList<Stroke>()
        repeat(n) {
            val count = a[i].toInt()
            val d = a[i + 1]
            val o = a[i + 2]
            i += 3
            out += Stroke(d, o, (0 until count).map { k -> a.copyOfRange(i + k * 6, i + k * 6 + 6) })
            i += count * 6
        }
        assertEquals("no trailing data", a.size, i)
        return out
    }

    @Test
    fun thumbnailIsOneStrokeInsideTheImage() {
        for (p in BrushPresetsTest.builtIns()) {
            val s = parse(BrushPreviewSpec.thumbnail(p.params, 300, 100)).single()
            assertTrue(p.id, s.d <= 50f && s.d >= 2f)
            assertTrue(p.id, s.d <= p.params.size || s.d == 2f)
            for (pt in s.pts) {
                assertTrue(pt[0] - s.d / 2 >= 0 && pt[0] + s.d / 2 <= 300)
                assertTrue(pt[1] - s.d / 2 >= -0.01 && pt[1] + s.d / 2 <= 100.01)
                assertTrue("thumbnails carry no pressure", pt[3].isNaN())
            }
        }
    }

    @Test
    fun detailShowsStrokeSizesOpacitiesAndPressureOnlyIfUsed() {
        val withPressure = BrushParams(size = 30f, pressureSize = true)
        val d = parse(BrushPreviewSpec.detail(withPressure, 800, 400).first)
        assertEquals(8, d.size)                                       // stroke, pressure, 3 sizes, 3 opacities
        assertTrue(d[0].pts.all { it[3].isNaN() })                  // the finger stroke has no pressure
        assertTrue(d[1].pts.all { it[3] in 0f..1f })                 // the pressure-response row
        assertEquals(listOf(0.25f, 0.5f, 1f), d.takeLast(3).map { it.o })
        val sizes = d.subList(2, 5).map { it.d }
        assertTrue(sizes[0] < sizes[1] && sizes[1] < sizes[2])
        val plain = parse(BrushPreviewSpec.detail(BrushParams(size = 30f), 800, 400).first)
        assertEquals(7, plain.size)
        assertTrue(plain.all { s -> s.pts.all { it[3].isNaN() } })   // nothing pressure-like for a brush without dynamics
        assertEquals(4, BrushPreviewSpec.detail(withPressure, 800, 400).second.size)
    }

    @Test
    fun cacheKeyFollowsEveryInput() {
        val p = BrushParams()
        val k = BrushPreviewSpec.key(p, "T", 100, 40, 1, 2, 3)
        assertEquals(k, BrushPreviewSpec.key(p.copy(), "T", 100, 40, 1, 2, 3))
        assertNotEquals(k, BrushPreviewSpec.key(p.copy(wetEdge = 0.1f), "T", 100, 40, 1, 2, 3))
        assertNotEquals(k, BrushPreviewSpec.key(p, "D", 100, 40, 1, 2, 3))
        assertNotEquals(k, BrushPreviewSpec.key(p, "T", 101, 40, 1, 2, 3))
        assertNotEquals(k, BrushPreviewSpec.key(p, "T", 100, 40, 1, 2, 4))
    }
}
