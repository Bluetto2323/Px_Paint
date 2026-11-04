package com.paintengine.pxpaint.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.abs

/** Block 8: colour math, gestures, design tokens, new settings. */
class WorkspaceUiTest {
    // ------------------------------------------------------------ colour
    @Test
    fun hsvRoundTripIsExactForEveryByteColour() {
        // Every 5th value of each channel: hsv -> argb gives back the same bytes.
        var worst = 0
        for (r in 0..255 step 5) for (g in 0..255 step 5) for (b in 0..255 step 5) {
            val c = ColorMath.rgb(r, g, b)
            val hsv = ColorMath.argbToHsv(c)
            val back = ColorMath.hsvToArgb(hsv[0], hsv[1], hsv[2])
            worst = maxOf(worst, abs(ColorMath.red(back) - r), abs(ColorMath.green(back) - g), abs(ColorMath.blue(back) - b))
        }
        assertEquals(0, worst)
    }

    @Test
    fun knownColours() {
        assertEquals(0xFFFF0000.toInt(), ColorMath.hsvToArgb(0f, 1f, 1f))
        assertEquals(0xFF00FF00.toInt(), ColorMath.hsvToArgb(120f, 1f, 1f))
        assertEquals(0xFF0000FF.toInt(), ColorMath.hsvToArgb(240f, 1f, 1f))
        assertEquals(0xFF0000FF.toInt(), ColorMath.hsvToArgb(600f, 1f, 1f))        // hue wraps
        assertEquals(0xFF808080.toInt(), ColorMath.hsvToArgb(77f, 0f, 128 / 255f))  // grey: hue irrelevant
        val h = ColorMath.argbToHsv(0xFF2B3A67.toInt())
        assertTrue(abs(h[0] - 225f) < 0.5f)
    }

    @Test
    fun hexInput() {
        assertEquals(0xFF2B3A67.toInt(), ColorMath.parseHex("#2B3A67"))
        assertEquals(0xFF2B3A67.toInt(), ColorMath.parseHex(" 2b3a67 "))
        assertEquals(0xFFFFAA00.toInt(), ColorMath.parseHex("#fa0"))
        assertNull(ColorMath.parseHex("#12345"))
        assertNull(ColorMath.parseHex("#GG0000"))
        assertNull(ColorMath.parseHex(""))
        assertNull(ColorMath.parseHex("#11223344"))   // alpha is not a brush colour
        assertEquals("#2B3A67", ColorMath.toHex(0xFF2B3A67.toInt()))
        assertEquals("#000000", ColorMath.toHex(0))
    }

    @Test
    fun eyedropperUnpremultiplies() {
        assertEquals(0xFF336699.toInt(), ColorMath.unpremultiply(0x33, 0x66, 0x99, 255))
        // 50 % alpha: premultiplied 0x40 -> straight 0x80.
        assertEquals(0xFF808080.toInt(), ColorMath.unpremultiply(0x40, 0x40, 0x40, 128))
        assertEquals(0, ColorMath.unpremultiply(0, 0, 0, 0))
    }

    @Test
    fun savedPalette() {
        var st = BrushState()
        st = st.withSaved(0xFF112233.toInt()).withSaved(0x00445566).withSaved(0xFF112233.toInt())
        assertEquals(listOf(0xFF112233.toInt(), 0xFF445566.toInt()), st.palette)   // opaque, no duplicates
        val back = BrushState.decode(st.encode())
        assertEquals(st.palette, back.palette)
        assertEquals(listOf(0xFF445566.toInt()), st.withoutSaved(0xFF112233.toInt()).palette)
        var full = BrushState()
        for (i in 0 until 40) full = full.withSaved(0xFF000000.toInt() or i)
        assertEquals(BrushState.MAX_PALETTE, full.palette.size)
        // Settings saved by Block 7 have no palette: empty, nothing else changes.
        val old = BrushState.decode(mapOf("color" to "123"))
        assertTrue(old.palette.isEmpty())
        assertEquals(123, old.color)
    }

    // ------------------------------------------------------------ gestures
    private fun tap(fingers: Int, gapMs: Long = 20, holdMs: Long = 120, moveBy: Float = 0f, stylus: Boolean = false,
                    stroke: Boolean = false, g: TapGesture = TapGesture(slopPx = 24f)): GestureShortcut? {
        for (i in 0 until fingers) g.pointerDown(i, i * gapMs, 100f + i * 80, 300f, finger = !(stylus && i == 0))
        for (i in 0 until fingers) g.pointerMove(i, 100f + i * 80 + moveBy, 300f)
        if (stroke) g.strokeCommitted()
        var r: GestureShortcut? = null
        for (i in 0 until fingers) r = g.pointerUp(i, (fingers - 1) * gapMs + holdMs + i * 5)
        return r
    }

    @Test
    fun fingerTaps() {
        assertEquals(GestureShortcut.UNDO, tap(2))
        assertEquals(GestureShortcut.REDO, tap(3))
        assertEquals(GestureShortcut.FULLSCREEN, tap(4))
        assertNull(tap(1))                           // a dot
        assertNull(tap(5))
    }

    @Test
    fun paintingNeverTriggersAShortcut() {
        assertNull(tap(2, moveBy = 60f))             // a pinch / pan
        assertNull(tap(2, holdMs = 900))             // a hold
        assertNull(tap(2, gapMs = 400))              // a second finger long after the first
        assertNull(tap(2, stylus = true))            // stylus involved
        assertNull(tap(2, stroke = true))            // a stroke was committed
    }

    @Test
    fun tapsCanBeTurnedOff() {
        val g = TapGesture(24f).apply { undoEnabled = false; redoEnabled = false; fullscreenEnabled = false }
        assertNull(tap(2, g = g))
        assertNull(tap(3, g = g))
        assertNull(tap(4, g = g))
    }

    @Test
    fun sequencesAreIndependent() {
        val g = TapGesture(24f)
        assertNull(tap(2, moveBy = 60f, g = g))
        assertEquals(GestureShortcut.UNDO, tap(2, g = g))   // the failed pinch does not poison the next tap
        g.pointerDown(0, 0, 0f, 0f, true)
        g.cancel()
        assertEquals(GestureShortcut.REDO, tap(3, g = g))
    }

    @Test
    fun quickPinchFits() {
        assertTrue(QuickPinch.isFit(400.0, 180.0, 200))
        assertTrue(!QuickPinch.isFit(400.0, 180.0, 900))    // slow: ordinary zoom
        assertTrue(!QuickPinch.isFit(400.0, 300.0, 200))    // not closed enough
        assertTrue(!QuickPinch.isFit(0.0, 0.0, 100))
    }

    // ------------------------------------------------------------ tokens / accessibility
    @Test
    fun textContrastMeetsWcagOnEverySurface() {
        for (mode in ThemeMode.entries) for (hc in listOf(false, true)) {
            val c = DesignTokens.colors(mode, hc)
            for (surface in listOf(c.bg, c.surface, c.raised)) {
                val name = "$mode hc=$hc surface=${ColorMath.toHex(surface)}"
                assertTrue("$name text", ColorMath.contrast(c.text, surface) >= 7.0)
                assertTrue("$name secondary", ColorMath.contrast(c.textSecondary, surface) >= 4.5)
                assertTrue("$name tertiary", ColorMath.contrast(c.textTertiary, surface) >= 3.0)
                assertTrue("$name accent", ColorMath.contrast(c.accent, surface) >= 4.5)
                assertTrue("$name danger", ColorMath.contrast(c.danger, surface) >= 4.5)
            }
            assertTrue("$mode on accent", ColorMath.contrast(c.onAccent, c.accent) >= 4.5)
            // The translucent glass over a white or black canvas still keeps text readable.
            for (under in listOf(0xFFFFFFFF.toInt(), 0xFF000000.toInt())) {
                for (glass in listOf(c.chrome, c.panel)) {
                    val g = ColorMath.over(glass, under)
                    assertTrue("$mode glass text over ${ColorMath.toHex(under)}", ColorMath.contrast(c.text, g) >= 7.0)
                    assertTrue("$mode glass secondary over ${ColorMath.toHex(under)}", ColorMath.contrast(c.textSecondary, g) >= 4.5)
                    assertTrue("$mode glass accent over ${ColorMath.toHex(under)}", ColorMath.contrast(c.accent, g) >= 3.0)
                }
            }
        }
    }

    @Test
    fun threeDistinctThemesWithTheRequestedCharacter() {
        val blue = DesignTokens.colors(ThemeMode.DEEP_BLUE, false)
        val black = DesignTokens.colors(ThemeMode.PURE_BLACK, false)
        val beige = DesignTokens.colors(ThemeMode.WARM_BEIGE, false)
        assertEquals(3, ThemeMode.entries.size)
        // The suggested palettes, as given (accent / secondary of the beige theme darkened for contrast).
        assertEquals(0xFF101A2B.toInt(), blue.bg); assertEquals(0xFF20344E.toInt(), blue.raised); assertEquals(0xFF6FA8E8.toInt(), blue.accent)
        assertEquals(0xFF08090B.toInt(), black.bg); assertEquals(0xFF1B1E23.toInt(), black.raised); assertEquals(0xFFA6B7D4.toInt(), black.accent)
        assertEquals(0xFFD8C8A8.toInt(), beige.bg); assertEquals(0xFFF0E6D3.toInt(), beige.raised); assertEquals(0xFF30291F.toInt(), beige.text)
        assertTrue(blue.isDark && black.isDark && !beige.isDark)
        // Deep Blue is blue (b > r), Pure Black near black, Warm Beige warm (r > b).
        assertTrue((blue.bg and 0xFF) > ((blue.bg shr 16) and 0xFF))
        assertTrue(ColorMath.luminance(black.bg) < 0.005)
        assertTrue(((beige.bg shr 16) and 0xFF) > (beige.bg and 0xFF))
        // Glass: translucent, the chrome more than panels.
        for (c in listOf(blue, black, beige)) {
            assertTrue((c.chrome ushr 24) in 0xC0..0xF0)
            assertTrue((c.panel ushr 24) > (c.chrome ushr 24) && (c.panel ushr 24) < 0xFF)
        }
    }

    @Test
    fun legacyThemeNamesMigrate() {
        assertEquals(ThemeMode.DEEP_BLUE, ThemeMode.parse("DARK"))
        assertEquals(ThemeMode.DEEP_BLUE, ThemeMode.parse("SYSTEM"))
        assertEquals(ThemeMode.WARM_BEIGE, ThemeMode.parse("LIGHT"))
        assertEquals(ThemeMode.PURE_BLACK, ThemeMode.parse("PURE_BLACK"))
        assertEquals(null, ThemeMode.parse("NEON"))
        assertEquals(ThemeMode.DEEP_BLUE, AppSettings.decode(mapOf(AppSettings.K_THEME to "NEON")).theme)
        assertEquals(ThemeMode.DEEP_BLUE, AppSettings().theme)
    }

    @Test
    fun touchAndPanelTokens() {
        assertTrue(DesignTokens.TOUCH >= 48)
        assertTrue(DesignTokens.TOUCH_LARGE > DesignTokens.TOUCH)
        assertTrue(DesignTokens.PANEL_MIN <= DesignTokens.PANEL_DEFAULT && DesignTokens.PANEL_DEFAULT <= DesignTokens.PANEL_MAX)
        assertTrue(DesignTokens.SLIDER_WIDTH * 1.33f >= 48 || DesignTokens.SLIDER_LENGTH >= 48)
        assertTrue(DesignTokens.DISABLED_ALPHA < DesignTokens.DIMMED_ALPHA)
    }

    // ------------------------------------------------------------ settings
    @Test
    fun newSettingsRoundTripAndOldSettingsKeepDefaults() {
        val st = AppSettings(tapUndo = false, tapRedo = false, quickPinchFit = false, tapFullscreen = false,
            stylusButtonPick = false, sliderSide = Side.RIGHT, pinnedPanel = PinnedPanel.LAYERS, panelWidthDp = 400)
        assertEquals(st, AppSettings.decode(st.encode()))
        val old = AppSettings.decode(mapOf(AppSettings.K_THEME to "LIGHT"))
        assertEquals(ThemeMode.WARM_BEIGE, old.theme)   // Blocks 2-10 light theme -> Warm Beige
        assertTrue(old.tapUndo && old.tapRedo && old.quickPinchFit && old.stylusButtonPick)
        assertEquals(PinnedPanel.NONE, old.pinnedPanel)
        assertEquals(LayoutPolicy.PANEL_DEFAULT_DP, old.panelWidthDp)
        assertEquals(LayoutPolicy.PANEL_MAX_DP, AppSettings.decode(mapOf(AppSettings.K_PANEL_WIDTH to "5000")).panelWidthDp)
    }
}
