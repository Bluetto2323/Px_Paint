package com.paintengine.pxpaint.ui

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Rect
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView

/**
 * Modal layers on top of the screens: sheets (bottom sheet on phones,
 * centred card on wide windows), anchored menus, dialogs and a non-intrusive
 * message bar. Back closes the top-most layer.
 */
@SuppressLint("ViewConstructor")
class OverlayHost(context: Context, private var theme: PxTheme) : FrameLayout(context) {
    private val layers = ArrayList<View>()
    private val handler = Handler(Looper.getMainLooper())
    private var messageView: TextView? = null
    private val hideMessage = Runnable {
        val v = messageView ?: return@Runnable
        messageView = null
        theme.motion.exit(v, dyDp = com.paintengine.pxpaint.core.MotionTokens.NUDGE) { removeView(v) }
    }
    var insets = Rect()

    fun setTheme(t: PxTheme) {
        theme = t
    }

    val hasLayer: Boolean get() = layers.isNotEmpty()

    /** Closes the top layer; false if there is none. */
    fun back(): Boolean {
        val top = layers.lastOrNull() ?: return false
        dismiss(top)
        return true
    }

    fun dismissAll() {
        while (layers.isNotEmpty()) dismiss(layers.last())
    }

    fun dismiss(layer: View) {
        if (layers.remove(layer)) {
            hideKeyboard()
            theme.motion.exit(layer, dyDp = 0f) { removeView(layer) }
        }
    }

    /**
     * Shows [content] in a sheet. Wide windows (>= 600 dp) get a centred card,
     * phones a bottom sheet. Returns the layer (pass it to [dismiss]).
     */
    fun sheet(content: View, title: String? = null, maxWidthDp: Int = 560): View {
        val wide = width / resources.displayMetrics.density >= 600
        val scrim = FrameLayout(context).apply {
            setBackgroundColor(theme.scrim)
            isClickable = true
            importantForAccessibility = IMPORTANT_FOR_ACCESSIBILITY_NO
        }
        val card = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            isClickable = true
            val r = theme.radiusL
            background = if (wide) theme.rounded(theme.surface, r) else android.graphics.drawable.GradientDrawable().apply {
                setColor(theme.surface)
                cornerRadii = floatArrayOf(r, r, r, r, 0f, 0f, 0f, 0f)
            }
            setPadding(0, theme.dp(if (wide) 8 else 6), 0, if (wide) theme.dp(8) else insets.bottom + theme.dp(8))
            if (!wide) addView(View(context).apply {
                background = theme.rounded(theme.outline, theme.dpF(2f))
            }, LinearLayout.LayoutParams(theme.dp(36), theme.dp(4)).apply {
                gravity = Gravity.CENTER_HORIZONTAL; bottomMargin = theme.dp(6)
            })
            if (title != null) addView(theme.text(context, title, PxTheme.Type.TITLE).apply {
                setPadding(theme.pad + theme.dp(4), theme.dp(12), theme.pad, theme.dp(8))
                markHeading()
            })
            addView(ScrollView(context).apply {
                isFillViewport = false
                addView(content)
            }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))
        }
        val maxW = theme.dp(maxWidthDp)
        val maxH = (height - insets.top - theme.dp(if (wide) 48 else 24)).coerceAtLeast(theme.dp(200))
        val lp = if (wide) LayoutParams(minOf(maxW, width - theme.dp(48)), LayoutParams.WRAP_CONTENT, Gravity.CENTER)
        else LayoutParams(minOf(width, maxW.coerceAtLeast(width)), LayoutParams.WRAP_CONTENT, Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL)
        scrim.addView(CappedHeight(context, maxH).apply { addView(card) }, lp)
        scrim.setOnClickListener { dismiss(scrim) }
        push(scrim)
        theme.motion.enter(card, dyDp = if (wide) com.paintengine.pxpaint.core.MotionTokens.SHIFT else com.paintengine.pxpaint.core.MotionTokens.SHEET)
        return scrim
    }

    /** Menu anchored to [anchor]; each item closes it. */
    fun menu(anchor: View, items: List<MenuItem>) {
        val scrim = FrameLayout(context).apply { isClickable = true }
        val list = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            background = theme.rounded(theme.raised, theme.radiusM, theme.outline)
            elevation = theme.dpF(8f)
            setPadding(0, theme.dp(6), 0, theme.dp(6))
        }
        for (item in items) {
            val row = LinearLayout(context).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                minimumHeight = theme.touchTarget
                setPadding(theme.dp(14), 0, theme.dp(20), 0)
                background = theme.pressable(null, 0f)
                isEnabled = item.enabled
                alpha = if (item.enabled) 1f else 0.45f
                val color = if (item.destructive) theme.danger else theme.text
                if (item.icon != null) addView(View(context).apply { background = IconDrawable(item.icon, color) },
                    LinearLayout.LayoutParams(theme.dp(20), theme.dp(20)).apply { marginEnd = theme.dp(14) })
                addView(theme.text(context, item.label, PxTheme.Type.BODY, color, lines = 1))
                setOnClickListener {
                    dismiss(scrim)
                    item.action()
                }
            }
            list.addView(row, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        }
        list.measure(MeasureSpec.makeMeasureSpec(width, MeasureSpec.AT_MOST), MeasureSpec.makeMeasureSpec(height, MeasureSpec.AT_MOST))
        val menuW = maxOf(list.measuredWidth, theme.dp(200))
        val loc = IntArray(2)
        anchor.getLocationInWindow(loc)
        val self = IntArray(2)
        getLocationInWindow(self)
        val ax = loc[0] - self[0]
        val ay = loc[1] - self[1]
        val left = (ax + anchor.width - menuW).coerceIn(theme.dp(8), maxOf(theme.dp(8), width - menuW - theme.dp(8)))
        val below = ay + anchor.height + theme.dp(4)
        val top = if (below + list.measuredHeight < height - insets.bottom) below
        else (ay - list.measuredHeight - theme.dp(4)).coerceAtLeast(insets.top + theme.dp(8))
        scrim.addView(list, LayoutParams(menuW, LayoutParams.WRAP_CONTENT).apply { leftMargin = left; topMargin = top })
        scrim.setOnClickListener { dismiss(scrim) }
        push(scrim)
    }

    data class MenuItem(
        val label: String, val icon: Icon? = null, val destructive: Boolean = false, val enabled: Boolean = true,
        val action: () -> Unit,
    )

    fun confirm(title: String, body: String, confirmLabel: String, cancelLabel: String, destructive: Boolean, onConfirm: () -> Unit) {
        val content = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(theme.pad + theme.dp(4), 0, theme.pad + theme.dp(4), theme.dp(8))
            addView(theme.text(context, body, PxTheme.Type.BODY, theme.textSecondary))
        }
        var layer: View? = null
        content.addView(buttonBar(listOf(
            Widgets.pill(context, theme, cancelLabel, false) { layer?.let { dismiss(it) } },
            Widgets.pill(context, theme, confirmLabel, true) { layer?.let { dismiss(it) }; onConfirm() }.apply {
                if (destructive) background = theme.pressable(theme.rounded(theme.danger, theme.dpF(999f)), theme.dpF(999f))
            },
        )))
        layer = sheet(content, title, 440)
    }

    fun prompt(title: String, initial: String, confirmLabel: String, cancelLabel: String, onConfirm: (String) -> Unit) {
        val input = EditText(context).apply {
            setText(initial)
            setSelectAllOnFocus(true)
            isSingleLine = true
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_CAP_SENTENCES
            imeOptions = EditorInfo.IME_ACTION_DONE
            theme.style(this, PxTheme.Type.BODY)
            setHintTextColor(theme.textTertiary)
            background = theme.rounded(theme.sunken, theme.radiusS, theme.outline)
            setPadding(theme.dp(14), theme.dp(12), theme.dp(14), theme.dp(12))
        }
        val content = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(theme.pad + theme.dp(4), 0, theme.pad + theme.dp(4), theme.dp(8))
            addView(input)
        }
        var layer: View? = null
        val confirm = { layer?.let { dismiss(it) }; onConfirm(input.text.toString()) }
        input.setOnEditorActionListener { _, id, _ -> if (id == EditorInfo.IME_ACTION_DONE) { confirm(); true } else false }
        content.addView(buttonBar(listOf(
            Widgets.pill(context, theme, cancelLabel, false) { layer?.let { dismiss(it) } },
            Widgets.pill(context, theme, confirmLabel, true) { confirm() },
        )))
        layer = sheet(content, title, 440)
        input.requestFocus()
        input.postDelayed({
            (context.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager).showSoftInput(input, 0)
        }, 120)
    }

    fun buttonBar(buttons: List<View>): LinearLayout = LinearLayout(context).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.END
        setPadding(0, theme.dp(20), 0, theme.dp(4))
        for ((i, b) in buttons.withIndex()) addView(b, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
            if (i > 0) marginStart = theme.dp(10)
        })
    }

    /** Short, non-blocking message at the bottom (announced by TalkBack). */
    fun message(text: String, long: Boolean = false) {
        handler.removeCallbacks(hideMessage)
        val replacing = messageView != null
        messageView?.let { it.animate().cancel(); removeView(it) }
        val tv = theme.text(context, text, PxTheme.Type.LABEL, if (theme.dark) 0xFF101114.toInt() else 0xFFF4F4F4.toInt()).apply {
            background = theme.rounded(if (theme.dark) 0xFFE9EBEE.toInt() else 0xFF26282C.toInt(), theme.radiusS)
            setPadding(theme.dp(16), theme.dp(12), theme.dp(16), theme.dp(12))
            gravity = Gravity.CENTER
            accessibilityLiveRegion = View.ACCESSIBILITY_LIVE_REGION_POLITE
            elevation = theme.dpF(6f)
        }
        addView(tv, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT, Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL).apply {
            bottomMargin = insets.bottom + theme.dp(96)
            leftMargin = theme.dp(24)
            rightMargin = theme.dp(24)
        })
        messageView = tv
        // A replaced message just swaps (no re-entry flicker on rapid messages).
        if (!replacing) theme.motion.enter(tv, dyDp = com.paintengine.pxpaint.core.MotionTokens.SHIFT, duration = com.paintengine.pxpaint.core.MotionTokens.SHORT)
        handler.postDelayed(hideMessage, if (long) 4500 else 2600)
    }

    private fun push(layer: View) {
        layers += layer
        addView(layer, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT))
        theme.motion.fadeTo(layer, 1f, from = 0f)
        messageView?.bringToFront()
    }

    private fun hideKeyboard() {
        (context.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager).hideSoftInputFromWindow(windowToken, 0)
    }

    /** Wraps a child and limits its height (sheets taller than the window scroll). */
    private class CappedHeight(context: Context, private val maxH: Int) : FrameLayout(context) {
        override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
            super.onMeasure(widthMeasureSpec, MeasureSpec.makeMeasureSpec(maxH, MeasureSpec.AT_MOST))
        }
    }
}

/** Convenience for vertical content blocks inside sheets. */
fun column(context: Context, theme: PxTheme, horizontalPadding: Boolean = true, block: LinearLayout.() -> Unit): LinearLayout =
    LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        if (horizontalPadding) setPadding(theme.pad + theme.dp(4), 0, theme.pad + theme.dp(4), theme.dp(8))
        block()
    }

fun ViewGroup.addWithMargins(v: View, top: Int = 0, bottom: Int = 0) {
    addView(v, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
        topMargin = top
        bottomMargin = bottom
    })
}
