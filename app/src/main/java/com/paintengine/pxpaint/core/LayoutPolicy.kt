package com.paintengine.pxpaint.core

/**
 * Adaptive layout decisions (pure, unit-tested). All sizes are in dp of the
 * *current* window (multi-window and rotation included).
 */
enum class DockPlacement { BOTTOM, LEFT, RIGHT }

data class WorkspaceSpec(
    val formFactor: FormFactor,
    /** Tool dock position. */
    val dock: DockPlacement,
    /** Color / tool settings panel permanently docked on the side (tablet, wide). */
    val sidePanelDocked: Boolean,
    /** Shorter top bar (landscape phone: vertical space is precious). */
    val compactTopBar: Boolean,
    /** Text labels under tool icons. */
    val toolLabels: Boolean,
    /** Minimum touch target in dp. */
    val touchTargetDp: Int,
)

enum class FormFactor { PHONE, TABLET }

object LayoutPolicy {
    const val TABLET_MIN_SMALLEST_WIDTH_DP = 600
    const val SIDE_PANEL_MIN_WIDTH_DP = 840
    const val TOUCH_TARGET_DP = 48
    const val LARGE_TOUCH_TARGET_DP = 56

    fun formFactor(smallestWidthDp: Int) =
        if (smallestWidthDp >= TABLET_MIN_SMALLEST_WIDTH_DP) FormFactor.TABLET else FormFactor.PHONE

    fun workspace(widthDp: Int, heightDp: Int, smallestWidthDp: Int, settings: AppSettings): WorkspaceSpec {
        val ff = formFactor(smallestWidthDp)
        val landscape = widthDp > heightDp
        val scale = settings.uiSize.scale
        val target = if (settings.largeTouchTargets) LARGE_TOUCH_TARGET_DP else TOUCH_TARGET_DP
        // Portrait phones (and narrow tablet windows) keep the dock at the
        // bottom where thumbs reach it; any other shape uses a side rail so
        // the canvas keeps its full height.
        val bottomDock = ff == FormFactor.PHONE && !landscape || widthDp * 1.0 / scale < 480
        val side = when (settings.dockSide) {
            DockSide.RIGHT -> DockPlacement.RIGHT
            DockSide.LEFT, DockSide.AUTO -> DockPlacement.LEFT
        }
        val dock = if (bottomDock) DockPlacement.BOTTOM else side
        // Labels need horizontal room: only if asked and the dock can fit them.
        val labels = settings.showToolLabels && (dock != DockPlacement.BOTTOM || widthDp / scale >= 360)
        return WorkspaceSpec(
            formFactor = ff,
            dock = dock,
            sidePanelDocked = ff == FormFactor.TABLET && widthDp / scale >= SIDE_PANEL_MIN_WIDTH_DP,
            compactTopBar = ff == FormFactor.PHONE && landscape,
            toolLabels = labels,
            touchTargetDp = target,
        )
    }

    /** Gallery grid columns: cards at least ~160 dp wide, 2..6 columns. */
    fun galleryColumns(widthDp: Int, scale: Float): Int {
        val usable = widthDp - 2 * 16
        val minCard = 160 * scale + 12
        return (usable / minCard).toInt().coerceIn(2, 6)
    }
}
