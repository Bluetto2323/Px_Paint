package com.paintengine.pxpaint.screens

import android.annotation.SuppressLint
import android.graphics.Typeface
import android.os.Build
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
import com.paintengine.pxpaint.core.BrushState
import com.paintengine.pxpaint.core.Format
import com.paintengine.pxpaint.core.LayerError
import com.paintengine.pxpaint.core.LayerInfo
import com.paintengine.pxpaint.core.HistoryState
import com.paintengine.pxpaint.core.LayerStack
import com.paintengine.pxpaint.core.LayoutPolicy
import com.paintengine.pxpaint.core.P
import com.paintengine.pxpaint.core.ProjectMeta
import com.paintengine.pxpaint.core.RotationSnap
import com.paintengine.pxpaint.core.S
import com.paintengine.pxpaint.core.SampleBuffer
import com.paintengine.pxpaint.core.ToolKind
import com.paintengine.pxpaint.core.TouchRouter
import com.paintengine.pxpaint.core.Affine
import com.paintengine.pxpaint.core.SelectionAction
import com.paintengine.pxpaint.core.SelectionErrors
import com.paintengine.pxpaint.core.SelectionGeometry
import com.paintengine.pxpaint.core.SelectionGesture
import com.paintengine.pxpaint.core.SelectionMode
import com.paintengine.pxpaint.core.SelectionShape
import com.paintengine.pxpaint.input.StylusInput
import com.paintengine.pxpaint.core.TileContent
import com.paintengine.pxpaint.core.BrushEncoding
import com.paintengine.pxpaint.core.BrushParams
import com.paintengine.pxpaint.core.BrushTool
import com.paintengine.pxpaint.core.TwoFinger
import com.paintengine.pxpaint.core.WorkspaceSpec
import com.paintengine.pxpaint.core.DesignTokens
import com.paintengine.pxpaint.core.MotionTokens
import com.paintengine.pxpaint.core.SaveTracker
import com.paintengine.pxpaint.core.GestureShortcut
import com.paintengine.pxpaint.core.PanelStyle
import com.paintengine.pxpaint.core.PinnedPanel
import com.paintengine.pxpaint.core.QuickPinch
import com.paintengine.pxpaint.core.Side
import com.paintengine.pxpaint.core.TapGesture
import com.paintengine.pxpaint.core.ToolbarPlacement
import com.paintengine.pxpaint.core.tr
import com.paintengine.pxpaint.ui.CheckerDrawable
import com.paintengine.pxpaint.ui.BrushPreviewView
import com.paintengine.pxpaint.ui.MaxHeight
import com.paintengine.pxpaint.ui.Icon
import com.paintengine.pxpaint.ui.IconButton
import com.paintengine.pxpaint.ui.IconDrawable
import com.paintengine.pxpaint.ui.OverlayHost
import com.paintengine.pxpaint.ui.PxSlider
import com.paintengine.pxpaint.ui.PxTheme
import com.paintengine.pxpaint.ui.QuickSlider
import com.paintengine.pxpaint.ui.SelectionOverlay
import com.paintengine.pxpaint.ui.Motion
import com.paintengine.pxpaint.ui.ProgressIndicator
import com.paintengine.pxpaint.ui.SaveIndicator
import com.paintengine.pxpaint.ui.SliderBubble
import com.paintengine.pxpaint.ui.Thumbnails
import com.paintengine.pxpaint.ui.Widgets
import com.paintengine.pxpaint.ui.column
import org.json.JSONObject
import kotlin.math.roundToInt

/**
 * The painting workspace: GPU canvas (EngineSurfaceView) under floating,
 * adaptive chrome (Block 8). Layout from core/LayoutPolicy, from the real
 * window size: phone portrait = bottom dock + bottom sheets, phone landscape
 * = side rail + side sheets, tablets = one top toolbar + popovers or a
 * pinned, resizable side panel. Size / opacity sliders on a screen edge,
 * "hide interface" mode, finger-tap shortcuts and the eyedropper.
 *
 * Input: one finger paints with the active tool from the first touch (no
 * delay). Every sample of every MotionEvent - historical ones first, in
 * order - goes to the native brush engine in screen pixels; the engine maps
 * them through the viewport and renders the stroke at the next frame. A
 * second finger landing right after the first means a pinch: the stroke is
 * cancelled and the canvas restored exactly (StrokeGate). Two fingers
 * pan/zoom/rotate the viewport (focal point under the fingers). The Move
 * tool, or "paint with a finger" off, makes one finger pan.
 *
 * Persistence: the canvas pixels are the engine's raster tiles. A save
 * exports only the tiles changed since the last save, compresses them and
 * rewrites content.pxtiles atomically (TileContent). Opening, saving,
 * clearing and closing all run on the single io thread, so they happen in
 * order and never see another canvas's content.
 */
class WorkspaceScreen(host: PxHost, private var meta: ProjectMeta) : Screen(host) {
    // The canvas is a SurfaceView: the host never fades this screen as a whole; its chrome enters itself.
    override val ownEntrance: Boolean get() = true
    private val motion get() = theme.motion
    private val saveTracker = SaveTracker()
    private var saveIndicator: SaveIndicator? = null
    private var chromeEntered = false
    private var shownPanel = Panel.NONE
    private var lastPinned = PinnedPanel.NONE
    private var loader: View? = null
    private var loaderProgress: ProgressIndicator? = null
    private var loaderText: TextView? = null
    private var compositeIndicator: ProgressIndicator? = null
    private var compositePolls = 0

    // Selection (Block 10). The engine owns the mask; this is the tool state.
    private val selGesture by lazy { SelectionGesture(tapSlopPx = theme.dpF(10f), closeRadiusPx = theme.dpF(22f)) }
    private var selShape = SelectionShape.RECTANGLE
    private var selMode = SelectionMode.REPLACE
    private var selAntialias = true
    private var selInfo = NativeEngine.SelectionInfo.NONE
    private var selVersion = -1L
    private var selOverlay: SelectionOverlay? = null
    private var selBar: LinearLayout? = null
    private var selBarRow: LinearLayout? = null
    private var selBarKey = ""
    private var selBarShown = false
    private var selRefreshing = false
    private var selRefreshAgain = false
    // Moving selected content: drag start (screen), offset at the drag start and now (canvas px).
    private var moveStartX = 0.0
    private var moveStartY = 0.0
    private var moveBaseX = 0
    private var moveBaseY = 0
    private var moveX = 0
    private var moveY = 0
    private val moveTarget = java.util.concurrent.atomic.AtomicLong()
    private val moveQueued = java.util.concurrent.atomic.AtomicBoolean()
    private val moveDelta = IntArray(2)
    // Applies the latest drag offset (made once: no allocation per pointer move).
    private val moveRunnable = Runnable {
        moveQueued.set(false)
        val t = moveTarget.get()
        engine?.selectionMove((t shr 32).toInt(), t.toInt())
    }
    private val selMatrix = FloatArray(6)
    private val selToScreen = Affine()
    private val selToCanvas = Affine()
    enum class Tool(val icon: Icon, val label: S, val desc: S) {
        MARKER(Icon.BRUSH, S.TOOL_MARKER, S.TOOL_MARKER_DESC),
        ERASER(Icon.ERASER, S.TOOL_ERASER, S.TOOL_ERASER_DESC),
        SELECT(Icon.SELECT, S.TOOL_SELECT, S.TOOL_SELECT_DESC),
        MOVE(Icon.MOVE, S.TOOL_MOVE, S.TOOL_MOVE_DESC);

        /** Brush tools (size / opacity sliders and the brush library apply). */
        val paints: Boolean get() = this == MARKER || this == ERASER
    }

    private enum class Panel { NONE, TOOL, COLOR, LAYERS, LIBRARY, HISTORY }

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
    // Layered persistence (io thread only): each layer's tiles as on disk.
    private val layerFiles = HashMap<Int, TileContent>()
    private val layersOnDisk = HashSet<Int>()
    private var writeAllLayers = false            // pre-layer project: first save writes every layer
    @Volatile private var ioReady = false
    // UI thread: the engine's layer stack (refreshed after every layer operation).
    private var layerStack: LayerStack? = null
    private val thumbCache = HashMap<Int, Pair<Long, android.graphics.Bitmap>>()
    private val thumbExec = java.util.concurrent.Executors.newSingleThreadExecutor { r -> Thread(r, "PxPaint-layer-thumbs") }
    private var lastBlockedMessage = 0L
    // UI thread: undo / redo availability (lock-free engine counts) and the
    // last fetched history list (History panel).
    private var historyCounts = 0 to 0
    private var history = HistoryState.EMPTY
    private var undoButton: IconButton? = null
    private var redoButton: IconButton? = null

    // Tools (sizes / opacities / spacing / colour live in host.brush, persisted)
    private var tool = Tool.MARKER
    private val color: Int get() = host.brush.color
    private var panel = Panel.NONE
    private var view = NativeEngine.ViewportInfo.EMPTY

    // Chrome references
    private var zoomChip: TextView? = null
    private var hud: TextView? = null
    private val toolButtons = HashMap<Tool, IconButton>()
    private var colorButton: View? = null

    // Input: TouchRouter decides, this screen executes with the event's data.
    private val samples = SampleBuffer()
    private var event: MotionEvent? = null      // the event being dispatched
    private var strokeT0 = 0L
    private var strokeKind = ToolKind.UNKNOWN
    private var ignoreUntilUp = false
    private var prevA = P(0.0, 0.0)
    private var prevB = P(0.0, 0.0)
    private var panLast = P(0.0, 0.0)
    private val router = TouchRouter(RouterActions())
    /** Debug-only scripted state (DebugDemo), run once the canvas is loaded. */
    var demo: DebugDemo.Script? = null


    private val historyRefresh = Runnable { refreshHistoryButtons() }

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
        // A hovering stylus is detected (and its axes shown in the stylus info) before it touches.
        sv.setOnHoverListener { _, e -> host.stylus.onEvent(e); false }
        root.addView(sv, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
        // Selection feedback between the canvas and the chrome (never takes touches).
        val so = SelectionOverlay(context, theme).also {
            it.engine = engine
            it.gesture = selGesture
            it.shape = selShape
            it.showDim = tool == Tool.SELECT
        }
        selOverlay = so
        root.addView(so, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
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
        else if (!closed) {
            engine?.setCanvasColors(theme.workspace, meta.paperArgb, meta.transparentBackground)
            buildChrome()   // back from the brush editor: brush rows / panels show the current brush
        }
        handler.removeCallbacks(poll)
        handler.post(poll)
    }

    override fun onRemoved() {
        removed = true
        thumbExec.shutdown()
        layerExec.shutdown()
        if (fullscreen) host.setImmersive(false)
        handler.removeCallbacks(poll)
        handler.removeCallbacks(historyRefresh)
        router.interrupt()
        closeCanvas()
    }

    override fun onPause() {
        router.interrupt()   // a stroke interrupted by the system keeps what was drawn
        save()
    }

    override fun onBack(): Boolean {
        if (fullscreen) {
            setFullscreen(false)
            return true
        }
        if (eyedropperArmed) {
            eyedropperArmed = false
            return true
        }
        if (selGesture.busy) {
            selectionCancel()
            return true
        }
        if (selInfo.moving) {
            runSelection(SelectionAction.MOVE_CANCEL)
            return true
        }
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
            closed = true
            leave()
            return
        }
        engine = e
        surfaceView?.engine = e
        selOverlay?.engine = e
        opened = true
        e.setScene(NativeEngine.Scene.CANVAS)
        e.setCanvasColors(theme.workspace, meta.paperArgb, meta.transparentBackground)
        // If the SurfaceView already has a surface (rebuild), hand it to the engine.
        surfaceView?.let { sv ->
            if (sv.hasSurface && sv.holder.surface.isValid) {
                e.surfaceCreated(sv.holder.surface)
                e.surfaceChanged(sv.surfaceWidth, sv.surfaceHeight)
            }
        }
        val m = meta
        host.io.execute {
            val result = runCatching {
                e.createCanvas(m.width, m.height, m.paperArgb, m.transparentBackground)?.let { throw OpenFailure(it) }
                val layered = host.store.loadLayers(m.id)
                if (layered != null) {
                    // Block 6 project: the layer stack, then each layer's pixels.
                    val st = layered.stack
                    if (!e.setLayerStack(
                            st.layers.map { it.id }.toIntArray(), st.layers.map { it.name }.toTypedArray(),
                            st.layers.map { (if (it.visible) 1 else 0) or (if (it.locked) 2 else 0) }.toIntArray(),
                            st.layers.map { it.opacity }.toFloatArray(), st.layers.map { it.blend.code }.toIntArray(), st.activeId,
                        )
                    ) throw OpenFailure("layer stack rejected")
                    val total = layered.tiles.size
                    var done = 0
                    for ((lid, tiles) in layered.tiles) {
                        for (batch in tiles.importBatches()) if (e.importLayerTiles(lid, batch) < 0) throw OpenFailure("tile import rejected")
                        done++
                        // Real progress (layers read), shown only if opening takes long.
                        if (total > 1) host.runOnMain { loaderProgress?.progress = done / total.toFloat(); loaderText?.text = tr(S.OPENING_LAYER, done, total) }
                    }
                    layerFiles.putAll(layered.tiles)
                    layersOnDisk.addAll(layered.tiles.keys)
                } else {
                    // Blocks 2-5 project: its single raster becomes layer 1; the
                    // first save writes the layered format (nothing is flattened
                    // or dropped: the old file is removed only after that).
                    val tiles = host.store.loadTiles(m.id)
                    if (tiles != null) {
                        for (batch in tiles.importBatches()) if (e.importLayerTiles(1, batch) < 0) throw OpenFailure("tile import rejected")
                        layerFiles[1] = tiles
                    } else {
                        e.loadCanvasContent(host.store.loadContent(m.id))
                        layerFiles[1] = TileContent(m.width, m.height)
                    }
                    writeAllLayers = true
                }
                ioReady = true
                LayerStack.parse(e.layers())
            }
            host.runOnMain {
                if (closed) return@runOnMain
                result.onSuccess {
                    layerStack = it
                    refreshPinned()
                    savedRevision = e.canvasContentRevision()
                    lastAutosave = SystemClock.uptimeMillis()
                    loaded = true
                    hideLoader()
                    host.screenReady(this)
                    demo?.let { d -> handler.postDelayed({ runDemo(d) }, 600) }
                }.onFailure {
                    // Never overwrite damaged content with an empty canvas.
                    host.overlays.message(tr(S.CANVAS_OPEN_FAILED, it.message ?: "?"), long = true)
                    closed = true
                    host.io.execute { e.closeCanvas() }
                    leave()
                }
            }
        }
        if (!host.tipsShown(TIP_GESTURES)) handler.postDelayed({ showTip() }, 400)
    }

    private class OpenFailure(message: String) : Exception(message)

    /** DebugDemo "layers": a few layers with different modes, one hidden, one locked; then the panel. */
    private fun demoLayers(e: NativeEngine, w: Float, h: Float, openPanel: Boolean = true) {
        val round = host.brushes.get("builtin.airbrush")?.params ?: return
        fun stroke(x0: Float, y0: Float, x1: Float, y1: Float, size: Float, argb: Int) {
            val n = 30
            val pts = FloatArray(n * 6) { Float.NaN }
            for (k in 0 until n) {
                val t = k / (n - 1f)
                pts[k * 6] = x0 + (x1 - x0) * t
                pts[k * 6 + 1] = y0 + (y1 - y0) * t
                pts[k * 6 + 2] = k * 8f
            }
            val b = NativeEngine.Brush(BrushEncoding.params(round, size = size, pressureSmoothing = 0f), BrushEncoding.flags(round, false), argb, null)
            e.strokeBegin(pts.copyOfRange(0, 6), b)
            e.strokeSamples(pts.copyOfRange(6, pts.size), n - 1)
            e.strokeEnd()
        }
        fun add(name: String): Int = e.layerOp(NativeEngine.LayerOp.ADD, 0, text = name).newId
        e.layerOp(NativeEngine.LayerOp.RENAME, 1, text = "Sketch")
        val shade = add("Shadows")
        stroke(w * 0.15f, h * 0.30f, w * 0.85f, h * 0.55f, 260f, 0xFF3050A0.toInt())
        e.layerOp(NativeEngine.LayerOp.BLEND, shade, com.paintengine.pxpaint.core.BlendMode.MULTIPLY.code)
        e.layerOp(NativeEngine.LayerOp.OPACITY, shade, value = 0.7f)
        val glow = add("Glow")
        stroke(w * 0.2f, h * 0.6f, w * 0.8f, h * 0.35f, 200f, 0xFFF0B030.toInt())
        e.layerOp(NativeEngine.LayerOp.BLEND, glow, com.paintengine.pxpaint.core.BlendMode.SCREEN.code)
        val notes = add("Notes")
        stroke(w * 0.1f, h * 0.7f, w * 0.9f, h * 0.7f, 60f, 0xFFE03030.toInt())
        e.layerOp(NativeEngine.LayerOp.VISIBLE, notes, 0)
        val ink = add("Ink")
        stroke(w * 0.1f, h * 0.25f, w * 0.9f, h * 0.62f, 24f, 0xFF101010.toInt())
        e.layerOp(NativeEngine.LayerOp.LOCKED, ink, 1)
        e.layerOp(NativeEngine.LayerOp.ACTIVE, shade)
        layerStack = LayerStack.parse(e.layers())
        if (openPanel) handler.postDelayed({ setPanel(Panel.LAYERS) }, 400)
    }

    /** DebugDemo: one stroke per built-in preset (finger-like samples), then the requested panel. */
    private fun runDemo(d: DebugDemo.Script) {
        demo = null
        val e = engine ?: return
        val sv = surfaceView ?: return
        val w = sv.width.toFloat()
        val h = sv.height.toFloat()
        val presets = host.brushes.all().filter { it.builtIn }
        val colors = intArrayOf(0xFF1F2A44.toInt(), 0xFF8C2F39.toInt(), 0xFF2E6F57.toInt(), 0xFF3B4C8C.toInt())
        for ((i, p) in presets.withIndex()) {
            val col = i % 2
            val row = i / 2
            val x0 = w * (0.08f + col * 0.47f)
            val y = h * (0.16f + row * 0.075f)
            val size = minOf(p.params.size, h * 0.045f)
            val brush = NativeEngine.Brush(
                BrushEncoding.params(p.params, size = size, pressureSmoothing = 0f), BrushEncoding.flags(p.params, false),
                colors[i % colors.size], null,
            )
            val n = 40
            val pts = FloatArray(n * NativeEngine.SAMPLE_FLOATS) { Float.NaN }
            for (k in 0 until n) {
                val t = k / (n - 1f)
                pts[k * 6] = x0 + w * 0.38f * t
                pts[k * 6 + 1] = y + h * 0.012f * kotlin.math.sin(t * 6.283f)
                pts[k * 6 + 2] = k * 8f
            }
            e.strokeBegin(pts.copyOfRange(0, 6), brush)
            e.strokeSamples(pts.copyOfRange(6, pts.size), n - 1)
            e.strokeEnd()
        }
        when (d.panel) {
            "layers" -> demoLayers(e, w, h)
            "history" -> {
                // The layered demo (strokes + layer operations, all recorded), then 3 steps undone and the panel.
                demoLayers(e, w, h, openPanel = false)
                historyOp(NativeEngine.HistoryOp.UNDO, 3, announce = false)
                handler.postDelayed({ setPanel(Panel.HISTORY) }, 600)
            }
            "selection" -> {
                // The layered picture, then the Select tool (shape from --es pxpaint.selshape): the CI drives it with real touches.
                demoLayers(e, w, h, openPanel = false)
                selShape = SelectionShape.of(d.shape?.uppercase())
                selOverlay?.shape = selShape
                selectTool(Tool.SELECT)
            }
            "workspace" -> setPanel(Panel.TOOL)
            "color" -> {
                applyColor(0xFF3B6EA8.toInt(), true)
                host.updateBrush(host.brush.withSaved(0xFF8C2F39.toInt()).withSaved(0xFF2E6F57.toInt()).withSaved(0xFFE0B040.toInt()))
                setPanel(Panel.COLOR)
            }
            "fullscreen" -> setFullscreen(true)
            "library" -> {
                d.filter?.let { libraryState.filter = it }
                setPanel(Panel.LIBRARY)
            }
            "editor" -> host.push(BrushEditorScreen(host, BrushTool.PAINT))
        }
    }

    /** Leaves the workspace after the current callback (never re-enters the host's navigation). */
    private fun leave() {
        handler.post { if (removed.not()) host.pop() }
    }

    private var removed = false

    private fun closeCanvas() {
        if (closed) return
        save()
        closed = true
        val e = engine ?: return
        host.io.execute { e.closeCanvas() }   // after the save above has exported
        surfaceView?.engine = e               // keep forwarding the surface release
    }

    /**
     * Persists changed tiles + thumbnail on the io thread. [force] also
     * refreshes the thumbnail when only the appearance changed.
     */
    private fun save(force: Boolean = false) {
        val e = engine ?: return
        if (!loaded || closed) return
        val rev = e.canvasContentRevision()
        if (rev == savedRevision && !force) return
        savedRevision = rev
        lastAutosave = SystemClock.uptimeMillis()
        val id = meta.id
        saveTracker.saveStarted()
        saveIndicator?.show(saveTracker.state)
        host.io.execute {
            if (!ioReady) {
                host.runOnMain { savedRevision = -1L; saveTracker.saveSkipped(); saveIndicator?.show(saveTracker.state) }
                return@execute
            }
            val r = runCatching {
                val stack = LayerStack.parse(e.layers()) ?: throw IllegalStateException("no layers")
                val changed = HashMap<Int, TileContent>()
                for (l in stack.layers) {
                    val c = layerFiles.getOrPut(l.id) { TileContent(meta.width, meta.height) }
                    var dirty = writeAllLayers || l.id !in layersOnDisk
                    // Only tiles changed since the last export, in bounded batches.
                    while (true) {
                        val records = e.exportLayerTiles(l.id, true, EXPORT_BATCH) ?: break
                        val n = c.applyExport(records)
                        if (n > 0) dirty = true
                        if (n < EXPORT_BATCH) break
                    }
                    if (dirty) changed[l.id] = c
                }
                val removed = layerFiles.keys.filter { stack.get(it) == null }
                val updated = host.store.saveLayers(id, stack, changed, removed)
                removed.forEach { layerFiles.remove(it); layersOnDisk.remove(it) }
                layersOnDisk.addAll(changed.keys)
                writeAllLayers = false
                val thumb = Thumbnails.fromEngine(updated, e.canvasThumbnail(Thumbnails.MAX_SIDE))
                host.store.saveThumbnail(id, Thumbnails.png(thumb))
                updated
            }
            host.runOnMain {
                r.onSuccess { meta = it.copy(name = meta.name) }
                    .onFailure {
                        savedRevision = -1L   // retry at the next autosave
                        host.overlays.message(tr(S.SAVE_FAILED, it.message ?: "?"), long = true)
                    }
                saveTracker.saveFinished(r.isSuccess, SystemClock.uptimeMillis())
                saveIndicator?.show(saveTracker.state)
            }
        }
    }

    // --------------------------------------------------------------- status
    private fun pollStatus() {
        val e = engine ?: return
        if (closed) return
        val s = runCatching { JSONObject(e.statusJson()) }.getOrNull() ?: return
        if (s.optBoolean("canvasOpen") && !s.optBoolean("canvasGpuReady") && s.optString("canvasError").isNotEmpty()) {
            // GPU storage could not be allocated: keep what was painted (the
            // content mirror is CPU-side), then go back to the gallery.
            host.overlays.message(tr(S.CANVAS_OPEN_FAILED, s.optString("canvasError")), long = true)
            closeCanvas()
            leave()
            return
        }
        updateViewport(e.viewport(ViewOp.QUERY))
        refreshHistoryButtons()
        // The selection changed elsewhere (undo / redo, a move landing): outline and actions follow.
        if (loaded && e.selectionVersion() != selVersion) refreshSelection()
        if (loaded) {
            saveTracker.poll(e.canvasContentRevision() != savedRevision, SystemClock.uptimeMillis())
            saveIndicator?.show(saveTracker.state)
        }
        // Heavy layer compositing (spread over frames): a small ring once it lasts.
        compositePolls = if (s.optBoolean("compositePending")) compositePolls + 1 else 0
        compositeIndicator?.let { ci ->
            val show = compositePolls >= 2
            if (show && ci.visibility != View.VISIBLE) { ci.visibility = View.VISIBLE; motion.fadeTo(ci, 1f, from = 0f) }
            else if (!show && ci.visibility == View.VISIBLE) motion.exit(ci, dyDp = 0f) { ci.visibility = View.GONE; ci.alpha = 1f }
        }
        hud?.let { h ->
            val fps = if (s.optBoolean("rateValid")) String.format(java.util.Locale.US, " · %.0f fps", s.optDouble("fps")) else ""
            h.text = String.format(
                java.util.Locale.US,
                "%s · frame work %.2f ms%s\nbrush %.2f ms (max %.2f) · %d/%d samples\ntiles %d/%d · CPU %s · GPU %s\ndraws %d · uploads %s",
                s.optString("backend"), s.optDouble("steadyAvgRecordMs") + s.optDouble("steadyAvgSubmitMs"), fps,
                s.optDouble("brushLastMs"), s.optDouble("brushMaxMs"),
                s.optLong("brushSamplesProcessed"), s.optLong("brushSamplesReceived"),
                s.optInt("canvasTilesAllocated"), s.optInt("canvasTilesTotal"),
                Format.bytes(s.optLong("canvasRasterBytes")), Format.bytes(s.optLong("canvasBytesAllocated")),
                s.optInt("canvasDrawCalls"), Format.bytes(s.optLong("canvasBytesUploaded")),
            )
        }
        if (loaded && SystemClock.uptimeMillis() - lastAutosave > AUTOSAVE_MS) save()
    }

    private fun updateViewport(v: NativeEngine.ViewportInfo) {
        if (v.zoom != view.zoom || v.rotation != view.rotation || v != view) selOverlay?.invalidate()
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
        host.stylus.onEvent(e)
        event = e
        try {
            dispatch(e)
        } finally {
            event = null
        }
    }

    // Finger taps (undo / redo / interface) and the quick pinch (fit): core/Gestures.
    private val taps by lazy { TapGesture(slopPx = theme.dpF(18f)) }
    private var pinchStartDist = 0.0
    private var pinchStartMs = 0L

    private fun dispatch(e: MotionEvent) {
        val i = e.actionIndex
        val id = e.getPointerId(i)
        // FLAG_CANCELED (API 33): the system decided the lifted pointer was
        // accidental (e.g. a palm); its stroke is cancelled, not committed.
        val canceled = Build.VERSION.SDK_INT >= 33 && (e.flags and FLAG_CANCELED) != 0
        val kind = StylusInput.kind(e, i)
        // Eyedropper: armed from the colour panel, or the stylus side button
        // held when the stylus touches. The whole touch picks, nothing is painted.
        if (e.actionMasked == MotionEvent.ACTION_DOWN) {
            val stylusButton = host.settings.stylusButtonPick && kind.isStylus &&
                (e.buttonState and MotionEvent.BUTTON_STYLUS_PRIMARY) != 0
            picking = eyedropperArmed || stylusButton
            if (picking && panel != Panel.NONE && spec?.pinned != PinnedPanel.COLOR) setPanel(Panel.NONE)
        }
        if (picking) {
            when (e.actionMasked) {
                MotionEvent.ACTION_DOWN, MotionEvent.ACTION_MOVE -> pickAt(e.getX(0), e.getY(0), false)
                MotionEvent.ACTION_UP -> {
                    pickAt(e.getX(i), e.getY(i), true)
                    picking = false
                    eyedropperArmed = false
                }
                MotionEvent.ACTION_CANCEL -> {
                    loupe?.let { chrome?.removeView(it) }
                    loupe = null
                    picking = false
                }
            }
            return
        }
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                ignoreUntilUp = false
                // A tap on the canvas closes a floating panel (pinned panels stay).
                if (panel != Panel.NONE) {
                    setPanel(Panel.NONE)
                    ignoreUntilUp = true
                    return
                }
                val st = host.settings
                taps.undoEnabled = st.tapUndo
                taps.redoEnabled = st.tapRedo
                taps.fullscreenEnabled = st.tapFullscreen
                taps.pointerDown(id, e.eventTime, e.getX(i), e.getY(i), kind == ToolKind.FINGER)
                router.fingerMode = st.fingerMode
                router.moveTool = tool == Tool.MOVE
                router.eraserTool = tool == Tool.ERASER
                router.selectTool = tool == Tool.SELECT
                // Deliver MOVE events as they arrive, not batched per frame.
                surfaceView?.requestUnbufferedDispatch(e)
                router.down(id, kind, e.eventTime, e.getX(i), e.getY(i))
            }
            MotionEvent.ACTION_POINTER_DOWN -> if (!ignoreUntilUp) {
                taps.pointerDown(id, e.eventTime, e.getX(i), e.getY(i), kind == ToolKind.FINGER)
                router.pointerDown(id, kind, e.eventTime, e.getX(i), e.getY(i))
            }
            MotionEvent.ACTION_MOVE -> if (!ignoreUntilUp) {
                for (k in 0 until e.pointerCount) taps.pointerMove(e.getPointerId(k), e.getX(k), e.getY(k))
                router.move()
            }
            MotionEvent.ACTION_POINTER_UP -> if (!ignoreUntilUp) {
                router.pointerUp(id, canceled)
                taps.pointerUp(id, e.eventTime, canceled)
            }
            MotionEvent.ACTION_UP -> {
                if (!ignoreUntilUp) {
                    router.up(id, canceled)                   // commits a stroke first (it then is no tap)
                    taps.pointerUp(id, e.eventTime, canceled)?.let { shortcut(it) }
                }
                ignoreUntilUp = false
            }
            MotionEvent.ACTION_CANCEL -> {
                router.cancel()
                taps.cancel()
                ignoreUntilUp = false
            }
        }
    }

    private fun shortcut(sc: GestureShortcut) {
        when (sc) {
            GestureShortcut.UNDO -> historyOp(NativeEngine.HistoryOp.UNDO)
            GestureShortcut.REDO -> historyOp(NativeEngine.HistoryOp.REDO)
            GestureShortcut.FULLSCREEN -> setFullscreen(!fullscreen)
            GestureShortcut.FIT -> {
                viewOp(ViewOp.FIT)
                zoomChip?.let { motion.pop(it) }        // canvas fit: the zoom chip answers
                host.overlays.message(tr(S.FIT_DONE))
            }
        }
    }

    private fun pos(id: Int): P? {
        val e = event ?: return null
        val i = e.findPointerIndex(id)
        return if (i < 0) null else P(e.getX(i).toDouble(), e.getY(i).toDouble())
    }

    /** Executes the router's decisions on the current event. */
    private inner class RouterActions : TouchRouter.Actions {
        override fun beginStroke(pointerId: Int, kind: ToolKind, eraser: Boolean) {
            if (tool == Tool.SELECT) { selectionDown(pointerId); return }
            val e = event ?: return
            val engine = engine ?: return
            selOverlay?.paused = true   // the ants stand still while painting
            strokeT0 = e.eventTime
            strokeKind = kind
            samples.clear()
            // The first sample: this pointer's current state (no history before the stroke).
            if (!StylusInput.append(e, pointerId, kind, strokeT0, samples, includeHistory = false)) return
            // The selected library preset of the tool, with its quick size / opacity.
            val p = host.brushes.effective(if (eraser) BrushTool.ERASER else BrushTool.PAINT)
            val accepted = engine.strokeBegin(
                samples.first(),
                NativeEngine.Brush(
                    BrushEncoding.params(p, pressureSmoothing = host.settings.pressureSmoothing),
                    BrushEncoding.flags(p, eraser),
                    if (eraser) 0xFF000000.toInt() else color,
                    host.pressureTable(),
                ),
            )
            if (!accepted) paintBlocked()   // hidden or locked layer: nothing painted
        }

        override fun strokeSamples(pointerId: Int) {
            if (tool == Tool.SELECT) { selectionDrag(pointerId); return }
            val e = event ?: return
            samples.clear()
            if (!StylusInput.append(e, pointerId, strokeKind, strokeT0, samples)) return
            val i = e.findPointerIndex(pointerId)
            router.strokeMovedTo(e.getX(i), e.getY(i))
            engine?.strokeSamples(samples.data, samples.count)
        }

        override fun endStroke(pointerId: Int) {
            if (tool == Tool.SELECT) { selectionUp(pointerId); return }
            selOverlay?.paused = false
            val e = event
            if (e != null) {
                // The final position (and its history) is part of the stroke.
                samples.clear()
                if (StylusInput.append(e, pointerId, strokeKind, strokeT0, samples)) engine?.strokeSamples(samples.data, samples.count)
            }
            engine?.strokeEnd()
            taps.strokeCommitted()
            // The stroke becomes a history step at the next frame: show it without waiting for the poll.
            handler.postDelayed(historyRefresh, 50)
        }

        override fun cancelStroke() {
            if (tool == Tool.SELECT) { selectionCancel(); return }
            selOverlay?.paused = false
            engine?.strokeCancel()
        }

        override fun panStart(pointerId: Int) {
            panLast = pos(pointerId) ?: return
        }

        override fun panMove(pointerId: Int) {
            val p = pos(pointerId) ?: return
            viewOp(ViewOp.PAN_BY, p.x - panLast.x, p.y - panLast.y)
            panLast = p
        }

        override fun gestureStart(a: Int, b: Int) {
            prevA = pos(a) ?: return
            prevB = pos(b) ?: return
            pinchStartDist = kotlin.math.hypot(prevA.x - prevB.x, prevA.y - prevB.y)
            pinchStartMs = SystemClock.uptimeMillis()
        }

        override fun gestureMove(a: Int, b: Int) {
            val pa = pos(a) ?: return
            val pb = pos(b) ?: return
            val st = TwoFinger.step(prevA, prevB, pa, pb, host.settings.twoFingerRotate)
            viewOp(ViewOp.GESTURE, st.prevFocal.x, st.prevFocal.y, st.newFocal.x, st.newFocal.y, st.scale, st.rotation)
            prevA = pa
            prevB = pb
        }

        override fun gestureEnd(a: Int, b: Int) {
            // Fingers closed fast: fit the canvas (an ordinary pinch zooms out).
            val dist = kotlin.math.hypot(prevA.x - prevB.x, prevA.y - prevB.y)
            if (host.settings.quickPinchFit && QuickPinch.isFit(pinchStartDist, dist, SystemClock.uptimeMillis() - pinchStartMs)) {
                shortcut(GestureShortcut.FIT)
                return
            }
            if (host.settings.rotationSnap && host.settings.twoFingerRotate) {
                RotationSnap.target(view.rotation)?.let { target ->
                    if (target != view.rotation) {
                        val f = P((prevA.x + prevB.x) / 2, (prevA.y + prevB.y) / 2)
                        viewOp(ViewOp.SET_ROTATION_AT, target, f.x, f.y)
                        // Rotation snapped: a tick and the chip answers (no animation of the canvas itself).
                        surfaceView?.performHapticFeedback(android.view.HapticFeedbackConstants.CLOCK_TICK)
                        zoomChip?.let { motion.pop(it) }
                    }
                }
            }
        }
    }

    private fun brushTool(t: Tool): BrushTool = if (t == Tool.ERASER) BrushTool.ERASER else BrushTool.PAINT

    /** Changes the selected preset of [t] (a built-in keeps it as a resettable customisation). */
    private fun updatePreset(t: Tool, change: (BrushParams) -> BrushParams) {
        val lib = host.brushes
        val bt = brushTool(t)
        host.updateBrushes(lib.update(lib.selectedId(bt), change(lib.selectedPreset(bt).params)))
    }

    private val libraryState = BrushLibraryPanel.State()

    // --------------------------------------------------------------- chrome
    // Layout (core/LayoutPolicy): phone portrait = top bar + bottom dock +
    // edge sliders + bottom sheets; phone landscape = short top bar + side
    // rail + edge sliders + side sheets; tablet = one top toolbar + edge
    // sliders + popovers, or a pinned, resizable side panel.
    private var fullscreen = false
    private var sizeSlider: QuickSlider? = null
    private var opacitySlider: QuickSlider? = null
    private var sliderBubble: SliderBubble? = null
    private var libraryButton: IconButton? = null
    private var pinnedView: View? = null
    private var slidersView: View? = null
    private val colorState = ColorPanel.State()
    /** Colour when the colour panel opened ("previous" swatch). */
    private var colorBefore = 0

    private fun buildChrome() {
        val t0 = SystemClock.elapsedRealtimeNanos()
        val c = chrome ?: return
        c.removeAllViews()
        selBar = null
        selBarRow = null
        toolButtons.clear()
        panelView = null
        pinnedView = null
        sizeSlider = null
        opacitySlider = null
        val s = LayoutPolicy.workspace(host.windowWidthDp, host.windowHeightDp, host.smallestWidthDp, host.settings)
        spec = s
        val ins = host.insets
        val m = theme.dp(8)
        if (fullscreen) {
            // Only a small corner button (and the four-finger tap) brings the interface back.
            c.addView(IconButton(context, theme, Icon.FULLSCREEN_EXIT, tr(S.FULLSCREEN_EXIT)).apply {
                background = theme.pressable(theme.rounded(theme.chrome, theme.radiusPill, theme.outline), theme.radiusPill)
                alpha = 0.85f
                setOnClickListener { setFullscreen(false) }
            }, FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP or Gravity.END).apply {
                setMargins(0, ins.top + m, ins.right + m, 0)
            })
            zoomChip = null
            hud = null
            return
        }
        val barH = theme.touchTarget + theme.dp(if (s.compactTopBar) 4 else 12)
        val pinnedW = if (s.pinned != PinnedPanel.NONE) theme.dp(s.panelWidthDp) + m + theme.touchTarget else 0
        val pinnedLeft = s.pinned != PinnedPanel.NONE && s.panelSide == Side.LEFT
        val pinnedRight = s.pinned != PinnedPanel.NONE && s.panelSide == Side.RIGHT

        // Top bar (actions; on tablets also the tools)
        val railW = if (s.toolbar == ToolbarPlacement.SIDE) theme.touchTarget + theme.dp(8) + m else 0
        c.addView(topBar(s), FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP).apply {
            setMargins(ins.left + m + (if (s.railSide == Side.LEFT) railW else 0), ins.top + m,
                ins.right + m + (if (s.railSide == Side.RIGHT) railW else 0), 0)
        })
        val belowBar = ins.top + m + barH + m

        // Pinned panel (tablets): beside the canvas, under the top bar.
        if (s.pinned != PinnedPanel.NONE) pinnedPanel(c, s, belowBar)

        // Zoom / rotation chip, centred over the canvas area.
        zoomChip = theme.text(context, "", PxTheme.Type.LABEL, theme.text).apply {
            gravity = Gravity.CENTER
            minHeight = theme.touchTarget
            minWidth = theme.touchTarget
            setPadding(theme.dp(14), 0, theme.dp(14), 0)
            background = theme.pressable(theme.rounded(theme.chrome, theme.radiusPill, theme.outline), theme.radiusPill)
            typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
            isClickable = true
            setOnClickListener { viewOp(ViewOp.FIT); motion.pop(this) }
            setOnLongClickListener { canvasMenu(this); true }
        }
        c.addView(zoomChip, FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP or Gravity.CENTER_HORIZONTAL).apply {
            topMargin = belowBar
            // Centred over the canvas area: FrameLayout shifts a centred child by the whole margin.
            if (pinnedRight) rightMargin = pinnedW / 2
            if (pinnedLeft) leftMargin = pinnedW / 2
        })
        updateViewport(view)

        // Contextual selection actions, under the zoom chip (over the canvas area).
        selBar = selectionBar().also { bar ->
            c.addView(bar, FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.TOP or Gravity.CENTER_HORIZONTAL).apply {
                topMargin = belowBar + theme.touchTarget + m
                val side = (if (s.toolbar == ToolbarPlacement.SIDE) railW else 0) + theme.touchTarget + theme.dp(8)
                leftMargin = ins.left + side + (if (pinnedLeft) pinnedW else 0)
                rightMargin = ins.right + side + (if (pinnedRight) pinnedW else 0)
            })
        }
        refreshSelectionBar()

        // Tools: bottom dock / side rail (on tablets they are in the top bar).
        var dockH = 0
        when (s.toolbar) {
            ToolbarPlacement.BOTTOM -> {
                c.addView(dock(s, vertical = false), FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT,
                    Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL).apply { bottomMargin = ins.bottom + theme.dp(12) })
                dockH = theme.touchTarget + theme.dp(12) + theme.dp(if (s.toolLabels) 18 else 0) + theme.dp(12)
            }
            ToolbarPlacement.SIDE -> {
                // Full height from the top (the top bar starts beside it): fits short landscape windows; scrolls if not.
                val left = s.railSide == Side.LEFT
                val rail = android.widget.ScrollView(context).apply {
                    isVerticalScrollBarEnabled = false
                    isFillViewport = false
                    addView(dock(s, vertical = true))
                }
                c.addView(rail, FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT,
                    (if (left) Gravity.START else Gravity.END) or Gravity.CENTER_VERTICAL).apply {
                    if (left) marginStart = ins.left + m else marginEnd = ins.right + m
                    topMargin = ins.top + m
                    bottomMargin = ins.bottom + m
                })
            }
            ToolbarPlacement.TOP -> Unit
        }

        // Quick size / opacity sliders on a screen edge.
        val sliderTop = belowBar + theme.dp(40)
        val sliderBottom = ins.bottom + dockH + m
        val availPx = (if (c.height > 0) c.height else (host.windowHeightDp * context.resources.displayMetrics.density).toInt()) - sliderTop - sliderBottom
        val lengthPx = ((availPx - theme.dp(16)) / 2).coerceIn(theme.dp(72), theme.dp(QuickSlider.LENGTH_DP))
        val sliders = quickSliders(s, lengthPx)
        slidersView = sliders
        val sliderLeft = s.sliderSide == Side.LEFT
        val railOnSliderSide = s.toolbar == ToolbarPlacement.SIDE && s.railSide == s.sliderSide
        val sideInset = theme.dp(2) + (if (railOnSliderSide) theme.touchTarget + theme.dp(24) else 0) +
            (if (sliderLeft && pinnedLeft || !sliderLeft && pinnedRight) pinnedW else 0)
        c.addView(sliders, FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT,
            (if (sliderLeft) Gravity.START else Gravity.END) or Gravity.CENTER_VERTICAL).apply {
            if (sliderLeft) marginStart = ins.left + sideInset else marginEnd = ins.right + sideInset
            topMargin = sliderTop
            bottomMargin = sliderBottom
        })
        sliderBubble = SliderBubble(context, theme).also { b ->
            c.addView(b, FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT,
                (if (sliderLeft) Gravity.START else Gravity.END) or Gravity.CENTER_VERTICAL).apply {
                val off = sideInset + theme.touchTarget + theme.dp(10)
                if (sliderLeft) marginStart = ins.left + off else marginEnd = ins.right + off
                topMargin = sliderTop
                bottomMargin = sliderBottom
            })
        }

        // Performance overlay
        hud = if (host.settings.performanceOverlay) theme.text(context, "", PxTheme.Type.CAPTION, theme.textSecondary).apply {
            typeface = Typeface.MONOSPACE
            background = theme.rounded(theme.chrome, theme.radiusS, theme.outline)
            setPadding(theme.dp(10), theme.dp(6), theme.dp(10), theme.dp(6))
            importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO
        }.also {
            c.addView(it, FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.TOP or (if (sliderLeft) Gravity.END else Gravity.START)).apply {
                topMargin = belowBar + theme.dp(40)
                if (sliderLeft) marginEnd = ins.right + m + (if (pinnedRight) pinnedW else 0) else marginStart = ins.left + m + (if (pinnedLeft) pinnedW else 0)
            })
        } else null

        shownPanel = Panel.NONE          // the rebuilt chrome has no panel yet
        if (panel != Panel.NONE) showPanel(panel)
        refreshDock()
        // Workspace transition: the chrome arrives once (not on every rebuild); a new pinned panel slides in.
        if (!chromeEntered) {
            chromeEntered = true
            for (i in 0 until c.childCount) {
                val v = c.getChildAt(i)
                val (dx, dy) = when {
                    v === pinnedView -> (if (s.panelSide == Side.LEFT) -MotionTokens.SLIDE else MotionTokens.SLIDE) to 0f
                    v === slidersView -> (if (s.sliderSide == Side.LEFT) -MotionTokens.SHIFT else MotionTokens.SHIFT) to 0f
                    i == 0 -> 0f to -MotionTokens.SHIFT                       // top bar
                    else -> 0f to MotionTokens.SHIFT
                }
                motion.enter(v, dyDp = dy, dxDp = dx, delay = if (i == 0) 0L else MotionTokens.STAGGER)
            }
        } else if (s.pinned != PinnedPanel.NONE && s.pinned != lastPinned) {
            pinnedView?.let { motion.enter(it, dyDp = 0f, dxDp = if (s.panelSide == Side.LEFT) -MotionTokens.SLIDE else MotionTokens.SLIDE) }
        }
        lastPinned = s.pinned
        if (!loaded && !closed) showLoaderLater()
        if (com.paintengine.BuildConfig.DEBUG) android.util.Log.i("PxPaint", String.format(java.util.Locale.US,
            "UI chrome %s (%dx%d dp, sw %d) built in %.2f ms", s.kind, host.windowWidthDp, host.windowHeightDp, host.smallestWidthDp,
            (SystemClock.elapsedRealtimeNanos() - t0) / 1e6))
    }

    /** Project loading: a card with a ring (real layer progress when there are several), only if it takes long. */
    private fun showLoaderLater() {
        handler.postDelayed({
            val c = chrome ?: return@postDelayed
            if (loaded || closed || loader != null) return@postDelayed
            val ring = ProgressIndicator(context, theme, sizeDp = 36f)
            val text = theme.text(context, "", PxTheme.Type.CAPTION, theme.textSecondary)
            val card = LinearLayout(context).apply {
                orientation = LinearLayout.VERTICAL
                gravity = Gravity.CENTER_HORIZONTAL
                background = theme.rounded(theme.chrome, theme.radiusL, theme.outline)
                setPadding(theme.dp(24), theme.dp(20), theme.dp(24), theme.dp(18))
                addView(ring)
                addView(theme.text(context, tr(S.OPENING_PROJECT, meta.name), PxTheme.Type.LABEL, lines = 1).apply {
                    setPadding(0, theme.dp(12), 0, theme.dp(2))
                })
                addView(text)
                accessibilityLiveRegion = View.ACCESSIBILITY_LIVE_REGION_POLITE
            }
            loader = card
            loaderProgress = ring
            loaderText = text
            c.addView(card, FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.CENTER))
            motion.enter(card, dyDp = 0f)
        }, MotionTokens.LOADING_DELAY)
    }

    private fun hideLoader() {
        val l = loader ?: return
        loader = null
        loaderProgress = null
        loaderText = null
        motion.exit(l, dyDp = 0f) { chrome?.removeView(l) }
    }

    private fun topBar(s: WorkspaceSpec): View = LinearLayout(context).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        background = theme.rounded(theme.chrome, theme.radiusL, theme.outline)
        val v = theme.dp(if (s.compactTopBar) 0 else 4)
        setPadding(theme.dp(4), v, theme.dp(4), v)
        addView(IconButton(context, theme, Icon.BACK, tr(S.BACK_TO_GALLERY)).apply { setOnClickListener { host.pop() } })
        if (s.showTitle) {
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
        } else {
            addView(View(context), LinearLayout.LayoutParams(0, 1, 1f))
        }
        // Status: compositing ring (heavy layer updates) and save state.
        compositeIndicator = ProgressIndicator(context, theme, sizeDp = 16f, strokeDp = 2f).apply {
            visibility = View.GONE
            contentDescription = tr(S.COMPOSITING)
        }.also { addView(it, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { marginEnd = theme.dp(4) }) }
        saveIndicator = SaveIndicator(context, theme) { save(force = true) }.also {
            it.show(saveTracker.state)
            addView(it)
        }
        // Undo / redo: tap = one step, long-press = the History panel.
        undoButton = IconButton(context, theme, Icon.UNDO, "${tr(S.UNDO)} — ${tr(S.UNDO_DESC)}").apply {
            setOnClickListener { historyOp(NativeEngine.HistoryOp.UNDO) }
            setOnLongClickListener { togglePanel(Panel.HISTORY); true }
        }.also { addView(it) }
        redoButton = IconButton(context, theme, Icon.REDO, "${tr(S.REDO)} — ${tr(S.REDO_DESC)}").apply {
            setOnClickListener { historyOp(NativeEngine.HistoryOp.REDO) }
            setOnLongClickListener { togglePanel(Panel.HISTORY); true }
        }.also { addView(it) }
        showHistoryButtons()
        if (s.toolbar == ToolbarPlacement.TOP) {
            addView(separator(vertical = false))
            addTools(this, s)
        }
        val more = IconButton(context, theme, Icon.MORE, tr(S.CANVAS_MENU))
        more.setOnClickListener { canvasMenu(more) }
        addView(more)
    }

    private fun separator(vertical: Boolean): View = View(context).apply { setBackgroundColor(theme.outline) }.also {
        it.layoutParams = if (vertical) LinearLayout.LayoutParams(theme.dp(24), maxOf(1, theme.dp(1f))).apply { setMargins(0, theme.dp(6), 0, theme.dp(6)) }
        else LinearLayout.LayoutParams(maxOf(1, theme.dp(1f)), theme.dp(24)).apply { setMargins(theme.dp(6), 0, theme.dp(6), 0) }
    }

    /** Brush, eraser, move | library, colour, layers: the same group in every layout. */
    private fun addTools(box: LinearLayout, s: WorkspaceSpec) {
        val vertical = box.orientation == LinearLayout.VERTICAL
        for (t in Tool.entries) {
            val b = IconButton(context, theme, t.icon, tr(t.label), label = if (s.toolLabels) tr(t.label) else null)
            b.setOnClickListener { selectTool(t) }
            if (t.paints) b.setOnLongClickListener {
                // Long press: straight to the brush library of that tool.
                if (tool != t) selectTool(t)
                setPanel(Panel.LIBRARY)
                refreshDock()
                true
            }
            toolButtons[t] = b
            box.addView(b, toolLp(vertical))
        }
        box.addView(separator(vertical))
        val preset = host.brushes.selectedPreset(brushTool(if (!tool.paints) Tool.MARKER else tool))
        libraryButton = IconButton(context, theme, Icon.LIBRARY, tr(S.BRUSH_LIBRARY_DESC, preset.displayName()),
            label = if (s.toolLabels) tr(S.BRUSH_LIBRARY) else null).apply {
            setOnClickListener { togglePanel(Panel.LIBRARY) }
        }.also { box.addView(it, toolLp(vertical)) }
        val swatch = FrameLayout(context).apply {
            minimumWidth = theme.touchTarget
            minimumHeight = theme.touchTarget
            contentDescription = tr(S.COLOR)
            tooltipText = tr(S.COLOR)
            isClickable = true
            background = theme.pressable(null, theme.radiusS)
            addView(View(context).apply { background = theme.oval(color, theme.text) },
                FrameLayout.LayoutParams(theme.dp(28), theme.dp(28), Gravity.CENTER))
            setOnClickListener { togglePanel(Panel.COLOR) }
        }
        colorButton = swatch
        box.addView(swatch, toolLp(vertical))
        box.addView(IconButton(context, theme, Icon.LAYERS, tr(S.LAYERS), label = if (s.toolLabels) tr(S.LAYERS) else null).apply {
            setOnClickListener { togglePanel(Panel.LAYERS) }
        }, toolLp(vertical))
    }

    private fun toolLp(vertical: Boolean) = if (vertical)
        LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { gravity = Gravity.CENTER_HORIZONTAL }
    else LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { gravity = Gravity.CENTER_VERTICAL }

    private fun dock(s: WorkspaceSpec, vertical: Boolean): View = LinearLayout(context).apply {
        orientation = if (vertical) LinearLayout.VERTICAL else LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER
        background = theme.rounded(theme.chrome, theme.radiusL, theme.outline)
        elevation = theme.dpF(4f)
        val p = theme.dp(if (vertical) 4 else 6)
        setPadding(p, p, p, p)
        addTools(this, s)
    }

    /** Size and opacity of the current tool, on a screen edge; a bubble shows the value while dragging. */
    private fun quickSliders(s: WorkspaceSpec, lengthPx: Int): View = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        gravity = Gravity.CENTER_HORIZONTAL
        val bt = brushTool(if (!tool.paints) Tool.MARKER else tool)
        val b = host.brushes.effective(bt)
        val size = QuickSlider(context, theme, tr(S.QUICK_SIZE), wedge = true).apply {
            valueText = { BrushState.sizeLabel(BrushState.sliderToSize(it)) }
            setValue(BrushState.sizeToSlider(b.size))
            onChange = { v, fromUser, _ ->
                if (fromUser) host.updateBrushes(host.brushes.setLive(currentBrushTool(), size = BrushState.sliderToSize(v)))
            }
            onPreview = { v -> sliderBubble?.show(v?.let { BrushState.sliderToSize(it) }, null, color, view.zoom) }
        }
        val opacity = QuickSlider(context, theme, tr(S.QUICK_OPACITY), wedge = false).apply {
            valueText = { "${(BrushState.sliderToOpacity(it) * 100).roundToInt()}%" }
            setValue(b.opacity)
            onChange = { v, fromUser, _ ->
                if (fromUser) host.updateBrushes(host.brushes.setLive(currentBrushTool(), opacity = BrushState.sliderToOpacity(v)))
            }
            onPreview = { v -> sliderBubble?.show(null, v?.let { BrushState.sliderToOpacity(it) }, color, view.zoom) }
        }
        sizeSlider = size
        opacitySlider = opacity
        addView(size, LinearLayout.LayoutParams(theme.touchTarget, lengthPx))
        addView(View(context), LinearLayout.LayoutParams(1, theme.dp(14)))
        addView(opacity, LinearLayout.LayoutParams(theme.touchTarget, lengthPx))
        if (!tool.paints) alpha = theme.disabledAlpha
    }

    private fun currentBrushTool(): BrushTool = brushTool(if (!tool.paints) Tool.MARKER else tool)

    private fun refreshDock() {
        for ((t, b) in toolButtons) b.isSelected = t == tool
        (colorButton as? FrameLayout)?.getChildAt(0)?.background = theme.oval(color, theme.text)
        val bt = currentBrushTool()
        val b = host.brushes.effective(bt)
        sizeSlider?.setValue(BrushState.sizeToSlider(b.size))
        opacitySlider?.setValue(b.opacity)
        libraryButton?.contentDescription = tr(S.BRUSH_LIBRARY_DESC, host.brushes.selectedPreset(bt).displayName())
        for (sl in listOfNotNull(sizeSlider, opacitySlider)) (sl.parent as? View)?.alpha = if (!tool.paints) theme.disabledAlpha else 1f
    }

    private fun selectTool(t: Tool) {
        if (t == tool && t != Tool.MOVE) {
            togglePanel(Panel.TOOL)
        } else {
            leavingSelectTool(t)
            tool = t
            selOverlay?.showDim = t == Tool.SELECT
            refreshSelectionBar()
            toolButtons[t]?.let { motion.pop(it) }   // brush selection: the new tool answers
            if (panel == Panel.TOOL || panel == Panel.LIBRARY) showPanel(panel)
            if (spec?.pinned == PinnedPanel.BRUSHES) refreshPinned()
            host.overlays.message(tr(t.desc))
        }
        refreshDock()
    }

    private fun setFullscreen(on: Boolean) {
        if (fullscreen == on) return
        fullscreen = on
        host.setImmersive(on)
        if (on) panel = Panel.NONE
        buildChrome()
        if (on) host.overlays.message(tr(S.FULLSCREEN_HINT))
    }

    // ------------------------------------------------------------ selection
    // Touches with the Select tool (TouchRouter routes them as "strokes").
    private fun selectionDown(pointerId: Int) {
        val p = pos(pointerId) ?: return
        if (selInfo.moving) {
            moveStartX = p.x
            moveStartY = p.y
            moveBaseX = moveX
            moveBaseY = moveY
            return
        }
        selGesture.shape = selShape
        selOverlay?.shape = selShape
        selGesture.down(p.x.toFloat(), p.y.toFloat())
        selOverlay?.invalidate()
        if (selGesture.phase == SelectionGesture.Phase.POLYGON) refreshSelectionBar()
    }

    private fun selectionDrag(pointerId: Int) {
        val e = event ?: return
        val i = e.findPointerIndex(pointerId)
        if (i < 0) return
        router.strokeMovedTo(e.getX(i), e.getY(i))
        if (selInfo.moving) {
            val engine = engine ?: return
            if (!engine.viewMatrix(selMatrix)) return
            selToScreen.set(selMatrix)
            if (!selToScreen.invertInto(selToCanvas)) return
            SelectionGeometry.canvasOffset(selToCanvas, e.getX(i) - moveStartX, e.getY(i) - moveStartY, moveDelta)
            val nx = moveBaseX + moveDelta[0]
            val ny = moveBaseY + moveDelta[1]
            if (nx != moveX || ny != moveY) {
                moveX = nx
                moveY = ny
                // Off the UI thread, coalesced: only the latest offset is applied when the worker is busy.
                moveTarget.set((nx.toLong() shl 32) or (ny.toLong() and 0xFFFFFFFFL))
                if (moveQueued.compareAndSet(false, true)) layerExec.execute(moveRunnable)
                selOverlay?.setMoveOffset(nx, ny)
            }
            return
        }
        // Every sample (historical first): the lasso follows the finger exactly.
        for (h in 0 until e.historySize) selGesture.move(e.getHistoricalX(i, h), e.getHistoricalY(i, h))
        selGesture.move(e.getX(i), e.getY(i))
        selOverlay?.invalidate()
    }

    private fun selectionUp(pointerId: Int) {
        if (selInfo.moving) return
        val p = pos(pointerId)
        val n = selGesture.points.count
        val x = p?.x?.toFloat() ?: if (n > 0) selGesture.points.x(n - 1) else 0f
        val y = p?.y?.toFloat() ?: if (n > 0) selGesture.points.y(n - 1) else 0f
        when (selGesture.up(x, y)) {
            SelectionGesture.Outcome.SELECT -> commitSelection()
            SelectionGesture.Outcome.TAP -> if (selMode == SelectionMode.REPLACE && selInfo.active) runSelection(SelectionAction.DESELECT)
            SelectionGesture.Outcome.NONE -> Unit
        }
        selOverlay?.invalidate()
        refreshSelectionBar()
    }

    private fun selectionCancel() {
        // A second finger right after the first (a pinch) or a system cancel: the shape is dropped.
        if (selGesture.phase != SelectionGesture.Phase.POLYGON) selGesture.cancel()
        selOverlay?.invalidate()
        refreshSelectionBar()
    }

    /** The finished shape goes to the engine (off the UI thread); the preview stays until the outline arrives. */
    private fun commitSelection() {
        val e = engine ?: return
        val g = selGesture
        val shape = when (selShape) {
            SelectionShape.RECTANGLE -> NativeEngine.SelectShape.RECT
            SelectionShape.ELLIPSE -> NativeEngine.SelectShape.ELLIPSE
            else -> NativeEngine.SelectShape.POLYGON
        }
        val count = if (shape == NativeEngine.SelectShape.POLYGON) g.points.count else 2
        val xy = g.points.data.copyOf(count * 2)
        val mode = NativeEngine.SelectMode.entries[selMode.code]
        val aa = selAntialias
        val name = selShape.name
        layerExec.execute {
            val t0 = System.nanoTime()
            val err = e.select(shape, mode, aa, xy, count)
            val ms = (System.nanoTime() - t0) / 1e6
            host.runOnMain {
                if (closed) return@runOnMain
                logSelection("select $name ${mode.name} ($count points)", ms, err)
                if (err != 0) host.overlays.message(tr(S.SEL_ERR_GENERIC))
                g.cancel()
                refreshSelection()
                handler.postDelayed(historyRefresh, 50)
            }
        }
    }

    /** Runs a selection action; refusals are explained, layers / history / outline follow. */
    private fun runSelection(a: SelectionAction) {
        val e = engine ?: return
        if (closed || !loaded) return
        when (a) {
            SelectionAction.POLYGON_CLOSE -> { if (selGesture.closePolygon()) commitSelection(); refreshSelectionBar(); selOverlay?.invalidate(); return }
            SelectionAction.POLYGON_UNDO -> { selGesture.undoVertex(); refreshSelectionBar(); selOverlay?.invalidate(); return }
            SelectionAction.POLYGON_CANCEL -> { selGesture.cancel(); refreshSelectionBar(); selOverlay?.invalidate(); return }
            else -> Unit
        }
        val op = when (a) {
            SelectionAction.MOVE -> NativeEngine.SelectionOp.MOVE_BEGIN
            SelectionAction.COPY -> NativeEngine.SelectionOp.COPY
            SelectionAction.COPY_MERGED -> NativeEngine.SelectionOp.COPY_MERGED
            SelectionAction.CUT -> NativeEngine.SelectionOp.CUT
            SelectionAction.PASTE -> NativeEngine.SelectionOp.PASTE
            SelectionAction.DUPLICATE -> NativeEngine.SelectionOp.DUPLICATE
            SelectionAction.CLEAR -> NativeEngine.SelectionOp.CLEAR
            SelectionAction.INVERT -> NativeEngine.SelectionOp.INVERT
            SelectionAction.DESELECT -> NativeEngine.SelectionOp.DESELECT
            SelectionAction.SELECT_ALL -> NativeEngine.SelectionOp.SELECT_ALL
            SelectionAction.FROM_CONTENT -> NativeEngine.SelectionOp.FROM_CONTENT
            SelectionAction.MOVE_DONE -> NativeEngine.SelectionOp.MOVE_COMMIT
            else -> NativeEngine.SelectionOp.MOVE_CANCEL
        }
        // Moving needs the Select tool (its drags move the content).
        if (a == SelectionAction.MOVE && tool != Tool.SELECT) selectTool(Tool.SELECT)
        val mode = NativeEngine.SelectMode.entries[selMode.code]
        val layerName = layerStack?.active?.name ?: ""
        layerExec.execute {
            val t0 = System.nanoTime()
            val r = e.selectionOp(op, mode)
            val ms = (System.nanoTime() - t0) / 1e6
            val changesLayers = a == SelectionAction.PASTE || a == SelectionAction.DUPLICATE
            val table = if (changesLayers) e.layers() else null
            host.runOnMain {
                if (closed) return@runOnMain
                logSelection(a.name, ms, r.error)
                val err = SelectionErrors.message(a, r.error)
                when {
                    err != null -> host.overlays.message(tr(err))
                    a == SelectionAction.COPY -> host.overlays.message(tr(S.SEL_DONE_COPY, layerName))
                    a == SelectionAction.COPY_MERGED -> host.overlays.message(tr(S.SEL_DONE_COPY_MERGED))
                    a == SelectionAction.CUT -> host.overlays.message(tr(S.SEL_DONE_CUT, layerName))
                    a == SelectionAction.PASTE -> host.overlays.message(tr(S.SEL_DONE_PASTE))
                    a == SelectionAction.DUPLICATE -> host.overlays.message(tr(S.SEL_DONE_DUPLICATE))
                    a == SelectionAction.CLEAR -> host.overlays.message(tr(S.SEL_DONE_CLEAR, layerName))
                    a == SelectionAction.FROM_CONTENT -> host.overlays.message(tr(S.SEL_DONE_FROM_CONTENT))
                    a == SelectionAction.MOVE -> host.overlays.message(tr(S.SEL_HINT_MOVE))
                }
                if (a == SelectionAction.MOVE && r.error == 0) { moveX = 0; moveY = 0; moveBaseX = 0; moveBaseY = 0 }
                table?.let { t -> LayerStack.parse(t)?.let { layerStack = it } }
                if (changesLayers && spec?.pinned == PinnedPanel.LAYERS) refreshPinned()
                if (changesLayers && panel == Panel.LAYERS) showPanel(Panel.LAYERS)
                refreshHistoryButtons()
                handler.postDelayed(historyRefresh, 50)
                refreshSelection()
            }
        }
    }

    /** Fetches the selection state, outline and coverage preview off the UI thread, then shows them. */
    private fun refreshSelection() {
        val e = engine ?: return
        if (closed) return
        if (selRefreshing) { selRefreshAgain = true; return }
        selRefreshing = true
        layerExec.execute {
            val version = e.selectionVersion()
            val info = e.selectionInfo()
            val contours = if (info.active) e.selectionContours() else null
            val size = IntArray(2)
            val preview = if (info.active) e.selectionPreview(SELECTION_PREVIEW_PX, size) else null
            host.runOnMain {
                selRefreshing = false
                if (closed) return@runOnMain
                selVersion = version
                selInfo = info
                if (!info.moving) { moveX = 0; moveY = 0 }
                selOverlay?.setSelection(contours, preview, size[0], size[1], meta.width, meta.height)
                selOverlay?.setMoveOffset(if (info.moving) moveX else 0, if (info.moving) moveY else 0)
                refreshSelectionBar()
                if (selRefreshAgain) { selRefreshAgain = false; refreshSelection() }
            }
        }
    }

    private fun logSelection(what: String, ms: Double, error: Int) {
        if (com.paintengine.BuildConfig.DEBUG) android.util.Log.i("PxPaint", String.format(java.util.Locale.US,
            "Selection: %s %.2f ms (error %d)", what, ms, error))
    }

    /** Leaving the Select tool: content being moved lands, a polygon in progress is dropped. */
    private fun leavingSelectTool(next: Tool) {
        if (tool != Tool.SELECT || next == Tool.SELECT) return
        if (selInfo.moving) runSelection(SelectionAction.MOVE_DONE)
        selGesture.cancel()
        selOverlay?.showDim = false
        selOverlay?.invalidate()
    }

    private fun selectionEditable(): Boolean = layerStack?.active?.let { it.visible && !it.locked } ?: false

    /** The contextual bar: only actions that can act now; hidden when there is nothing to do. */
    private fun selectionBar(): LinearLayout {
        val row = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        selBarRow = row
        selBarKey = ""
        selBarShown = false
        return LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            background = theme.rounded(theme.chrome, theme.radiusPill, theme.outline)
            elevation = theme.dpF(3f)
            setPadding(theme.dp(4), theme.dp(2), theme.dp(4), theme.dp(2))
            contentDescription = tr(S.SEL_ACTIONS)
            addView(android.widget.HorizontalScrollView(context).apply {
                isHorizontalScrollBarEnabled = false
                addView(row)
            })
            visibility = View.GONE
        }
    }

    private fun refreshSelectionBar() {
        val bar = selBar ?: return
        val row = selBarRow ?: return
        val polygon = if (selGesture.phase == SelectionGesture.Phase.POLYGON) selGesture.points.count else 0
        val actions = if (tool == Tool.SELECT || selInfo.active || selInfo.moving)
            SelectionAction.available(selInfo.active, selInfo.clipboard, selInfo.moving, selectionEditable(), polygon) else emptyList()
        val key = actions.joinToString(",") { it.name }
        if (key != selBarKey) {
            selBarKey = key
            row.removeAllViews()
            for (a in actions) {
                val primary = a == SelectionAction.MOVE_DONE || a == SelectionAction.POLYGON_CLOSE
                row.addView(Widgets.chip(context, theme, tr(a.label), primary) { runSelection(a) }.apply {
                    contentDescription = tr(a.label)
                }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, theme.touchTarget).apply {
                    marginStart = theme.dp(2); marginEnd = theme.dp(2)
                })
            }
        }
        val show = actions.isNotEmpty() && !fullscreen
        if (show == selBarShown) return
        selBarShown = show
        if (show) {
            bar.visibility = View.VISIBLE
            motion.enter(bar, dyDp = -MotionTokens.SHIFT)
        } else {
            motion.exit(bar, dyDp = -MotionTokens.SHIFT) { if (!selBarShown) bar.visibility = View.GONE }
        }
    }

    /** Selection tool options (the tool's panel): shape, mode, edges, whole-canvas actions. */
    private fun selectionSettings(): View = column(context, theme, horizontalPadding = false) {
        addView(theme.text(context, tr(S.SEL_TITLE), PxTheme.Type.HEADLINE).apply { minHeight = theme.dp(36); gravity = Gravity.CENTER_VERTICAL })
        addView(theme.text(context, tr(S.SEL_SHAPE), PxTheme.Type.LABEL, theme.textSecondary).apply { setPadding(0, theme.dp(8), 0, theme.dp(6)) })
        addView(Widgets.segmented(context, theme, SelectionShape.entries.map { it to tr(it.label) }, selShape) {
            selGesture.cancel()
            selShape = it
            selOverlay?.shape = it
            host.overlays.message(tr(when (it) {
                SelectionShape.RECTANGLE -> S.SEL_HINT_RECTANGLE
                SelectionShape.ELLIPSE -> S.SEL_HINT_ELLIPSE
                SelectionShape.LASSO -> S.SEL_HINT_LASSO
                SelectionShape.POLYGON -> S.SEL_HINT_POLYGON
            }))
            showPanel(panel)
            refreshSelectionBar()
        })
        addView(theme.text(context, tr(S.SEL_MODE), PxTheme.Type.LABEL, theme.textSecondary).apply { setPadding(0, theme.dp(12), 0, theme.dp(6)) })
        addView(Widgets.segmented(context, theme, SelectionMode.entries.map { it to tr(it.label) }, selMode) {
            selMode = it
            showPanel(panel)
        })
        addView(compactSwitch(tr(S.SEL_ANTIALIAS), tr(S.SEL_ANTIALIAS_DESC), selAntialias, true) { selAntialias = it })
        val whole = mutableListOf(SelectionAction.SELECT_ALL, SelectionAction.FROM_CONTENT)
        if (selInfo.active) { whole += SelectionAction.INVERT; whole += SelectionAction.DESELECT }
        addView(com.paintengine.pxpaint.ui.FlowRow(context, theme.dp(8)).apply {
            setPadding(0, theme.dp(8), 0, 0)
            for (a in whole) addView(Widgets.chip(context, theme, tr(a.label), false) { runSelection(a); setPanel(Panel.NONE) }.apply {
                contentDescription = tr(a.label)
            })
        })
    }

    // --------------------------------------------------------------- panels
    private var panelView: View? = null

    private fun pinnedPanelOf(s: WorkspaceSpec?): Panel = when (s?.pinned) {
        PinnedPanel.BRUSHES -> Panel.TOOL
        PinnedPanel.COLOR -> Panel.COLOR
        PinnedPanel.LAYERS -> Panel.LAYERS
        else -> Panel.NONE
    }

    private fun pinnable(p: Panel): PinnedPanel? = when (p) {
        Panel.TOOL -> PinnedPanel.BRUSHES
        Panel.COLOR -> PinnedPanel.COLOR
        Panel.LAYERS -> PinnedPanel.LAYERS
        else -> null
    }

    private fun togglePanel(p: Panel) {
        if (p != Panel.NONE && p == pinnedPanelOf(spec)) return   // already open beside the canvas
        setPanel(if (panel == p) Panel.NONE else p)
    }

    private fun setPanel(p: Panel) {
        if (p == Panel.COLOR && panel != Panel.COLOR) colorBefore = color
        if (p != Panel.NONE && p == pinnedPanelOf(spec)) {
            // Already open beside the canvas: just refresh it.
            panel = Panel.NONE
            panelView?.let { chrome?.removeView(it) }
            panelView = null
            refreshPinned()
            return
        }
        val old = panelView
        panel = p
        panelView = null
        if (old != null) {
            if (p == Panel.NONE) {
                // Panel dismissal: slides / fades away the way it came.
                val sheet = spec?.panelStyle == PanelStyle.BOTTOM_SHEET
                motion.exit(old, dyDp = if (sheet) MotionTokens.SHEET else 0f) { chrome?.removeView(old) }
            } else chrome?.removeView(old)
        }
        if (p == Panel.NONE) shownPanel = Panel.NONE
        slidersView?.let { if (it.visibility != View.VISIBLE) { it.visibility = View.VISIBLE; motion.fadeTo(it, 1f, from = 0f) } }
        if (p != Panel.NONE) showPanel(p)
    }

    private fun panelContent(p: Panel): View? = when (p) {
        Panel.TOOL -> if (tool == Tool.SELECT) selectionSettings() else toolSettings()
        Panel.COLOR -> colorPicker()
        Panel.LAYERS -> layersPanel()
        Panel.LIBRARY -> libraryPanel()
        Panel.HISTORY -> historyPanel()
        Panel.NONE -> null
    }

    private fun showPanel(p: Panel) {
        val c = chrome ?: return
        panelView?.let { c.removeView(it) }
        panelView = null
        val s = spec ?: return
        if (fullscreen) return
        if (p == pinnedPanelOf(s)) {
            refreshPinned()
            return
        }
        val content = panelContent(p) ?: return
        val ins = host.insets
        val m = theme.dp(8)
        val barH = theme.touchTarget + theme.dp(if (s.compactTopBar) 4 else 12)
        val belowBar = ins.top + m + barH + m
        val heightPx = if (c.height > 0) c.height else (host.windowHeightDp * context.resources.displayMetrics.density).toInt()
        val pinnedW = if (s.pinned != PinnedPanel.NONE) theme.dp(s.panelWidthDp) + m + theme.touchTarget else 0
        val frame: View
        val lp: FrameLayout.LayoutParams
        when (s.panelStyle) {
            PanelStyle.BOTTOM_SHEET -> {
                val dockH = theme.touchTarget + theme.dp(24) + theme.dp(if (s.toolLabels) 18 else 0)
                val maxH = minOf(theme.dp(s.sheetMaxHeightDp), heightPx - belowBar - ins.bottom - dockH - m)
                frame = panelFrame(p, content, s, sheet = true, docked = false, maxHeight = maxH)
                lp = FrameLayout.LayoutParams(minOf(c.width.takeIf { it > 0 } ?: Int.MAX_VALUE, theme.dp(DesignTokens.SHEET_MAX_WIDTH)) - 2 * m,
                    FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL).apply {
                    bottomMargin = ins.bottom + dockH
                }
            }
            PanelStyle.SIDE_SHEET -> {
                frame = panelFrame(p, content, s, sheet = false, docked = false, maxHeight = heightPx - belowBar - ins.bottom - m)
                lp = FrameLayout.LayoutParams(theme.dp(s.panelWidthDp), FrameLayout.LayoutParams.WRAP_CONTENT,
                    Gravity.TOP or (if (s.panelSide == Side.LEFT) Gravity.START else Gravity.END)).apply {
                    topMargin = belowBar
                    if (s.panelSide == Side.LEFT) marginStart = ins.left + m else marginEnd = ins.right + m
                }
            }
            PanelStyle.POPOVER -> {
                frame = panelFrame(p, content, s, sheet = false, docked = false, maxHeight = heightPx - belowBar - ins.bottom - m)
                lp = FrameLayout.LayoutParams(theme.dp(s.panelWidthDp), FrameLayout.LayoutParams.WRAP_CONTENT,
                    Gravity.TOP or (if (s.panelSide == Side.LEFT) Gravity.START else Gravity.END)).apply {
                    topMargin = belowBar
                    // Beside the pinned panel, never over it.
                    if (s.panelSide == Side.LEFT) marginStart = ins.left + m + pinnedW else marginEnd = ins.right + m + pinnedW
                }
            }
        }
        panelView = frame
        c.addView(frame, lp)
        // A bottom sheet covers the edge sliders: hide them until it closes.
        slidersView?.visibility = if (s.panelStyle == PanelStyle.BOTTOM_SHEET) View.INVISIBLE else View.VISIBLE
        // Floating panel appearance: only when it newly opens (an update of the open panel just swaps).
        if (p != shownPanel) when (s.panelStyle) {
            PanelStyle.BOTTOM_SHEET -> motion.enter(frame, dyDp = MotionTokens.SHEET)
            PanelStyle.SIDE_SHEET -> motion.enter(frame, dyDp = 0f, dxDp = if (s.panelSide == Side.LEFT) -MotionTokens.SLIDE else MotionTokens.SLIDE)
            PanelStyle.POPOVER -> {
                motion.enter(frame, dyDp = -MotionTokens.NUDGE * 2)
                if (motion.movement) { frame.pivotY = 0f; frame.scaleX = MotionTokens.ENTER_SCALE; frame.scaleY = MotionTokens.ENTER_SCALE; frame.animate().scaleX(1f).scaleY(1f) }
            }
        }
        shownPanel = p
    }

    /**
     * Card around a panel: a header with the sheet's drag handle (swipe down
     * closes), pin / unpin (tablets) and close; the content scrolls inside,
     * never taller than [maxHeight].
     */
    @SuppressLint("ClickableViewAccessibility")
    private fun panelFrame(p: Panel, content: View, s: WorkspaceSpec, sheet: Boolean, docked: Boolean, maxHeight: Int): View {
        val card = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            background = theme.rounded(theme.chrome, theme.radiusL, theme.outline)
            elevation = theme.dpF(if (docked) 2f else 8f)
            isClickable = true
        }
        val header = FrameLayout(context).apply {
            minimumHeight = theme.dp(if (sheet) 28 else 8)
        }
        if (sheet) {
            header.addView(View(context).apply { background = theme.rounded(theme.textTertiary, theme.radiusPill) },
                FrameLayout.LayoutParams(theme.dp(36), theme.dp(4), Gravity.CENTER_HORIZONTAL or Gravity.TOP).apply { topMargin = theme.dp(10) })
            var downY = 0f
            header.setOnTouchListener { _, e ->
                when (e.actionMasked) {
                    MotionEvent.ACTION_DOWN -> downY = e.rawY
                    MotionEvent.ACTION_MOVE -> card.translationY = (e.rawY - downY).coerceAtLeast(0f)
                    MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                        if (e.rawY - downY > theme.dpF(56f)) setPanel(Panel.NONE)
                        else card.animate().translationY(0f).setDuration(motion.d(MotionTokens.MEDIUM)).setInterpolator(Motion.spring).start()
                    }
                }
                true
            }
        }
        val actions = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL }
        val pin = pinnable(p)
        if (pin != null && s.canPin) {
            actions.addView(IconButton(context, theme, Icon.PIN, tr(if (docked) S.PANEL_UNPIN else S.PANEL_PIN), sizeDp = 34f).apply {
                isSelected = docked
                alpha = if (docked) 1f else 0.7f
                setOnClickListener {
                    if (!docked) panel = Panel.NONE
                    host.updateSettings(host.settings.copy(pinnedPanel = if (docked) PinnedPanel.NONE else pin))
                }
            })
        }
        if (!docked) actions.addView(IconButton(context, theme, Icon.CLOSE, tr(S.PANEL_CLOSE), sizeDp = 34f).apply {
            setOnClickListener { setPanel(Panel.NONE) }
        })
        if (actions.childCount > 0) header.addView(actions, FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT,
            FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.END or Gravity.TOP).apply { setMargins(0, theme.dp(4), theme.dp(6), 0) })
        card.addView(header, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        val body = FrameLayout(context).apply {
            setPadding(theme.dp(16), 0, theme.dp(16), theme.dp(16))
            addView(content)
        }
        val scroll = android.widget.ScrollView(context).apply {
            isVerticalScrollBarEnabled = true
            addView(body)
        }
        card.addView(scroll, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        return MaxHeight(context, maxHeight.coerceAtLeast(theme.dp(200))).apply { addView(card) }
    }

    /**
     * The pinned panel (tablets): beside the canvas, under the top bar. A grip
     * just outside its inner edge (a full touch target over the canvas edge)
     * drags to resize it; accessibility: scroll forward / backward = +/- 20 dp.
     */
    @SuppressLint("ClickableViewAccessibility")
    private fun pinnedPanel(c: FrameLayout, s: WorkspaceSpec, top: Int) {
        val p = pinnedPanelOf(s)
        val content = panelContent(p) ?: return
        val ins = host.insets
        val m = theme.dp(8)
        val gripW = theme.touchTarget
        val heightPx = if (c.height > 0) c.height else (host.windowHeightDp * context.resources.displayMetrics.density).toInt()
        val frame = panelFrame(p, content, s, sheet = false, docked = true, maxHeight = heightPx - top - ins.bottom - m)
        val left = s.panelSide == Side.LEFT
        val box = FrameLayout(context)
        box.addView(frame, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.WRAP_CONTENT).apply {
            if (left) marginEnd = gripW else marginStart = gripW
        })
        val lp = FrameLayout.LayoutParams(theme.dp(s.panelWidthDp) + gripW, FrameLayout.LayoutParams.WRAP_CONTENT,
            Gravity.TOP or (if (left) Gravity.START else Gravity.END)).apply {
            topMargin = top
            if (left) marginStart = ins.left + m else marginEnd = ins.right + m
        }
        fun resizeTo(w: Int, save: Boolean) {
            lp.width = theme.dp(w) + gripW
            box.layoutParams = lp
            if (save && w != host.settings.panelWidthDp) host.updateSettings(host.settings.copy(panelWidthDp = w))
        }
        val grip = object : View(context) {
            override fun onInitializeAccessibilityNodeInfo(info: android.view.accessibility.AccessibilityNodeInfo) {
                super.onInitializeAccessibilityNodeInfo(info)
                info.addAction(android.view.accessibility.AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_FORWARD)
                info.addAction(android.view.accessibility.AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_BACKWARD)
            }

            override fun performAccessibilityAction(action: Int, arguments: android.os.Bundle?): Boolean {
                val step = when (action) {
                    android.view.accessibility.AccessibilityNodeInfo.ACTION_SCROLL_FORWARD -> 20
                    android.view.accessibility.AccessibilityNodeInfo.ACTION_SCROLL_BACKWARD -> -20
                    else -> return super.performAccessibilityAction(action, arguments)
                }
                resizeTo(LayoutPolicy.resizePanel(host.settings.panelWidthDp, step, host.windowWidthDp, theme.scale), true)
                return true
            }
        }.apply {
            background = IconDrawable(Icon.GRIP, theme.textTertiary)
            contentDescription = tr(S.PANEL_RESIZE)
            isClickable = true
        }
        var startX = 0f
        var startW = 0
        grip.setOnTouchListener { _, e ->
            val dpx = context.resources.displayMetrics.density * theme.scale
            when (e.actionMasked) {
                MotionEvent.ACTION_DOWN -> { startX = e.rawX; startW = s.panelWidthDp }
                MotionEvent.ACTION_MOVE, MotionEvent.ACTION_UP -> {
                    // Towards the canvas = wider.
                    val delta = ((if (left) e.rawX - startX else startX - e.rawX) / dpx).roundToInt()
                    resizeTo(LayoutPolicy.resizePanel(startW, delta, host.windowWidthDp, theme.scale), e.actionMasked == MotionEvent.ACTION_UP)
                }
            }
            true
        }
        box.addView(grip, FrameLayout.LayoutParams(gripW, theme.dp(72), (if (left) Gravity.END else Gravity.START) or Gravity.TOP).apply {
            topMargin = theme.dp(120)
        })
        pinnedView = box
        c.addView(box, lp)
    }

    /** Rebuilds only the pinned panel (its content changed); the rest of the chrome stays. */
    private fun refreshPinned() {
        val s = spec ?: return
        val c = chrome ?: return
        if (s.pinned == PinnedPanel.NONE || fullscreen) return
        val old = pinnedView ?: return
        val idx = c.indexOfChild(old)
        val lp = old.layoutParams as? FrameLayout.LayoutParams ?: return
        c.removeView(old)
        pinnedPanel(c, s, lp.topMargin)
        pinnedView?.let { v -> if (idx >= 0) { c.removeView(v); c.addView(v, idx) } }
    }

    private fun toolSettings(): View = column(context, theme, horizontalPadding = false) {
        val t = if (!tool.paints) Tool.MARKER else tool
        val bt = brushTool(t)
        addView(theme.text(context, tr(t.label), PxTheme.Type.HEADLINE).apply { minHeight = theme.dp(36); gravity = Gravity.CENTER_VERTICAL })
        // The tool's brush: live preview + name; opens the library.
        val preset = host.brushes.selectedPreset(bt)
        addView(LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            minimumHeight = theme.touchTarget + theme.dp(8)
            setPadding(theme.dp(6), theme.dp(4), theme.dp(4), theme.dp(4))
            background = theme.pressable(theme.rounded(theme.surface, theme.radiusS, theme.outline), theme.radiusS)
            isClickable = true
            contentDescription = "${tr(S.CHANGE_BRUSH)}: ${preset.displayName()}"
            setOnClickListener { setPanel(Panel.LIBRARY) }
            addView(BrushPreviewView(context, theme, host.previews, false) { host.pressureTable() }.also { it.show(preset.params) },
                LinearLayout.LayoutParams(theme.dp(96), theme.dp(40)))
            addView(column(context, theme, horizontalPadding = false) {
                setPadding(theme.dp(10), 0, 0, 0)
                addView(theme.text(context, preset.displayName(), PxTheme.Type.LABEL, lines = 1))
                addView(theme.text(context, tr(S.CHANGE_BRUSH), PxTheme.Type.CAPTION, theme.accent, lines = 1))
            }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            addView(View(context).apply { background = IconDrawable(Icon.CHEVRON_RIGHT, theme.textSecondary) },
                LinearLayout.LayoutParams(theme.dp(18), theme.dp(18)))   // explicit size (see Widgets.row)
        })
        // Quick switching: the recently used brushes.
        val recents = host.brushes.recents.mapNotNull { host.brushes.get(it) }.filter { it.id != preset.id }.take(6)
        if (recents.isNotEmpty()) {
            addView(theme.text(context, tr(S.RECENT_BRUSHES), PxTheme.Type.LABEL, theme.textSecondary).apply { setPadding(0, theme.dp(12), 0, theme.dp(6)) })
            addView(android.widget.HorizontalScrollView(context).apply {
                isHorizontalScrollBarEnabled = false
                addView(LinearLayout(context).apply {
                    orientation = LinearLayout.HORIZONTAL
                    for (r in recents) addView(Widgets.chip(context, theme, r.displayName(), false) {
                        host.updateBrushes(host.brushes.select(bt, r.id))
                        showPanel(panel)
                        refreshDock()
                    }.apply { (layoutParams as? LinearLayout.LayoutParams)?.marginEnd = theme.dp(6) },
                        LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, theme.touchTarget).apply { marginEnd = theme.dp(6) })
                })
            })
        }
        val b = host.brushes.effective(bt)
        val percent = { f: Float -> "${(f * 100).roundToInt()}%" }
        addView(sliderRow(tr(S.SIZE), BrushState.sizeToSlider(b.size), { BrushState.sizeLabel(BrushState.sliderToSize(it)) }) {
            host.updateBrushes(host.brushes.setLive(bt, size = BrushState.sliderToSize(it)))
            sizeSlider?.setValue(it)
        })
        addView(sliderRow(tr(S.OPACITY), b.opacity, { percent(BrushState.sliderToOpacity(it)) }) {
            host.updateBrushes(host.brushes.setLive(bt, opacity = BrushState.sliderToOpacity(it)))
            opacitySlider?.setValue(it)
        })
        addView(Widgets.row(context, theme, tr(S.EDIT_BRUSH), null,
            View(context).apply { background = IconDrawable(Icon.CHEVRON_RIGHT, theme.textSecondary) }.also {
                it.layoutParams = LinearLayout.LayoutParams(theme.dp(18), theme.dp(18))
            }).apply {
            setPadding(0, paddingTop, 0, paddingBottom)
            isClickable = true
            setOnClickListener { host.push(BrushEditorScreen(host, bt)) }
        })
        // Pressure / tilt: the full controls when a pressure stylus is in use, else one row.
        if (host.stylus.pressureAvailable) addView(stylusSettings(t))
        else addView(Widgets.row(context, theme, tr(S.STYLUS_PRESSURE_ROW), tr(S.NO_PRESSURE_STYLUS),
            View(context).apply { background = IconDrawable(Icon.CHEVRON_RIGHT, theme.textSecondary) }.also {
                it.layoutParams = LinearLayout.LayoutParams(theme.dp(18), theme.dp(18))
            }).apply {
            setPadding(0, paddingTop, 0, paddingBottom)
            isClickable = true
            setOnClickListener { host.push(PressureScreen(host)) }
        })
    }

    private fun libraryPanel(): View {
        val t = if (!tool.paints) Tool.MARKER else tool
        val panel = BrushLibraryPanel(host, brushTool(t), libraryState,
            onEdit = { bt -> host.push(BrushEditorScreen(host, bt)) },
            onChanged = {
                showPanel(Panel.LIBRARY)
                if (spec?.pinned == PinnedPanel.BRUSHES) refreshPinned()
                refreshDock()
            })
        return panel.view()
    }

    /** Pressure / tilt dynamics of the tool (stylus strokes only). */
    private fun stylusSettings(t: Tool): View = column(context, theme, horizontalPadding = false) {
        setPadding(0, theme.dp(14), 0, 0)
        addView(Widgets.divider(context, theme))
        addView(theme.text(context, tr(S.STYLUS_SECTION), PxTheme.Type.HEADLINE).apply { setPadding(0, theme.dp(12), 0, 0) })
        val m = host.stylus
        addView(theme.text(context, if (m.pressureAvailable) tr(S.PRESSURE_STYLUS_READY) else tr(S.NO_PRESSURE_STYLUS),
            PxTheme.Type.CAPTION, theme.textSecondary).apply { setPadding(0, theme.dp(4), 0, theme.dp(4)) })
        val b = host.brushes.effective(brushTool(t))
        val percent = { f: Float -> "${(f * 100).roundToInt()}%" }
        fun fraction(v: Float) = (BrushState.MIN_PRESSURE_FRACTION + v * (1f - BrushState.MIN_PRESSURE_FRACTION)).let { (it * 100).roundToInt() / 100f }
        fun slider(f: Float) = ((f - BrushState.MIN_PRESSURE_FRACTION) / (1f - BrushState.MIN_PRESSURE_FRACTION)).coerceIn(0f, 1f)
        addView(compactSwitch(tr(S.PRESSURE_SIZE), null, b.pressureSize, true) {
            updatePreset(t) { p -> p.copy(pressureSize = it) }
        })
        addView(sliderRow(tr(S.MIN_SIZE), slider(b.minSize), { percent(fraction(it)) }) {
            updatePreset(t) { p -> p.copy(minSize = fraction(it)) }
        })
        addView(compactSwitch(tr(S.PRESSURE_OPACITY), null, b.pressureOpacity, true) {
            updatePreset(t) { p -> p.copy(pressureOpacity = it) }
        })
        addView(sliderRow(tr(S.MIN_OPACITY), slider(b.minOpacity), { percent(fraction(it)) }) {
            updatePreset(t) { p -> p.copy(minOpacity = fraction(it)) }
        })
        val tilt = m.tiltAvailable
        addView(compactSwitch(tr(S.TILT_SHAPE), if (tilt) null else tr(S.TILT_UNAVAILABLE), b.tilt && tilt, tilt) {
            updatePreset(t) { p -> p.copy(tilt = it) }
        })
        addView(Widgets.row(context, theme, tr(S.PRESSURE_SETTINGS), null,
            View(context).apply { background = IconDrawable(Icon.CHEVRON_RIGHT, theme.textSecondary) }.also {
                it.layoutParams = LinearLayout.LayoutParams(theme.dp(18), theme.dp(18))
            }).apply {
            setPadding(0, paddingTop, 0, paddingBottom)
            isClickable = true
            setOnClickListener { host.push(PressureScreen(host)) }
        })
    }

    private fun compactSwitch(title: String, desc: String?, checked: Boolean, enabled: Boolean, onChange: (Boolean) -> Unit): View =
        Widgets.switchRow(context, theme, title, desc, checked, onChange).apply {
            setPadding(0, paddingTop, 0, paddingBottom)
            isEnabled = enabled
            alpha = if (enabled) 1f else 0.5f
            if (!enabled) (this as? ViewGroup)?.let { g -> for (i in 0 until g.childCount) g.getChildAt(i).isEnabled = false }
        }

    /** Label and current value above a slider. */
    private fun sliderRow(label: String, initial: Float, format: (Float) -> String, apply: (Float) -> Unit): View =
        column(context, theme, horizontalPadding = false) {
            setPadding(0, theme.dp(8), 0, 0)
            val value = theme.text(context, format(initial), PxTheme.Type.LABEL, theme.textSecondary)
            addView(LinearLayout(context).apply {
                orientation = LinearLayout.HORIZONTAL
                addView(theme.text(context, label, PxTheme.Type.LABEL), LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
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

    private fun colorPicker(): View = ColorPanel(host, colorOps, colorState).view()

    private val colorOps = object : ColorPanel.Ops {
        override val color: Int get() = this@WorkspaceScreen.color
        override val previous: Int get() = if (colorBefore != 0) colorBefore else this@WorkspaceScreen.color
        override val recent: List<Int> get() = host.brush.recentColors
        override val palette: List<Int> get() = host.brush.palette
        override fun pick(color: Int, final: Boolean) = applyColor(color, final)
        override fun eyedropper() = armEyedropper()
        override fun save(color: Int) {
            if (host.brush.palette.size >= BrushState.MAX_PALETTE) host.overlays.message(tr(S.COLOR_PALETTE_FULL))
            host.updateBrush(host.brush.withSaved(color))
            showColorPanels()
        }
        override fun remove(color: Int) {
            host.updateBrush(host.brush.withoutSaved(color))
            showColorPanels()
        }
    }

    /** The brush colour changes (picker, eyedropper); painting tools follow it. */
    private fun applyColor(c: Int, final: Boolean) {
        // While the wheel is dragged only the swatch follows; the brush colour is set on release.
        (colorButton as? FrameLayout)?.getChildAt(0)?.background = theme.oval(c, theme.text)
        if (!final) return
        host.updateBrush(host.brush.copy(color = c))
        colorButton?.let { motion.pop(it) }      // colour selection: the swatch answers
        if (tool != Tool.MARKER) {
            leavingSelectTool(Tool.MARKER)
            tool = Tool.MARKER
            refreshDock()
            refreshSelectionBar()
        }
        remember(c)
    }

    private fun showColorPanels() {
        if (panel == Panel.COLOR) showPanel(Panel.COLOR)
        if (spec?.pinned == PinnedPanel.COLOR) refreshPinned()
    }

    // ------------------------------------------------------------- eyedropper
    private var eyedropperArmed = false
    private var picking = false
    private var loupe: View? = null

    private fun armEyedropper() {
        eyedropperArmed = true
        if (spec?.pinned != PinnedPanel.COLOR) setPanel(Panel.NONE)
        host.overlays.message(tr(S.EYEDROPPER_HINT))
    }

    /** Samples the colour under the finger / stylus; [final] applies it. */
    private fun pickAt(x: Float, y: Float, final: Boolean) {
        val e = engine ?: return
        val c = e.sampleColor(x.toDouble(), y.toDouble())
        val ch = chrome
        if (!final && c != null && ch != null) {
            val l = loupe ?: View(context).also {
                loupe = it
                ch.addView(it, FrameLayout.LayoutParams(theme.dp(56), theme.dp(56)))
                motion.pop(it, 0.6f)          // eyedropper activation: the loupe grows in
            }
            l.background = theme.oval(c, theme.text)
            l.translationX = x - theme.dp(28)
            l.translationY = y - theme.dp(28) - theme.dp(72)   // above the finger
        }
        if (final) {
            loupe?.let { chrome?.removeView(it) }
            loupe = null
            if (c == null) host.overlays.message(tr(S.EYEDROPPER_NOTHING))
            else {
                applyColor(c, true)
                host.overlays.message(tr(S.COLOR_PICKED, com.paintengine.pxpaint.core.ColorMath.toHex(c)))
                showColorPanels()
            }
        }
    }

    private fun remember(c: Int) {
        // Debounced: the colour that stays selected for a moment becomes "recent".
        handler.removeCallbacks(rememberColor)
        handler.postDelayed(rememberColor, 700)
        pendingRecent = c
    }

    private var pendingRecent = 0
    private val rememberColor = Runnable { host.updateBrush(host.brush.withRecent(pendingRecent)) }

    private val layersState = LayersPanel.State()
    private fun layersPanel(): View = LayersPanel(host, layerOps, layersState).view()

    // ---------------------------------------------------------------- history
    private fun historyPanel(): View {
        if (history.undo != historyCounts.first || history.redo != historyCounts.second) fetchHistory()
        return HistoryPanel(host, historyOps).view()
    }

    private val historyOps = object : HistoryPanel.Ops {
        override val history: HistoryState get() = this@WorkspaceScreen.history
        override fun goTo(index: Int) {
            val delta = history.moveTo(index)
            if (delta < 0) historyOp(NativeEngine.HistoryOp.UNDO, -delta, announce = false)
            else if (delta > 0) historyOp(NativeEngine.HistoryOp.REDO, delta, announce = false)
        }
        override fun clear() = historyOp(NativeEngine.HistoryOp.CLEAR)
    }

    /** Announces the step undone (the first undone one) or redone (the last done one). */
    private fun announceStep(h: HistoryState?, op: NativeEngine.HistoryOp, announce: Boolean) {
        if (!announce || h == null) return
        val step = if (op == NativeEngine.HistoryOp.UNDO) h.steps.getOrNull(h.undo) else h.steps.getOrNull(h.undo - 1)
        step?.let {
            val what = tr(it.kind.label) + if (it.label.isNotEmpty()) " · ${it.label}" else ""
            host.overlays.message(tr(if (op == NativeEngine.HistoryOp.UNDO) S.UNDONE else S.REDONE, what))
        }
    }

    /** Enabled state of undo / redo from the engine's lock-free counts (cheap: every poll and after edits). */
    private fun refreshHistoryButtons() {
        val e = engine ?: return
        if (closed) return
        val c = e.historyCounts()
        if (c == historyCounts) return
        historyCounts = c
        showHistoryButtons()
        if (panel == Panel.HISTORY) fetchHistory()
    }

    private fun showHistoryButtons() {
        val (u, r) = historyCounts
        undoButton?.let { it.isEnabled = u > 0; it.alpha = if (u > 0) 1f else 0.38f }
        redoButton?.let { it.isEnabled = r > 0; it.alpha = if (r > 0) 1f else 0.38f }
    }

    /** History list for the panel, read off the UI thread (it consumes queued input). */
    private fun fetchHistory() {
        val e = engine ?: return
        if (closed) return
        layerExec.execute {
            val h = HistoryState.parse(e.historyTable()) ?: return@execute
            host.runOnMain {
                if (closed) return@runOnMain
                history = h
                historyCounts = h.undo to h.redo
                showHistoryButtons()
                if (panel == Panel.HISTORY) showPanel(Panel.HISTORY)
            }
        }
    }

    /**
     * Undo / redo [count] steps, or clear the history, on the layer thread
     * (after every queued layer operation, in order). The layer stack, the
     * panels and the buttons follow; the step undone / redone is announced.
     */
    private fun historyOp(op: NativeEngine.HistoryOp, count: Int = 1, announce: Boolean = true) {
        val e = engine ?: return
        if (closed || !loaded) return
        layerExec.execute {
            val done = e.history(op, count)
            val table = e.layers()
            val h = HistoryState.parse(e.historyTable())
            host.runOnMain {
                if (closed) return@runOnMain
                LayerStack.parse(table)?.let { layerStack = it }
                if (spec?.pinned == PinnedPanel.LAYERS) refreshPinned()
                if (h != null) {
                    history = h
                    historyCounts = h.undo to h.redo
                }
                showHistoryButtons()
                when {
                    done < 0 -> host.overlays.message(tr(S.HISTORY_REFUSED), long = true)
                    op == NativeEngine.HistoryOp.CLEAR -> host.overlays.message(tr(S.HISTORY_CLEARED))
                    done == 0 -> host.overlays.message(tr(if (op == NativeEngine.HistoryOp.UNDO) S.NOTHING_TO_UNDO else S.NOTHING_TO_REDO))
                    done > 0 && op == NativeEngine.HistoryOp.UNDO -> { undoButton?.let { motion.nudge(it, -28f) }; announceStep(h, op, announce) }
                    done > 0 && op == NativeEngine.HistoryOp.REDO -> { redoButton?.let { motion.nudge(it, 28f) }; announceStep(h, op, announce) }
                }
                if (panel == Panel.LAYERS || panel == Panel.HISTORY) showPanel(panel)
                refreshSelection()
            }
        }
    }

    private val layerOps = object : LayersPanel.Ops {
        override val stack: LayerStack? get() = layerStack
        override fun run(op: NativeEngine.LayerOp, id: Int, arg: Int, value: Float, text: String?, done: S?) =
            runLayerOp(op, id, arg, value, text, done)
        override fun thumbnail(layer: LayerInfo, sizePx: Int, onReady: (android.graphics.Bitmap?) -> Unit): android.graphics.Bitmap? =
            layerThumbnail(layer, sizePx, onReady)
        override fun background(): View = backgroundSection()
    }

    // One thread for every layer operation, in the order they were asked:
    // merge / flatten / duplicate of big documents never block the UI thread.
    private val layerExec = java.util.concurrent.Executors.newSingleThreadExecutor { r -> Thread(r, "PxPaint-layers") }

    /** Runs a layer operation; refusals are explained, the panel and stack follow. */
    private fun runLayerOp(op: NativeEngine.LayerOp, id: Int, arg: Int = 0, value: Float = 0f, text: String? = null, done: S? = null) {
        val e = engine ?: return
        if (closed) return
        layerExec.execute {
            val r = e.layerOp(op, id, arg, value, text)
            val table = e.layers()
            host.runOnMain {
                if (closed) return@runOnMain
                val err = LayerError.of(r.error)
                if (err != LayerError.NONE) err.message?.let { host.overlays.message(tr(it)) }
                else done?.let { host.overlays.message(tr(it)) }
                LayerStack.parse(table)?.let { layerStack = it }
                if (spec?.pinned == PinnedPanel.LAYERS) refreshPinned()
                refreshHistoryButtons()
                // The opacity slider keeps its own state while it is dragged.
                if (panel == Panel.LAYERS && op != NativeEngine.LayerOp.OPACITY) showPanel(Panel.LAYERS)
            }
        }
    }

    /** Cached thumbnail of a layer (its own pixels), refreshed off the UI thread when its content changed. */
    private fun layerThumbnail(layer: LayerInfo, sizePx: Int, onReady: (android.graphics.Bitmap?) -> Unit): android.graphics.Bitmap? {
        val cached = thumbCache[layer.id]
        if (cached != null && cached.first == layer.version) return cached.second
        val e = engine ?: return cached?.second
        val id = layer.id
        val version = layer.version
        thumbExec.execute {
            val px = e.layerThumbnail(id, sizePx) ?: return@execute
            if (px.size < 2 || px[0] <= 0 || px[1] <= 0) return@execute
            val bmp = android.graphics.Bitmap.createBitmap(px, 2, px[0], px[0], px[1], android.graphics.Bitmap.Config.ARGB_8888)
            host.runOnMain {
                thumbCache[id] = version to bmp
                onReady(bmp)
            }
        }
        return cached?.second
    }

    /** The active layer refused a stroke: say why (at most every 2 s). */
    private fun paintBlocked() {
        val now = SystemClock.uptimeMillis()
        if (now - lastBlockedMessage < 2000) return
        lastBlockedMessage = now
        val reason = layerStack?.paintBlocker() ?: return
        reason.message?.let { host.overlays.message(tr(it)) }
    }

    /** Background (paper / transparency): not a layer; the bottom layer blends with it. */
    private fun backgroundSection(): View = column(context, theme, horizontalPadding = false) {
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
                }.let { sw -> FrameLayout(context).apply {
                    // 36 dp swatch inside a full touch target.
                    addView(sw, FrameLayout.LayoutParams(theme.dp(36), theme.dp(36), Gravity.CENTER))
                    sw.isClickable = false
                    sw.importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO
                    contentDescription = sw.contentDescription
                    isClickable = true
                    setOnClickListener { setPaper(argb, transparent) }
                } }, LinearLayout.LayoutParams(theme.touchTarget, theme.touchTarget).apply { marginEnd = theme.dp(2) })
            }
        })
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
        host.io.execute { runCatching { host.store.updateAppearance(id, argb, transparent) } }
        save(force = true)   // thumbnail with the new paper
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
            OverlayHost.MenuItem(tr(S.HISTORY), Icon.HISTORY) { setPanel(Panel.HISTORY) },
            OverlayHost.MenuItem(tr(S.CANVAS_INFO), Icon.INFO) { canvasInfo() },
            OverlayHost.MenuItem(tr(S.TEST_RENDERING), Icon.TARGET) { testRendering() },
            OverlayHost.MenuItem(tr(S.CLEAR_CANVAS), Icon.TRASH, destructive = true) { confirmClear() },
            OverlayHost.MenuItem(tr(S.SETTINGS), Icon.SLIDERS) { host.push(SettingsScreen(host)) },
        ))
    }

    private fun confirmClear() {
        host.overlays.confirm(tr(S.CLEAR_CANVAS), tr(S.CLEAR_CONFIRM), tr(S.ACTION_CLEAR), tr(S.ACTION_CANCEL), true) {
            // The active layer only; its tiles are saved as removed by the next save.
            layerStack?.active?.let { runLayerOp(NativeEngine.LayerOp.CLEAR, it.id) }
        }
    }

    private fun canvasInfo() {
        val s = runCatching { JSONObject(engine?.statusJson() ?: "{}") }.getOrDefault(JSONObject())
        val rows = listOf(
            tr(S.INFO_SIZE) to Format.dimensions(meta.width, meta.height),
            tr(S.INFO_STORAGE) to s.optString("canvasStorage"),
            tr(S.INFO_TILES) to "${s.optInt("canvasTilesAllocated")} / ${s.optInt("canvasTilesTotal")} · ${Format.bytes(s.optLong("canvasBytesAllocated"))}",
            tr(S.INFO_CONTENT) to "${s.optLong("canvasRasterTiles")} · ${Format.bytes(s.optLong("canvasRasterBytes"))}",
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
        /** Longer side of the coverage image used to dim the unselected area. */
        const val SELECTION_PREVIEW_PX = 512
        const val TIP_GESTURES = "workspace.gestures"
        private const val AUTOSAVE_MS = 20_000L
        private const val EXPORT_BATCH = 32
        private const val FLAG_CANCELED = 0x20   // MotionEvent.FLAG_CANCELED (API 33)
    }
}
