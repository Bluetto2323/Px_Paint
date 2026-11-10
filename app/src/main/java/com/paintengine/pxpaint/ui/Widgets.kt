package com.paintengine.pxpaint.ui

import android.annotation.SuppressLint
import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Bitmap
import android.graphics.BitmapShader
import android.graphics.Canvas
import android.graphics.ColorFilter
import android.graphics.Paint
import android.graphics.PixelFormat
import android.graphics.Shader
import android.graphics.drawable.Drawable
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.accessibility.AccessibilityNodeInfo
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.Switch
import android.widget.TextView
import kotlin.math.roundToInt

/** Square icon button with a minimum touch target, tooltip and selected state. */
@SuppressLint("ViewConstructor")
class IconButton(
    context: Context,
    private val theme: PxTheme,
    icon: Icon,
    description: String,
    sizeDp: Float = 40f,
    private val label: String? = null,
) : LinearLayout(context) {
    private var drawable = IconDrawable(icon, theme.text)
    private val iconView = View(context)
    private val labelView: TextView?

    init {
        orientation = VERTICAL
        gravity = Gravity.CENTER
        contentDescription = description
        tooltipText = description
        isClickable = true
        isFocusable = true
        minimumWidth = theme.touchTarget
        minimumHeight = theme.touchTarget
        val box = theme.dp(sizeDp)
        iconView.background = drawable
        val inset = theme.dp(sizeDp * 0.22f)
        addView(FrameLayout(context).apply {
            addView(iconView, FrameLayout.LayoutParams(box - 2 * inset, box - 2 * inset, Gravity.CENTER))
        }, LayoutParams(box, box))
        labelView = label?.let {
            theme.text(context, it, PxTheme.Type.MICRO, theme.textSecondary, lines = 1).also { tv ->
                tv.gravity = Gravity.CENTER
                addView(tv, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT).apply { topMargin = theme.dp(2) })
            }
        }
        refresh()
    }

    init {
        // Toolbar interactions: a quick press-down scale (native StateListAnimator), when motion allows.
        if (theme.motion.movement) {
            stateListAnimator = android.animation.StateListAnimator().apply {
                fun to(s: Float, ms: Long) = android.animation.ObjectAnimator.ofPropertyValuesHolder(this@IconButton,
                    android.animation.PropertyValuesHolder.ofFloat(View.SCALE_X, s),
                    android.animation.PropertyValuesHolder.ofFloat(View.SCALE_Y, s)).setDuration(ms)
                addState(intArrayOf(android.R.attr.state_pressed), to(com.paintengine.pxpaint.core.MotionTokens.PRESS_SCALE, com.paintengine.pxpaint.core.MotionTokens.MICRO))
                addState(intArrayOf(), to(1f, com.paintengine.pxpaint.core.MotionTokens.SHORT))
            }
        }
    }

    override fun setSelected(selected: Boolean) {
        super.setSelected(selected)
        refresh()
    }

    /** Swaps the icon (e.g. the "more tools" button shows the active extra tool). */
    fun setIcon(icon: Icon) {
        drawable = IconDrawable(icon, theme.text)
        iconView.background = drawable
        refresh()
    }

    /** Round selection background (floating capsules) instead of the rounded square. */
    var round = false
        set(v) { field = v; refresh() }

    /** Selected = filled with the brand gradient (the toolbar's active tool). */
    var brand = false
        set(v) { field = v; refresh() }

    /** A floating glass disc of its own (the controls along the top). */
    var orb = false
        set(v) { field = v; elevation = if (v) theme.dpF(3f) else 0f; refresh() }

    override fun setEnabled(enabled: Boolean) {
        super.setEnabled(enabled)
        refresh()
    }

    private fun refresh() {
        val onBrand = brand && isSelected
        val color = when {
            !isEnabled -> theme.textTertiary
            onBrand -> PxTheme.ON_BRAND
            isSelected -> theme.accent
            else -> theme.text
        }
        drawable.setColor(color)
        labelView?.setTextColor(if (isSelected) theme.accent else theme.textSecondary)
        val r = if (round || orb) theme.radiusPill else theme.radiusS
        background = theme.pressable(when {
            onBrand -> theme.brand(r)
            isSelected -> theme.rounded(theme.accentSoft, r, if (orb) theme.edge else 0)
            orb -> theme.glass(r)
            else -> null
        }, r)
        alpha = if (isEnabled) 1f else if (orb) 0.7f else 0.55f
    }
}

/** Checkerboard for transparent backgrounds (paper preview, thumbnails). */
class CheckerDrawable(cellPx: Int, dark: Boolean) : Drawable() {
    private val paint = Paint().apply {
        val a = if (dark) 0xFF3A3D42.toInt() else 0xFFFFFFFF.toInt()
        val b = if (dark) 0xFF2A2C30.toInt() else 0xFFE3E1DC.toInt()
        val bmp = Bitmap.createBitmap(cellPx * 2, cellPx * 2, Bitmap.Config.ARGB_8888)
        Canvas(bmp).apply {
            drawColor(a)
            val p = Paint().apply { color = b }
            drawRect(0f, 0f, cellPx.toFloat(), cellPx.toFloat(), p)
            drawRect(cellPx.toFloat(), cellPx.toFloat(), cellPx * 2f, cellPx * 2f, p)
        }
        shader = BitmapShader(bmp, Shader.TileMode.REPEAT, Shader.TileMode.REPEAT)
    }

    override fun draw(canvas: Canvas) = canvas.drawRect(bounds, paint)
    override fun setAlpha(alpha: Int) { paint.alpha = alpha }
    override fun setColorFilter(colorFilter: ColorFilter?) { paint.colorFilter = colorFilter }
    @Deprecated("Deprecated in Java")
    override fun getOpacity(): Int = PixelFormat.TRANSLUCENT
}

object Widgets {
    fun pill(context: Context, theme: PxTheme, text: String, primary: Boolean, onClick: () -> Unit): TextView =
        theme.text(context, text, PxTheme.Type.LABEL, if (primary) theme.onAccent else theme.text).apply {
            gravity = Gravity.CENTER
            minHeight = theme.touchTarget
            minWidth = theme.dp(88)
            setPadding(theme.dp(20), 0, theme.dp(20), 0)
            val fill = if (primary) theme.accent else theme.raised
            background = theme.pressable(
                theme.rounded(fill, theme.dpF(999f), if (primary) 0 else theme.outline), theme.dpF(999f),
            )
            isClickable = true
            isFocusable = true
            setOnClickListener { onClick() }
        }

    fun chip(context: Context, theme: PxTheme, text: String, selected: Boolean, onClick: () -> Unit): TextView =
        theme.text(context, text, PxTheme.Type.LABEL, if (selected) theme.accent else theme.textSecondary, lines = 1).apply {
            gravity = Gravity.CENTER
            minHeight = theme.touchTarget
            minWidth = theme.touchTarget
            setPadding(theme.dp(14), 0, theme.dp(14), 0)
            background = theme.pressable(
                theme.rounded(if (selected) theme.accentSoft else theme.surface, theme.dpF(999f),
                    if (selected) theme.accent else theme.outline),
                theme.dpF(999f),
            )
            isSelected = selected
            isClickable = true
            contentDescription = text
            setOnClickListener { onClick() }
        }

    /** Single-choice row of chips. */
    fun <T> segmented(
        context: Context, theme: PxTheme, options: List<Pair<T, String>>, selected: T, onSelect: (T) -> Unit,
    ): ViewGroup {
        val row = FlowRow(context, theme.dp(8))
        for ((value, label) in options) {
            row.addView(chip(context, theme, label, value == selected) { onSelect(value) })
        }
        return row
    }

    fun switchRow(
        context: Context, theme: PxTheme, title: String, description: String?, checked: Boolean,
        onChange: (Boolean) -> Unit,
    ): View {
        val sw = Switch(context).apply {
            isChecked = checked
            thumbTintList = ColorStateList(
                arrayOf(intArrayOf(android.R.attr.state_checked), intArrayOf()),
                intArrayOf(theme.accent, if (theme.dark) 0xFFBFC4CA.toInt() else 0xFFFFFFFF.toInt()),
            )
            trackTintList = ColorStateList(
                arrayOf(intArrayOf(android.R.attr.state_checked), intArrayOf()),
                intArrayOf(theme.accentSoft or 0x60000000, theme.outline),
            )
            contentDescription = title
            setOnCheckedChangeListener { _, isChecked -> onChange(isChecked) }
        }
        return row(context, theme, title, description, sw).apply { setOnClickListener { sw.toggle() } }
    }

    /** Settings-style row: title + description on the left, [trailing] on the right. */
    fun row(context: Context, theme: PxTheme, title: String, description: String?, trailing: View?): LinearLayout =
        LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            minimumHeight = theme.touchTarget + theme.dp(8)
            setPadding(theme.pad, theme.dp(10), theme.pad, theme.dp(10))
            background = theme.pressable(null, 0f)
            val texts = LinearLayout(context).apply {
                orientation = LinearLayout.VERTICAL
                addView(theme.text(context, title, PxTheme.Type.BODY))
                if (description != null) addView(
                    theme.text(context, description, PxTheme.Type.CAPTION, theme.textSecondary).apply { setPadding(0, theme.dp(3), 0, 0) },
                )
            }
            addView(texts, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            // Keep a size the caller gave the trailing view: a plain View measured
            // WRAP_CONTENT takes all the width (View.getDefaultSize with AT_MOST)
            // and squeezes the texts to nothing.
            if (trailing != null) {
                val given = trailing.layoutParams
                addView(trailing, LinearLayout.LayoutParams(
                    given?.width ?: LinearLayout.LayoutParams.WRAP_CONTENT, given?.height ?: LinearLayout.LayoutParams.WRAP_CONTENT,
                ).apply { marginStart = theme.dp(12) })
            }
        }

    fun sectionHeader(context: Context, theme: PxTheme, title: String): TextView =
        theme.text(context, title.uppercase(), PxTheme.Type.MICRO, theme.accent).apply {
            setPadding(theme.pad, theme.dp(22), theme.pad, theme.dp(6))
            isFocusable = false
            markHeading()
        }

    fun divider(context: Context, theme: PxTheme): View = View(context).apply {
        setBackgroundColor(theme.outline)
        layoutParams = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, maxOf(1, theme.dp(0.6f)))
    }
}

/** Lays children out left to right, wrapping to new lines. */
class FlowRow(context: Context, private val spacing: Int) : ViewGroup(context) {
    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val maxW = MeasureSpec.getSize(widthMeasureSpec).let { if (it == 0) Int.MAX_VALUE else it } - paddingLeft - paddingRight
        var x = 0
        var y = 0
        var lineH = 0
        var widest = 0
        for (i in 0 until childCount) {
            val c = getChildAt(i)
            if (c.visibility == GONE) continue
            measureChild(c, MeasureSpec.makeMeasureSpec(maxW, MeasureSpec.AT_MOST), heightMeasureSpec)
            if (x > 0 && x + c.measuredWidth > maxW) {
                y += lineH + spacing
                x = 0
                lineH = 0
            }
            x += c.measuredWidth + spacing
            widest = maxOf(widest, x - spacing)
            lineH = maxOf(lineH, c.measuredHeight)
        }
        val w = if (MeasureSpec.getMode(widthMeasureSpec) == MeasureSpec.EXACTLY) MeasureSpec.getSize(widthMeasureSpec)
        else widest + paddingLeft + paddingRight
        setMeasuredDimension(w, y + lineH + paddingTop + paddingBottom)
    }

    override fun onLayout(changed: Boolean, l: Int, t: Int, r: Int, b: Int) {
        val maxW = r - l - paddingLeft - paddingRight
        var x = 0
        var y = 0
        var lineH = 0
        for (i in 0 until childCount) {
            val c = getChildAt(i)
            if (c.visibility == GONE) continue
            if (x > 0 && x + c.measuredWidth > maxW) {
                y += lineH + spacing
                x = 0
                lineH = 0
            }
            c.layout(paddingLeft + x, paddingTop + y, paddingLeft + x + c.measuredWidth, paddingTop + y + c.measuredHeight)
            x += c.measuredWidth + spacing
            lineH = maxOf(lineH, c.measuredHeight)
        }
    }
}

/** Fixed-column grid; every cell gets the same width, its own height. */
class GridLayoutView(context: Context, var columns: Int, private val spacing: Int) : ViewGroup(context) {
    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val w = MeasureSpec.getSize(widthMeasureSpec)
        val inner = w - paddingLeft - paddingRight
        val cellW = ((inner - spacing * (columns - 1)) / columns).coerceAtLeast(1)
        var y = 0
        var rowH = 0
        var col = 0
        for (i in 0 until childCount) {
            val c = getChildAt(i)
            c.measure(MeasureSpec.makeMeasureSpec(cellW, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(0, MeasureSpec.UNSPECIFIED))
            rowH = maxOf(rowH, c.measuredHeight)
            col++
            if (col == columns) {
                y += rowH + spacing
                rowH = 0
                col = 0
            }
        }
        if (col > 0) y += rowH else if (y > 0) y -= spacing
        setMeasuredDimension(w, y + paddingTop + paddingBottom)
    }

    override fun onLayout(changed: Boolean, l: Int, t: Int, r: Int, b: Int) {
        val inner = r - l - paddingLeft - paddingRight
        val cellW = ((inner - spacing * (columns - 1)) / columns).coerceAtLeast(1)
        var y = paddingTop
        var rowH = 0
        for (i in 0 until childCount) {
            val col = i % columns
            if (col == 0 && i > 0) {
                y += rowH + spacing
                rowH = 0
            }
            val c = getChildAt(i)
            val x = paddingLeft + col * (cellW + spacing)
            c.layout(x, y, x + cellW, y + c.measuredHeight)
            rowH = maxOf(rowH, c.measuredHeight)
        }
    }
}

/**
 * Horizontal slider (0..1) with large touch area, accent fill and an
 * accessible value (TalkBack reads it, volume keys adjust it).
 */
@SuppressLint("ViewConstructor")
class PxSlider(context: Context, private val theme: PxTheme, description: String) : View(context) {
    var value = 0.5f
        private set
    var onChange: ((Float, Boolean) -> Unit)? = null   // value, fromUser
    var valueText: (Float) -> String = { "${(it * 100).roundToInt()}%" }
    private val track = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = theme.outline }
    private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = theme.accent }
    private val thumb = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = if (theme.dark) 0xFFF4F4F4.toInt() else 0xFFFFFFFF.toInt() }
    private val thumbStroke = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; color = theme.accent; strokeWidth = theme.dpF(2f)
    }

    init {
        contentDescription = description
        isFocusable = true
        minimumHeight = theme.touchTarget
    }

    fun setValue(v: Float, fromUser: Boolean = false) {
        val c = v.coerceIn(0f, 1f)
        if (c == value) return
        value = c
        if (android.os.Build.VERSION.SDK_INT >= 30) stateDescription = valueText(c)
        invalidate()
        onChange?.invoke(c, fromUser)
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        setMeasuredDimension(MeasureSpec.getSize(widthMeasureSpec), resolveSize(theme.touchTarget, heightMeasureSpec))
    }

    override fun onDraw(canvas: Canvas) {
        val r = theme.dpF(10f)
        val left = paddingLeft + r
        val right = width - paddingRight - r
        val cy = height / 2f
        val h = theme.dpF(4f)
        track.alpha = if (isEnabled) 255 else 90
        fillPaint.alpha = if (isEnabled) 255 else 90
        canvas.drawRoundRect(left, cy - h / 2, right, cy + h / 2, h, h, track)
        val x = left + (right - left) * value
        canvas.drawRoundRect(left, cy - h / 2, x, cy + h / 2, h, h, fillPaint)
        if (isEnabled) {
            canvas.drawCircle(x, cy, r, thumb)
            canvas.drawCircle(x, cy, r, thumbStroke)
        }
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (!isEnabled) return false
        val r = theme.dpF(10f)
        val left = paddingLeft + r
        val right = width - paddingRight - r
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_MOVE -> {
                parent?.requestDisallowInterceptTouchEvent(true)
                setValue((event.x - left) / (right - left), fromUser = true)
            }
        }
        return true
    }

    override fun onInitializeAccessibilityNodeInfo(info: AccessibilityNodeInfo) {
        super.onInitializeAccessibilityNodeInfo(info)
        info.className = "android.widget.SeekBar"
        info.rangeInfo = AccessibilityNodeInfo.RangeInfo.obtain(AccessibilityNodeInfo.RangeInfo.RANGE_TYPE_FLOAT, 0f, 1f, value)
        if (isEnabled) {
            info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_FORWARD)
            info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_BACKWARD)
        }
    }

    override fun performAccessibilityAction(action: Int, arguments: android.os.Bundle?): Boolean = when (action) {
        AccessibilityNodeInfo.ACTION_SCROLL_FORWARD -> { setValue(value + 0.05f, true); true }
        AccessibilityNodeInfo.ACTION_SCROLL_BACKWARD -> { setValue(value - 0.05f, true); true }
        else -> super.performAccessibilityAction(action, arguments)
    }
}

/** Marks a view as a heading for screen readers (API 28+; no-op before). */
fun View.markHeading() {
    if (android.os.Build.VERSION.SDK_INT >= 28) isAccessibilityHeading = true
}

/** Wraps one child and limits its height (floating panels scroll inside it). */
class MaxHeight(context: Context, private val maxH: Int) : FrameLayout(context) {
    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val spec = when (MeasureSpec.getMode(heightMeasureSpec)) {
            MeasureSpec.UNSPECIFIED -> MeasureSpec.makeMeasureSpec(maxH, MeasureSpec.AT_MOST)
            else -> MeasureSpec.makeMeasureSpec(minOf(maxH, MeasureSpec.getSize(heightMeasureSpec)), MeasureSpec.AT_MOST)
        }
        super.onMeasure(widthMeasureSpec, spec)
    }
}

/**
 * Equal-width tiles in as many columns as fit (at least two): each column is
 * at least [minTile] wide, [spacing] between tiles; each row as tall as its
 * tallest tile. A child that is not a tile (an empty-list message) spans the row.
 */
class TileGrid(context: Context, private val minTile: Int, private val spacing: Int) : ViewGroup(context) {
    var columns = 2
        private set

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val w = MeasureSpec.getSize(widthMeasureSpec) - paddingLeft - paddingRight
        columns = maxOf(2, (w + spacing) / (minTile + spacing))
        val tileW = ((w - (columns - 1) * spacing) / columns).coerceAtLeast(1)
        var y = 0
        var col = 0
        var rowH = 0
        for (i in 0 until childCount) {
            val c = getChildAt(i)
            if (c.visibility == GONE) continue
            if (c is android.widget.TextView) {
                measureChild(c, MeasureSpec.makeMeasureSpec(w, MeasureSpec.EXACTLY), heightMeasureSpec)
                if (col > 0) { y += rowH + spacing; col = 0; rowH = 0 }
                y += c.measuredHeight + spacing
                continue
            }
            c.measure(MeasureSpec.makeMeasureSpec(tileW, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(0, MeasureSpec.UNSPECIFIED))
            rowH = maxOf(rowH, c.measuredHeight)
            if (++col == columns) { y += rowH + spacing; col = 0; rowH = 0 }
        }
        if (col > 0) y += rowH + spacing
        setMeasuredDimension(MeasureSpec.getSize(widthMeasureSpec), (y - spacing).coerceAtLeast(0) + paddingTop + paddingBottom)
    }

    override fun onLayout(changed: Boolean, l: Int, t: Int, r: Int, b: Int) {
        val w = r - l - paddingLeft - paddingRight
        val tileW = ((w - (columns - 1) * spacing) / columns).coerceAtLeast(1)
        var y = paddingTop
        var col = 0
        var rowH = 0
        for (i in 0 until childCount) {
            val c = getChildAt(i)
            if (c.visibility == GONE) continue
            if (c is android.widget.TextView) {
                if (col > 0) { y += rowH + spacing; col = 0; rowH = 0 }
                c.layout(paddingLeft, y, paddingLeft + c.measuredWidth, y + c.measuredHeight)
                y += c.measuredHeight + spacing
                continue
            }
            val x = paddingLeft + col * (tileW + spacing)
            c.layout(x, y, x + tileW, y + c.measuredHeight)
            rowH = maxOf(rowH, c.measuredHeight)
            if (++col == columns) { y += rowH + spacing; col = 0; rowH = 0 }
        }
    }
}
