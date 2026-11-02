package com.paintengine.pxpaint.screens

import android.annotation.SuppressLint
import android.graphics.Typeface
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import com.paintengine.engine.EngineSurfaceView
import com.paintengine.engine.NativeEngine
import com.paintengine.engine.NativeEngine.ViewOp
import com.paintengine.pxpaint.core.ContentLog
import com.paintengine.pxpaint.core.DockPlacement
import com.paintengine.pxpaint.core.DrawGate
import com.paintengine.pxpaint.core.Format
import com.paintengine.pxpaint.core.LayoutPolicy
import com.paintengine.pxpaint.core.P
import com.paintengine.pxpaint.core.ProjectMeta
import com.paintengine.pxpaint.core.RotationSnap
import com.paintengine.pxpaint.core.S
import com.paintengine.pxpaint.core.StrokeSampler
import com.paintengine.pxpaint.core.TwoFinger
import com.paintengine.pxpaint.core.WorkspaceSpec
import com.paintengine.pxpaint.core.tr
import com.paintengine.pxpaint.ui.CheckerDrawable
import com.paintengine.pxpaint.ui.ColorPicker
import com.paintengine.pxpaint.ui.Icon
import com.paintengine.pxpaint.ui.IconButton
import com.paintengine.pxpaint.ui.IconDrawable
import com.paintengine.pxpaint.ui.OverlayHost
import com.paintengine.pxpaint.ui.PxSlider
import com.paintengine.pxpaint.ui.PxTheme
import com.paintengine.pxpaint.ui.Thumbnails
import com.paintengine.pxpaint.ui.Widgets
import com.paintengine.pxpaint.ui.column
import org.json.JSONObject
import kotlin.math.exp
import kotlin.math.ln
import kotlin.math.roundToInt

/**
 * The painting workspace: GPU canvas (EngineSurfaceView) under floating,
 * adaptive chrome. Layout from LayoutPolicy: bottom dock on portrait
 * phones, side rail in landscape / on tablets, docked side panel on wide
 * tablets.
 *
 * Input: one finger paints with the active tool (after a short DrawGate so
 * a pinch never leaves a stray dab), two fingers pan/zoom/rotate the
 * viewport (exact, focal point under the fingers). The Move tool, or
 * "paint with a finger" off, makes one finger pan.
 */
class WorkspaceScreen(host: PxHost, private var meta: ProjectMeta) : Screen(host) {
    enum class Tool(val icon: Icon, val label: S, val desc: S) {
        MARKER(Icon.MARKER, S.TOOL_MARKER, S.TOOL_MARKER_DESC),
        ERASER(Icon.ERASER, S.TOOL_ERASER, S.TOOL_ERASER_DESC),
        MOVE(Icon.MOVE, S.TOOL_MOVE, S.TOOL_MOVE_DESC),
    }

    private enum class Panel { NONE, TOOL, COLOR, LAYERS }

    private val handler = Handler(Looper.getMainLooper())
    private var engine: NativeEngine? = null
    private var surfaceView: EngineSurfaceView? = null
    private var chrome: FrameLayout? = null
    private var spec: WorkspaceSpec? = null

    // Document state
    private var opened = false
    private var loaded = false
    private var closed = false
    private var savedRevision = -1L
    private var lastAutosave = 0L
    private var limitWarned = false

    // Tools
    private var tool = Tool.MARKER
    private var markerSize = 24
    private var eraserSize = 64
    private var color = 0xFF2B3A67.toInt()
    private val recentColors = ArrayList<Int>()
    private var panel = Panel.NONE
    private var view = NativeEngine.ViewportInfo.EMPTY

    // Chrome references
    private var zoomChip: TextView? = null
    private var hud: TextView? = null
    private val toolButtons = HashMap<Tool, IconButton>()
    private var colorButton: View? = null

    // Input
    private val gate = DrawGate()
    private val sampler = StrokeSampler(4.0)
    private var mode = Mode.IDLE
    private var idA = -1
    private var idB = -1
    private var prevA = P(0.0, 0.0)
    private var prevB = P(0.0, 0.0)
    private var panLast = P(0.0, 0.0)
    private val tick = Runnable { onGateTick() }

    private enum class Mode { IDLE, DRAW, PAN, GESTURE, IGNORE }

    private val poll = object : Runnable {
        override fun run() {
            pollStatus()
            handler.postDelayed(this, 250)
        }
    }

    // ---------------------------------------------------------------- views
    override fun createView(): View {
        val root = FrameLayout(context).apply { setBackgroundColor(theme.workspace) }
        val sv = surfaceView ?: EngineSurfaceView(context).also { surfaceView = it }
        (sv.parent as? ViewGroup)?.removeView(sv)
        sv.contentDescription = meta.name
        sv.engine = engine
        @SuppressLint("ClickableViewAccessibility")
        sv.setOnTouchListener { _, e -> onCanvasTouch(e); true }
        root.addView(sv, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
        val c = FrameLayout(context)
        chrome = c
        root.addView(c, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
        buildChrome()
        return root
    }

    override fun onLayoutChanged(): Boolean {
        buildChrome()
        return false   // keep the SurfaceView: only the chrome adapts
    }

    override fun onShown() {
        if (!opened) openCanvas()
        else if (!closed) engine?.setCanvasColors(theme.workspace, meta.paperArgb, meta.transparentBackground)
        handler.removeCallbacks(poll)
        handler.post(poll)
    }

    override fun onRemoved() {
        handler.removeCallbacks(poll)
        handler.removeCallbacks(tick)
        closeCanvas()
    }

    override fun onPause() {
        save()
    }

    override fun onBack(): Boolean {
        if (panel != Panel.NONE) {
            setPanel(Panel.NONE)
            return true
        }
        return false   // the host pops; onRemoved saves and closes
    }

    // ------------------------------------------------------------- document
    private fun openCanvas() {
        val e = host.engine()
        if (e == null) {
            host.overlays.message(tr(S.CANVAS_OPEN_FAILED, "engine"))
            host.pop()
            return
        }
        engine = e
        surfaceView?.engine = e
        opened = true
        e.setScene(NativeEngine.Scene.CANVAS)
        e.setCanvasColors(theme.workspace, meta.paperArgb, meta.transparentBackground)
        val err = e.createCanvas(meta.width, meta.height, meta.paperArgb, meta.transparentBackground)
        if (err != null) {
            host.overlays.message(tr(S.CANVAS_OPEN_FAILED, err), long = true)
            closed = true
            host.pop()
            return
        }
        // If the SurfaceView already has a surface (rebuild), hand it to the engine.
        surfaceView?.let { sv ->
            if (sv.hasSurface && sv.holder.surface.isValid) {
                e.surfaceCreated(sv.holder.surface)
                e.surfaceChanged(sv.surfaceWidth, sv.surfaceHeight)
            }
        }
        val id = meta.id
        host.io.execute {
            val ops = runCatching { host.store.loadContent(id) }
            host.runOnMain {
                if (closed) return@runOnMain
                ops.onSuccess {
                    e.loadCanvasContent(it)
                    savedRevision = e.canvasContentRevision()
                    lastAutosave = SystemClock.uptimeMillis()
                    loaded = true
                }.onFailure {
                    // Never overwrite damaged content with an empty canvas.
                    host.overlays.message(tr(S.CANVAS_OPEN_FAILED, it.message ?: "?"), long = true)
                    closed = true
                    e.closeCanvas()
                    host.pop()
                }
            }
        }
        if (!host.tipsShown(TIP_GESTURES)) handler.postDelayed({ showTip() }, 400)
    }

    private fun closeCanvas() {
        if (closed) return
        save()
        closed = true
        engine?.closeCanvas()
        surfaceView?.engine = engine   // keep forwarding the surface release
    }

    /** Persists content + thumbnail on the io thread if anything changed. */
    private fun save() {
        val e = engine ?: return
        if (!loaded || closed) return
        val rev = e.canvasContentRevision()
        if (rev == savedRevision) return
        savedRevision = rev
        lastAutosave = SystemClock.uptimeMillis()
        val ops = e.canvasContent()
        val id = meta.id
        host.io.execute {
            val r = runCatching {
                val updated = host.store.saveContent(id, ops)
                val thumb = Thumbnails.render(updated, ContentLog.clip(ops, updated.width, updated.height))
                host.store.saveThumbnail(id, Thumbnails.png(thumb))
                updated
            }
            host.runOnMain {
                r.onSuccess { meta = it }.onFailure { host.overlays.message(tr(S.SAVE_FAILED, it.message ?: "?"), long = true) }
            }
        }
    }

    // --------------------------------------------------------------- status
    private fun pollStatus() {
        val e = engine ?: return
        if (closed) return
        val s = runCatching { JSONObject(e.statusJson()) }.getOrNull() ?: return
        if (s.optBoolean("canvasOpen") && !s.optBoolean("canvasGpuReady") && s.optString("canvasError").isNotEmpty()) {
            host.overlays.message(tr(S.CANVAS_OPEN_FAILED, s.optString("canvasError")), long = true)
            closed = true
            e.closeCanvas()
            host.pop()
            return
        }
        updateViewport(e.viewport(ViewOp.QUERY))
        hud?.let { h ->
            val fps = if (s.optBoolean("rateValid")) String.format(java.util.Locale.US, " · %.0f fps", s.optDouble("fps")) else ""
            h.text = String.format(
                java.util.Locale.US, "%s · work %.2f ms%s\ntiles %d/%d · %s · ops %d\ndraws %d · uploads %s",
                s.optString("backend"), s.optDouble("steadyAvgRecordMs") + s.optDouble("steadyAvgSubmitMs"), fps,
                s.optInt("canvasTilesAllocated"), s.optInt("canvasTilesTotal"),
                Format.bytes(s.optLong("canvasBytesAllocated")), s.optInt("canvasJournalOps"),
                s.optInt("canvasDrawCalls"), Format.bytes(s.optLong("canvasBytesUploaded")),
            )
        }
        if (loaded && SystemClock.uptimeMillis() - lastAutosave > AUTOSAVE_MS) save()
    }

    private fun updateViewport(v: NativeEngine.ViewportInfo) {
        view = v
        val chip = zoomChip ?: return
        val txt = "${Format.zoomPercent(v.zoom)} · ${Format.degrees(v.rotation)}"
        if (chip.text != txt) {
            chip.text = txt
            chip.contentDescription = tr(S.ZOOM_CHIP_DESC, Format.zoomPercent(v.zoom), Format.degrees(v.rotation))
        }
    }

    private fun viewOp(op: ViewOp, vararg args: Double) {
        val e = engine ?: return
        updateViewport(e.viewport(op, *args))
    }

    private fun screenCenter() = doubleArrayOf((surfaceView?.width ?: 0) / 2.0, (surfaceView?.height ?: 0) / 2.0)

    // ---------------------------------------------------------------- input
    private fun onCanvasTouch(e: MotionEvent) {
        if (engine == null || !loaded || closed) return
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                if (panel != Panel.NONE && spec?.sidePanelDocked != true) {
                    setPanel(Panel.NONE)
                    mode = Mode.IGNORE
                    return
                }
                val p = P(e.x.toDouble(), e.y.toDouble())
                val paints = tool != Tool.MOVE && host.settings.fingerDrawing
                if (paints) {
                    mode = Mode.DRAW
                    endStroke()
                    gate.down(e.eventTime, p)
                    handler.postDelayed(tick, 100)
                } else {
                    mode = Mode.PAN
                    panLast = p
                }
            }
            MotionEvent.ACTION_POINTER_DOWN -> {
                if (mode == Mode.DRAW) {
                    gate.secondPointer()
                    endStroke()
                    handler.removeCallbacks(tick)
                }
                if (e.pointerCount >= 2) startGesture(e)
            }
            MotionEvent.ACTION_MOVE -> when (mode) {
                Mode.DRAW -> {
                    for (h in 0 until e.historySize) {
                        draw(gate.move(e.getHistoricalEventTime(h), P(e.getHistoricalX(0, h).toDouble(), e.getHistoricalY(0, h).toDouble())))
                    }
                    draw(gate.move(e.eventTime, P(e.x.toDouble(), e.y.toDouble())))
                }
                Mode.PAN -> {
                    val p = P(e.x.toDouble(), e.y.toDouble())
                    viewOp(ViewOp.PAN_BY, p.x - panLast.x, p.y - panLast.y)
                    panLast = p
                }
                Mode.GESTURE -> {
                    val ia = e.findPointerIndex(idA)
                    val ib = e.findPointerIndex(idB)
                    if (ia < 0 || ib < 0) return
                    val a = P(e.getX(ia).toDouble(), e.getY(ia).toDouble())
                    val b = P(e.getX(ib).toDouble(), e.getY(ib).toDouble())
                    val st = TwoFinger.step(prevA, prevB, a, b, host.settings.twoFingerRotate)
                    viewOp(ViewOp.GESTURE, st.prevFocal.x, st.prevFocal.y, st.newFocal.x, st.newFocal.y, st.scale, st.rotation)
                    prevA = a
                    prevB = b
                }
                else -> Unit
            }
            MotionEvent.ACTION_POINTER_UP -> if (mode == Mode.GESTURE) {
                val lifted = e.getPointerId(e.actionIndex)
                if (lifted == idA || lifted == idB) {
                    if (e.pointerCount - 1 >= 2) startGesture(e, excludeId = lifted)
                    else endGesture()
                }
            }
            MotionEvent.ACTION_UP -> {
                if (mode == Mode.DRAW) draw(gate.up())
                handler.removeCallbacks(tick)
                endStroke()
                gate.reset()
                mode = Mode.IDLE
            }
            MotionEvent.ACTION_CANCEL -> {
                handler.removeCallbacks(tick)
                endStroke()
                gate.reset()
                if (mode == Mode.GESTURE) endGesture()
                mode = Mode.IDLE
            }
        }
    }

    private fun startGesture(e: MotionEvent, excludeId: Int = -1) {
        val ids = (0 until e.pointerCount).map { e.getPointerId(it) }.filter { it != excludeId }
        if (ids.size < 2) return
        idA = ids[0]
        idB = ids[1]
        val ia = e.findPointerIndex(idA)
        val ib = e.findPointerIndex(idB)
        prevA = P(e.getX(ia).toDouble(), e.getY(ia).toDouble())
        prevB = P(e.getX(ib).toDouble(), e.getY(ib).toDouble())
        mode = Mode.GESTURE
    }

    private fun endGesture() {
        mode = Mode.IGNORE
        if (host.settings.rotationSnap && host.settings.twoFingerRotate) {
            RotationSnap.target(view.rotation)?.let { target ->
                if (target != view.rotation) {
                    val f = P((prevA.x + prevB.x) / 2, (prevA.y + prevB.y) / 2)
                    viewOp(ViewOp.SET_ROTATION_AT, target, f.x, f.y)
                }
            }
        }
    }

    private fun onGateTick() {
        if (mode != Mode.DRAW) return
        val pts = gate.tick(SystemClock.uptimeMillis())
        if (pts.isNotEmpty()) draw(pts) else if (gate.state == DrawGate.State.PENDING) handler.postDelayed(tick, 30)
    }

    private fun draw(points: List<P>) {
        if (points.isEmpty()) return
        val e = engine ?: return
        val size = if (tool == Tool.ERASER) eraserSize else markerSize
        val argb = if (tool == Tool.ERASER) 0 else color
        // Dab spacing is ~30% of the tip, measured on screen.
        sampler.spacing = maxOf(1.0, size * 0.3 * view.zoom)
        for (p in points) {
            val dabs = if (strokeStarted) sampler.moveTo(p) else sampler.begin(p).also { strokeStarted = true }
            for (d in dabs) {
                if (!e.stamp(d.x, d.y, size, argb)) checkLimit(d)
            }
        }
    }

    /** Set by the first dab of a stroke; cleared whenever the sampler ends. */
    private var strokeStarted = false

    private fun endStroke() {
        sampler.end()
        strokeStarted = false
    }

    private fun checkLimit(d: P) {
        val e = engine ?: return
        val c = e.screenToCanvas(d.x, d.y)
        val inside = c[0] >= 0 && c[1] >= 0 && c[0] < meta.width && c[1] < meta.height
        if (inside && !limitWarned && ContentLog.opCount(e.canvasContent()) >= ContentLog.MAX_OPS) {
            limitWarned = true
            host.overlays.message(tr(S.CANVAS_FULL, ContentLog.MAX_OPS), long = true)
        }
    }

    // --------------------------------------------------------------- chrome
    private fun buildChrome() {
        val c = chrome ?: return
        c.removeAllViews()
        toolButtons.clear()
        val s = LayoutPolicy.workspace(host.windowWidthDp, host.windowHeightDp, host.smallestWidthDp, host.settings)
        spec = s
        val ins = host.insets
        val m = theme.dp(8)

        // Top bar
        val bar = topBar(s)
        c.addView(bar, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP).apply {
            setMargins(ins.left + m, ins.top + m, ins.right + m, 0)
        })

        // Zoom / rotation chip
        zoomChip = theme.text(context, "", PxTheme.Type.LABEL, theme.text).apply {
            gravity = Gravity.CENTER
            minHeight = theme.dp(32)
            setPadding(theme.dp(12), 0, theme.dp(12), 0)
            background = theme.pressable(theme.rounded(theme.chrome, theme.dpF(999f), theme.outline), theme.dpF(999f))
            typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
            isClickable = true
            setOnClickListener { viewOp(ViewOp.FIT) }
            setOnLongClickListener { canvasMenu(this); true }
        }
        val barH = theme.touchTarget + theme.dp(if (s.compactTopBar) 8 else 16)
        c.addView(zoomChip, FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP or Gravity.CENTER_HORIZONTAL).apply {
            topMargin = ins.top + m + barH + theme.dp(8)
        })
        updateViewport(view)

        // Dock
        val dock = dock(s)
        val dockLp = when (s.dock) {
            DockPlacement.BOTTOM -> FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL).apply {
                bottomMargin = ins.bottom + theme.dp(14)
            }
            DockPlacement.LEFT -> FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.START or Gravity.CENTER_VERTICAL).apply {
                marginStart = ins.left + theme.dp(10)
            }
            DockPlacement.RIGHT -> FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.END or Gravity.CENTER_VERTICAL).apply {
                marginEnd = ins.right + theme.dp(10)
            }
        }
        c.addView(dock, dockLp)

        // Docked side panel (wide tablets): tool settings + colour, always visible.
        if (s.sidePanelDocked) {
            val side = column(context, theme, horizontalPadding = false) {
                setPadding(theme.dp(16), theme.dp(16), theme.dp(16), theme.dp(16))
                background = theme.rounded(theme.chrome, theme.radiusL, theme.outline)
                addView(toolSettings())
                addView(Widgets.divider(context, theme).apply { (layoutParams as LinearLayout.LayoutParams).setMargins(0, theme.dp(16), 0, theme.dp(16)) })
                addView(colorPicker())
            }
            val onRight = s.dock != DockPlacement.RIGHT
            c.addView(android.widget.ScrollView(context).apply { addView(side) }, FrameLayout.LayoutParams(theme.dp(300), FrameLayout.LayoutParams.WRAP_CONTENT,
                (if (onRight) Gravity.END else Gravity.START) or Gravity.CENTER_VERTICAL).apply {
                if (onRight) marginEnd = ins.right + theme.dp(12) else marginStart = ins.left + theme.dp(12)
                topMargin = ins.top + barH + theme.dp(56)
                bottomMargin = ins.bottom + theme.dp(16)
            })
        }

        // Performance overlay
        hud = if (host.settings.performanceOverlay) theme.text(context, "", PxTheme.Type.CAPTION, theme.textSecondary).apply {
            typeface = Typeface.MONOSPACE
            background = theme.rounded(theme.chrome, theme.radiusS, theme.outline)
            setPadding(theme.dp(10), theme.dp(6), theme.dp(10), theme.dp(6))
            importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO
        }.also {
            c.addView(it, FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP or Gravity.START).apply {
                setMargins(ins.left + theme.dp(12), ins.top + m + barH + theme.dp(52), 0, 0)
            })
        } else null

        if (panel != Panel.NONE) showPanel(panel)
        refreshDock()
    }

    private fun topBar(s: WorkspaceSpec): View = LinearLayout(context).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        background = theme.rounded(theme.chrome, theme.radiusL, theme.outline)
        val v = theme.dp(if (s.compactTopBar) 2 else 6)
        setPadding(theme.dp(4), v, theme.dp(4), v)
        addView(IconButton(context, theme, Icon.BACK, tr(S.BACK_TO_GALLERY)).apply { setOnClickListener { host.pop() } })
        val title = theme.text(context, meta.name, if (s.compactTopBar) PxTheme.Type.LABEL else PxTheme.Type.HEADLINE, lines = 1).apply {
            setPadding(theme.dp(6), 0, theme.dp(6), 0)
            gravity = Gravity.CENTER_VERTICAL
            minHeight = theme.touchTarget
            background = theme.pressable(null, theme.radiusS)
            isClickable = true
            contentDescription = "${tr(S.ACTION_RENAME)}: ${meta.name}"
            setOnClickListener { rename() }
        }
        addView(title, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        for ((icon, label) in listOf(Icon.UNDO to S.UNDO, Icon.REDO to S.REDO)) {
            addView(IconButton(context, theme, icon, "${tr(label)} — ${tr(S.UNDO_SOON)}").apply {
                alpha = 0.4f
                setOnClickListener { host.overlays.message(tr(S.UNDO_SOON)) }
            })
        }
        val more = IconButton(context, theme, Icon.MORE, tr(S.CANVAS_MENU))
        more.setOnClickListener { canvasMenu(more) }
        addView(more)
    }

    private fun dock(s: WorkspaceSpec): View {
        val vertical = s.dock != DockPlacement.BOTTOM
        val box = LinearLayout(context).apply {
            orientation = if (vertical) LinearLayout.VERTICAL else LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER
            background = theme.rounded(theme.chrome, theme.radiusL, theme.outline)
            elevation = theme.dpF(4f)
            val p = theme.dp(6)
            setPadding(p, p, p, p)
        }
        for (t in Tool.entries) {
            val b = IconButton(context, theme, t.icon, tr(t.label), label = if (s.toolLabels) tr(t.label) else null)
            b.setOnClickListener { selectTool(t) }
            toolButtons[t] = b
            box.addView(b)
        }
        box.addView(View(context).apply { setBackgroundColor(theme.outline) },
            if (vertical) LinearLayout.LayoutParams(theme.dp(24), maxOf(1, theme.dp(1f))).apply { setMargins(0, theme.dp(6), 0, theme.dp(6)) }
            else LinearLayout.LayoutParams(maxOf(1, theme.dp(1f)), theme.dp(24)).apply { setMargins(theme.dp(6), 0, theme.dp(6), 0) })
        val swatch = FrameLayout(context).apply {
            minimumWidth = theme.touchTarget
            minimumHeight = theme.touchTarget
            contentDescription = tr(S.COLOR)
            tooltipText = tr(S.COLOR)
            isClickable = true
            background = theme.pressable(null, theme.radiusS)
            addView(View(context).apply { background = theme.oval(color, theme.text) },
                FrameLayout.LayoutParams(theme.dp(28), theme.dp(28), Gravity.CENTER))
            if (!s.sidePanelDocked) setOnClickListener { togglePanel(Panel.COLOR) }
        }
        colorButton = swatch
        box.addView(swatch)
        box.addView(IconButton(context, theme, Icon.LAYERS, tr(S.LAYERS), label = if (s.toolLabels) tr(S.LAYERS) else null).apply {
            setOnClickListener { togglePanel(Panel.LAYERS) }
        })
        return box
    }

    private fun refreshDock() {
        for ((t, b) in toolButtons) b.isSelected = t == tool
        (colorButton as? FrameLayout)?.getChildAt(0)?.background = theme.oval(color, theme.text)
    }

    private fun selectTool(t: Tool) {
        if (t == tool && t != Tool.MOVE && spec?.sidePanelDocked != true) {
            togglePanel(Panel.TOOL)
        } else {
            tool = t
            if (panel == Panel.TOOL) showPanel(Panel.TOOL)
            if (spec?.sidePanelDocked == true) buildChrome()
            host.overlays.message(tr(t.desc))
        }
        refreshDock()
    }

    // --------------------------------------------------------------- panels
    private var panelView: View? = null

    private fun togglePanel(p: Panel) = setPanel(if (panel == p) Panel.NONE else p)

    private fun setPanel(p: Panel) {
        panel = p
        panelView?.let { chrome?.removeView(it) }
        panelView = null
        if (p != Panel.NONE) showPanel(p)
    }

    private fun showPanel(p: Panel) {
        val c = chrome ?: return
        panelView?.let { c.removeView(it) }
        val s = spec ?: return
        if (p == Panel.TOOL && tool == Tool.MOVE) return
        val content = when (p) {
            Panel.TOOL -> toolSettings()
            Panel.COLOR -> colorPicker()
            Panel.LAYERS -> layersPanel()
            Panel.NONE -> return
        }
        val card = column(context, theme, horizontalPadding = false) {
            setPadding(theme.dp(16), theme.dp(14), theme.dp(16), theme.dp(16))
            background = theme.rounded(theme.chrome, theme.radiusL, theme.outline)
            elevation = theme.dpF(8f)
            isClickable = true
            addView(content)
        }
        val ins = host.insets
        val width = minOf(theme.dp(320), c.width - theme.dp(24)).coerceAtLeast(theme.dp(240))
        val lp = when (s.dock) {
            DockPlacement.BOTTOM -> FrameLayout.LayoutParams(width, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL).apply {
                bottomMargin = ins.bottom + theme.dp(14) + theme.touchTarget + theme.dp(if (s.toolLabels) 40 else 24)
            }
            DockPlacement.LEFT -> FrameLayout.LayoutParams(width, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.START or Gravity.CENTER_VERTICAL).apply {
                marginStart = ins.left + theme.dp(10) + theme.touchTarget + theme.dp(if (s.toolLabels) 40 else 24)
            }
            DockPlacement.RIGHT -> FrameLayout.LayoutParams(width, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.END or Gravity.CENTER_VERTICAL).apply {
                marginEnd = ins.right + theme.dp(10) + theme.touchTarget + theme.dp(if (s.toolLabels) 40 else 24)
            }
        }
        val scroll = android.widget.ScrollView(context).apply { addView(card) }
        panelView = scroll
        c.addView(scroll, lp)
    }

    private fun toolSettings(): View = column(context, theme, horizontalPadding = false) {
        val t = if (tool == Tool.MOVE) Tool.MARKER else tool
        addView(theme.text(context, tr(t.label), PxTheme.Type.HEADLINE))
        addView(theme.text(context, tr(t.desc), PxTheme.Type.CAPTION, theme.textSecondary).apply { setPadding(0, theme.dp(4), 0, theme.dp(10)) })
        val current = if (t == Tool.ERASER) eraserSize else markerSize
        val sizeLabel = theme.text(context, "$current px", PxTheme.Type.LABEL, theme.textSecondary)
        addView(LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(theme.text(context, tr(S.SIZE), PxTheme.Type.LABEL), LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            addView(sizeLabel)
        })
        addView(PxSlider(context, theme, tr(S.SIZE)).apply {
            valueText = { "${sizeFor(it)} px" }
            setValue(sliderFor(current))
            onChange = { v, _ ->
                val px = sizeFor(v)
                if (t == Tool.ERASER) eraserSize = px else markerSize = px
                sizeLabel.text = "$px px"
            }
        })
        addView(LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(0, theme.dp(8), 0, 0)
            addView(theme.text(context, tr(S.OPACITY), PxTheme.Type.LABEL, theme.textTertiary), LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            addView(theme.text(context, "100%", PxTheme.Type.LABEL, theme.textTertiary))
        })
        addView(PxSlider(context, theme, "${tr(S.OPACITY)} — ${tr(S.OPACITY_SOON)}").apply {
            setValue(1f)
            isEnabled = false
        })
        addView(theme.text(context, tr(S.OPACITY_SOON), PxTheme.Type.CAPTION, theme.textTertiary))
    }

    private fun sizeFor(v: Float): Int = exp(v * ln(MAX_SIZE.toDouble())).roundToInt().coerceIn(1, MAX_SIZE)
    private fun sliderFor(px: Int): Float = (ln(px.toDouble()) / ln(MAX_SIZE.toDouble())).toFloat()

    private fun colorPicker(): View = column(context, theme, horizontalPadding = false) {
        addView(theme.text(context, tr(S.COLOR), PxTheme.Type.HEADLINE).apply { setPadding(0, 0, 0, theme.dp(12)) })
        addView(ColorPicker(context, theme, color, recentColors) { c ->
            color = c
            if (tool == Tool.ERASER || tool == Tool.MOVE) tool = Tool.MARKER
            remember(c)
            refreshDock()
        })
    }

    private fun remember(c: Int) {
        // Debounced: the colour that stays selected for a moment becomes "recent".
        handler.removeCallbacks(rememberColor)
        handler.postDelayed(rememberColor, 700)
        pendingRecent = c
    }

    private var pendingRecent = 0
    private val rememberColor = Runnable {
        recentColors.remove(pendingRecent)
        recentColors.add(0, pendingRecent)
        while (recentColors.size > 8) recentColors.removeAt(recentColors.lastIndex)
    }

    private fun layersPanel(): View = column(context, theme, horizontalPadding = false) {
        addView(theme.text(context, tr(S.LAYERS), PxTheme.Type.HEADLINE).apply { setPadding(0, 0, 0, theme.dp(10)) })
        addView(layerRow(tr(S.LAYER_PAINT), Icon.MARKER, null, selected = true))
        addView(layerRow(tr(S.LAYER_BACKGROUND), null, if (meta.transparentBackground) null else meta.paperArgb, selected = false))
        // Background colour (real: the engine draws the paper / checkerboard).
        val papers = listOf(0xFFFFFFFF.toInt() to false, 0xFFF3EBDD.toInt() to false, 0xFF8A8D91.toInt() to false,
            0xFF111111.toInt() to false, 0xFFFFFFFF.toInt() to true)
        addView(LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(0, theme.dp(8), 0, theme.dp(12))
            for ((argb, transparent) in papers) {
                val selected = meta.transparentBackground == transparent && (transparent || meta.paperArgb == argb)
                addView(View(context).apply {
                    background = if (transparent) CheckerDrawable(theme.dp(5), false) else theme.oval(argb, if (selected) theme.accent else theme.outline)
                    if (transparent) {
                        outlineProvider = object : android.view.ViewOutlineProvider() {
                            override fun getOutline(v: View, o: android.graphics.Outline) = o.setOval(0, 0, v.width, v.height)
                        }
                        clipToOutline = true
                    }
                    foreground = if (selected) IconDrawable(Icon.CHECK, if (transparent || argb == 0xFF111111.toInt()) theme.accent else 0xFF222222.toInt()) else null
                    contentDescription = if (transparent) tr(S.BG_TRANSPARENT) else String.format(java.util.Locale.US, "#%06X", argb and 0xFFFFFF)
                    isClickable = true
                    setOnClickListener { setPaper(argb, transparent) }
                }, LinearLayout.LayoutParams(theme.dp(36), theme.dp(36)).apply { marginEnd = theme.dp(10) })
            }
        })
        addView(theme.text(context, tr(S.LAYERS_SOON_TITLE), PxTheme.Type.LABEL))
        addView(theme.text(context, tr(S.LAYERS_SOON_BODY), PxTheme.Type.CAPTION, theme.textSecondary).apply { setPadding(0, theme.dp(4), 0, 0) })
    }

    private fun layerRow(name: String, icon: Icon?, swatch: Int?, selected: Boolean): View = LinearLayout(context).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        minimumHeight = theme.touchTarget
        setPadding(theme.dp(10), 0, theme.dp(10), 0)
        background = theme.rounded(if (selected) theme.accentSoft else theme.surface, theme.radiusS, if (selected) theme.accent else theme.outline)
        val thumb = View(context).apply {
            background = when {
                swatch != null -> theme.rounded(swatch, theme.dpF(4f), theme.outline)
                icon != null -> IconDrawable(icon, theme.textSecondary)
                else -> CheckerDrawable(theme.dp(4), false)
            }
        }
        addView(thumb, LinearLayout.LayoutParams(theme.dp(24), theme.dp(24)))
        addView(theme.text(context, name, PxTheme.Type.LABEL).apply { setPadding(theme.dp(12), 0, 0, 0) })
        (layoutParams as? LinearLayout.LayoutParams)?.bottomMargin = theme.dp(6)
    }.also {
        it.layoutParams = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { bottomMargin = theme.dp(6) }
    }

    private fun setPaper(argb: Int, transparent: Boolean) {
        meta = meta.copy(paperArgb = argb, transparentBackground = transparent)
        engine?.setCanvasColors(theme.workspace, argb, transparent)
        val id = meta.id
        host.io.execute {
            runCatching {
                val updated = host.store.updateAppearance(id, argb, transparent)
                val ops = host.store.loadContent(id)
                host.store.saveThumbnail(id, Thumbnails.png(Thumbnails.render(updated, ops)))
            }
        }
        showPanel(Panel.LAYERS)
    }

    // ----------------------------------------------------------- menus & tips
    private fun canvasMenu(anchor: View) {
        val c = screenCenter()
        host.overlays.menu(anchor, listOf(
            OverlayHost.MenuItem(tr(S.FIT), Icon.FIT) { viewOp(ViewOp.FIT) },
            OverlayHost.MenuItem(tr(S.ACTUAL_PIXELS), Icon.PIXELS) { viewOp(ViewOp.SET_ZOOM_AT, 1.0, c[0], c[1]) },
            OverlayHost.MenuItem(tr(S.RESET_ROTATION), Icon.ROTATE) { viewOp(ViewOp.SET_ROTATION_AT, 0.0, c[0], c[1]) },
            OverlayHost.MenuItem(tr(S.CENTER), Icon.CENTER) { viewOp(ViewOp.CENTER) },
            OverlayHost.MenuItem(tr(S.CANVAS_INFO), Icon.INFO) { canvasInfo() },
            OverlayHost.MenuItem(tr(S.TEST_RENDERING), Icon.TARGET) { testRendering() },
            OverlayHost.MenuItem(tr(S.CLEAR_CANVAS), Icon.TRASH, destructive = true) { confirmClear() },
            OverlayHost.MenuItem(tr(S.SETTINGS), Icon.SLIDERS) { host.push(SettingsScreen(host)) },
        ))
    }

    private fun confirmClear() {
        host.overlays.confirm(tr(S.CLEAR_CANVAS), tr(S.CLEAR_CONFIRM), tr(S.ACTION_CLEAR), tr(S.ACTION_CANCEL), true) {
            engine?.clearCanvas()
            limitWarned = false
            save()
        }
    }

    private fun canvasInfo() {
        val s = runCatching { JSONObject(engine?.statusJson() ?: "{}") }.getOrDefault(JSONObject())
        val rows = listOf(
            tr(S.INFO_SIZE) to Format.dimensions(meta.width, meta.height),
            tr(S.INFO_STORAGE) to s.optString("canvasStorage"),
            tr(S.INFO_TILES) to "${s.optInt("canvasTilesAllocated")} / ${s.optInt("canvasTilesTotal")} · ${Format.bytes(s.optLong("canvasBytesAllocated"))}",
            tr(S.INFO_CONTENT) to s.optInt("canvasJournalOps").toString(),
            tr(S.INFO_GRAPHICS) to "${s.optString("backend")} · ${s.optString("gpu")}",
        )
        val content = column(context, theme) {
            for ((k, v) in rows) {
                addView(theme.text(context, k, PxTheme.Type.CAPTION, theme.textSecondary).apply { setPadding(0, theme.dp(10), 0, theme.dp(2)) })
                addView(theme.text(context, v, PxTheme.Type.BODY))
            }
        }
        host.overlays.sheet(content, meta.name)
    }

    private fun testRendering() {
        val e = engine ?: return
        val before = runCatching { JSONObject(e.statusJson()).optInt("canvasSelfTestRuns") }.getOrDefault(0)
        host.overlays.message(tr(S.SELFTEST_RUNNING))
        e.runCanvasSelfTest()
        val started = SystemClock.uptimeMillis()
        val check = object : Runnable {
            override fun run() {
                val s = runCatching { JSONObject(e.statusJson()) }.getOrNull() ?: return
                val status = s.optString("canvasSelfTest")
                if (s.optInt("canvasSelfTestRuns") > before && status != "pending") {
                    host.overlays.message(
                        if (status == "PASSED") tr(S.SELFTEST_PASSED) else tr(S.SELFTEST_FAILED, s.optString("canvasSelfTestDetail")),
                        long = status != "PASSED",
                    )
                } else if (SystemClock.uptimeMillis() - started < 15_000 && !closed) {
                    handler.postDelayed(this, 200)
                }
            }
        }
        handler.postDelayed(check, 200)
    }

    private fun rename() {
        host.overlays.prompt(tr(S.RENAME_TITLE), meta.name, tr(S.ACTION_SAVE), tr(S.ACTION_CANCEL)) { name ->
            val id = meta.id
            host.io.execute {
                val r = runCatching { host.store.rename(id, name) }
                host.runOnMain {
                    r.onSuccess {
                        meta = meta.copy(name = it.name)
                        buildChrome()
                    }.onFailure { host.overlays.message(tr(S.STORE_ERROR, it.message ?: "?")) }
                }
            }
        }
    }

    private fun showTip() {
        val c = chrome ?: return
        if (closed || host.tipsShown(TIP_GESTURES)) return
        val card = column(context, theme, horizontalPadding = false) {
            setPadding(theme.dp(18), theme.dp(16), theme.dp(18), theme.dp(12))
            background = theme.rounded(theme.raised, theme.radiusL, theme.accent, 1.2f)
            elevation = theme.dpF(10f)
            isClickable = true
            accessibilityLiveRegion = View.ACCESSIBILITY_LIVE_REGION_POLITE
            addView(theme.text(context, tr(S.TIP_TITLE), PxTheme.Type.HEADLINE))
            addView(theme.text(context, tr(S.TIP_BODY), PxTheme.Type.BODY, theme.textSecondary).apply { setPadding(0, theme.dp(6), 0, theme.dp(8)) })
        }
        val got = Widgets.pill(context, theme, tr(S.ACTION_GOT_IT), true) {}
        card.addView(got, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { gravity = Gravity.END })
        got.setOnClickListener {
            host.markTipShown(TIP_GESTURES)
            c.removeView(card)
        }
        c.addView(card, FrameLayout.LayoutParams(minOf(theme.dp(380), c.width - theme.dp(32)), FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.CENTER))
    }

    companion object {
        const val TIP_GESTURES = "workspace.gestures"
        private const val AUTOSAVE_MS = 20_000L
        private const val MAX_SIZE = 256
    }
}
