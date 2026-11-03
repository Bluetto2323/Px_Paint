package com.paintengine.pxpaint.core

/** What one finger does on the canvas (two fingers always move / zoom / rotate). */
enum class FingerMode { DRAW, PAN, GESTURES_ONLY }

/**
 * Decides what each pointer of a touch sequence does: paint, pan, or take
 * part in a two-finger gesture. Pure logic, unit-tested; the workspace
 * feeds it MotionEvent actions and executes the resulting [Actions] with the
 * event's own data (samples of a pointer id, positions).
 *
 * Rules
 *  * A stylus (tip, eraser end) or mouse always paints, whatever [fingerMode]
 *    (the Move tool makes it pan). Its eraser end always erases.
 *  * A finger paints, pans or does nothing per [fingerMode]. Finger strokes
 *    never use pressure (the engine draws them at full size / opacity).
 *  * While a stylus paints, finger contacts are ignored (palm rejection).
 *  * A stylus landing while a finger paints: the finger contact was a palm,
 *    its stroke is cancelled (canvas restored) and the stylus stroke starts.
 *  * A second finger while one finger paints: [StrokeGate] cancels the
 *    stroke if it had only just started (a pinch), else keeps it; then the
 *    two fingers move / zoom / rotate.
 *  * A pointer lifted with FLAG_CANCELED (the system recognised a palm or an
 *    accidental touch) cancels its stroke instead of committing it.
 *  * Strokes follow their pointer ID, not pointer index 0.
 */
class TouchRouter(private val actions: Actions, private val gate: StrokeGate = StrokeGate()) {
    interface Actions {
        fun beginStroke(pointerId: Int, kind: ToolKind, eraser: Boolean)
        /** Send every sample (historical, then current) of [pointerId] in the current event. */
        fun strokeSamples(pointerId: Int)
        /** Send the final samples of [pointerId], then end (commit) the stroke. */
        fun endStroke(pointerId: Int)
        fun cancelStroke()
        fun panStart(pointerId: Int)
        fun panMove(pointerId: Int)
        fun gestureStart(a: Int, b: Int)
        fun gestureMove(a: Int, b: Int)
        fun gestureEnd(a: Int, b: Int)
    }

    enum class Mode { IDLE, STROKE, PAN, WAIT, GESTURE, IGNORE }

    var fingerMode = FingerMode.DRAW
    /** The Move tool is selected: every single pointer pans. */
    var moveTool = false
    /** The Eraser tool is selected: strokes erase. */
    var eraserTool = false

    var mode = Mode.IDLE
        private set
    var strokePointer = -1
        private set
    var strokeKind = ToolKind.UNKNOWN
        private set
    private var panPointer = -1
    private var gestureA = -1
    private var gestureB = -1
    private val fingers = ArrayList<Int>(4)   // finger ids down, in order
    private val rejected = HashSet<Int>()     // pointers ignored until lifted

    val strokeActive: Boolean get() = mode == Mode.STROKE

    fun down(id: Int, kind: ToolKind, timeMs: Long, x: Float, y: Float) {
        reset()
        startSingle(id, kind, timeMs, x, y)
    }

    fun pointerDown(id: Int, kind: ToolKind, timeMs: Long, x: Float, y: Float) {
        val stylusLike = kind.isStylus || kind == ToolKind.MOUSE
        if (!stylusLike) fingers += id
        when (mode) {
            Mode.STROKE -> when {
                strokeKind.isStylus || strokeKind == ToolKind.MOUSE -> rejected += id   // palm rejection
                stylusLike -> {
                    // The finger "stroke" was a palm resting before the stylus.
                    actions.cancelStroke()
                    gate.end()
                    beginStroke(id, kind, timeMs, x, y)
                }
                else -> {
                    if (gate.onSecondPointer(timeMs) == StrokeGate.Decision.CANCEL) actions.cancelStroke()
                    else actions.endStroke(strokePointer)
                    strokePointer = -1
                    startGestureFromFingers()
                }
            }
            Mode.PAN -> when {
                stylusLike && !moveTool -> {
                    panPointer = -1
                    beginStroke(id, kind, timeMs, x, y)
                }
                !stylusLike && fingers.size >= 2 -> {
                    panPointer = -1
                    startGestureFromFingers()
                }
                else -> rejected += id
            }
            Mode.WAIT, Mode.IGNORE, Mode.IDLE -> when {
                stylusLike -> startSingle(id, kind, timeMs, x, y)
                fingers.size >= 2 -> startGestureFromFingers()
                mode == Mode.IDLE -> startSingle(id, kind, timeMs, x, y)
            }
            Mode.GESTURE -> if (stylusLike) rejected += id  // fingers beyond two are simply not used
        }
    }

    /** ACTION_MOVE. */
    fun move() {
        when (mode) {
            Mode.STROKE -> actions.strokeSamples(strokePointer)
            Mode.PAN -> actions.panMove(panPointer)
            Mode.GESTURE -> actions.gestureMove(gestureA, gestureB)
            else -> Unit
        }
    }

    fun pointerUp(id: Int, canceled: Boolean) {
        lifted(id, canceled)
        fingers.remove(id)
        rejected.remove(id)
    }

    /** ACTION_UP: the last pointer left. */
    fun up(id: Int, canceled: Boolean) {
        lifted(id, canceled)
        reset()
    }

    /** ACTION_CANCEL: the system took the touch away (e.g. a dialog, a palm). */
    fun cancel() {
        when (mode) {
            Mode.STROKE -> actions.cancelStroke()
            Mode.GESTURE -> actions.gestureEnd(gestureA, gestureB)
            else -> Unit
        }
        reset()
    }

    /** The screen goes away mid-stroke (pause): keep what was drawn. */
    fun interrupt() {
        if (mode == Mode.STROKE) actions.endStroke(strokePointer)
        if (mode == Mode.GESTURE) actions.gestureEnd(gestureA, gestureB)
        reset()
        mode = Mode.IGNORE
    }

    // ------------------------------------------------------------------
    private fun lifted(id: Int, canceled: Boolean) {
        when (mode) {
            Mode.STROKE -> if (id == strokePointer) {
                if (canceled) actions.cancelStroke() else actions.endStroke(id)
                gate.end()
                strokePointer = -1
                mode = Mode.IGNORE
            }
            Mode.PAN -> if (id == panPointer) {
                panPointer = -1
                mode = Mode.IGNORE
            }
            Mode.GESTURE -> if (id == gestureA || id == gestureB) {
                actions.gestureEnd(gestureA, gestureB)
                val remaining = fingers.filter { it != id && it !in rejected }
                if (remaining.size >= 2) {
                    gestureA = remaining[0]
                    gestureB = remaining[1]
                    actions.gestureStart(gestureA, gestureB)
                } else {
                    mode = Mode.IGNORE
                }
            }
            else -> Unit
        }
    }

    private fun startSingle(id: Int, kind: ToolKind, timeMs: Long, x: Float, y: Float) {
        val stylusLike = kind.isStylus || kind == ToolKind.MOUSE
        if (!stylusLike && id !in fingers) fingers += id
        when {
            moveTool -> {
                panPointer = id
                mode = Mode.PAN
                actions.panStart(id)
            }
            stylusLike -> beginStroke(id, kind, timeMs, x, y)
            fingerMode == FingerMode.DRAW -> beginStroke(id, kind, timeMs, x, y)
            fingerMode == FingerMode.PAN -> {
                panPointer = id
                mode = Mode.PAN
                actions.panStart(id)
            }
            else -> mode = Mode.WAIT
        }
    }

    private fun beginStroke(id: Int, kind: ToolKind, timeMs: Long, x: Float, y: Float) {
        strokePointer = id
        strokeKind = kind
        mode = Mode.STROKE
        gate.down(timeMs, P(x.toDouble(), y.toDouble()))
        actions.beginStroke(id, kind, eraser = kind == ToolKind.ERASER || eraserTool)
    }

    private fun startGestureFromFingers() {
        val usable = fingers.filter { it !in rejected }
        if (usable.size < 2) {
            mode = Mode.IGNORE
            return
        }
        gestureA = usable[usable.size - 2]
        gestureB = usable[usable.size - 1]
        mode = Mode.GESTURE
        actions.gestureStart(gestureA, gestureB)
    }

    /** Called by the workspace with the stroke pointer's position (slop tracking). */
    fun strokeMovedTo(x: Float, y: Float) = gate.move(P(x.toDouble(), y.toDouble()))

    private fun reset() {
        mode = Mode.IDLE
        strokePointer = -1
        strokeKind = ToolKind.UNKNOWN
        panPointer = -1
        gestureA = -1
        gestureB = -1
        fingers.clear()
        rejected.clear()
        gate.end()
    }
}
