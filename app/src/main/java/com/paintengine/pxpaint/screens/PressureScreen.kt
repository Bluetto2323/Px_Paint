package com.paintengine.pxpaint.screens

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import com.paintengine.pxpaint.core.AxisSupport
import com.paintengine.pxpaint.core.PressureCurve
import com.paintengine.pxpaint.core.PressureNormalizer
import com.paintengine.pxpaint.core.S
import com.paintengine.pxpaint.core.tr
import com.paintengine.pxpaint.input.StylusInput
import com.paintengine.pxpaint.ui.Icon
import com.paintengine.pxpaint.ui.IconButton
import com.paintengine.pxpaint.ui.PxSlider
import com.paintengine.pxpaint.ui.PxTheme
import com.paintengine.pxpaint.ui.Widgets
import com.paintengine.pxpaint.ui.markHeading
import java.util.Locale
import kotlin.math.roundToInt

/**
 * Stylus & pressure settings: what the device reports, the response curve
 * (sensitivity + two-point curve, presets), pressure smoothing, and a test
 * pad that plots the stylus's reported pressure against the brush response
 * so the curve can be tuned by feel. Changes apply to the next stroke.
 * Nothing here invents pressure: a finger on the pad is shown as "no pressure".
 */
class PressureScreen(host: PxHost) : Screen(host) {
    private var curveView: CurveEditor? = null
    private var pad: PressurePad? = null
    private var reading: TextView? = null
    private var devicesText: TextView? = null
    private val stylusListener = { refreshDevices() }

    private val curve: PressureCurve get() = host.settings.pressureCurve
    private val sensitivity: Float get() = host.settings.pressureSensitivity

    override fun createView(): View {
        val root = FrameLayout(context).apply { setBackgroundColor(theme.bg) }
        val ins = host.insets
        val page = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(theme.pad, 0, theme.pad, ins.bottom + theme.dp(32))
        }

        // Devices
        page.addView(Widgets.sectionHeader(context, theme, tr(S.STYLUS_DEVICES)).apply { setPadding(0, theme.dp(16), 0, theme.dp(6)) })
        devicesText = theme.text(context, "", PxTheme.Type.BODY, theme.textSecondary).also { page.addView(it) }

        // Test pad + live reading
        page.addView(Widgets.sectionHeader(context, theme, tr(S.TEST_AREA)).apply { setPadding(0, theme.dp(20), 0, theme.dp(6)) })
        page.addView(theme.text(context, tr(S.TEST_HINT), PxTheme.Type.CAPTION, theme.textSecondary))
        pad = PressurePad(context).also {
            page.addView(it, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, theme.dp(180)).apply { topMargin = theme.dp(8) })
        }
        reading = theme.text(context, "", PxTheme.Type.CAPTION, theme.textSecondary).apply {
            typeface = android.graphics.Typeface.MONOSPACE
            setPadding(0, theme.dp(6), 0, 0)
        }.also { page.addView(it) }

        // Curve
        page.addView(Widgets.sectionHeader(context, theme, tr(S.CURVE)).apply { setPadding(0, theme.dp(20), 0, theme.dp(6)) })
        page.addView(Widgets.segmented(context, theme, listOf(
            PressureCurve.LINEAR to tr(S.CURVE_LINEAR), PressureCurve.SOFT to tr(S.CURVE_SOFT),
            PressureCurve.FIRM to tr(S.CURVE_FIRM), PressureCurve.S_CURVE to tr(S.CURVE_S),
        ), curve) { setCurve(it, rebuild = true) })
        curveView = CurveEditor(context).also {
            page.addView(it, LinearLayout.LayoutParams(theme.dp(260), theme.dp(260)).apply {
                gravity = Gravity.CENTER_HORIZONTAL
                topMargin = theme.dp(12)
            })
        }
        page.addView(theme.text(context, tr(S.CURVE_HINT), PxTheme.Type.CAPTION, theme.textSecondary).apply { setPadding(0, theme.dp(6), 0, 0) })

        // Sensitivity & smoothing
        page.addView(sliderBlock(tr(S.SENSITIVITY), (sensitivity + 1f) / 2f, { v -> signedPercent(v * 2f - 1f) }) { v ->
            host.updateSettings(host.settings.copy(pressureSensitivity = ((v * 2f - 1f) * 20f).roundToInt() / 20f), rebuild = false)
            curveView?.invalidate()
            pad?.invalidate()
        })
        page.addView(sliderBlock(tr(S.SMOOTHING), host.settings.pressureSmoothing, { v -> "${(v * 100).roundToInt()}%" }) { v ->
            host.updateSettings(host.settings.copy(pressureSmoothing = (v * 20f).roundToInt() / 20f), rebuild = false)
        })
        page.addView(Widgets.pill(context, theme, tr(S.RESET_DEFAULTS), false) {
            host.updateSettings(host.settings.copy(pressureCurve = PressureCurve.LINEAR, pressureSensitivity = 0f, pressureSmoothing = 0.35f))
        }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = theme.dp(16) })

        val sv = ScrollView(context).apply {
            clipToPadding = false
            setPadding(ins.left, 0, ins.right, 0)
            addView(MaxWidth(context, theme.dp(720)).apply { addView(page) })
        }
        root.addView(topBar(), FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP))
        root.addView(sv, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT).apply {
            topMargin = ins.top + theme.touchTarget + theme.dp(16)
        })
        refreshDevices()
        showReading(null)
        return root
    }

    override fun onShown() = host.stylus.addListener(stylusListener)
    override fun onRemoved() = host.stylus.removeListener(stylusListener)

    private fun setCurve(c: PressureCurve, rebuild: Boolean) {
        host.updateSettings(host.settings.copy(pressureCurve = c.sanitized()), rebuild = rebuild)
        curveView?.invalidate()
        pad?.invalidate()
    }

    private fun refreshDevices() {
        val m = host.stylus
        val lines = ArrayList<String>()
        for (d in m.devices) lines += tr(S.STYLUS_DEVICE_LINE, d.name, axesText(d.axes))
        m.seen?.let { lines += tr(S.STYLUS_SEEN, axesText(it)) }
        if (lines.isEmpty()) lines += tr(S.STYLUS_NONE)
        devicesText?.text = lines.joinToString("\n")
    }

    private fun axesText(a: AxisSupport): String {
        val names = ArrayList<String>()
        if (a.pressure) names += tr(S.AXIS_PRESSURE)
        if (a.tilt) names += tr(S.AXIS_TILT)
        if (a.orientation) names += tr(S.AXIS_ORIENTATION)
        if (a.distance) names += tr(S.AXIS_DISTANCE)
        return if (names.isEmpty()) tr(S.AXIS_NONE) else names.joinToString(", ")
    }

    private fun showReading(r: Sample?) {
        val t = reading ?: return
        if (r == null) {
            t.text = tr(S.TEST_READING, tr(S.NOT_REPORTED), tr(S.NOT_REPORTED), tr(S.NOT_REPORTED), tr(S.NOT_REPORTED), tr(S.NOT_REPORTED))
            return
        }
        if (!r.stylus) {
            t.text = tr(S.TEST_FINGER)
            return
        }
        fun f(v: Float, unit: String = "") = if (v.isNaN()) tr(S.NOT_REPORTED) else String.format(Locale.US, "%.2f%s", v, unit)
        fun deg(v: Float) = if (v.isNaN()) tr(S.NOT_REPORTED) else "${Math.toDegrees(v.toDouble()).roundToInt()}°"
        t.text = tr(S.TEST_READING, f(r.pressure), f(r.response), deg(r.tilt), deg(r.orientation), f(r.distance))
    }

    private fun signedPercent(v: Float): String {
        val p = (v * 100).roundToInt()
        return if (p > 0) "+$p%" else "$p%"
    }

    private fun sliderBlock(label: String, initial: Float, format: (Float) -> String, apply: (Float) -> Unit): View =
        LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, theme.dp(16), 0, 0)
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

    private fun topBar(): View = LinearLayout(context).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        setBackgroundColor(theme.bg)
        setPadding(host.insets.left + theme.dp(4), host.insets.top + theme.dp(8), host.insets.right + theme.dp(8), theme.dp(8))
        addView(IconButton(context, theme, Icon.BACK, tr(S.ACTION_CLOSE)).apply { setOnClickListener { host.pop() } })
        addView(theme.text(context, tr(S.SEC_STYLUS), PxTheme.Type.TITLE).apply {
            setPadding(theme.dp(8), 0, 0, 0)
            markHeading()
        })
    }

    private class Sample(
        val stylus: Boolean, val pressure: Float, val response: Float, val tilt: Float, val orientation: Float,
        val distance: Float, val t: Long,
    )

    /** Draws its graph from the stylus samples: reported pressure (grey) and response (accent). */
    @SuppressLint("ViewConstructor")
    private inner class PressurePad(context: Context) : View(context) {
        private val history = ArrayDeque<Sample>()
        private val bg = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = theme.surface }
        private val grid = Paint().apply { color = theme.outline; strokeWidth = theme.dpF(1f) }
        private val raw = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = theme.textTertiary; style = Paint.Style.STROKE; strokeWidth = theme.dpF(2f)
        }
        private val resp = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = theme.accent; style = Paint.Style.STROKE; strokeWidth = theme.dpF(2.5f)
        }
        private val path = Path()

        init {
            contentDescription = tr(S.TEST_AREA)
            isClickable = true
        }

        @SuppressLint("ClickableViewAccessibility")
        override fun onTouchEvent(e: MotionEvent): Boolean {
            host.stylus.onEvent(e)
            if (e.actionMasked == MotionEvent.ACTION_DOWN) parent?.requestDisallowInterceptTouchEvent(true)
            val i = e.actionIndex.coerceIn(0, e.pointerCount - 1)
            val kind = StylusInput.kind(e, i)
            val a = StylusInput.axes(e)
            val stylus = kind.isStylus
            // Every sample, historical first, exactly as the canvas reads them.
            for (h in 0..e.historySize) {
                val cur = h == e.historySize
                val p = if (stylus && a.pressure) {
                    PressureNormalizer.normalize(if (cur) e.getPressure(i) else e.getHistoricalPressure(i, h), a.pressureMin, a.pressureMax)
                } else Float.NaN
                fun axis(ax: Int, ok: Boolean) = if (stylus && ok) (if (cur) e.getAxisValue(ax, i) else e.getHistoricalAxisValue(ax, i, h)) else Float.NaN
                val s = Sample(
                    stylus, p, if (p.isNaN()) Float.NaN else curve.response(p, sensitivity),
                    axis(MotionEvent.AXIS_TILT, a.tilt), axis(MotionEvent.AXIS_ORIENTATION, a.orientation),
                    axis(MotionEvent.AXIS_DISTANCE, a.distance), if (cur) e.eventTime else e.getHistoricalEventTime(h),
                )
                history.addLast(s)
                if (history.size > 600) history.removeFirst()
                if (cur) {
                    showReading(s)
                    curveView?.marker = s.pressure
                }
            }
            invalidate()
            if (e.actionMasked == MotionEvent.ACTION_UP || e.actionMasked == MotionEvent.ACTION_CANCEL) curveView?.marker = Float.NaN
            return true
        }

        override fun onDraw(c: Canvas) {
            val w = width.toFloat()
            val h = height.toFloat()
            c.drawRoundRect(0f, 0f, w, h, theme.radiusM, theme.radiusM, bg)
            for (k in 1..3) c.drawLine(0f, h * k / 4, w, h * k / 4, grid)
            if (history.isEmpty()) return
            val tEnd = history.last().t
            val span = 3000f   // last 3 s
            fun x(t: Long) = w - (tEnd - t) / span * w
            fun y(v: Float) = h - v * (h - theme.dpF(8f)) - theme.dpF(4f)
            for ((paint, sel) in listOf(raw to { s: Sample -> s.pressure }, resp to { s: Sample -> s.response })) {
                path.reset()
                var started = false
                for (s in history) {
                    val v = sel(s)
                    if (v.isNaN() || tEnd - s.t > span) {
                        started = false
                        continue
                    }
                    if (!started) path.moveTo(x(s.t), y(v)) else path.lineTo(x(s.t), y(v))
                    started = true
                }
                c.drawPath(path, paint)
            }
        }
    }

    /** Response curve with two draggable control points (finger or stylus). */
    @SuppressLint("ViewConstructor")
    private inner class CurveEditor(context: Context) : View(context) {
        var marker = Float.NaN
            set(v) {
                field = v
                invalidate()
            }
        private val bg = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = theme.surface }
        private val grid = Paint().apply { color = theme.outline; strokeWidth = theme.dpF(1f) }
        private val line = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = theme.accent; style = Paint.Style.STROKE; strokeWidth = theme.dpF(3f); strokeCap = Paint.Cap.ROUND
        }
        private val handle = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = theme.text }
        private val markPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = theme.accent }
        private val path = Path()
        private var dragging = -1
        private var working: PressureCurve? = null

        init {
            contentDescription = tr(S.CURVE)
        }

        private val inset get() = theme.dpF(14f)
        private fun px(x: Float) = inset + x * (width - 2 * inset)
        private fun py(y: Float) = height - inset - y * (height - 2 * inset)

        override fun onDraw(c: Canvas) {
            val cv = working ?: curve
            c.drawRoundRect(0f, 0f, width.toFloat(), height.toFloat(), theme.radiusM, theme.radiusM, bg)
            for (k in 1..3) {
                c.drawLine(px(k / 4f), py(0f), px(k / 4f), py(1f), grid)
                c.drawLine(px(0f), py(k / 4f), px(1f), py(k / 4f), grid)
            }
            path.reset()
            for (i in 0..100) {
                val x = i / 100f
                val y = cv.response(x, sensitivity)
                if (i == 0) path.moveTo(px(x), py(y)) else path.lineTo(px(x), py(y))
            }
            c.drawPath(path, line)
            val r = theme.dpF(9f)
            c.drawCircle(px(cv.x1), py(cv.y1), r, handle)
            c.drawCircle(px(cv.x2), py(cv.y2), r, handle)
            if (!marker.isNaN()) c.drawCircle(px(marker), py(cv.response(marker, sensitivity)), theme.dpF(6f), markPaint)
        }

        @SuppressLint("ClickableViewAccessibility")
        override fun onTouchEvent(e: MotionEvent): Boolean {
            val cv = working ?: curve
            val x = ((e.x - inset) / (width - 2 * inset)).coerceIn(0f, 1f)
            val y = ((height - inset - e.y) / (height - 2 * inset)).coerceIn(0f, 1f)
            when (e.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    parent?.requestDisallowInterceptTouchEvent(true)
                    val d1 = (x - cv.x1) * (x - cv.x1) + (y - cv.y1) * (y - cv.y1)
                    val d2 = (x - cv.x2) * (x - cv.x2) + (y - cv.y2) * (y - cv.y2)
                    dragging = if (d1 <= d2) 1 else 2
                    working = cv
                }
                MotionEvent.ACTION_MOVE -> if (dragging > 0) {
                    working = (if (dragging == 1) cv.copy(x1 = x, y1 = y) else cv.copy(x2 = x, y2 = y)).sanitized()
                    invalidate()
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    working?.let { setCurve(it, rebuild = false) }
                    working = null
                    dragging = -1
                    invalidate()
                }
            }
            return true
        }
    }
}
