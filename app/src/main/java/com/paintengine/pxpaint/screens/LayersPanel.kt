package com.paintengine.pxpaint.screens

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.view.Gravity
import android.view.View
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import com.paintengine.engine.NativeEngine.LayerOp
import com.paintengine.pxpaint.core.BlendMode
import com.paintengine.pxpaint.core.LayerInfo
import com.paintengine.pxpaint.core.LayerStack
import com.paintengine.pxpaint.core.S
import com.paintengine.pxpaint.core.tr
import com.paintengine.pxpaint.ui.CheckerDrawable
import com.paintengine.pxpaint.ui.Icon
import com.paintengine.pxpaint.ui.IconButton
import com.paintengine.pxpaint.ui.OverlayHost
import com.paintengine.pxpaint.ui.PxSlider
import com.paintengine.pxpaint.ui.PxTheme
import com.paintengine.pxpaint.ui.Widgets
import kotlin.math.roundToInt

/**
 * The Layers panel (floating card of the workspace, the canvas stays
 * visible around it). Top layer first. Each row: the layer's own thumbnail
 * (over a checkerboard: transparency shows), name, mode / opacity, lock and
 * visibility toggles; the active layer is highlighted. Under the list, the
 * active layer's opacity, blend mode and actions (move up / down, merge
 * down, duplicate, rename, delete); flatten in the header menu.
 *
 * Every action goes through [Ops] (the workspace), which asks the engine,
 * shows why an operation is refused, and refreshes the panel.
 */
class LayersPanel(private val host: PxHost, private val ops: Ops) {
    interface Ops {
        val stack: LayerStack?
        fun run(op: LayerOp, id: Int, arg: Int = 0, value: Float = 0f, text: String? = null, done: S? = null)
        fun thumbnail(layer: LayerInfo, sizePx: Int, onReady: (Bitmap?) -> Unit): Bitmap?
        fun background(): View
    }

    private val context: Context get() = host.context
    private val theme: PxTheme get() = host.theme

    fun view(): View = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        val stack = ops.stack
        addView(LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(theme.text(context, tr(S.LAYERS), PxTheme.Type.HEADLINE), LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            addView(IconButton(context, theme, Icon.PLUS, tr(S.LAYER_ADD)).apply {
                isEnabled = stack != null && stack.layers.size < LayerStack.MAX_LAYERS
                setOnClickListener {
                    val s = ops.stack ?: return@setOnClickListener
                    ops.run(LayerOp.ADD, s.activeId, text = s.newLayerName(tr(S.LAYER_DEFAULT_NAME)))
                }
            })
            val more = IconButton(context, theme, Icon.MORE, tr(S.MORE_OPTIONS))
            more.setOnClickListener { headerMenu(more) }
            addView(more)
        })
        if (stack == null) return@apply
        val list = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        for (l in stack.topDown) list.addView(row(l, l.id == stack.activeId), LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { bottomMargin = theme.dp(4) })
        addView(list, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
            topMargin = theme.dp(6)
        })
        stack.active?.let { addView(activeControls(stack, it)) }
        addView(Widgets.divider(context, theme).apply { (layoutParams as LinearLayout.LayoutParams).setMargins(0, theme.dp(12), 0, theme.dp(10)) })
        addView(ops.background())
    }

    private fun subtitle(l: LayerInfo): String {
        val parts = ArrayList<String>()
        if (!l.visible) parts += tr(S.LAYER_HIDDEN)
        if (l.locked) parts += tr(S.LAYER_LOCKED)
        if (l.blend != BlendMode.NORMAL) parts += tr(l.blend.label)
        if (l.opacity < 1f) parts += "${(l.opacity * 100).roundToInt()}%"
        return parts.joinToString(" · ")
    }

    private fun row(l: LayerInfo, active: Boolean): View = LinearLayout(context).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        minimumHeight = theme.touchTarget + theme.dp(4)
        setPadding(theme.dp(6), theme.dp(3), 0, theme.dp(3))
        background = theme.pressable(theme.rounded(if (active) theme.accentSoft else theme.surface, theme.radiusS,
            if (active) theme.accent else theme.outline), theme.radiusS)
        isSelected = active
        isClickable = true
        contentDescription = "${l.name}${if (active) ", " + tr(S.LAYER_SELECTED, "") else ""}"
        setOnClickListener { if (!active) ops.run(LayerOp.ACTIVE, l.id) }
        setOnLongClickListener { layerMenu(this, l); true }
        alpha = if (l.visible) 1f else 0.6f
        val thumb = LayerThumb(context, theme)
        val size = theme.dp(42)
        thumb.bitmap = ops.thumbnail(l, size) { b -> thumb.bitmap = b }
        addView(thumb, LinearLayout.LayoutParams(size, size))
        addView(LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(theme.dp(10), 0, theme.dp(4), 0)
            addView(theme.text(context, l.name, PxTheme.Type.LABEL, lines = 1))
            val sub = subtitle(l)
            if (sub.isNotEmpty()) addView(theme.text(context, sub, PxTheme.Type.CAPTION, theme.textSecondary, lines = 1))
        }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        addView(IconButton(context, theme, if (l.locked) Icon.LOCK else Icon.UNLOCK, tr(if (l.locked) S.LAYER_UNLOCK else S.LAYER_LOCK), sizeDp = 34f).apply {
            isSelected = l.locked
            alpha = if (l.locked) 1f else 0.45f
            setOnClickListener { ops.run(LayerOp.LOCKED, l.id, if (l.locked) 0 else 1) }
        })
        addView(IconButton(context, theme, if (l.visible) Icon.EYE else Icon.EYE_OFF, tr(if (l.visible) S.LAYER_HIDE else S.LAYER_SHOW), sizeDp = 34f).apply {
            setOnClickListener { ops.run(LayerOp.VISIBLE, l.id, if (l.visible) 0 else 1) }
        })
    }

    private fun activeControls(stack: LayerStack, l: LayerInfo): View = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(0, theme.dp(8), 0, 0)
        // Opacity of the layer (independent from the brush opacity).
        val value = theme.text(context, "${(l.opacity * 100).roundToInt()}%", PxTheme.Type.LABEL, theme.textSecondary)
        addView(LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(theme.text(context, tr(S.LAYER_OPACITY), PxTheme.Type.LABEL), LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            addView(value)
        })
        addView(PxSlider(context, theme, tr(S.LAYER_OPACITY)).apply {
            valueText = { "${(it * 100).roundToInt()}%" }
            setValue(l.opacity)
            onChange = { v, fromUser ->
                val o = (v * 100).roundToInt() / 100f
                value.text = "${(o * 100).roundToInt()}%"
                if (fromUser) ops.run(LayerOp.OPACITY, l.id, value = o)
            }
        })
        addView(theme.text(context, tr(S.LAYER_BLEND), PxTheme.Type.LABEL).apply { setPadding(0, theme.dp(8), 0, theme.dp(6)) })
        addView(HorizontalScrollView(context).apply {
            isHorizontalScrollBarEnabled = false
            addView(LinearLayout(context).apply {
                orientation = LinearLayout.HORIZONTAL
                for (m in BlendMode.entries) addView(Widgets.chip(context, theme, tr(m.label), m == l.blend) {
                    if (m != l.blend) ops.run(LayerOp.BLEND, l.id, m.code)
                }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                    marginEnd = theme.dp(6)
                })
            })
        })
        // Actions on the active layer: disabled (not hidden) when they cannot apply.
        addView(LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, theme.dp(8), 0, 0)
            fun action(icon: Icon, label: S, enabled: Boolean, run: () -> Unit) = addView(IconButton(context, theme, icon, tr(label), sizeDp = 38f).apply {
                isEnabled = enabled
                setOnClickListener { run() }
            }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            val i = stack.indexOf(l.id)
            action(Icon.ARROW_UP, S.LAYER_MOVE_UP, stack.canMoveUp(l.id)) { ops.run(LayerOp.MOVE, l.id, i + 1) }
            action(Icon.ARROW_DOWN, S.LAYER_MOVE_DOWN, stack.canMoveDown(l.id)) { ops.run(LayerOp.MOVE, l.id, i - 1) }
            action(Icon.MERGE, S.LAYER_MERGE_DOWN, i > 0) { mergeDown(l) }
            action(Icon.COPY, S.LAYER_DUPLICATE, stack.layers.size < LayerStack.MAX_LAYERS) {
                ops.run(LayerOp.DUPLICATE, l.id, done = S.LAYER_DONE_DUPLICATED)
            }
            action(Icon.EDIT, S.LAYER_RENAME, true) { rename(l) }
            action(Icon.TRASH, S.LAYER_DELETE, stack.layers.size > 1) { delete(l) }
        })
    }

    private fun mergeDown(l: LayerInfo) {
        val s = ops.stack ?: return
        s.mergeDownBlocker(l.id)?.let { e -> e.message?.let { host.overlays.message(tr(it)) }; return }
        ops.run(LayerOp.MERGE_DOWN, l.id, done = S.LAYER_DONE_MERGED)
    }

    private fun rename(l: LayerInfo) {
        host.overlays.prompt(tr(S.LAYER_RENAME), l.name, tr(S.ACTION_SAVE), tr(S.ACTION_CANCEL)) { raw ->
            val name = LayerStack.cleanName(raw) ?: return@prompt host.overlays.message(tr(S.LAYER_ERR_BAD_VALUE))
            if (name != l.name) ops.run(LayerOp.RENAME, l.id, text = name)
        }
    }

    private fun delete(l: LayerInfo) {
        val s = ops.stack ?: return
        s.deleteBlocker(l.id)?.let { e -> e.message?.let { host.overlays.message(tr(it)) }; return }
        host.overlays.confirm(tr(S.LAYER_DELETE), tr(S.LAYER_DELETE_BODY, l.name), tr(S.ACTION_DELETE), tr(S.ACTION_CANCEL), true) {
            ops.run(LayerOp.DELETE, l.id, done = S.LAYER_DONE_DELETED)
        }
    }

    private fun flatten() {
        val s = ops.stack ?: return
        s.flattenBlocker()?.let { e -> e.message?.let { host.overlays.message(tr(it)) }; return }
        val hidden = s.hiddenCount
        host.overlays.confirm(tr(S.LAYER_FLATTEN), if (hidden > 0) tr(S.LAYER_FLATTEN_HIDDEN_BODY, hidden) else tr(S.LAYER_FLATTEN_BODY),
            tr(S.LAYER_FLATTEN), tr(S.ACTION_CANCEL), hidden > 0) {
            ops.run(LayerOp.FLATTEN, 0, text = s.layers.first().name, done = S.LAYER_DONE_FLATTENED)
        }
    }

    private fun headerMenu(anchor: View) {
        val s = ops.stack ?: return
        host.overlays.menu(anchor, listOf(
            OverlayHost.MenuItem(tr(S.LAYER_FLATTEN), Icon.LAYERS, enabled = s.layers.size > 1) { flatten() },
        ))
    }

    private fun layerMenu(anchor: View, l: LayerInfo) {
        val s = ops.stack ?: return
        val i = s.indexOf(l.id)
        host.overlays.menu(anchor, listOf(
            OverlayHost.MenuItem(tr(S.LAYER_RENAME), Icon.EDIT) { rename(l) },
            OverlayHost.MenuItem(tr(S.LAYER_DUPLICATE), Icon.COPY) { ops.run(LayerOp.DUPLICATE, l.id, done = S.LAYER_DONE_DUPLICATED) },
            OverlayHost.MenuItem(tr(S.LAYER_MOVE_UP), Icon.ARROW_UP, enabled = s.canMoveUp(l.id)) { ops.run(LayerOp.MOVE, l.id, i + 1) },
            OverlayHost.MenuItem(tr(S.LAYER_MOVE_DOWN), Icon.ARROW_DOWN, enabled = s.canMoveDown(l.id)) { ops.run(LayerOp.MOVE, l.id, i - 1) },
            OverlayHost.MenuItem(tr(S.LAYER_MERGE_DOWN), Icon.MERGE, enabled = i > 0) { mergeDown(l) },
            OverlayHost.MenuItem(tr(if (l.locked) S.LAYER_UNLOCK else S.LAYER_LOCK), if (l.locked) Icon.UNLOCK else Icon.LOCK) {
                ops.run(LayerOp.LOCKED, l.id, if (l.locked) 0 else 1)
            },
            OverlayHost.MenuItem(tr(S.LAYER_DELETE), Icon.TRASH, destructive = true, enabled = s.layers.size > 1) { delete(l) },
        ))
    }

    /** Layer thumbnail over a checkerboard (transparent pixels show as such). */
    @SuppressLint("ViewConstructor")
    private class LayerThumb(context: Context, theme: PxTheme) : View(context) {
        var bitmap: Bitmap? = null
            set(v) {
                field = v
                invalidate()
            }
        private val checker = CheckerDrawable(theme.dp(4), theme.dark)
        private val paint = Paint(Paint.FILTER_BITMAP_FLAG)
        private val frame = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            style = Paint.Style.STROKE
            strokeWidth = theme.dpF(1f)
            color = theme.outline
        }
        private val r = RectF()

        override fun onDraw(canvas: Canvas) {
            val b = bitmap
            // Fit the layer's aspect ratio inside the square.
            val w = width.toFloat()
            val h = height.toFloat()
            var dw = w
            var dh = h
            if (b != null && b.width > 0 && b.height > 0) {
                val s = minOf(w / b.width, h / b.height)
                dw = b.width * s
                dh = b.height * s
            }
            r.set((w - dw) / 2, (h - dh) / 2, (w + dw) / 2, (h + dh) / 2)
            checker.setBounds(r.left.toInt(), r.top.toInt(), r.right.toInt(), r.bottom.toInt())
            checker.draw(canvas)
            if (b != null) canvas.drawBitmap(b, null, r, paint)
            canvas.drawRect(r, frame)
        }
    }
}
