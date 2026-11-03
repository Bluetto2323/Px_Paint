package com.paintengine.pxpaint.core

/**
 * Adaptive workspace layout (pure, unit-tested). Every decision comes from
 * the *current* window in dp (rotation, split screen, freeform windows and
 * foldables included) and the interface size, never from a device model.
 *
 * Four layouts:
 *  * PHONE_PORTRAIT   compact top bar, tools in a bottom dock where thumbs
 *                     reach, size / opacity sliders on a screen edge, panels
 *                     as bottom sheets (at most ~55 % of the height).
 *  * PHONE_LANDSCAPE  short top bar, tools in a side rail, sliders on the
 *                     other edge, panels as side sheets (the canvas keeps
 *                     its full height and at least half of the width).
 *  * TABLET_PORTRAIT /
 *    TABLET_LANDSCAPE one top toolbar (actions left, tools right), sliders
 *                     on an edge, panels as popovers under the toolbar or
 *                     pinned as a persistent, resizable side panel when the
 *                     canvas keeps at least [MIN_CANVAS_DP] of width.
 */
enum class FormFactor { PHONE, TABLET }

enum class LayoutKind { PHONE_PORTRAIT, PHONE_LANDSCAPE, TABLET_PORTRAIT, TABLET_LANDSCAPE }

/** Where the tool group lives. */
enum class ToolbarPlacement { BOTTOM, SIDE, TOP }

enum class Side {
    LEFT, RIGHT;
    val opposite: Side get() = if (this == LEFT) RIGHT else LEFT
}

/** How a panel (brush, colour, layers, library, history) is presented. */
enum class PanelStyle { BOTTOM_SHEET, SIDE_SHEET, POPOVER }

/** Panel kept open beside the canvas on tablets (user setting). */
enum class PinnedPanel { NONE, BRUSHES, COLOR, LAYERS }

data class WorkspaceSpec(
    val formFactor: FormFactor,
    val kind: LayoutKind,
    val toolbar: ToolbarPlacement,
    /** Side of the tool rail (SIDE toolbar). */
    val railSide: Side,
    /** Edge of the quick size / opacity sliders. */
    val sliderSide: Side,
    val panelStyle: PanelStyle,
    /** Side of side sheets, popovers and the pinned panel. */
    val panelSide: Side,
    /** Width of side sheets / popovers / the pinned panel. */
    val panelWidthDp: Int,
    /** Maximum height of a bottom sheet. */
    val sheetMaxHeightDp: Int,
    /** A panel can be pinned in this window (enough room for the canvas). */
    val canPin: Boolean,
    /** The pinned panel actually shown (NONE when there is no room). */
    val pinned: PinnedPanel,
    val compactTopBar: Boolean,
    /** Project name in the top bar. */
    val showTitle: Boolean,
    /** Text labels under tool icons. */
    val toolLabels: Boolean,
    /** Minimum touch target in dp. */
    val touchTargetDp: Int,
    /** Canvas width left beside a pinned panel (dp, interface scale applied). */
    val canvasWidthDp: Int,
)

object LayoutPolicy {
    const val TABLET_MIN_SMALLEST_WIDTH_DP = 600
    const val TOUCH_TARGET_DP = 48
    const val LARGE_TOUCH_TARGET_DP = 56
    /** Narrowest canvas accepted beside a pinned panel. */
    const val MIN_CANVAS_DP = 480
    const val PANEL_MIN_DP = 280
    const val PANEL_MAX_DP = 480
    const val PANEL_DEFAULT_DP = 320
    /** Below this window height a landscape window is a "phone landscape". */
    const val SHORT_HEIGHT_DP = 480

    fun formFactor(smallestWidthDp: Int) =
        if (smallestWidthDp >= TABLET_MIN_SMALLEST_WIDTH_DP) FormFactor.TABLET else FormFactor.PHONE

    fun workspace(widthDp: Int, heightDp: Int, smallestWidthDp: Int, settings: AppSettings): WorkspaceSpec {
        val scale = settings.uiSize.scale
        // Effective size: what fits at the chosen interface size.
        val w = (widthDp / scale).toInt()
        val h = (heightDp / scale).toInt()
        val landscape = widthDp > heightDp
        val ff = formFactor(smallestWidthDp)
        val tabletLayout = ff == FormFactor.TABLET && w >= 600 && h >= SHORT_HEIGHT_DP
        val kind = when {
            tabletLayout -> if (landscape) LayoutKind.TABLET_LANDSCAPE else LayoutKind.TABLET_PORTRAIT
            // A landscape window wide enough for a rail; otherwise the bottom dock fits better.
            landscape && w >= 480 -> LayoutKind.PHONE_LANDSCAPE
            else -> LayoutKind.PHONE_PORTRAIT
        }
        val rail = if (settings.dockSide == DockSide.RIGHT) Side.RIGHT else Side.LEFT
        val target = if (settings.largeTouchTargets) LARGE_TOUCH_TARGET_DP else TOUCH_TARGET_DP
        val toolbar: ToolbarPlacement
        val sliderSide: Side
        val style: PanelStyle
        val panelSide: Side
        val panelWidth: Int
        when (kind) {
            LayoutKind.PHONE_PORTRAIT -> {
                toolbar = ToolbarPlacement.BOTTOM
                sliderSide = settings.sliderSide
                style = PanelStyle.BOTTOM_SHEET
                panelSide = settings.sliderSide.opposite
                panelWidth = minOf(w, 560)
            }
            LayoutKind.PHONE_LANDSCAPE -> {
                toolbar = ToolbarPlacement.SIDE
                sliderSide = rail.opposite          // never on top of the rail
                style = PanelStyle.SIDE_SHEET
                panelSide = rail.opposite
                panelWidth = minOf(360, (w * 0.45).toInt()).coerceAtLeast(240)
            }
            else -> {
                toolbar = ToolbarPlacement.TOP
                sliderSide = settings.sliderSide
                style = PanelStyle.POPOVER
                panelSide = settings.sliderSide.opposite   // the sliders stay visible
                panelWidth = settings.panelWidthDp.coerceIn(PANEL_MIN_DP, minOf(PANEL_MAX_DP, (w * 0.4).toInt()).coerceAtLeast(PANEL_MIN_DP))
            }
        }
        val canPin = tabletLayout && w - panelWidth >= MIN_CANVAS_DP
        val pinned = if (canPin) settings.pinnedPanel else PinnedPanel.NONE
        val showTitle = when (toolbar) {
            ToolbarPlacement.TOP -> w >= 720
            else -> w >= 340
        }
        return WorkspaceSpec(
            formFactor = ff,
            kind = kind,
            toolbar = toolbar,
            railSide = rail,
            sliderSide = sliderSide,
            panelStyle = style,
            panelSide = panelSide,
            panelWidthDp = panelWidth,
            sheetMaxHeightDp = (h * 0.55).toInt().coerceAtLeast(240),
            canPin = canPin,
            pinned = pinned,
            compactTopBar = kind == LayoutKind.PHONE_LANDSCAPE,
            showTitle = showTitle,
            toolLabels = settings.showToolLabels && (toolbar != ToolbarPlacement.BOTTOM || w >= 360),
            touchTargetDp = target,
            canvasWidthDp = if (pinned != PinnedPanel.NONE) w - panelWidth else w,
        )
    }

    /** Pinned panel width after a drag of the resize handle by [deltaDp] (towards the canvas = wider). */
    fun resizePanel(currentDp: Int, deltaDp: Int, windowWidthDp: Int, scale: Float): Int {
        val w = (windowWidthDp / scale).toInt()
        val max = minOf(PANEL_MAX_DP, w - MIN_CANVAS_DP).coerceAtLeast(PANEL_MIN_DP)
        return (currentDp + deltaDp).coerceIn(PANEL_MIN_DP, max)
    }

    /** Gallery grid columns: cards at least ~160 dp wide, 2..6 columns. */
    fun galleryColumns(widthDp: Int, scale: Float): Int {
        val usable = widthDp - 2 * 16
        val minCard = 160 * scale + 12
        return (usable / minCard).toInt().coerceIn(2, 6)
    }
}
