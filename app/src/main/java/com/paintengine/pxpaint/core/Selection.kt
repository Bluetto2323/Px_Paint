package com.paintengine.pxpaint.core

import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.hypot
import kotlin.math.roundToInt
import kotlin.math.sin

/**
 * Selection tool logic (Block 10), pure Kotlin and unit-tested: the gesture
 * state machine that turns touches into shapes, the screen <-> canvas
 * transform used for previews and moves, the outline format of the engine,
 * and which contextual actions exist in each state. The engine does the
 * selecting (native SelectionMask); the workspace wires this to the engine
 * and to ui/SelectionOverlay.
 */
enum class SelectionShape(val label: S) {
    RECTANGLE(S.SEL_RECTANGLE), ELLIPSE(S.SEL_ELLIPSE), LASSO(S.SEL_LASSO), POLYGON(S.SEL_POLYGON);

    companion object {
        fun of(name: String?): SelectionShape = entries.firstOrNull { it.name == name } ?: RECTANGLE
    }
}

/** How a new shape combines with the selection (engine codes 0..3). */
enum class SelectionMode(val code: Int, val label: S) {
    REPLACE(0, S.SEL_MODE_REPLACE), ADD(1, S.SEL_MODE_ADD), SUBTRACT(2, S.SEL_MODE_SUBTRACT), INTERSECT(3, S.SEL_MODE_INTERSECT);

    companion object {
        fun of(name: String?): SelectionMode = entries.firstOrNull { it.name == name } ?: REPLACE
    }
}

/** 2D affine transform, engine layout: x' = a x + c y + tx, y' = b x + d y + ty. Mutable, allocation-free. */
class Affine {
    var a = 1.0; var b = 0.0; var c = 0.0; var d = 1.0; var tx = 0.0; var ty = 0.0

    fun set(m: FloatArray) {
        a = m[0].toDouble(); b = m[1].toDouble(); c = m[2].toDouble(); d = m[3].toDouble(); tx = m[4].toDouble(); ty = m[5].toDouble()
    }

    fun set(o: Affine) { a = o.a; b = o.b; c = o.c; d = o.d; tx = o.tx; ty = o.ty }
    fun x(px: Double, py: Double) = a * px + c * py + tx
    fun y(px: Double, py: Double) = b * px + d * py + ty
    /** Uniform scale factor (canvas px -> screen px). */
    val scale: Double get() = hypot(a, b)
    val determinant: Double get() = a * d - b * c

    /** [out] = inverse of this; false if not invertible. */
    fun invertInto(out: Affine): Boolean {
        val det = determinant
        if (det == 0.0 || det.isNaN()) return false
        val ia = d / det; val ib = -b / det; val ic = -c / det; val id = a / det
        out.a = ia; out.b = ib; out.c = ic; out.d = id
        out.tx = -(ia * tx + ic * ty); out.ty = -(ib * tx + id * ty)
        return true
    }

    /** Linear part only (an offset, not a point). */
    fun dx(vx: Double, vy: Double) = a * vx + c * vy
    fun dy(vx: Double, vy: Double) = b * vx + d * vy
}

/** Growable (x, y) float buffer; grows by doubling, so a lasso does not allocate per sample. */
class PointBuffer(initialPoints: Int = 4096) {
    var data = FloatArray(initialPoints * 2); private set
    var count = 0; private set

    fun clear() { count = 0 }
    fun add(x: Float, y: Float) {
        if (count * 2 + 2 > data.size) data = data.copyOf(data.size * 2)
        data[count * 2] = x
        data[count * 2 + 1] = y
        count++
    }
    fun set(i: Int, x: Float, y: Float) { data[i * 2] = x; data[i * 2 + 1] = y }
    fun x(i: Int) = data[i * 2]
    fun y(i: Int) = data[i * 2 + 1]
    fun removeLast() { if (count > 0) count-- }

    /** Absolute area of the closed polygon (shoelace), in squared units. */
    fun area(): Double {
        var s = 0.0
        for (i in 0 until count) {
            val j = (i + 1) % count
            s += x(i).toDouble() * y(j) - x(j).toDouble() * y(i)
        }
        return abs(s) / 2
    }
}

/**
 * Touches -> selection shapes, in screen pixels.
 *
 *  * Rectangle / ellipse: drag from one corner to the other.
 *  * Lasso: drag around the area (points closer than [minStepPx] are skipped).
 *  * Polygon: each tap (or press-drag-lift) places a vertex, with a rubber
 *    band to the pointer; tapping the first vertex or [closePolygon] closes it.
 *  * A tap without a drag (rectangle, ellipse, lasso) = [Outcome.TAP]
 *    (the workspace deselects, like a click outside in desktop apps).
 */
class SelectionGesture(private val tapSlopPx: Float, private val closeRadiusPx: Float, private val minStepPx: Float = 1.5f) {
    enum class Phase { IDLE, DRAG, POLYGON }
    enum class Outcome { NONE, SELECT, TAP }

    var shape = SelectionShape.RECTANGLE
    var phase = Phase.IDLE; private set
    /** Rectangle / ellipse: 2 points (start, end); lasso / polygon: the vertices. */
    val points = PointBuffer()
    /** Polygon: the pointer position while it is down (rubber band); NaN when up. */
    var cursorX = Float.NaN; private set
    var cursorY = Float.NaN; private set
    private var downX = 0f
    private var downY = 0f
    private var travelled = 0f

    val busy: Boolean get() = phase != Phase.IDLE

    fun down(x: Float, y: Float) {
        downX = x; downY = y; travelled = 0f
        when {
            shape == SelectionShape.POLYGON && phase == Phase.POLYGON -> { cursorX = x; cursorY = y }
            shape == SelectionShape.POLYGON -> {
                phase = Phase.POLYGON
                points.clear()
                points.add(x, y)
                cursorX = x; cursorY = y
            }
            else -> {
                phase = Phase.DRAG
                points.clear()
                points.add(x, y)
                if (shape != SelectionShape.LASSO) points.add(x, y)
            }
        }
    }

    fun move(x: Float, y: Float) {
        travelled = maxOf(travelled, hypot(x - downX, y - downY))
        when (phase) {
            Phase.DRAG -> if (shape == SelectionShape.LASSO) {
                val n = points.count
                if (hypot(x - points.x(n - 1), y - points.y(n - 1)) >= minStepPx) points.add(x, y)
            } else {
                points.set(1, x, y)
            }
            Phase.POLYGON -> { cursorX = x; cursorY = y }
            Phase.IDLE -> Unit
        }
    }

    fun up(x: Float, y: Float): Outcome {
        move(x, y)
        return when (phase) {
            Phase.DRAG -> {
                phase = Phase.IDLE
                val tap = travelled < tapSlopPx
                when {
                    tap -> Outcome.TAP
                    shape == SelectionShape.LASSO && (points.count < 3 || points.area() < 4.0) -> Outcome.TAP
                    shape != SelectionShape.LASSO && (abs(points.x(1) - points.x(0)) < 1f || abs(points.y(1) - points.y(0)) < 1f) -> Outcome.TAP
                    else -> Outcome.SELECT
                }
            }
            Phase.POLYGON -> {
                cursorX = Float.NaN; cursorY = Float.NaN
                val n = points.count
                if (n >= 3 && nearFirst(x, y)) {
                    phase = Phase.IDLE
                    Outcome.SELECT
                } else {
                    if (hypot(x - points.x(n - 1), y - points.y(n - 1)) >= tapSlopPx / 2) points.add(x, y)
                    Outcome.NONE
                }
            }
            Phase.IDLE -> Outcome.NONE
        }
    }

    /** Polygon: closes it if it has 3 vertices (true = select it). */
    fun closePolygon(): Boolean {
        if (phase != Phase.POLYGON || points.count < 3) return false
        phase = Phase.IDLE
        cursorX = Float.NaN; cursorY = Float.NaN
        return true
    }

    /** Polygon: removes the last vertex (the first one cancels). */
    fun undoVertex() {
        if (phase != Phase.POLYGON) return
        points.removeLast()
        if (points.count == 0) cancel()
    }

    fun cancel() {
        phase = Phase.IDLE
        points.clear()
        cursorX = Float.NaN; cursorY = Float.NaN
    }

    /** The pointer is over the first polygon vertex (closing target). */
    fun nearFirst(x: Float, y: Float): Boolean = points.count > 0 && hypot(x - points.x(0), y - points.y(0)) <= closeRadiusPx
}

/** Geometry shared by the preview (overlay) and the engine call, so the preview is what gets selected. */
object SelectionGeometry {
    /**
     * Canvas-space bounds of a rectangle / ellipse dragged between two screen
     * points: both mapped to the canvas, axis-aligned there (a rotated view
     * shows it rotated, as the engine selects it); a rectangle snaps to whole
     * pixels like the engine. [out] = left, top, right, bottom.
     */
    fun canvasBox(toCanvas: Affine, sx0: Float, sy0: Float, sx1: Float, sy1: Float, snap: Boolean, out: DoubleArray) {
        val x0 = toCanvas.x(sx0.toDouble(), sy0.toDouble()); val y0 = toCanvas.y(sx0.toDouble(), sy0.toDouble())
        val x1 = toCanvas.x(sx1.toDouble(), sy1.toDouble()); val y1 = toCanvas.y(sx1.toDouble(), sy1.toDouble())
        var l = minOf(x0, x1); var t = minOf(y0, y1); var r = maxOf(x0, x1); var b = maxOf(y0, y1)
        if (snap) { l = Math.rint(l); t = Math.rint(t); r = Math.rint(r); b = Math.rint(b) }
        out[0] = l; out[1] = t; out[2] = r; out[3] = b
    }

    /** Screen points of a canvas box's outline: 4 corners, or [segments] points of the inscribed ellipse. */
    fun outline(toScreen: Affine, box: DoubleArray, ellipse: Boolean, segments: Int, out: FloatArray): Int {
        if (!ellipse) {
            for (i in 0 until 4) {
                val px = if (i == 1 || i == 2) box[2] else box[0]
                val py = if (i >= 2) box[3] else box[1]
                out[i * 2] = toScreen.x(px, py).toFloat()
                out[i * 2 + 1] = toScreen.y(px, py).toFloat()
            }
            return 4
        }
        val cx = (box[0] + box[2]) / 2; val cy = (box[1] + box[3]) / 2
        val rx = (box[2] - box[0]) / 2; val ry = (box[3] - box[1]) / 2
        for (i in 0 until segments) {
            val t = 2 * Math.PI * i / segments
            val px = cx + rx * cos(t); val py = cy + ry * sin(t)
            out[i * 2] = toScreen.x(px, py).toFloat()
            out[i * 2 + 1] = toScreen.y(px, py).toFloat()
        }
        return segments
    }

    /** Whole canvas pixels of a screen drag (the move offset), through the inverse view, into [out] (x, y). */
    fun canvasOffset(toCanvas: Affine, sdx: Double, sdy: Double, out: IntArray) {
        out[0] = toCanvas.dx(sdx, sdy).roundToInt()
        out[1] = toCanvas.dy(sdx, sdy).roundToInt()
    }
}

/** The engine's outline format: [loops, n0, x, y, ... (n0 points), n1, ...]. */
object SelectionOutline {
    /** Loop count, or -1 if malformed. */
    fun loops(v: FloatArray?): Int {
        if (v == null || v.isEmpty()) return -1
        val loops = v[0].toInt()
        var i = 1
        repeat(loops) {
            if (i >= v.size) return -1
            val n = v[i].toInt()
            if (n < 0 || i + 1 + n * 2 > v.size) return -1
            i += 1 + n * 2
        }
        return if (i == v.size) loops else -1
    }

    /** Total vertices of all loops. */
    fun points(v: FloatArray): Int {
        var i = 1
        var total = 0
        repeat(v[0].toInt()) {
            val n = v[i].toInt()
            total += n
            i += 1 + n * 2
        }
        return total
    }

    /** Calls [loop] for each loop with (start index of its first x, point count). */
    inline fun forEachLoop(v: FloatArray, loop: (start: Int, n: Int) -> Unit) {
        var i = 1
        repeat(v[0].toInt()) {
            val n = v[i].toInt()
            loop(i + 1, n)
            i += 1 + n * 2
        }
    }
}

/** Contextual selection actions (no control is shown that cannot act). */
enum class SelectionAction(val label: S) {
    MOVE(S.SEL_ACT_MOVE), COPY(S.SEL_ACT_COPY), COPY_MERGED(S.SEL_ACT_COPY_MERGED), CUT(S.SEL_ACT_CUT), PASTE(S.SEL_ACT_PASTE),
    DUPLICATE(S.SEL_ACT_DUPLICATE), CLEAR(S.SEL_ACT_CLEAR), INVERT(S.SEL_ACT_INVERT), DESELECT(S.SEL_ACT_DESELECT),
    SELECT_ALL(S.SEL_ACT_SELECT_ALL), FROM_CONTENT(S.SEL_ACT_FROM_CONTENT),
    MOVE_DONE(S.SEL_ACT_MOVE_DONE), MOVE_CANCEL(S.SEL_ACT_MOVE_CANCEL),
    FLIP_H(S.SEL_ACT_FLIP_H), FLIP_V(S.SEL_ACT_FLIP_V), SNAP(S.SEL_ACT_SNAP), KEEP_RATIO(S.SEL_ACT_KEEP_RATIO), SKEW(S.SEL_ACT_SKEW),
    POLYGON_CLOSE(S.SEL_ACT_POLYGON_CLOSE),
    POLYGON_UNDO(S.SEL_ACT_POLYGON_UNDO), POLYGON_CANCEL(S.SEL_ACT_POLYGON_CANCEL);

    /** Shown in the contextual bar itself; the others go to its overflow menu (minimal bar on phones). */
    val primary: Boolean get() = this !in OVERFLOW

    companion object {
        private val OVERFLOW = setOf(COPY_MERGED, CUT, DUPLICATE, INVERT, SNAP, KEEP_RATIO, SKEW)

        /** Splits [actions] into the bar's chips and its overflow menu. */
        fun split(actions: List<SelectionAction>): Pair<List<SelectionAction>, List<SelectionAction>> =
            actions.partition { it.primary }

        /**
         * Actions available now. [editable]: the active layer is visible and
         * unlocked (pixel edits); [polygonPoints]: vertices of a polygon being drawn.
         */
        fun available(active: Boolean, clipboard: Boolean, moving: Boolean, editable: Boolean, polygonPoints: Int): List<SelectionAction> = when {
            moving -> listOf(MOVE_DONE, MOVE_CANCEL, FLIP_H, FLIP_V, SNAP, KEEP_RATIO, SKEW)
            polygonPoints > 0 -> buildList {
                if (polygonPoints >= 3) add(POLYGON_CLOSE)
                add(POLYGON_UNDO)
                add(POLYGON_CANCEL)
            }
            active -> buildList {
                if (editable) add(MOVE)
                add(COPY)
                add(COPY_MERGED)
                if (editable) add(CUT)
                if (clipboard) add(PASTE)
                add(DUPLICATE)
                if (editable) add(CLEAR)
                add(INVERT)
                add(DESELECT)
            }
            else -> buildList {
                if (clipboard) add(PASTE)
                add(SELECT_ALL)
                add(FROM_CONTENT)
            }
        }
    }
}

/** Engine error codes of selection operations -> what the user is told (null = nothing to say). */
object SelectionErrors {
    fun message(op: SelectionAction, error: Int): S? = when (error) {
        0 -> null
        5 -> S.SEL_ERR_LOCKED
        6 -> S.SEL_ERR_HIDDEN
        7 -> S.SEL_ERR_TOO_MANY
        8 -> S.SEL_ERR_MEMORY
        else -> when (op) {
            SelectionAction.COPY, SelectionAction.COPY_MERGED, SelectionAction.CUT, SelectionAction.DUPLICATE,
            SelectionAction.CLEAR, SelectionAction.MOVE -> S.SEL_ERR_NOTHING
            SelectionAction.FROM_CONTENT -> S.SEL_ERR_NO_CONTENT
            else -> S.SEL_ERR_GENERIC
        }
    }
}
