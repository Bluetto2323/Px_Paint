package com.paintengine.pxpaint.screens

import android.content.Context
import android.graphics.Typeface
import android.text.InputFilter
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.view.inputmethod.EditorInfo
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import com.paintengine.pxpaint.core.ColorMath
import com.paintengine.pxpaint.core.DesignTokens
import com.paintengine.pxpaint.core.S
import com.paintengine.pxpaint.core.tr
import com.paintengine.pxpaint.ui.ColorWheel
import com.paintengine.pxpaint.ui.FlowRow
import com.paintengine.pxpaint.ui.Icon
import com.paintengine.pxpaint.ui.IconButton
import com.paintengine.pxpaint.ui.PxSlider
import com.paintengine.pxpaint.ui.PxTheme
import com.paintengine.pxpaint.ui.Widgets
import kotlin.math.roundToInt

/**
 * The colour workspace: wheel (hue ring + saturation / brightness square),
 * current and previous colour, HSV or RGB sliders with values, HEX input,
 * recent colours, the saved palette and the eyedropper. Colours are straight
 * sRGB ARGB, exactly what the brush paints.
 */
class ColorPanel(private val host: PxHost, private val ops: Ops, private val state: State) {
    interface Ops {
        val color: Int
        /** The colour before this panel was opened (tap to go back to it). */
        val previous: Int
        val recent: List<Int>
        val palette: List<Int>
        /** Live while dragging ([final] = false), then once more when released. */
        fun pick(color: Int, final: Boolean)
        fun eyedropper()
        fun save(color: Int)
        fun remove(color: Int)
    }

    class State {
        var rgb = false
    }

    private val context: Context get() = host.context
    private val theme: PxTheme get() = host.theme
    private var h = 0f
    private var s = 0f
    private var v = 0f
    private var syncing = false
    private lateinit var wheel: ColorWheel
    private lateinit var current: View
    private lateinit var hex: EditText
    private lateinit var hexError: TextView
    private val sliders = ArrayList<Triple<PxSlider, TextView, Int>>()   // slider, value, channel

    private val color: Int get() = ColorMath.hsvToArgb(h, s, v)

    fun view(): View = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        ColorMath.argbToHsv(ops.color).let { h = it[0]; s = it[1]; v = it[2] }
        // Header: title, eyedropper, previous | current.
        addView(LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(theme.text(context, tr(S.COLOR), PxTheme.Type.HEADLINE), LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            addView(IconButton(context, theme, Icon.EYEDROPPER, "${tr(S.EYEDROPPER)}: ${tr(S.EYEDROPPER_HINT)}").apply {
                setOnClickListener { ops.eyedropper() }
            })
            val pair = LinearLayout(context).apply {
                orientation = LinearLayout.HORIZONTAL
                background = theme.rounded(theme.surface, theme.radiusS, theme.outline)
                setPadding(theme.dp(3), theme.dp(3), theme.dp(3), theme.dp(3))
            }
            val prev = View(context).apply {
                background = theme.rounded(ops.previous, theme.dpF(7f))
                contentDescription = "${tr(S.COLOR_PREVIOUS)} ${ColorMath.toHex(ops.previous)}"
                isClickable = true
                setOnClickListener { setColor(ops.previous, true) }
            }
            current = View(context).apply {
                background = theme.rounded(ops.color, theme.dpF(7f))
                contentDescription = "${tr(S.COLOR_CURRENT)} ${ColorMath.toHex(ops.color)}"
                importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_YES
            }
            pair.addView(prev, LinearLayout.LayoutParams(theme.touchTarget, theme.touchTarget))
            pair.addView(current, LinearLayout.LayoutParams(theme.touchTarget, theme.touchTarget).apply { marginStart = theme.dp(3) })
            addView(pair, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                marginStart = theme.dp(6)
            })
        })
        wheel = ColorWheel(context, theme, tr(S.COLOR_WHEEL)).apply {
            setHsv(h, s, v)
            onChange = { hh, ss, vv, final ->
                h = hh; s = ss; v = vv
                refresh(fromWheel = true)
                ops.pick(color, final)
            }
        }
        addView(wheel, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
            topMargin = theme.dp(10); bottomMargin = theme.dp(6)
        })
        // HSV / RGB values.
        addView(Widgets.segmented(context, theme, listOf(false to tr(S.COLOR_HSV), true to tr(S.COLOR_RGB)), state.rgb) {
            state.rgb = it
            rebuildSliders()
        })
        addView(slidersBox, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        rebuildSliders()
        // HEX
        addView(LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, theme.dp(8), 0, 0)
            addView(theme.text(context, tr(S.COLOR_HEX), PxTheme.Type.LABEL), LinearLayout.LayoutParams(theme.dp(56), LinearLayout.LayoutParams.WRAP_CONTENT))
            hex = EditText(context).apply {
                theme.style(this, PxTheme.Type.BODY)
                typeface = Typeface.MONOSPACE
                setText(ColorMath.toHex(color))
                isSingleLine = true
                inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS or InputType.TYPE_TEXT_FLAG_CAP_CHARACTERS
                imeOptions = EditorInfo.IME_ACTION_DONE
                filters = arrayOf(InputFilter.LengthFilter(7))
                minHeight = theme.touchTarget
                setPadding(theme.dp(12), 0, theme.dp(12), 0)
                background = theme.rounded(theme.surface, theme.radiusS, theme.outline)
                contentDescription = tr(S.COLOR_HEX)
                setOnEditorActionListener { _, _, _ -> applyHex(); true }
                setOnFocusChangeListener { _, has -> if (!has) applyHex() }
            }
            addView(hex, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        })
        hexError = theme.text(context, "", PxTheme.Type.CAPTION, theme.danger).apply { visibility = View.GONE }
        addView(hexError)
        // Recent
        if (ops.recent.isNotEmpty()) {
            addView(theme.text(context, tr(S.COLOR_RECENT), PxTheme.Type.LABEL, theme.textSecondary).apply { setPadding(0, theme.dp(14), 0, theme.dp(6)) })
            addView(FlowRow(context, theme.dp(6)).apply {
                for (c in ops.recent) addView(swatch(c, null))
            })
        }
        // Palette
        addView(LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, theme.dp(10), 0, 0)
            addView(theme.text(context, tr(S.COLOR_PALETTE), PxTheme.Type.LABEL, theme.textSecondary),
                LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            addView(IconButton(context, theme, Icon.PLUS, tr(S.COLOR_PALETTE_ADD), sizeDp = 34f).apply {
                setOnClickListener { ops.save(color) }
            })
        })
        if (ops.palette.isEmpty()) {
            addView(theme.text(context, tr(S.COLOR_PALETTE_EMPTY), PxTheme.Type.CAPTION, theme.textSecondary))
        } else {
            addView(FlowRow(context, theme.dp(6)).apply {
                for (c in ops.palette) addView(swatch(c) { ops.remove(c) })
            })
        }
    }

    private val slidersBox: LinearLayout by lazy { LinearLayout(context).apply { orientation = LinearLayout.VERTICAL } }

    private fun rebuildSliders() {
        slidersBox.removeAllViews()
        sliders.clear()
        val names = if (state.rgb) listOf(S.COLOR_RED, S.COLOR_GREEN, S.COLOR_BLUE) else listOf(S.COLOR_HUE, S.COLOR_SATURATION, S.COLOR_BRIGHTNESS)
        for ((i, n) in names.withIndex()) {
            val value = theme.text(context, "", PxTheme.Type.LABEL, theme.textSecondary).apply {
                gravity = Gravity.END
                typeface = Typeface.MONOSPACE
            }
            val slider = PxSlider(context, theme, tr(n)).apply {
                valueText = { f -> channelText(i, f) }
                onChange = { f, fromUser ->
                    value.text = channelText(i, f)
                    if (fromUser && !syncing) fromSlider(i, f)
                }
            }
            slidersBox.addView(LinearLayout(context).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                addView(theme.text(context, tr(n), PxTheme.Type.LABEL), LinearLayout.LayoutParams(theme.dp(84), LinearLayout.LayoutParams.WRAP_CONTENT))
                addView(slider, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
                addView(value, LinearLayout.LayoutParams(theme.dp(44), LinearLayout.LayoutParams.WRAP_CONTENT))
            })
            sliders += Triple(slider, value, i)
        }
        refresh(fromWheel = false)
    }

    private fun channelText(i: Int, f: Float): String = when {
        state.rgb -> "${(f * 255).roundToInt()}"
        i == 0 -> "${(f * 360).roundToInt() % 360}°"
        else -> "${(f * 100).roundToInt()}%"
    }

    private fun fromSlider(i: Int, f: Float) {
        if (state.rgb) {
            val c = color
            val ch = (f * 255).roundToInt()
            val n = when (i) {
                0 -> ColorMath.rgb(ch, ColorMath.green(c), ColorMath.blue(c))
                1 -> ColorMath.rgb(ColorMath.red(c), ch, ColorMath.blue(c))
                else -> ColorMath.rgb(ColorMath.red(c), ColorMath.green(c), ch)
            }
            val hsv = ColorMath.argbToHsv(n)
            if (hsv[1] > 0f) h = hsv[0]       // grey keeps the hue the user had
            s = hsv[1]; v = hsv[2]
        } else when (i) {
            0 -> h = (f * 360f).coerceAtMost(359.9f)
            1 -> s = f
            else -> v = f
        }
        refresh(fromWheel = false)
        ops.pick(color, true)
    }

    private fun setColor(c: Int, final: Boolean) {
        ColorMath.argbToHsv(c).let { h = it[0]; s = it[1]; v = it[2] }
        refresh(fromWheel = false)
        ops.pick(color, final)
    }

    private fun applyHex() {
        val t = hex.text?.toString() ?: return
        val c = ColorMath.parseHex(t)
        if (c == null) {
            hexError.text = tr(S.COLOR_HEX_INVALID)
            hexError.visibility = View.VISIBLE
            return
        }
        hexError.visibility = View.GONE
        if (c != color) setColor(c, true)
    }

    private fun refresh(fromWheel: Boolean) {
        syncing = true
        val c = color
        if (!fromWheel) wheel.setHsv(h, s, v)
        current.background = theme.rounded(c, theme.dpF(7f))
        current.contentDescription = "${tr(S.COLOR_CURRENT)} ${ColorMath.toHex(c)}"
        for ((slider, value, i) in sliders) {
            val f = if (state.rgb) when (i) {
                0 -> ColorMath.red(c) / 255f
                1 -> ColorMath.green(c) / 255f
                else -> ColorMath.blue(c) / 255f
            } else when (i) {
                0 -> h / 360f
                1 -> s
                else -> v
            }
            slider.setValue(f)
            value.text = channelText(i, f)
        }
        if (::hex.isInitialized && !hex.hasFocus()) hex.setText(ColorMath.toHex(c))
        syncing = false
    }

    /** A colour swatch: tap = use it; long-press = [onLong] (palette: remove). */
    private fun swatch(c: Int, onLong: (() -> Unit)?): View = FrameLayout(context).apply {
        val box = theme.touchTarget - theme.dp(4)
        layoutParams = android.view.ViewGroup.LayoutParams(box, box)
        isClickable = true
        contentDescription = tr(S.COLOR_SWATCH, ColorMath.toHex(c)) + if (onLong != null) ", ${tr(S.COLOR_PALETTE_REMOVE)}" else ""
        background = theme.pressable(null, theme.radiusS)
        setOnClickListener { setColor(c, true) }
        if (onLong != null) setOnLongClickListener {
            host.overlays.confirm(tr(S.COLOR_PALETTE_REMOVE), ColorMath.toHex(c), tr(S.ACTION_DELETE), tr(S.ACTION_CANCEL), true) { onLong() }
            true
        }
        addView(View(context).apply { background = theme.rounded(c, theme.dpF(8f), theme.outline) },
            FrameLayout.LayoutParams(theme.dp(DesignTokens.SWATCH), theme.dp(DesignTokens.SWATCH), Gravity.CENTER))
    }
}
