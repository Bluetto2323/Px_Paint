package com.paintengine.pxpaint.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class PressureTest {
    @Test
    fun normalisationUsesTheDeviceRangeAndClamps() {
        assertEquals(0.5f, PressureNormalizer.normalize(0.5f, 0f, 1f), 1e-6f)
        assertEquals(0.5f, PressureNormalizer.normalize(2048f, 0f, 4096f), 1e-6f)   // raw-unit range
        assertEquals(1f, PressureNormalizer.normalize(1.3f, 0f, 1f), 0f)             // driver overshoot
        assertEquals(0f, PressureNormalizer.normalize(-0.1f, 0f, 1f), 0f)
        assertEquals(0.7f, PressureNormalizer.normalize(0.7f, 1f, 1f), 1e-6f)        // degenerate range -> nominal
        assertEquals(0.25f, PressureNormalizer.normalize(0.25f, Float.NaN, Float.NaN), 1e-6f)
        assertTrue(PressureNormalizer.normalize(Float.NaN, 0f, 1f).isNaN())         // missing stays missing
    }

    @Test
    fun toolTypesAreClassified() {
        assertEquals(ToolKind.FINGER, ToolKind.fromToolType(1))
        assertEquals(ToolKind.STYLUS, ToolKind.fromToolType(2))
        assertEquals(ToolKind.MOUSE, ToolKind.fromToolType(3))
        assertEquals(ToolKind.ERASER, ToolKind.fromToolType(4))
        assertEquals(ToolKind.FINGER, ToolKind.fromToolType(5))   // palm: never a stylus
        assertEquals(ToolKind.UNKNOWN, ToolKind.fromToolType(0))
        assertTrue(ToolKind.STYLUS.isStylus && ToolKind.ERASER.isStylus)
        assertFalse(ToolKind.FINGER.isStylus || ToolKind.MOUSE.isStylus)
    }

    @Test
    fun curvesAreMonotoneAndPinnedToTheEnds() {
        for (c in listOf(PressureCurve.LINEAR, PressureCurve.SOFT, PressureCurve.FIRM, PressureCurve.S_CURVE,
            PressureCurve(0.9f, 0f, 0.1f, 1f), PressureCurve(0.2f, 0.9f, 0.3f, 0.1f))) {
            assertEquals(0f, c.eval(0f), 1e-6f)
            assertEquals(1f, c.eval(1f), 1e-6f)
            var prev = 0f
            for (i in 0..1000) {
                val v = c.eval(i / 1000f)
                assertTrue("$c not monotone at ${i / 1000f}", v >= prev - 1e-6f)
                assertTrue(v in 0f..1f)
                prev = v
            }
        }
        // Passes through its control points.
        val s = PressureCurve.SOFT
        assertEquals(s.y1, s.eval(s.x1), 1e-5f)
        assertEquals(s.y2, s.eval(s.x2), 1e-5f)
        // The linear preset is the identity.
        for (i in 0..20) assertEquals(i / 20f, PressureCurve.LINEAR.eval(i / 20f), 1e-4f)
        // Soft gives more at light pressure, firm less.
        assertTrue(PressureCurve.SOFT.eval(0.2f) > 0.3f && PressureCurve.FIRM.eval(0.2f) < 0.1f)
    }

    @Test
    fun sensitivityAndTable() {
        assertEquals(1f, PressureCurve.gamma(0f), 1e-6f)
        assertTrue(PressureCurve.gamma(1f) < 0.4f && PressureCurve.gamma(-1f) > 2.5f)
        assertEquals(PressureCurve.gamma(1f), PressureCurve.gamma(5f), 0f)    // clamped
        val t = PressureCurve.LINEAR.table(0f)
        assertEquals(PressureCurve.TABLE_SIZE, t.size)
        assertEquals(0f, t[0], 0f)
        assertEquals(1f, t[t.size - 1], 0f)
        assertEquals(0.5f, t[128], 1e-3f)
        val sensitive = PressureCurve.LINEAR.table(1f)
        assertTrue("light touch counts more", sensitive[64] > t[64] + 0.2f)
        for (curve in listOf(PressureCurve.FIRM, PressureCurve.S_CURVE))
            for (sens in listOf(-1f, -0.3f, 0f, 0.6f, 1f)) {
                val tab = curve.table(sens)
                for (i in 1 until tab.size) assertTrue(tab[i] >= tab[i - 1])
                assertEquals(curve.response(0.37f, sens), tab[95] + (tab[96] - tab[95]) * (0.37f * 256 - 95), 2e-3f)
            }
    }

    @Test
    fun curveEncodingAndSanitising() {
        val c = PressureCurve(0.3f, 0.2f, 0.7f, 0.9f)
        assertEquals(c, PressureCurve.decode(c.encode()))
        assertNull(PressureCurve.decode("1;2;3"))
        assertNull(PressureCurve.decode(null))
        val s = PressureCurve(0.8f, 0.9f, 0.2f, 0.1f).sanitized()   // crossed points -> ordered
        assertTrue(s.x1 < s.x2 && s.y1 <= s.y2)
    }
}
