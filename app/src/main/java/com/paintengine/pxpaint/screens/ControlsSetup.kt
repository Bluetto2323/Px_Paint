package com.paintengine.pxpaint.screens

import android.view.View
import android.widget.LinearLayout
import com.paintengine.pxpaint.core.AppSettings
import com.paintengine.pxpaint.core.DockSide
import com.paintengine.pxpaint.core.FingerMode
import com.paintengine.pxpaint.core.S
import com.paintengine.pxpaint.core.Side
import com.paintengine.pxpaint.core.tr
import com.paintengine.pxpaint.ui.PxTheme
import com.paintengine.pxpaint.ui.Widgets

/**
 * Block 12.5: the controls, customisable in one sheet. Offered once when the
 * app opens for the first time (then [AppSettings.controlsSetupDone]); also
 * reachable from Settings → Gestures. Every change is saved at once; the
 * same settings stay in their Settings sections.
 */
object ControlsSetup {
    fun show(host: PxHost) {
        val theme = host.theme
        val ctx = host.context
        if (!host.settings.controlsSetupDone) host.updateSettings(host.settings.copy(controlsSetupDone = true), rebuild = false)
        val body = LinearLayout(ctx).apply { orientation = LinearLayout.VERTICAL }
        var layer: View? = null
        fun build() {
            body.removeAllViews()
            val s = host.settings
            val set: (AppSettings) -> Unit = { n -> host.updateSettings(n, rebuild = false); build() }
            fun label(text: S) = body.addView(theme.text(ctx, tr(text), PxTheme.Type.LABEL, theme.textSecondary).apply {
                setPadding(0, theme.dp(12), 0, theme.dp(6))
            })
            body.addView(theme.text(ctx, tr(S.CONTROLS_SETUP_BODY), PxTheme.Type.BODY, theme.textSecondary))
            label(S.FINGER_MODE)
            body.addView(Widgets.segmented(ctx, theme, listOf(
                FingerMode.DRAW to tr(S.FINGER_DRAW), FingerMode.PAN to tr(S.FINGER_PAN), FingerMode.GESTURES_ONLY to tr(S.FINGER_GESTURES),
            ), s.fingerMode) { set(host.settings.copy(fingerMode = it)) })
            body.addView(Widgets.switchRow(ctx, theme, tr(S.GESTURE_HOLD_PICK), tr(S.GESTURE_HOLD_PICK_DESC), s.longPressPick) {
                set(host.settings.copy(longPressPick = it))
            })
            body.addView(Widgets.switchRow(ctx, theme, tr(S.GESTURE_TAP_UNDO), null, s.tapUndo) { set(host.settings.copy(tapUndo = it)) })
            body.addView(Widgets.switchRow(ctx, theme, tr(S.GESTURE_TAP_REDO), null, s.tapRedo) { set(host.settings.copy(tapRedo = it)) })
            body.addView(Widgets.switchRow(ctx, theme, tr(S.GESTURE_TAP_FULLSCREEN), null, s.tapFullscreen) { set(host.settings.copy(tapFullscreen = it)) })
            body.addView(Widgets.switchRow(ctx, theme, tr(S.GESTURE_PINCH_FIT), tr(S.GESTURE_PINCH_FIT_DESC), s.quickPinchFit) {
                set(host.settings.copy(quickPinchFit = it))
            })
            body.addView(Widgets.switchRow(ctx, theme, tr(S.TWO_FINGER_ROTATE), null, s.twoFingerRotate) { set(host.settings.copy(twoFingerRotate = it)) })
            body.addView(Widgets.switchRow(ctx, theme, tr(S.GESTURE_STYLUS_PICK), tr(S.GESTURE_STYLUS_PICK_DESC), s.stylusButtonPick) {
                set(host.settings.copy(stylusButtonPick = it))
            })
            label(S.SLIDER_SIDE)
            body.addView(Widgets.segmented(ctx, theme, listOf(Side.LEFT to tr(S.SIDE_LEFT), Side.RIGHT to tr(S.SIDE_RIGHT)), s.sliderSide) {
                set(host.settings.copy(sliderSide = it))
            })
            label(S.DOCK_SIDE)
            body.addView(Widgets.segmented(ctx, theme, listOf(
                DockSide.AUTO to tr(S.SIDE_AUTO), DockSide.LEFT to tr(S.SIDE_LEFT), DockSide.RIGHT to tr(S.SIDE_RIGHT),
            ), s.dockSide) { set(host.settings.copy(dockSide = it)) })
            body.addView(Widgets.switchRow(ctx, theme, tr(S.TOOL_LABELS), null, s.showToolLabels) { set(host.settings.copy(showToolLabels = it)) })
            body.addView(Widgets.pill(ctx, theme, tr(S.ACTION_DONE), primary = true) { layer?.let { host.overlays.dismiss(it) } },
                LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                    topMargin = theme.dp(16)
                })
        }
        build()
        val scroll = android.widget.ScrollView(ctx).apply {
            setPadding(theme.pad, 0, theme.pad, theme.dp(16))
            addView(body)
        }
        layer = host.overlays.sheet(scroll, tr(S.CONTROLS_SETUP_TITLE))
    }
}
