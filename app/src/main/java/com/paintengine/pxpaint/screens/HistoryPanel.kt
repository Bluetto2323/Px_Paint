package com.paintengine.pxpaint.screens

import android.content.Context
import android.view.Gravity
import android.view.View
import android.widget.LinearLayout
import com.paintengine.pxpaint.core.Format
import com.paintengine.pxpaint.core.HistoryState
import com.paintengine.pxpaint.core.HistoryStep
import com.paintengine.pxpaint.core.S
import com.paintengine.pxpaint.core.tr
import com.paintengine.pxpaint.ui.Icon
import com.paintengine.pxpaint.ui.IconButton
import com.paintengine.pxpaint.ui.IconDrawable
import com.paintengine.pxpaint.ui.PxTheme

/**
 * The History panel (floating card of the workspace, like Layers): the
 * steps kept by the engine, newest first. Undone steps (still redoable) are
 * dimmed above the current one, which is highlighted; "Start" is the canvas
 * before the oldest kept step. Tapping a step undoes / redoes up to it.
 * Under the list: memory used and "Clear history".
 */
class HistoryPanel(private val host: PxHost, private val ops: Ops) {
    interface Ops {
        val history: HistoryState
        /** Moves to the state right after step [index] (-1: Start). */
        fun goTo(index: Int)
        fun clear()
    }

    private val context: Context get() = host.context
    private val theme: PxTheme get() = host.theme

    fun view(): View = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        val h = ops.history
        addView(LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(theme.text(context, tr(S.HISTORY), PxTheme.Type.HEADLINE), LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            addView(IconButton(context, theme, Icon.TRASH, tr(S.HISTORY_CLEAR)).apply {
                isEnabled = h.undo + h.redo > 0
                alpha = if (isEnabled) 1f else 0.38f
                setOnClickListener { confirmClear() }
            })
        })
        val total = h.undo + h.redo
        addView(theme.text(context, tr(S.HISTORY_MEMORY, total, Format.bytes(h.bytes), Format.bytes(h.budget)),
            PxTheme.Type.CAPTION, theme.textSecondary).apply { setPadding(0, theme.dp(2), 0, theme.dp(6)) })
        if (total == 0) {
            addView(theme.text(context, tr(S.HISTORY_EMPTY), PxTheme.Type.BODY, theme.textSecondary).apply {
                setPadding(0, theme.dp(6), 0, theme.dp(6))
            })
            return@apply
        }
        addView(theme.text(context, tr(S.HISTORY_HINT), PxTheme.Type.CAPTION, theme.textSecondary).apply {
            setPadding(0, 0, 0, theme.dp(8))
        })
        val list = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        val lp = { LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { bottomMargin = theme.dp(4) } }
        // Newest first; the list arrives oldest first.
        for (i in h.steps.indices.reversed()) list.addView(row(h.steps[i], i, current = i == h.undo - 1), lp())
        list.addView(startRow(current = h.undo == 0), lp())
        addView(list)
        if (h.dropped > 0) addView(theme.text(context, tr(S.HISTORY_DROPPED, h.dropped), PxTheme.Type.CAPTION, theme.textSecondary).apply {
            setPadding(0, theme.dp(6), 0, 0)
        })
    }

    private fun describe(s: HistoryStep): String = tr(s.kind.label)

    private fun row(s: HistoryStep, index: Int, current: Boolean): View =
        item(describe(s), s.label, current, s.undone, index)

    private fun startRow(current: Boolean): View = item(tr(S.HISTORY_START), tr(S.HISTORY_START_DESC), current, false, -1)

    private fun item(title: String, subtitle: String, current: Boolean, undone: Boolean, index: Int): View = LinearLayout(context).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        minimumHeight = theme.touchTarget
        setPadding(theme.dp(10), theme.dp(4), theme.dp(10), theme.dp(4))
        background = theme.pressable(theme.rounded(if (current) theme.accentSoft else theme.surface, theme.radiusS,
            if (current) theme.accent else theme.outline), theme.radiusS)
        isSelected = current
        isClickable = true
        alpha = if (undone) 0.5f else 1f
        val state = when {
            current -> tr(S.HISTORY_CURRENT)
            undone -> tr(S.HISTORY_UNDONE)
            else -> ""
        }
        contentDescription = listOf(title, subtitle, state).filter { it.isNotEmpty() }.joinToString(", ")
        setOnClickListener { if (!current) ops.goTo(index) }
        addView(View(context).apply {
            background = IconDrawable(if (index < 0) Icon.HISTORY else if (undone) Icon.REDO else Icon.UNDO,
                if (current) theme.accent else theme.textSecondary)
        }, LinearLayout.LayoutParams(theme.dp(20), theme.dp(20)))
        addView(LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(theme.dp(12), 0, 0, 0)
            addView(theme.text(context, title, PxTheme.Type.LABEL, lines = 1))
            if (subtitle.isNotEmpty()) addView(theme.text(context, subtitle, PxTheme.Type.CAPTION, theme.textSecondary, lines = 1))
        }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        if (state.isNotEmpty()) addView(theme.text(context, state, PxTheme.Type.MICRO, if (current) theme.accent else theme.textSecondary, lines = 1))
    }

    private fun confirmClear() {
        host.overlays.confirm(tr(S.HISTORY_CLEAR), tr(S.HISTORY_CLEAR_BODY), tr(S.ACTION_CLEAR), tr(S.ACTION_CANCEL), true) {
            ops.clear()
        }
    }
}
