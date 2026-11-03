package com.paintengine.pxpaint.screens

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import com.paintengine.engine.NativeEngine
import com.paintengine.pxpaint.core.Accumulation
import com.paintengine.pxpaint.core.AngleMode
import com.paintengine.pxpaint.core.BrushCategory
import com.paintengine.pxpaint.core.BrushGrain
import com.paintengine.pxpaint.core.BrushParams
import com.paintengine.pxpaint.core.BrushPreset
import com.paintengine.pxpaint.core.BrushPreviewSpec
import com.paintengine.pxpaint.core.BrushState
import com.paintengine.pxpaint.core.BrushTip
import com.paintengine.pxpaint.core.BrushTool
import com.paintengine.pxpaint.core.S
import com.paintengine.pxpaint.core.SampleBuffer
import com.paintengine.pxpaint.core.ToolKind
import com.paintengine.pxpaint.core.tr
import com.paintengine.pxpaint.input.StylusInput
import com.paintengine.pxpaint.ui.BrushPreviewView
import com.paintengine.pxpaint.ui.BrushPreviews
import com.paintengine.pxpaint.ui.FlowRow
import com.paintengine.pxpaint.ui.Icon
import com.paintengine.pxpaint.ui.IconButton
import com.paintengine.pxpaint.ui.OverlayHost
import com.paintengine.pxpaint.ui.PxSlider
import com.paintengine.pxpaint.ui.PxTheme
import com.paintengine.pxpaint.ui.Widgets
import com.paintengine.pxpaint.ui.markHeading
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicInteger
import kotlin.math.roundToInt

/**
 * Brush editor for the brush selected for [tool]: a live preview rendered by
 * the engine (stroke, pressure response, sizes, opacities), a pad to try the
 * brush with a finger or stylus (same renderer), and every parameter of the
 * brush model. Edits apply immediately and persist: a built-in brush keeps
 * them as a resettable customisation, a user brush is changed in place.
 */
class BrushEditorScreen(host: PxHost, private val tool: BrushTool) : Screen(host) {
    private val handler = Handler(Looper.getMainLooper())
    private var preview: BrushPreviewView? = null
    private var pad: TryPad? = null
    private var titleView: TextView? = null
    private var notes: LinearLayout? = null
    private val id: String get() = host.brushes.selectedId(tool)
    private val preset: BrushPreset get() = host.brushes.selectedPreset(tool)
    private val params: BrushParams get() = host.brushes.effective(tool)
    private val refreshPreview = Runnable {
        preview?.show(params)
        pad?.paramsChanged()
    }

    override fun createView(): View {
        val root = FrameLayout(context).apply { setBackgroundColor(theme.bg) }
        val ins = host.insets
        val page = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(theme.pad, theme.dp(4), theme.pad, ins.bottom + theme.dp(32))
        }
        preview = BrushPreviewView(context, theme, host.previews, true) { host.pressureTable() }.also {
            page.addView(it, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, theme.dp(250)))
            it.show(params)
        }
        notes = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }.also { page.addView(it) }
        refreshNotes()

        page.addView(header(tr(S.TRY_PAD)))
        pad = TryPad(context).also {
            page.addView(it, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, theme.dp(170)))
        }
        page.addView(Widgets.pill(context, theme, tr(S.ACTION_CLEAR), false) { pad?.clear() },
            LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                topMargin = theme.dp(8)
            })

        val p = params
        val pct = { v: Float -> "${(v * 100).roundToInt()}%" }
        fun r2(v: Float) = (v * 100f).roundToInt() / 100f

        page.addView(header(tr(S.SEC_BRUSH_STROKE)))
        page.addView(slider(tr(S.SIZE), BrushState.sizeToSlider(p.size), { BrushState.sizeLabel(BrushState.sliderToSize(it)) }) { v ->
            set { it.copy(size = BrushState.sliderToSize(v)) }
        })
        page.addView(slider(tr(S.OPACITY), p.opacity, { pct(BrushState.sliderToOpacity(it)) }) { v -> set { it.copy(opacity = BrushState.sliderToOpacity(v)) } })
        page.addView(slider(tr(S.P_FLOW), p.flow, { pct(r2(it).coerceAtLeast(0.01f)) }) { v -> set { it.copy(flow = r2(v).coerceAtLeast(0.01f)) } })
        page.addView(slider(tr(S.SPACING), BrushState.spacingToSlider(p.spacing), { pct(BrushState.sliderToSpacing(it)) }) { v ->
            set { it.copy(spacing = BrushState.sliderToSpacing(v)) }
        })
        page.addView(label(tr(S.P_ACCUMULATION)))
        page.addView(choice(Accumulation.entries.map { it to tr(it.label) }, { params.accumulation }) { a -> set { it.copy(accumulation = a) } })

        page.addView(header(tr(S.SEC_BRUSH_SHAPE)))
        page.addView(label(tr(S.P_TIP)))
        page.addView(choice(BrushTip.entries.map { it to tr(it.label) }, { params.tip }) { t -> set { it.copy(tip = t) } })
        page.addView(slider(tr(S.P_HARDNESS), p.hardness, { pct(r2(it)) }) { v -> set { it.copy(hardness = r2(v)) } })
        page.addView(slider(tr(S.P_ROUNDNESS), (p.roundness - 0.05f) / 0.95f, { pct(r2(0.05f + it * 0.95f)) }) { v ->
            set { it.copy(roundness = r2(0.05f + v * 0.95f)) }
        })
        page.addView(slider(tr(S.P_ANGLE), (p.angle + 180f) / 360f, { "${(it * 360f - 180f).roundToInt()}°" }) { v ->
            set { it.copy(angle = (v * 360f - 180f).roundToInt().toFloat()) }
        })
        page.addView(choice(AngleMode.entries.map { it to tr(it.label) }, { params.angleMode }) { m -> set { it.copy(angleMode = m) } })

        page.addView(header(tr(S.SEC_BRUSH_TEXTURE)))
        page.addView(label(tr(S.P_GRAIN)))
        page.addView(choice(BrushGrain.entries.map { it to tr(it.label) }, { params.grain }) { g -> set { it.copy(grain = g) } })
        page.addView(slider(tr(S.P_GRAIN_DEPTH), p.grainDepth, { pct(r2(it)) }) { v -> set { it.copy(grainDepth = r2(v)) } })
        page.addView(slider(tr(S.P_GRAIN_PRESSURE), p.grainPressure, { pct(r2(it)) }) { v -> set { it.copy(grainPressure = r2(v)) } })
        page.addView(slider(tr(S.P_WET_EDGE), p.wetEdge, { pct(r2(it)) }) { v -> set { it.copy(wetEdge = r2(v)) } })

        page.addView(header(tr(S.SEC_BRUSH_JITTER)))
        page.addView(slider(tr(S.P_SIZE_JITTER), p.sizeJitter, { pct(r2(it)) }) { v -> set { it.copy(sizeJitter = r2(v)) } })
        page.addView(slider(tr(S.P_SCATTER), p.scatter / 2f, { pct(r2(it * 2f)) }) { v -> set { it.copy(scatter = r2(v * 2f)) } })
        page.addView(slider(tr(S.P_ANGLE_JITTER), p.angleJitter, { pct(r2(it)) }) { v -> set { it.copy(angleJitter = r2(v)) } })
        page.addView(slider(tr(S.P_FLOW_JITTER), p.flowJitter, { pct(r2(it)) }) { v -> set { it.copy(flowJitter = r2(v)) } })

        page.addView(header(tr(S.STYLUS_SECTION)))
        page.addView(theme.text(context, if (host.stylus.pressureAvailable) tr(S.PRESSURE_STYLUS_READY) else tr(S.NO_PRESSURE_STYLUS),
            PxTheme.Type.CAPTION, theme.textSecondary).apply { setPadding(0, theme.dp(2), 0, theme.dp(4)) })
        fun fraction(v: Float) = (BrushState.MIN_PRESSURE_FRACTION + v * (1f - BrushState.MIN_PRESSURE_FRACTION)).let { (it * 100).roundToInt() / 100f }
        fun fslider(f: Float) = ((f - BrushState.MIN_PRESSURE_FRACTION) / (1f - BrushState.MIN_PRESSURE_FRACTION)).coerceIn(0f, 1f)
        page.addView(switch(tr(S.PRESSURE_SIZE), null, p.pressureSize, true) { b -> set { it.copy(pressureSize = b) } })
        page.addView(slider(tr(S.MIN_SIZE), fslider(p.minSize), { pct(fraction(it)) }) { v -> set { it.copy(minSize = fraction(v)) } })
        page.addView(switch(tr(S.PRESSURE_OPACITY), null, p.pressureOpacity, true) { b -> set { it.copy(pressureOpacity = b) } })
        page.addView(slider(tr(S.MIN_OPACITY), fslider(p.minOpacity), { pct(fraction(it)) }) { v -> set { it.copy(minOpacity = fraction(v)) } })
        val tilt = host.stylus.tiltAvailable
        page.addView(switch(tr(S.TILT_SHAPE), if (tilt) null else tr(S.TILT_UNAVAILABLE), p.tilt && tilt, tilt) { b -> set { it.copy(tilt = b) } })
        page.addView(slider(tr(S.P_TILT_STRENGTH), p.tiltStrength, { pct(r2(it)) }) { v -> set { it.copy(tiltStrength = r2(v)) } })

        if (!preset.builtIn) {
            page.addView(header(tr(S.SEC_BRUSH_CATEGORY)))
            page.addView(choice(BrushCategory.entries.map { it to tr(it.label) }, { preset.category }) { c ->
                host.updateBrushes(host.brushes.setCategory(id, c))
            })
        }

        val sv = ScrollView(context).apply {
            clipToPadding = false
            setPadding(ins.left, 0, ins.right, 0)
            addView(MaxWidth(context, theme.dp(720)).apply { addView(page) })
        }
        root.addView(topBar(), FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP))
        root.addView(sv, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT).apply {
            topMargin = ins.top + theme.touchTarget + theme.dp(16)
        })
        return root
    }

    override fun onRemoved() {
        handler.removeCallbacksAndMessages(null)
        pad?.close()
    }

    /** Applies a change to the brush (built-in: as a resettable override). */
    private fun set(change: (BrushParams) -> BrushParams) {
        val before = host.brushes.isModified(id)
        host.updateBrushes(host.brushes.update(id, change(params)))
        handler.removeCallbacks(refreshPreview)
        handler.postDelayed(refreshPreview, 60)
        if (before != host.brushes.isModified(id)) refreshNotes()
        else if (params.wetEdge > 0f != (notes?.tag == true)) refreshNotes()
    }

    private fun refreshNotes() {
        val n = notes ?: return
        n.removeAllViews()
        val p = preset
        if (p.builtIn) {
            n.addView(theme.text(context, tr(S.BUILTIN_NOTE), PxTheme.Type.CAPTION, theme.textSecondary).apply { setPadding(0, theme.dp(8), 0, 0) })
            if (host.brushes.isModified(p.id)) n.addView(Widgets.pill(context, theme, tr(S.RESET_BRUSH), false) { reset() },
                LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = theme.dp(8) })
        }
        val wet = params.wetEdge > 0f
        n.tag = wet
        if (wet) n.addView(theme.text(context, tr(S.WATERCOLOR_NOTE), PxTheme.Type.CAPTION, theme.textSecondary).apply { setPadding(0, theme.dp(6), 0, 0) })
    }

    private fun reset() {
        val p = preset
        host.overlays.confirm(tr(S.RESET_BRUSH), tr(S.RESET_BRUSH_BODY, p.displayName()), tr(S.RESET_BRUSH), tr(S.ACTION_CANCEL), false) {
            host.updateBrushes(host.brushes.reset(p.id))
            host.overlays.message(tr(S.BRUSH_RESET))
            host.replaceTop(BrushEditorScreen(host, tool))   // every control back to the original values
        }
    }

    private fun topBar(): View = LinearLayout(context).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        setBackgroundColor(theme.bg)
        setPadding(host.insets.left + theme.dp(4), host.insets.top + theme.dp(8), host.insets.right + theme.dp(8), theme.dp(8))
        addView(IconButton(context, theme, Icon.BACK, tr(S.ACTION_CLOSE)).apply { setOnClickListener { host.pop() } })
        titleView = theme.text(context, preset.displayName(), PxTheme.Type.TITLE, lines = 1).apply {
            setPadding(theme.dp(8), 0, 0, 0)
            markHeading()
        }
        addView(titleView, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        val more = IconButton(context, theme, Icon.MORE, tr(S.MORE_OPTIONS))
        more.setOnClickListener { menu(more) }
        addView(more)
    }

    private fun menu(anchor: View) {
        val lib = host.brushes
        val p = preset
        val items = ArrayList<OverlayHost.MenuItem>()
        items += OverlayHost.MenuItem(tr(S.SAVE_AS_NEW), Icon.COPY) {
            val newId = BrushLibraryPanel.newUserId(host.brushes)
            val next = host.brushes.duplicate(p.id, newId, tool)
            if (next.get(newId) != null) {
                host.updateBrushes(next.select(tool, newId))
                host.overlays.message(tr(S.BRUSH_DUPLICATED))
                host.replaceTop(BrushEditorScreen(host, tool))
            }
        }
        if (lib.canRename(p.id)) items += OverlayHost.MenuItem(tr(S.ACTION_RENAME), Icon.EDIT) {
            host.overlays.prompt(tr(S.RENAME_BRUSH), p.name, tr(S.ACTION_SAVE), tr(S.ACTION_CANCEL)) { name ->
                host.updateBrushes(host.brushes.rename(p.id, name))
                titleView?.text = preset.displayName()
            }
        }
        if (lib.isModified(p.id)) items += OverlayHost.MenuItem(tr(S.RESET_BRUSH), Icon.RESET) { reset() }
        if (lib.canDelete(p.id)) items += OverlayHost.MenuItem(tr(S.ACTION_DELETE), Icon.TRASH, destructive = true) {
            host.overlays.confirm(tr(S.DELETE_TITLE, p.displayName()), tr(S.DELETE_BRUSH_BODY), tr(S.ACTION_DELETE), tr(S.ACTION_CANCEL), true) {
                host.updateBrushes(host.brushes.delete(p.id))
                host.overlays.message(tr(S.BRUSH_DELETED))
                host.pop()
            }
        }
        host.overlays.menu(anchor, items)
    }

    // ------------------------------------------------------------- widgets
    private fun header(title: String): View = Widgets.sectionHeader(context, theme, title).apply {
        setPadding(0, theme.dp(22), 0, theme.dp(4))
    }

    private fun label(text: String): View =
        theme.text(context, text, PxTheme.Type.LABEL).apply { setPadding(0, theme.dp(12), 0, theme.dp(6)) }

    private fun slider(label: String, initial: Float, format: (Float) -> String, apply: (Float) -> Unit): View =
        LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, theme.dp(10), 0, 0)
            val value = theme.text(context, format(initial), PxTheme.Type.LABEL, theme.textSecondary)
            addView(LinearLayout(context).apply {
                orientation = LinearLayout.HORIZONTAL
                addView(theme.text(context, label, PxTheme.Type.BODY), LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
                addView(value)
            })
            addView(PxSlider(context, theme, label).apply {
                valueText = format
                setValue(initial)
                onChange = { v, fromUser ->
                    value.text = format(v)
                    if (fromUser) apply(v)
                }
            })
        }

    /** Single choice as chips; re-selects in place (no page rebuild, scroll kept). */
    private fun <T> choice(options: List<Pair<T, String>>, current: () -> T, onSelect: (T) -> Unit): View {
        val row = FlowRow(context, theme.dp(8))
        fun fill() {
            row.removeAllViews()
            for ((v, text) in options) row.addView(Widgets.chip(context, theme, text, v == current()) {
                onSelect(v)
                fill()
            })
        }
        fill()
        return row
    }

    private fun switch(title: String, desc: String?, checked: Boolean, enabled: Boolean, onChange: (Boolean) -> Unit): View =
        Widgets.switchRow(context, theme, title, desc, checked, onChange).apply {
            setPadding(0, paddingTop, 0, paddingBottom)
            isEnabled = enabled
            alpha = if (enabled) 1f else 0.5f
            if (!enabled) (this as? android.view.ViewGroup)?.let { g -> for (i in 0 until g.childCount) g.getChildAt(i).isEnabled = false }
        }

    /**
     * Scratch pad: what you draw here is rendered by the engine's brush code
     * (finger or stylus, with the stylus's real axes). Strokes are kept and
     * re-rendered when a parameter changes, so the effect is visible at once.
     */
    @SuppressLint("ViewConstructor")
    private inner class TryPad(context: Context) : View(context) {
        private val strokes = ArrayList<FloatArray>()
        private val current = SampleBuffer()
        private var pointer = -1
        private var kind = ToolKind.UNKNOWN
        private var t0 = 0L
        private var image: Bitmap? = null
        private val exec = Executors.newSingleThreadExecutor { r -> Thread(r, "PxPaint-trypad") }
        private val generation = AtomicInteger()
        private val bg = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = BrushPreviews.PAPER }
        private val bmpPaint = Paint(Paint.FILTER_BITMAP_FLAG)
        private val hint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = 0xFF8A8E95.toInt()
            textSize = theme.dpF(13f)
            textAlign = Paint.Align.CENTER
        }
        private val rect = RectF()

        init {
            contentDescription = tr(S.TRY_PAD)
        }

        fun clear() {
            strokes.clear()
            image = null
            generation.incrementAndGet()
            invalidate()
        }

        fun close() = exec.shutdownNow()

        fun paramsChanged() = render()

        @SuppressLint("ClickableViewAccessibility")
        override fun onTouchEvent(e: MotionEvent): Boolean {
            when (e.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    parent?.requestDisallowInterceptTouchEvent(true)
                    host.stylus.onEvent(e)
                    pointer = e.getPointerId(0)
                    kind = StylusInput.kind(e, 0)
                    t0 = e.eventTime
                    current.clear()
                    StylusInput.append(e, pointer, kind, t0, current, includeHistory = false)
                    render()
                }
                MotionEvent.ACTION_MOVE -> {
                    StylusInput.append(e, pointer, kind, t0, current)
                    render()
                }
                MotionEvent.ACTION_UP -> {
                    StylusInput.append(e, pointer, kind, t0, current)
                    if (current.count > 0) strokes += current.data.copyOf(current.count * SampleBuffer.STRIDE)
                    while (strokes.size > MAX_STROKES) strokes.removeAt(0)
                    current.clear()
                    pointer = -1
                    render()
                }
                MotionEvent.ACTION_CANCEL -> {
                    current.clear()
                    pointer = -1
                    render()
                }
            }
            return true
        }

        private fun render() {
            val w = width
            val h = height
            if (w <= 0 || h <= 0) return
            val all = ArrayList(strokes)
            if (current.count > 0) all += current.data.copyOf(current.count * SampleBuffer.STRIDE)
            val p = params
            val d = BrushPreviewSpec.fitDiameter(p.size, h * 0.8f)
            val curve = host.pressureTable()
            val gen = generation.incrementAndGet()
            exec.execute {
                if (gen != generation.get()) return@execute   // a newer request supersedes this one
                val b = BrushPreviewSpec.Builder()
                for (s in all) {
                    val pts = (0 until s.size / NativeEngine.SAMPLE_FLOATS).map { i ->
                        s.copyOfRange(i * NativeEngine.SAMPLE_FLOATS, (i + 1) * NativeEngine.SAMPLE_FLOATS)
                    }
                    b.stroke(d, -1f, pts)
                }
                val bmp = host.previews.render(p, w, h, BrushPreviews.INK, BrushPreviews.PAPER, curve, b.build())
                post {
                    if (gen == generation.get() || image == null) {
                        image = bmp
                        invalidate()
                    }
                }
            }
        }

        override fun onDraw(canvas: Canvas) {
            rect.set(0f, 0f, width.toFloat(), height.toFloat())
            val r = theme.dpF(12f)
            canvas.drawRoundRect(rect, r, r, bg)
            val img = image
            if (img != null && strokes.isNotEmpty() || current.count > 0) {
                img?.let { canvas.drawBitmap(it, 0f, 0f, bmpPaint) }
            } else {
                canvas.drawText(tr(S.TRY_PAD), width / 2f, height / 2f, hint)
            }
        }
    }

    companion object {
        private const val MAX_STROKES = 16
    }
}
