package com.paintengine.pxpaint.ui

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.SweepGradient
import android.os.Bundle
import android.view.MotionEvent
import android.view.View
import android.view.accessibility.AccessibilityNodeInfo
import com.paintengine.pxpaint.core.ColorMath
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.hypot
import kotlin.math.min
import kotlin.math.roundToInt
import kotlin.math.sin

/**
 * Colour wheel: a hue ring around a saturation (x) / brightness (y) square.
 * Dragging starts on the ring or in the square and stays there until the
 * finger lifts. The square is drawn from [ColorMath.hsvToArgb], the same
 * conversion the picker reports, so the picked pixel is the colour painted.
 * Accessibility: scroll forward / backward steps the hue by 5 degrees;
 * the sliders and HEX field of the colour panel give exact values.
 */
@SuppressLint("ViewConstructor")
class ColorWheel(context: Context, private val theme: PxTheme, description: String) : View(context) {
    var hue = 0f; private set
    var sat = 0f; private set
    var value = 0f; private set
    /** h, s, v, final (finger lifted). */
    var onChange: ((Float, Float, Float, Boolean) -> Unit)? = null

    private val ringPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE }
    private val markerOuter = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; color = Color.WHITE }
    private val markerInner = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; color = 0xFF000000.toInt() }
    private val bmpPaint = Paint(Paint.FILTER_BITMAP_FLAG)
    private var square: Bitmap? = null
    private var squareHue = -1f
    private val rect = RectF()
    private var target = 0   // 1 ring, 2 square

    init {
        contentDescription = description
        isFocusable = true
        isClickable = true
    }

    fun setHsv(h: Float, s: Float, v: Float) {
        hue = ((h % 360f) + 360f) % 360f
        sat = s.coerceIn(0f, 1f)
        value = v.coerceIn(0f, 1f)
        if (android.os.Build.VERSION.SDK_INT >= 30) stateDescription = ColorMath.toHex(ColorMath.hsvToArgb(hue, sat, value))
        invalidate()
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val w = MeasureSpec.getSize(widthMeasureSpec)
        val side = min(w, theme.dp(280))
        setMeasuredDimension(w, side)
    }

    private val cx get() = width / 2f
    private val cy get() = height / 2f
    private val outerR get() = min(width, height) / 2f - theme.dpF(2f)
    private val ringW get() = theme.dpF(22f)
    private val half get() = (outerR - ringW - theme.dpF(8f)) / 1.4142f

    override fun onDraw(canvas: Canvas) {
        // Hue ring (sweep gradient; Android angles run clockwise from 3 o'clock, like ours).
        val colors = IntArray(13) { ColorMath.hsvToArgb(it * 30f, 1f, 1f) }
        ringPaint.shader = SweepGradient(cx, cy, colors, null)
        ringPaint.strokeWidth = ringW
        val rr = outerR - ringW / 2f
        canvas.drawCircle(cx, cy, rr, ringPaint)
        // Saturation / brightness square of the current hue, rendered once per hue.
        val n = 64
        if (square == null || squareHue != hue) {
            val px = IntArray(n * n) { i ->
                val x = i % n
                val y = i / n
                ColorMath.hsvToArgb(hue, x / (n - 1f), 1f - y / (n - 1f))
            }
            square = (square ?: Bitmap.createBitmap(n, n, Bitmap.Config.ARGB_8888)).also { it.setPixels(px, 0, n, 0, 0, n, n) }
            squareHue = hue
        }
        val h = half
        rect.set(cx - h, cy - h, cx + h, cy + h)
        canvas.drawBitmap(square!!, null, rect, bmpPaint)
        // Markers: white ring with a dark edge, readable on any colour.
        markerOuter.strokeWidth = theme.dpF(3f)
        markerInner.strokeWidth = theme.dpF(1f)
        val a = Math.toRadians(hue.toDouble())
        val hx = cx + (rr * cos(a)).toFloat()
        val hy = cy + (rr * sin(a)).toFloat()
        val mr = ringW / 2f - theme.dpF(1f)
        canvas.drawCircle(hx, hy, mr, markerOuter)
        canvas.drawCircle(hx, hy, mr + theme.dpF(1.5f), markerInner)
        val sx = cx - h + 2 * h * sat
        val sy = cy - h + 2 * h * (1f - value)
        canvas.drawCircle(sx, sy, theme.dpF(8f), markerOuter)
        canvas.drawCircle(sx, sy, theme.dpF(9.5f), markerInner)
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        val dx = e.x - cx
        val dy = e.y - cy
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                val d = hypot(dx, dy)
                target = when {
                    d >= outerR - ringW - theme.dpF(10f) && d <= outerR + theme.dpF(10f) -> 1
                    kotlin.math.abs(dx) <= half + theme.dpF(10f) && kotlin.math.abs(dy) <= half + theme.dpF(10f) -> 2
                    else -> 0
                }
                if (target == 0) return false
                parent?.requestDisallowInterceptTouchEvent(true)
                apply(dx, dy, false)
            }
            MotionEvent.ACTION_MOVE -> if (target != 0) apply(dx, dy, false)
            MotionEvent.ACTION_UP -> if (target != 0) {
                apply(dx, dy, true)
                target = 0
            }
            MotionEvent.ACTION_CANCEL -> target = 0
        }
        return true
    }

    private fun apply(dx: Float, dy: Float, final: Boolean) {
        if (target == 1) {
            var deg = Math.toDegrees(atan2(dy.toDouble(), dx.toDouble())).toFloat()
            if (deg < 0) deg += 360f
            setHsv(deg, sat, value)
        } else {
            val h = half
            setHsv(hue, ((dx + h) / (2 * h)).coerceIn(0f, 1f), (1f - (dy + h) / (2 * h)).coerceIn(0f, 1f))
        }
        onChange?.invoke(hue, sat, value, final)
    }

    override fun onInitializeAccessibilityNodeInfo(info: AccessibilityNodeInfo) {
        super.onInitializeAccessibilityNodeInfo(info)
        info.className = "android.widget.SeekBar"
        info.rangeInfo = AccessibilityNodeInfo.RangeInfo.obtain(AccessibilityNodeInfo.RangeInfo.RANGE_TYPE_FLOAT, 0f, 360f, hue)
        info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_FORWARD)
        info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_BACKWARD)
    }

    override fun performAccessibilityAction(action: Int, arguments: Bundle?): Boolean {
        val step = when (action) {
            AccessibilityNodeInfo.ACTION_SCROLL_FORWARD -> 5f
            AccessibilityNodeInfo.ACTION_SCROLL_BACKWARD -> -5f
            else -> return super.performAccessibilityAction(action, arguments)
        }
        setHsv((hue + step).roundToInt().toFloat(), sat, value)
        onChange?.invoke(hue, sat, value, true)
        return true
    }
}
