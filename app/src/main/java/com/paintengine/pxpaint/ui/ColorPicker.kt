package com.paintengine.pxpaint.ui

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.ComposeShader
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.PorterDuff
import android.graphics.RectF
import android.graphics.Shader
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.widget.LinearLayout
import android.widget.TextView
import java.util.Locale

/**
 * HSV colour picker: saturation/value field, hue bar, current colour and
 * recent colours. Reports straight (non-premultiplied) opaque ARGB.
 */
@SuppressLint("ViewConstructor")
class ColorPicker(
    context: Context,
    private val theme: PxTheme,
    initial: Int,
    recent: List<Int>,
    private val onPick: (Int) -> Unit,
) : LinearLayout(context) {
    private val hsv = FloatArray(3)
    private val field = SvField()
    private val hueBar = HueBar()
    private val swatch = View(context)
    private val hex: TextView
    private val recentRow = LinearLayout(context)

    init {
        orientation = VERTICAL
        Color.colorToHSV(initial or 0xFF000000.toInt(), hsv)
        addView(field, LayoutParams(LayoutParams.MATCH_PARENT, theme.dp(180)))
        addView(hueBar, LayoutParams(LayoutParams.MATCH_PARENT, theme.dp(40)).apply { topMargin = theme.dp(12) })
        val info = LinearLayout(context).apply {
            orientation = HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, theme.dp(12), 0, 0)
        }
        info.addView(swatch, LayoutParams(theme.dp(32), theme.dp(32)))
        hex = theme.text(context, "", PxTheme.Type.LABEL, theme.textSecondary)
        info.addView(hex, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT).apply { marginStart = theme.dp(10) })
        addView(info)
        recentRow.orientation = HORIZONTAL
        recentRow.setPadding(0, theme.dp(10), 0, 0)
        addView(recentRow)
        setRecent(recent)
        sync(fromUser = false)
    }

    fun color(): Int = Color.HSVToColor(hsv)

    fun setRecent(colors: List<Int>) {
        recentRow.removeAllViews()
        for (c in colors.take(8)) {
            recentRow.addView(View(context).apply {
                background = theme.pressable(theme.oval(c, theme.outline), theme.dpF(999f))
                contentDescription = String.format(Locale.US, "#%06X", c and 0xFFFFFF)
                setOnClickListener {
                    Color.colorToHSV(c, hsv)
                    sync(fromUser = true)
                }
            }, LayoutParams(theme.dp(34), theme.dp(34)).apply { marginEnd = theme.dp(8) })
        }
        recentRow.visibility = if (colors.isEmpty()) GONE else VISIBLE
    }

    private fun sync(fromUser: Boolean) {
        val c = color()
        swatch.background = theme.oval(c, theme.outline)
        hex.text = String.format(Locale.US, "#%06X", c and 0xFFFFFF)
        field.invalidate()
        hueBar.invalidate()
        if (fromUser) onPick(c)
    }

    private inner class SvField : View(context) {
        private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
        private val ring = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = theme.dpF(2.5f); color = Color.WHITE }
        private val ringShadow = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = theme.dpF(4.5f); color = 0x66000000 }
        private val rect = RectF()

        init {
            contentDescription = "Saturation and brightness"
        }

        override fun onDraw(canvas: Canvas) {
            rect.set(0f, 0f, width.toFloat(), height.toFloat())
            val hue = Color.HSVToColor(floatArrayOf(hsv[0], 1f, 1f))
            val sat = LinearGradient(0f, 0f, width.toFloat(), 0f, Color.WHITE, hue, Shader.TileMode.CLAMP)
            val value = LinearGradient(0f, 0f, 0f, height.toFloat(), Color.WHITE, Color.BLACK, Shader.TileMode.CLAMP)
            paint.shader = ComposeShader(value, sat, PorterDuff.Mode.MULTIPLY)
            canvas.drawRoundRect(rect, theme.radiusS, theme.radiusS, paint)
            val x = hsv[1] * width
            val y = (1f - hsv[2]) * height
            canvas.drawCircle(x, y, theme.dpF(9f), ringShadow)
            canvas.drawCircle(x, y, theme.dpF(9f), ring)
        }

        @SuppressLint("ClickableViewAccessibility")
        override fun onTouchEvent(e: MotionEvent): Boolean {
            parent?.requestDisallowInterceptTouchEvent(true)
            hsv[1] = (e.x / width).coerceIn(0f, 1f)
            hsv[2] = (1f - e.y / height).coerceIn(0f, 1f)
            sync(fromUser = e.actionMasked == MotionEvent.ACTION_UP || e.actionMasked == MotionEvent.ACTION_MOVE)
            return true
        }
    }

    private inner class HueBar : View(context) {
        private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
        private val knob = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = theme.dpF(3f); color = Color.WHITE }
        private val rect = RectF()

        init {
            contentDescription = "Hue"
        }

        override fun onDraw(canvas: Canvas) {
            val inset = theme.dpF(10f)
            rect.set(0f, inset, width.toFloat(), height - inset)
            val colors = IntArray(7) { Color.HSVToColor(floatArrayOf(it * 60f, 1f, 1f)) }
            paint.shader = LinearGradient(0f, 0f, width.toFloat(), 0f, colors, null, Shader.TileMode.CLAMP)
            canvas.drawRoundRect(rect, rect.height() / 2, rect.height() / 2, paint)
            val x = hsv[0] / 360f * width
            canvas.drawCircle(x.coerceIn(inset, width - inset), height / 2f, inset - theme.dpF(1f), knob)
        }

        @SuppressLint("ClickableViewAccessibility")
        override fun onTouchEvent(e: MotionEvent): Boolean {
            parent?.requestDisallowInterceptTouchEvent(true)
            hsv[0] = (e.x / width * 360f).coerceIn(0f, 359.9f)
            sync(fromUser = e.actionMasked == MotionEvent.ACTION_UP || e.actionMasked == MotionEvent.ACTION_MOVE)
            return true
        }
    }
}
