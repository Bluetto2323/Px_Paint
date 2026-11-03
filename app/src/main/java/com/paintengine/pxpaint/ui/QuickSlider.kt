package com.paintengine.pxpaint.ui

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.os.Bundle
import android.view.MotionEvent
import android.view.View
import android.view.accessibility.AccessibilityNodeInfo
import com.paintengine.pxpaint.core.DesignTokens
import kotlin.math.abs
import kotlin.math.roundToInt

/**
 * Vertical on-canvas slider for brush size / opacity (0..1, bottom = 0).
 *
 * Precise on a touchscreen: the value moves *relative* to the drag (a touch
 * does not jump it), at full track speed, and slows to a quarter when the
 * finger moves sideways away from the track (fine adjustment). A bubble with
 * the value and a size / opacity preview is drawn beside the track while
 * dragging ([onPreview]). Accessibility: a range with 1 % steps
 * (scroll forward / backward, set progress).
 */
@SuppressLint("ViewConstructor")
class QuickSlider(
    context: Context,
    private val theme: PxTheme,
    description: String,
    /** Fill style: size shows a growing wedge, opacity a solid fill. */
    private val wedge: Boolean,
) : View(context) {
    var value = 0.5f
        private set
    /** value, fromUser, final (finger lifted). */
    var onChange: ((Float, Boolean, Boolean) -> Unit)? = null
    var onPreview: ((Float?) -> Unit)? = null   // dragging value, null when released
    var valueText: (Float) -> String = { "${(it * 100).roundToInt()}%" }

    private val trackPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = theme.chrome }
    private val outlinePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; color = theme.outline; strokeWidth = theme.dpF(1f)
    }
    private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = theme.textSecondary }
    private val thumbPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = theme.text }
    private val rect = RectF()
    private val path = android.graphics.Path()
    private var lastY = 0f
    private var downX = 0f
    private var dragging = false

    init {
        contentDescription = description
        isFocusable = true
        isClickable = true
        if (android.os.Build.VERSION.SDK_INT >= 30) stateDescription = valueText(value)
    }

    fun setValue(v: Float, fromUser: Boolean = false, final: Boolean = true) {
        val c = v.coerceIn(0f, 1f)
        if (c == value) return
        value = c
        if (android.os.Build.VERSION.SDK_INT >= 30) stateDescription = valueText(c)
        invalidate()
        onChange?.invoke(c, fromUser, final)
    }

    override fun onDraw(canvas: Canvas) {
        // The view is a full touch target wide; the visible track is narrower, centred.
        val tw = minOf(width.toFloat(), theme.dpF(DesignTokens.SLIDER_WIDTH.toFloat()))
        canvas.save()
        canvas.translate((width - tw) / 2f, 0f)
        val w = tw
        val h = height.toFloat()
        val pad = theme.dpF(6f)
        val r = w / 2f
        rect.set(0f, 0f, w, h)
        canvas.drawRoundRect(rect, r, r, trackPaint)
        canvas.drawRoundRect(rect, r, r, outlinePaint)
        val top = pad + theme.dpF(4f)
        val bottom = h - pad - theme.dpF(4f)
        val y = bottom - (bottom - top) * value
        val cx = w / 2f
        fillPaint.alpha = if (dragging) 230 else 150
        if (wedge) {
            // Thin at the bottom, wide at the top: reads as "size".
            val maxHalf = w / 2f - pad
            path.reset()
            path.moveTo(cx, bottom)
            path.lineTo(cx + maxHalf * (0.12f + 0.88f * value) * 0.9f, y)
            path.lineTo(cx - maxHalf * (0.12f + 0.88f * value) * 0.9f, y)
            path.close()
            canvas.drawPath(path, fillPaint)
        } else {
            rect.set(pad, y, w - pad, bottom)
            canvas.drawRoundRect(rect, theme.dpF(6f), theme.dpF(6f), fillPaint)
        }
        // Thumb: a horizontal bar the full width of the track.
        rect.set(pad - theme.dpF(1f), y - theme.dpF(3f), w - pad + theme.dpF(1f), y + theme.dpF(3f))
        canvas.drawRoundRect(rect, theme.dpF(3f), theme.dpF(3f), thumbPaint)
        canvas.restore()
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        if (!isEnabled) return false
        val travel = (height - theme.dpF(20f)).coerceAtLeast(1f)
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                parent?.requestDisallowInterceptTouchEvent(true)
                lastY = e.y
                downX = e.x
                dragging = true
                onPreview?.invoke(value)
                invalidate()
            }
            MotionEvent.ACTION_MOVE -> {
                // Moving sideways away from the track: fine adjustment (x 0.25).
                val fine = abs(e.x - downX) > theme.dpF(48f)
                val dv = (lastY - e.y) / travel * (if (fine) 0.25f else 1f)
                lastY = e.y
                setValue(value + dv, fromUser = true, final = false)
                onPreview?.invoke(value)
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                dragging = false
                onPreview?.invoke(null)
                onChange?.invoke(value, true, true)
                invalidate()
            }
        }
        return true
    }

    override fun onInitializeAccessibilityNodeInfo(info: AccessibilityNodeInfo) {
        super.onInitializeAccessibilityNodeInfo(info)
        info.className = "android.widget.SeekBar"
        info.rangeInfo = AccessibilityNodeInfo.RangeInfo.obtain(AccessibilityNodeInfo.RangeInfo.RANGE_TYPE_PERCENT, 0f, 100f, value * 100f)
        info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_FORWARD)
        info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_BACKWARD)
        if (android.os.Build.VERSION.SDK_INT >= 24) info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SET_PROGRESS)
    }

    override fun performAccessibilityAction(action: Int, arguments: Bundle?): Boolean {
        when (action) {
            AccessibilityNodeInfo.ACTION_SCROLL_FORWARD -> setValue(value + 0.01f, fromUser = true)
            AccessibilityNodeInfo.ACTION_SCROLL_BACKWARD -> setValue(value - 0.01f, fromUser = true)
            android.R.id.accessibilityActionSetProgress -> {
                val p = arguments?.getFloat(AccessibilityNodeInfo.ACTION_ARGUMENT_PROGRESS_VALUE) ?: return false
                setValue(p / 100f, fromUser = true)
            }
            else -> return super.performAccessibilityAction(action, arguments)
        }
        return true
    }

    companion object {
        val LENGTH_DP = DesignTokens.SLIDER_LENGTH
        val WIDTH_DP = DesignTokens.SLIDER_WIDTH
    }
}

/**
 * Value bubble beside the quick sliders while one is dragged: the brush tip
 * at its real on-screen size (canvas size x zoom, up to 120 dp) or the colour
 * at the chosen opacity, with the number.
 */
@SuppressLint("ViewConstructor")
class SliderBubble(context: Context, private val theme: PxTheme) : View(context) {
    private var size: Float? = null
    private var opacity: Float? = null
    private var color = 0
    private var zoom = 1.0
    private val bg = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = theme.chrome }
    private val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; color = theme.outline; strokeWidth = theme.dpF(1f) }
    private val dot = Paint(Paint.ANTI_ALIAS_FLAG)
    private val label = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = theme.text
        textAlign = Paint.Align.CENTER
        textSize = theme.dpF(13f)
        typeface = android.graphics.Typeface.create("sans-serif-medium", android.graphics.Typeface.NORMAL)
    }
    private val rect = RectF()
    private val checker = CheckerDrawable(theme.dp(6), theme.dark)

    init {
        visibility = GONE
        importantForAccessibility = IMPORTANT_FOR_ACCESSIBILITY_NO   // the slider announces its value
    }

    fun show(size: Float?, opacity: Float?, color: Int, zoom: Double) {
        if (size == null && opacity == null) {
            visibility = GONE
            return
        }
        this.size = size
        this.opacity = opacity
        this.color = color
        this.zoom = if (zoom > 0) zoom else 1.0
        visibility = VISIBLE
        invalidate()
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val s = theme.dp(132)
        setMeasuredDimension(s, s + theme.dp(24))
    }

    override fun onDraw(canvas: Canvas) {
        val w = width.toFloat()
        val box = w
        rect.set(0f, 0f, w, height.toFloat())
        canvas.drawRoundRect(rect, theme.radiusM, theme.radiusM, bg)
        canvas.drawRoundRect(rect, theme.radiusM, theme.radiusM, stroke)
        val cx = w / 2f
        val cy = box / 2f
        val text: String
        val sz = size
        if (sz != null) {
            val r = (sz * zoom / 2.0).toFloat().coerceIn(theme.dpF(1f), theme.dpF(60f))
            dot.color = color or 0xFF000000.toInt()
            canvas.drawCircle(cx, cy, r, dot)
            text = com.paintengine.pxpaint.core.BrushState.sizeLabel(sz)
        } else {
            val o = opacity ?: 1f
            val c = checker
            rect.set(cx - theme.dpF(36f), cy - theme.dpF(36f), cx + theme.dpF(36f), cy + theme.dpF(36f))
            c.bounds = android.graphics.Rect(rect.left.toInt(), rect.top.toInt(), rect.right.toInt(), rect.bottom.toInt())
            c.draw(canvas)
            dot.color = (color and 0x00FFFFFF) or ((o * 255).roundToInt().coerceIn(0, 255) shl 24)
            canvas.drawRoundRect(rect, theme.dpF(8f), theme.dpF(8f), dot)
            text = "${(o * 100).roundToInt()}%"
        }
        canvas.drawText(text, cx, height - theme.dpF(12f), label)
    }
}
