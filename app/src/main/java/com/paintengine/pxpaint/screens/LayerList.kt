package com.paintengine.pxpaint.screens

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.View
import android.view.ViewConfiguration
import android.widget.LinearLayout
import android.widget.ScrollView
import com.paintengine.pxpaint.core.LayerDrag
import com.paintengine.pxpaint.ui.PxTheme

/**
 * The layer rows (top layer first), reorderable by pressing on a row
 * (Block 11):
 *
 *  * tap: the row's own click (select; options when already selected);
 *  * press and hold ~0.3 s without moving: the row lifts (haptic tick) and
 *    follows the finger; the other rows open a gap where it would land and an
 *    outlined slot shows it; near the panel's edges the list scrolls by itself;
 *  * release: [onDrop] (layer id, new list position), or back in place;
 *  * moving before the hold is a normal scroll, so scrolling stays natural;
 *    a press that starts on a view tagged [NO_DRAG] (visibility, options)
 *    never drags.
 *
 * Pure arithmetic in core/LayerDrag. Rows are laid out by LinearLayout;
 * dragging only changes translations (no layout pass per move).
 */
@SuppressLint("ViewConstructor")
class LayerList(context: Context, private val theme: PxTheme, private val spacing: Int) : LinearLayout(context) {
    /** (layer id, new list position, the dragged row's visual top) on release when the position changed. */
    var onDrop: ((id: Int, toPosition: Int, visualTop: Int) -> Unit)? = null
    var onDragStateChanged: ((Boolean) -> Unit)? = null

    private val slop = ViewConfiguration.get(context).scaledTouchSlop
    private var armed = -1           // row index pressed, waiting for the hold
    private var dragging = -1        // row index being dragged
    private var target = -1
    private var downX = 0f
    private var downY = 0f
    private var lastY = 0f
    private var lastRawY = 0f
    private val loc = IntArray(2)
    private var tops = IntArray(0)
    private var heights = IntArray(0)
    private val slotPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = theme.dpF(2f)
        color = theme.accent
        pathEffect = android.graphics.DashPathEffect(floatArrayOf(theme.dpF(6f), theme.dpF(4f)), 0f)
    }
    private val slotFill = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = theme.accentSoft }
    private val slot = RectF()

    private val startDrag = Runnable { beginDrag() }
    private val autoScrollStep = object : Runnable {
        override fun run() {
            if (dragging < 0) return
            val sv = scroller() ?: return
            sv.getLocationOnScreen(loc)
            val speed = LayerDrag.autoScroll(lastRawY, loc[1].toFloat(), (loc[1] + sv.height).toFloat(), theme.dpF(56f), theme.dpF(14f))
            if (speed != 0f) {
                val before = sv.scrollY
                sv.scrollBy(0, speed.toInt())
                if (sv.scrollY != before) {
                    // The list moved under a still finger: the row stays under it.
                    getLocationOnScreen(loc)
                    lastY = lastRawY - loc[1]
                    follow()
                }
            }
            postOnAnimation(this)
        }
    }

    init {
        orientation = VERTICAL
        setWillNotDraw(false)
    }

    private fun scroller(): ScrollView? {
        var p = parent
        while (p != null) {
            if (p is ScrollView) return p
            p = p.parent
        }
        return null
    }

    private fun rowAt(y: Float): Int {
        for (i in 0 until childCount) {
            val c = getChildAt(i)
            if (y >= c.top && y < c.bottom) return i
        }
        return -1
    }

    /** True if the press starts on a control of the row that must never drag (eye, options). */
    private fun onNoDrag(row: View, x: Float, y: Float): Boolean {
        if (row !is android.view.ViewGroup) return false
        val rx = x - row.left
        val ry = y - row.top
        fun hit(g: android.view.ViewGroup, ox: Float, oy: Float): Boolean {
            for (i in 0 until g.childCount) {
                val c = g.getChildAt(i)
                if (ox < c.left || ox >= c.right || oy < c.top || oy >= c.bottom) continue
                if (c.tag == NO_DRAG) return true
                if (c is android.view.ViewGroup && hit(c, ox - c.left, oy - c.top)) return true
            }
            return false
        }
        return hit(row, rx, ry)
    }

    override fun onInterceptTouchEvent(e: MotionEvent): Boolean {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                cancelArm()
                val i = rowAt(e.y)
                if (i >= 0 && childCount > 1 && !onNoDrag(getChildAt(i), e.x, e.y)) {
                    armed = i
                    downX = e.x
                    downY = e.y
                    lastRawY = e.rawY
                    postDelayed(startDrag, LayerDrag.LONG_PRESS_MS)
                }
            }
            MotionEvent.ACTION_MOVE -> {
                if (dragging >= 0) {
                    // The event that starts intercepting is not delivered to onTouchEvent: handle it here.
                    lastY = e.y
                    lastRawY = e.rawY
                    follow()
                    return true
                }
                if (armed >= 0 && (Math.abs(e.x - downX) > slop || Math.abs(e.y - downY) > slop)) cancelArm()   // a scroll
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                // A drag that started but did not move yet: the release is ours (no click on the row).
                if (dragging >= 0) {
                    finishDrag(drop = e.actionMasked == MotionEvent.ACTION_UP)
                    return true
                }
                cancelArm()
            }
        }
        return dragging >= 0
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        if (dragging < 0) return super.onTouchEvent(e)
        when (e.actionMasked) {
            MotionEvent.ACTION_MOVE -> {
                lastY = e.y
                lastRawY = e.rawY
                follow()
            }
            MotionEvent.ACTION_UP -> finishDrag(drop = true)
            MotionEvent.ACTION_CANCEL -> finishDrag(drop = false)
        }
        return true
    }

    private fun cancelArm() {
        removeCallbacks(startDrag)
        armed = -1
    }

    private fun beginDrag() {
        val i = armed
        armed = -1
        if (i < 0 || i >= childCount) return
        dragging = i
        target = i
        lastY = downY
        tops = IntArray(childCount) { getChildAt(it).top }
        heights = IntArray(childCount) { getChildAt(it).height }
        val row = getChildAt(i)
        row.isPressed = false
        row.elevation = theme.dpF(10f)
        row.animate().scaleX(1.03f).scaleY(1.03f).setDuration(theme.motion.d(com.paintengine.pxpaint.core.MotionTokens.MICRO)).start()
        row.translationZ = theme.dpF(4f)   // drawn over its neighbours (children are drawn by Z; no reordering, no layout)
        performHapticFeedback(HapticFeedbackConstants.LONG_PRESS)
        parent?.requestDisallowInterceptTouchEvent(true)
        onDragStateChanged?.invoke(true)
        postOnAnimation(autoScrollStep)
        invalidate()
    }

    /** The dragged row under the finger, the others making room, the landing slot. */
    private fun follow() {
        val i = dragging
        if (i < 0) return
        val row = rowView(i) ?: return
        val dy = lastY - downY
        row.translationY = dy
        val center = tops[i] + heights[i] / 2f + dy
        val to = LayerDrag.target(tops, heights, i, center)
        if (to != target) {
            target = to
            val step = heights[i] + spacing
            for (k in tops.indices) {
                if (k == i) continue
                val v = rowView(k) ?: continue
                v.animate().translationY(LayerDrag.shift(k, i, to, step).toFloat())
                    .setDuration(theme.motion.d(com.paintengine.pxpaint.core.MotionTokens.SHORT)).start()
            }
            performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK)
        }
        invalidate()
    }

    private fun rowView(i: Int): View? = if (i in 0 until childCount) getChildAt(i) else null

    private fun finishDrag(drop: Boolean) {
        val i = dragging
        removeCallbacks(autoScrollStep)
        dragging = -1
        onDragStateChanged?.invoke(false)
        val row = rowView(i)
        val to = target
        val moved = drop && i >= 0 && to >= 0 && to != i
        if (moved && row != null) {
            val id = row.tag as? Int
            if (id != null) {
                onDrop?.invoke(id, to, (tops[i] + row.translationY).toInt())
                invalidate()
                return
            }
        }
        // Back in place.
        for (k in 0 until childCount) {
            val v = getChildAt(k)
            v.animate().translationY(0f).scaleX(1f).scaleY(1f)
                .setDuration(theme.motion.d(com.paintengine.pxpaint.core.MotionTokens.SHORT)).start()
            v.elevation = 0f
            v.translationZ = 0f
        }
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val i = dragging
        if (i < 0 || tops.isEmpty()) return
        val top = LayerDrag.landingTop(tops, heights, i, target).toFloat()
        val r = theme.radiusS
        slot.set(slotPaint.strokeWidth, top + slotPaint.strokeWidth, width - slotPaint.strokeWidth, top + heights[i] - slotPaint.strokeWidth)
        canvas.drawRoundRect(slot, r, r, slotFill)
        canvas.drawRoundRect(slot, r, r, slotPaint)
    }

    companion object {
        /** Tag of row controls that must never start a drag (visibility, options). */
        const val NO_DRAG = "layer-no-drag"
    }
}
