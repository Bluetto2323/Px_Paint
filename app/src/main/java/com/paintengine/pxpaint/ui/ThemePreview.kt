package com.paintengine.pxpaint.ui

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Typeface
import android.view.View
import com.paintengine.pxpaint.core.DesignTokens
import com.paintengine.pxpaint.core.ThemeMode
import com.paintengine.pxpaint.core.tr

/**
 * A miniature of the workspace drawn with a theme's real tokens (Settings →
 * Theme): the workspace backdrop, a white canvas with a stroke (artwork
 * colours never change with the theme), the floating glass controls, and the
 * accent on the active tool. Not a picture: it is drawn from the same
 * ColorTokens the interface uses.
 */
@SuppressLint("ViewConstructor")
class ThemePreview(context: Context, private val ui: PxTheme, val mode: ThemeMode, selected: Boolean) : View(context) {
    private val c = DesignTokens.colors(mode, ui.settings.highContrast)
    private val fill = Paint(Paint.ANTI_ALIAS_FLAG)
    private val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeCap = Paint.Cap.ROUND }
    private val label = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        textSize = ui.dpF(12f)
        typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
        textAlign = Paint.Align.CENTER
    }
    private val r = RectF()
    private val name = tr(mode.label)

    init {
        isSelected = selected
        isClickable = true
        isFocusable = true
        contentDescription = name
        minimumHeight = ui.dp(132)
    }

    override fun onDraw(canvas: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        val rad = ui.dpF(14f)
        val inset = ui.dpF(3f)
        // Card: the theme's background.
        r.set(inset, inset, w - inset, h - inset)
        fill.color = c.workspace
        canvas.drawRoundRect(r, rad, rad, fill)
        // The canvas (artwork stays white whatever the theme) with a stroke on it.
        val cl = w * 0.2f; val ct = h * 0.14f; val cr = w * 0.8f; val cb = h * 0.62f
        fill.color = 0xFFFFFFFF.toInt()
        r.set(cl, ct, cr, cb)
        canvas.drawRect(r, fill)
        stroke.strokeWidth = ui.dpF(3f)
        stroke.color = 0xFF2E5C8A.toInt()
        canvas.drawLine(cl + (cr - cl) * 0.18f, ct + (cb - ct) * 0.7f, cl + (cr - cl) * 0.8f, ct + (cb - ct) * 0.3f, stroke)
        // Floating glass capsule with tools; the active one in the accent colour.
        val bw = w * 0.62f; val bh = ui.dpF(18f)
        val bl = (w - bw) / 2; val bt = h * 0.66f
        r.set(bl, bt, bl + bw, bt + bh)
        fill.color = c.chrome
        canvas.drawRoundRect(r, bh / 2, bh / 2, fill)
        stroke.strokeWidth = ui.dpF(1f)
        stroke.color = c.edge
        canvas.drawRoundRect(r, bh / 2, bh / 2, stroke)
        for (i in 0 until 5) {
            val cx = bl + bw * (i + 0.5f) / 5
            fill.color = if (i == 0) c.accent else c.textSecondary
            canvas.drawCircle(cx, bt + bh / 2, ui.dpF(if (i == 0) 3.2f else 2.4f), fill)
        }
        // Name, in the theme's own text colour.
        label.color = c.text
        canvas.drawText(name, w / 2, h - ui.dpF(12f), label)
        // Selection ring in the CURRENT interface's accent.
        if (isSelected) {
            stroke.color = ui.accent
            stroke.strokeWidth = ui.dpF(2.5f)
            r.set(inset, inset, w - inset, h - inset)
            canvas.drawRoundRect(r, rad, rad, stroke)
        } else {
            stroke.color = ui.outline
            stroke.strokeWidth = ui.dpF(1f)
            r.set(inset, inset, w - inset, h - inset)
            canvas.drawRoundRect(r, rad, rad, stroke)
        }
    }
}
