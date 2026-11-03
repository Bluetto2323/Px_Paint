package com.paintengine.pxpaint.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class LayoutPolicyTest {
    private val s = AppSettings()

    private fun ws(w: Int, h: Int, sw: Int = minOf(w, h), st: AppSettings = s) = LayoutPolicy.workspace(w, h, sw, st)

    @Test
    fun phonePortrait() {
        // Galaxy A33: 1080x2400 px at ~2.6 density -> ~411x914 dp.
        val w = ws(411, 914)
        assertEquals(FormFactor.PHONE, w.formFactor)
        assertEquals(LayoutKind.PHONE_PORTRAIT, w.kind)
        assertEquals(ToolbarPlacement.BOTTOM, w.toolbar)
        assertEquals(PanelStyle.BOTTOM_SHEET, w.panelStyle)
        assertEquals(Side.LEFT, w.sliderSide)
        assertFalse(w.canPin)
        assertEquals(PinnedPanel.NONE, w.pinned)
        assertFalse(w.compactTopBar)
        assertTrue(w.showTitle)
        assertEquals(48, w.touchTargetDp)
        // A bottom sheet never takes more than ~55 % of the height: the canvas stays visible.
        assertEquals(502, w.sheetMaxHeightDp)
    }

    @Test
    fun phoneLandscapeUsesARailAndSideSheetsOnTheOtherSide() {
        val w = ws(914, 411)
        assertEquals(LayoutKind.PHONE_LANDSCAPE, w.kind)
        assertEquals(ToolbarPlacement.SIDE, w.toolbar)
        assertEquals(Side.LEFT, w.railSide)
        assertEquals(Side.RIGHT, w.sliderSide)
        assertEquals(PanelStyle.SIDE_SHEET, w.panelStyle)
        assertEquals(Side.RIGHT, w.panelSide)
        assertTrue(w.compactTopBar)
        // The side sheet leaves more than half of the width to the canvas.
        assertTrue(w.panelWidthDp <= 914 * 0.45)
        val right = ws(914, 411, st = s.copy(dockSide = DockSide.RIGHT))
        assertEquals(Side.RIGHT, right.railSide)
        assertEquals(Side.LEFT, right.sliderSide)
        assertEquals(Side.LEFT, right.panelSide)
    }

    @Test
    fun tabletsGetATopToolbarPopoversAndPinnablePanels() {
        val land = ws(1280, 800)
        assertEquals(FormFactor.TABLET, land.formFactor)
        assertEquals(LayoutKind.TABLET_LANDSCAPE, land.kind)
        assertEquals(ToolbarPlacement.TOP, land.toolbar)
        assertEquals(PanelStyle.POPOVER, land.panelStyle)
        assertTrue(land.canPin)
        assertEquals(PinnedPanel.NONE, land.pinned)                       // off by default
        val pinned = ws(1280, 800, st = s.copy(pinnedPanel = PinnedPanel.LAYERS))
        assertEquals(PinnedPanel.LAYERS, pinned.pinned)
        assertEquals(320, pinned.panelWidthDp)
        assertEquals(960, pinned.canvasWidthDp)
        assertEquals(Side.RIGHT, pinned.panelSide)                         // sliders stay on the left
        val port = ws(800, 1280, st = s.copy(pinnedPanel = PinnedPanel.COLOR))
        assertEquals(LayoutKind.TABLET_PORTRAIT, port.kind)
        assertTrue(port.canPin)                                            // 800 - 320 = 480 dp of canvas
        assertEquals(PinnedPanel.COLOR, port.pinned)
        assertTrue(port.showTitle)
        assertFalse(ws(673, 841).showTitle)                               // unfolded foldable: the tools need the toolbar
        assertTrue(ws(1280, 800).showTitle)
    }

    @Test
    fun pinningNeverLeavesANarrowCanvas() {
        // Small tablet in portrait with a wide panel: no room, the panel floats instead.
        val small = ws(600, 960, st = s.copy(pinnedPanel = PinnedPanel.LAYERS, panelWidthDp = 400))
        assertEquals(LayoutKind.TABLET_PORTRAIT, small.kind)
        assertFalse(small.canPin)
        assertEquals(PinnedPanel.NONE, small.pinned)
        assertTrue(small.canvasWidthDp >= LayoutPolicy.MIN_CANVAS_DP)
        // Panel width setting clamped to 40 % of the window.
        assertTrue(ws(1280, 800, st = s.copy(panelWidthDp = 9999)).panelWidthDp <= LayoutPolicy.PANEL_MAX_DP)
        assertEquals(LayoutPolicy.PANEL_MIN_DP, ws(1280, 800, st = s.copy(panelWidthDp = 10)).panelWidthDp)
    }

    @Test
    fun resizingThePinnedPanel() {
        assertEquals(360, LayoutPolicy.resizePanel(320, 40, 1280, 1f))
        assertEquals(LayoutPolicy.PANEL_MAX_DP, LayoutPolicy.resizePanel(320, 900, 1280, 1f))
        assertEquals(LayoutPolicy.PANEL_MIN_DP, LayoutPolicy.resizePanel(320, -900, 1280, 1f))
        // Never more than leaves MIN_CANVAS_DP of canvas.
        assertEquals(800 - LayoutPolicy.MIN_CANVAS_DP, LayoutPolicy.resizePanel(300, 100, 800, 1f))
    }

    @Test
    fun everyWindowShape() {
        data class Case(val name: String, val w: Int, val h: Int, val sw: Int, val kind: LayoutKind)
        val cases = listOf(
            Case("small phone portrait", 320, 568, 320, LayoutKind.PHONE_PORTRAIT),
            Case("small phone landscape", 568, 320, 320, LayoutKind.PHONE_LANDSCAPE),
            Case("large phone portrait", 430, 932, 430, LayoutKind.PHONE_PORTRAIT),
            Case("large phone landscape", 932, 430, 430, LayoutKind.PHONE_LANDSCAPE),
            Case("foldable folded", 360, 780, 360, LayoutKind.PHONE_PORTRAIT),
            Case("foldable unfolded", 673, 841, 673, LayoutKind.TABLET_PORTRAIT),
            Case("foldable unfolded landscape", 841, 673, 673, LayoutKind.TABLET_LANDSCAPE),
            Case("tablet portrait", 800, 1280, 800, LayoutKind.TABLET_PORTRAIT),
            Case("tablet landscape", 1280, 800, 800, LayoutKind.TABLET_LANDSCAPE),
            Case("large display", 1920, 1080, 1080, LayoutKind.TABLET_LANDSCAPE),
            Case("tablet split screen half", 640, 800, 800, LayoutKind.TABLET_PORTRAIT),
            Case("tablet split screen third", 400, 800, 800, LayoutKind.PHONE_PORTRAIT),
            Case("tablet short freeform window", 900, 420, 800, LayoutKind.PHONE_LANDSCAPE),
            Case("phone split screen", 411, 450, 411, LayoutKind.PHONE_PORTRAIT),
            Case("tiny freeform window", 420, 300, 800, LayoutKind.PHONE_PORTRAIT),
        )
        for (c in cases) {
            val w = ws(c.w, c.h, c.sw)
            assertEquals(c.name, c.kind, w.kind)
            // Invariants of every layout.
            assertTrue(c.name, w.touchTargetDp >= 48)
            assertTrue(c.name, w.panelWidthDp in 200..560)
            assertTrue(c.name, w.canvasWidthDp >= minOf(c.w, LayoutPolicy.MIN_CANVAS_DP) || w.pinned == PinnedPanel.NONE)
            if (w.kind == LayoutKind.PHONE_LANDSCAPE) assertTrue(c.name, w.panelWidthDp <= c.w / 2)
        }
    }

    @Test
    fun interfaceSizeChangesTheLayout() {
        // Very large UI scale on a small landscape phone: no room for a rail.
        assertEquals(LayoutKind.PHONE_PORTRAIT, ws(560, 320, st = s.copy(uiSize = UiSize.EXTRA_LARGE)).kind)
        // A small tablet at extra-large size behaves like a phone.
        assertEquals(LayoutKind.PHONE_PORTRAIT, ws(600, 960, st = s.copy(uiSize = UiSize.EXTRA_LARGE)).kind)
    }

    @Test
    fun labelsOnlyWhenRequestedAndThereIsRoom() {
        assertFalse(ws(411, 914).toolLabels)
        assertTrue(ws(411, 914, st = s.copy(showToolLabels = true)).toolLabels)
        assertFalse(ws(320, 640, st = s.copy(showToolLabels = true)).toolLabels)
    }

    @Test
    fun largeTouchTargets() {
        assertEquals(56, ws(411, 914, st = s.copy(largeTouchTargets = true)).touchTargetDp)
    }

    @Test
    fun galleryColumnsAdaptToWidth() {
        assertEquals(2, LayoutPolicy.galleryColumns(320, 1f))
        assertEquals(2, LayoutPolicy.galleryColumns(411, 1f))
        assertEquals(4, LayoutPolicy.galleryColumns(800, 1f))
        assertEquals(6, LayoutPolicy.galleryColumns(1600, 1f))
        assertEquals(2, LayoutPolicy.galleryColumns(411, 1.3f))
    }
}
