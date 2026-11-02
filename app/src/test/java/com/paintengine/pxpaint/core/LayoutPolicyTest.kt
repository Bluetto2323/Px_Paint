package com.paintengine.pxpaint.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class LayoutPolicyTest {
    private val s = AppSettings()

    @Test
    fun phonePortraitUsesABottomDock() {
        // Galaxy A33: 1080x2400 px at ~2.6 density -> ~411x914 dp.
        val w = LayoutPolicy.workspace(411, 914, 411, s)
        assertEquals(FormFactor.PHONE, w.formFactor)
        assertEquals(DockPlacement.BOTTOM, w.dock)
        assertFalse(w.sidePanelDocked)
        assertFalse(w.compactTopBar)
        assertEquals(48, w.touchTargetDp)
    }

    @Test
    fun phoneLandscapeUsesASideRailAndCompactTopBar() {
        val w = LayoutPolicy.workspace(914, 411, 411, s)
        assertEquals(DockPlacement.LEFT, w.dock)
        assertTrue(w.compactTopBar)
        assertFalse(w.sidePanelDocked)
        assertEquals(DockPlacement.RIGHT, LayoutPolicy.workspace(914, 411, 411, s.copy(dockSide = DockSide.RIGHT)).dock)
    }

    @Test
    fun tabletsGetRailAndDockedPanelWhenWideEnough() {
        val landscape = LayoutPolicy.workspace(1280, 800, 800, s)
        assertEquals(FormFactor.TABLET, landscape.formFactor)
        assertEquals(DockPlacement.LEFT, landscape.dock)
        assertTrue(landscape.sidePanelDocked)
        val portrait = LayoutPolicy.workspace(800, 1280, 800, s)
        assertEquals(DockPlacement.LEFT, portrait.dock)
        assertFalse(portrait.sidePanelDocked)          // 800 dp < 840 dp
    }

    @Test
    fun narrowWindowsFallBackToBottomDock() {
        // Split-screen on a tablet: tablet smallest width but a 400 dp window.
        assertEquals(DockPlacement.BOTTOM, LayoutPolicy.workspace(400, 1200, 600, s).dock)
        // Very large UI scale on a small landscape phone.
        assertEquals(DockPlacement.BOTTOM, LayoutPolicy.workspace(560, 320, 320, s.copy(uiSize = UiSize.EXTRA_LARGE)).dock)
    }

    @Test
    fun labelsOnlyWhenRequestedAndThereIsRoom() {
        assertFalse(LayoutPolicy.workspace(411, 914, 411, s).toolLabels)
        assertTrue(LayoutPolicy.workspace(411, 914, 411, s.copy(showToolLabels = true)).toolLabels)
        assertFalse(LayoutPolicy.workspace(320, 640, 320, s.copy(showToolLabels = true)).toolLabels)
    }

    @Test
    fun largeTouchTargets() {
        assertEquals(56, LayoutPolicy.workspace(411, 914, 411, s.copy(largeTouchTargets = true)).touchTargetDp)
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
