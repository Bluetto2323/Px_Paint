package com.paintengine.pxpaint.ui

import android.graphics.Canvas
import android.graphics.ColorFilter
import android.graphics.Paint
import android.graphics.Path
import android.graphics.PixelFormat
import android.graphics.RectF
import android.graphics.drawable.Drawable

/**
 * PxPaint's own line icons, drawn on a 24-unit grid with round caps
 * (no icon font, no bitmap assets, sharp at any interface size).
 */
enum class Icon {
    PLUS, SEARCH, SORT, SLIDERS, BACK, MORE, MARKER, ERASER, MOVE, LAYERS, UNDO, REDO, FIT, ROTATE, TRASH,
    COPY, EDIT, INFO, CHECK, CLOSE, SWAP, TARGET, CHEVRON_RIGHT, LOGO, CENTER, PIXELS, STAR, STAR_FILLED, RESET, BRUSH,
    EYE, EYE_OFF, LOCK, UNLOCK, ARROW_UP, ARROW_DOWN, MERGE, HISTORY,
    LIBRARY, EYEDROPPER, PIN, FULLSCREEN, FULLSCREEN_EXIT, GRIP, SELECT, FILL, GRADIENT, SMUDGE, LASSO,
}

class IconDrawable(private val icon: Icon, color: Int, private val strokeUnits: Float = 1.8f) : Drawable() {
    private val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeCap = Paint.Cap.ROUND
        strokeJoin = Paint.Join.ROUND
        this.color = color
    }
    private val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL; this.color = color }
    private val path = Path()
    private val r = RectF()

    fun setColor(color: Int) {
        stroke.color = color
        fill.color = color
        invalidateSelf()
    }

    override fun draw(canvas: Canvas) {
        val b = bounds
        if (b.isEmpty) return
        val size = minOf(b.width(), b.height()).toFloat()
        canvas.save()
        canvas.translate(b.left + (b.width() - size) / 2f, b.top + (b.height() - size) / 2f)
        canvas.scale(size / 24f, size / 24f)
        stroke.strokeWidth = strokeUnits
        path.reset()
        build(canvas)
        canvas.restore()
    }

    private fun line(c: Canvas, x0: Float, y0: Float, x1: Float, y1: Float) = c.drawLine(x0, y0, x1, y1, stroke)
    private fun circle(c: Canvas, x: Float, y: Float, rad: Float, filled: Boolean = false) =
        c.drawCircle(x, y, rad, if (filled) fill else stroke)

    private fun poly(c: Canvas, vararg pts: Float, close: Boolean = false) {
        path.reset()
        path.moveTo(pts[0], pts[1])
        var i = 2
        while (i < pts.size) {
            path.lineTo(pts[i], pts[i + 1])
            i += 2
        }
        if (close) path.close()
        c.drawPath(path, stroke)
    }

    private fun roundRect(c: Canvas, l: Float, t: Float, rr: Float, b: Float, rad: Float) {
        r.set(l, t, rr, b)
        c.drawRoundRect(r, rad, rad, stroke)
    }

    private fun arc(c: Canvas, cx: Float, cy: Float, rad: Float, start: Float, sweep: Float) {
        r.set(cx - rad, cy - rad, cx + rad, cy + rad)
        c.drawArc(r, start, sweep, false, stroke)
    }

    private fun build(c: Canvas) {
        when (icon) {
            Icon.PLUS -> { line(c, 12f, 5f, 12f, 19f); line(c, 5f, 12f, 19f, 12f) }
            Icon.SEARCH -> { circle(c, 10.5f, 10.5f, 6f); line(c, 15f, 15f, 20f, 20f) }
            Icon.SORT -> { line(c, 5f, 7f, 19f, 7f); line(c, 7.5f, 12f, 16.5f, 12f); line(c, 10f, 17f, 14f, 17f) }
            Icon.SLIDERS -> {
                line(c, 4f, 8f, 20f, 8f); line(c, 4f, 16f, 20f, 16f)
                circle(c, 9f, 8f, 2.4f, true); circle(c, 15f, 16f, 2.4f, true)
            }
            Icon.BACK -> poly(c, 15f, 5f, 8f, 12f, 15f, 19f)
            Icon.CHEVRON_RIGHT -> poly(c, 9f, 5f, 16f, 12f, 9f, 19f)
            Icon.MORE -> { circle(c, 12f, 5.5f, 1.6f, true); circle(c, 12f, 12f, 1.6f, true); circle(c, 12f, 18.5f, 1.6f, true) }
            Icon.MARKER -> {
                // Chisel marker seen at an angle, with a dab under the tip.
                poly(c, 14.5f, 4.5f, 19.5f, 9.5f, 10.5f, 18.5f, 6.5f, 18.5f, 5.5f, 13.5f, close = true)
                line(c, 12.5f, 6.5f, 17.5f, 11.5f)
                line(c, 4f, 21f, 9f, 21f)
            }
            Icon.ERASER -> {
                poly(c, 13f, 4.5f, 20f, 11.5f, 12f, 19.5f, 8f, 19.5f, 4.5f, 16f, close = true)
                line(c, 8.5f, 9f, 15.5f, 16f)
                line(c, 12f, 19.5f, 20f, 19.5f)
            }
            Icon.LASSO -> {
                // A rope loop with its tail.
                path.reset(); path.addOval(3.5f, 4f, 20.5f, 15f, android.graphics.Path.Direction.CW); c.drawPath(path, stroke)
                path.reset(); path.moveTo(7.5f, 14.2f); path.cubicTo(5f, 16.5f, 8.5f, 18.5f, 7f, 21f); c.drawPath(path, stroke)
                c.drawCircle(7.6f, 14.4f, 1.3f, stroke)
            }
            Icon.SMUDGE -> {
                // A fingertip pressing down, with the smear it leaves.
                path.reset(); path.moveTo(9f, 3.5f); path.lineTo(9f, 12.5f)
                path.quadTo(9f, 16.5f, 12f, 16.5f); path.quadTo(15f, 16.5f, 15f, 12.5f); path.lineTo(15f, 3.5f)
                c.drawPath(path, stroke)
                path.reset(); path.moveTo(10.8f, 13.2f); path.quadTo(12f, 14.6f, 13.2f, 13.2f); c.drawPath(path, stroke)
                path.reset(); path.moveTo(3.5f, 20f); path.cubicTo(7f, 17.5f, 9f, 22f, 12f, 20f); path.cubicTo(15f, 18f, 17f, 22.5f, 20.5f, 20f)
                c.drawPath(path, stroke)
            }
            Icon.MOVE -> {
                line(c, 12f, 3.5f, 12f, 20.5f); line(c, 3.5f, 12f, 20.5f, 12f)
                poly(c, 9.5f, 6f, 12f, 3.5f, 14.5f, 6f); poly(c, 9.5f, 18f, 12f, 20.5f, 14.5f, 18f)
                poly(c, 6f, 9.5f, 3.5f, 12f, 6f, 14.5f); poly(c, 18f, 9.5f, 20.5f, 12f, 18f, 14.5f)
            }
            Icon.FILL -> {
                // Paint bucket tipped over, with a drop.
                poly(c, 4.5f, 11f, 11f, 4.5f, 18f, 11.5f, 11.5f, 18f, close = true)
                line(c, 4.5f, 11f, 18f, 11.5f)
                path.reset(); path.moveTo(20f, 14f); path.cubicTo(21.5f, 16.5f, 21.5f, 18.5f, 20f, 18.5f)
                path.cubicTo(18.5f, 18.5f, 18.5f, 16.5f, 20f, 14f); c.drawPath(path, fill)
            }
            Icon.GRADIENT -> {
                // A square whose stripes fade from solid to light.
                poly(c, 4f, 4f, 20f, 4f, 20f, 20f, 4f, 20f, close = true)
                for ((k, x) in listOf(7f, 10f, 13f).withIndex()) {
                    stroke.alpha = 255 - k * 70
                    line(c, x, 5.5f, x, 18.5f)
                }
                stroke.alpha = 255
            }
            Icon.SELECT -> {
                // Dashed selection rectangle with a pointer at its corner.
                line(c, 4f, 4f, 7f, 4f); line(c, 10.5f, 4f, 13.5f, 4f); line(c, 17f, 4f, 20f, 4f)
                line(c, 4f, 4f, 4f, 7f); line(c, 4f, 10.5f, 4f, 13.5f); line(c, 4f, 17f, 4f, 20f)
                line(c, 20f, 7.5f, 20f, 10f); line(c, 7.5f, 20f, 10f, 20f)
                poly(c, 13f, 13f, 20.5f, 16f, 17.2f, 17.2f, 16f, 20.5f, close = true)
            }
            Icon.LAYERS -> {
                poly(c, 12f, 4f, 20.5f, 8.5f, 12f, 13f, 3.5f, 8.5f, close = true)
                poly(c, 3.5f, 12.5f, 12f, 17f, 20.5f, 12.5f)
                poly(c, 3.5f, 16f, 12f, 20.5f, 20.5f, 16f)
            }
            Icon.UNDO -> { poly(c, 8.5f, 5f, 4.5f, 9f, 8.5f, 13f); path.reset(); path.moveTo(4.5f, 9f); path.lineTo(14f, 9f); path.cubicTo(17.5f, 9f, 20f, 11.5f, 20f, 14.5f); path.cubicTo(20f, 17.5f, 17.5f, 19.5f, 14f, 19.5f); path.lineTo(9f, 19.5f); c.drawPath(path, stroke) }
            Icon.REDO -> { poly(c, 15.5f, 5f, 19.5f, 9f, 15.5f, 13f); path.reset(); path.moveTo(19.5f, 9f); path.lineTo(10f, 9f); path.cubicTo(6.5f, 9f, 4f, 11.5f, 4f, 14.5f); path.cubicTo(4f, 17.5f, 6.5f, 19.5f, 10f, 19.5f); path.lineTo(15f, 19.5f); c.drawPath(path, stroke) }
            Icon.FIT -> {
                poly(c, 4f, 9f, 4f, 4f, 9f, 4f); poly(c, 15f, 4f, 20f, 4f, 20f, 9f)
                poly(c, 20f, 15f, 20f, 20f, 15f, 20f); poly(c, 9f, 20f, 4f, 20f, 4f, 15f)
                roundRect(c, 8f, 8f, 16f, 16f, 1.5f)
            }
            Icon.ROTATE -> { arc(c, 12f, 12.5f, 7f, -60f, 300f); poly(c, 15.5f, 3.5f, 15.5f, 6.8f, 19f, 6.8f) }
            Icon.TRASH -> {
                line(c, 4.5f, 6.5f, 19.5f, 6.5f); poly(c, 9f, 6.5f, 9.5f, 4f, 14.5f, 4f, 15f, 6.5f)
                poly(c, 6.5f, 6.5f, 7.5f, 20f, 16.5f, 20f, 17.5f, 6.5f); line(c, 10.5f, 10f, 10.5f, 16.5f); line(c, 13.5f, 10f, 13.5f, 16.5f)
            }
            Icon.COPY -> { roundRect(c, 8.5f, 8.5f, 19.5f, 19.5f, 2f); poly(c, 15.5f, 8.5f, 15.5f, 4.5f, 4.5f, 4.5f, 4.5f, 15.5f, 8.5f, 15.5f) }
            Icon.EDIT -> { poly(c, 15f, 4.5f, 19.5f, 9f, 9f, 19.5f, 4.5f, 19.5f, 4.5f, 15f, close = true); line(c, 12.5f, 7f, 17f, 11.5f) }
            Icon.INFO -> { circle(c, 12f, 12f, 8.5f); line(c, 12f, 11f, 12f, 16.5f); circle(c, 12f, 7.8f, 1.2f, true) }
            Icon.CHECK -> poly(c, 5f, 12.5f, 10f, 17.5f, 19f, 7f)
            Icon.CLOSE -> { line(c, 6f, 6f, 18f, 18f); line(c, 18f, 6f, 6f, 18f) }
            Icon.SWAP -> { poly(c, 7f, 4f, 4f, 7f, 7f, 10f); line(c, 4f, 7f, 18f, 7f); poly(c, 17f, 14f, 20f, 17f, 17f, 20f); line(c, 20f, 17f, 6f, 17f) }
            Icon.TARGET -> { circle(c, 12f, 12f, 6.5f); line(c, 12f, 2.5f, 12f, 7f); line(c, 12f, 17f, 12f, 21.5f); line(c, 2.5f, 12f, 7f, 12f); line(c, 17f, 12f, 21.5f, 12f) }
            Icon.CENTER -> { circle(c, 12f, 12f, 2.2f, true); roundRect(c, 4f, 4f, 20f, 20f, 3f) }
            Icon.PIXELS -> { roundRect(c, 4f, 4f, 11f, 11f, 1f); roundRect(c, 13f, 4f, 20f, 11f, 1f); roundRect(c, 4f, 13f, 11f, 20f, 1f); roundRect(c, 13f, 13f, 20f, 20f, 1f) }
            Icon.LOGO -> drawLogo(c)
            Icon.STAR, Icon.STAR_FILLED -> {
                path.reset()
                for (i in 0 until 10) {
                    val a = Math.toRadians(-90.0 + i * 36.0)
                    val rad = if (i % 2 == 0) 8.5 else 3.8
                    val x = (12 + rad * Math.cos(a)).toFloat()
                    val y = (12.8 + rad * Math.sin(a)).toFloat()
                    if (i == 0) path.moveTo(x, y) else path.lineTo(x, y)
                }
                path.close()
                if (icon == Icon.STAR_FILLED) c.drawPath(path, fill)
                c.drawPath(path, stroke)
            }
            Icon.EYE, Icon.EYE_OFF -> {
                path.reset()
                path.moveTo(2.5f, 12f)
                path.cubicTo(5.5f, 6.5f, 18.5f, 6.5f, 21.5f, 12f)
                path.cubicTo(18.5f, 17.5f, 5.5f, 17.5f, 2.5f, 12f)
                c.drawPath(path, stroke)
                circle(c, 12f, 12f, 2.6f)
                if (icon == Icon.EYE_OFF) line(c, 4.5f, 4.5f, 19.5f, 19.5f)
            }
            Icon.LOCK, Icon.UNLOCK -> {
                roundRect(c, 5.5f, 11f, 18.5f, 20f, 2f)
                if (icon == Icon.LOCK) arc(c, 12f, 9.5f, 4f, 180f, 180f) else arc(c, 15.5f, 8.5f, 3.5f, 180f, 160f)
                if (icon == Icon.LOCK) { line(c, 8f, 9.5f, 8f, 11f); line(c, 16f, 9.5f, 16f, 11f) } else line(c, 12f, 8.5f, 12f, 11f)
            }
            Icon.ARROW_UP -> { line(c, 12f, 19f, 12f, 5f); poly(c, 6.5f, 10.5f, 12f, 5f, 17.5f, 10.5f) }
            Icon.ARROW_DOWN -> { line(c, 12f, 5f, 12f, 19f); poly(c, 6.5f, 13.5f, 12f, 19f, 17.5f, 13.5f) }
            Icon.MERGE -> {
                poly(c, 12f, 3.5f, 20f, 7.5f, 12f, 11.5f, 4f, 7.5f, close = true)
                line(c, 12f, 13.5f, 12f, 20.5f)
                poly(c, 8.5f, 17f, 12f, 20.5f, 15.5f, 17f)
            }
            Icon.LIBRARY -> {
                // Brush library: three brushes standing in a cup.
                roundRect(c, 6f, 13f, 18f, 20.5f, 2f)
                line(c, 9f, 13f, 7.5f, 4.5f)
                line(c, 12f, 13f, 12f, 3.5f)
                line(c, 15f, 13f, 16.5f, 4.5f)
            }
            Icon.EYEDROPPER -> {
                // Pipette: bulb top right, glass tube down to a tip bottom left.
                poly(c, 14.5f, 6.5f, 17.5f, 9.5f)
                circle(c, 17f, 7f, 2.6f)
                poly(c, 15.8f, 8.2f, 7f, 17f, 5f, 19f, 7f, 17f)
                line(c, 13.6f, 6.4f, 17.6f, 10.4f)
                line(c, 7f, 17f, 9.5f, 17f)
            }
            Icon.PIN -> {
                poly(c, 9f, 4f, 15f, 4f)
                poly(c, 10f, 4f, 10f, 10f, 7f, 13.5f, 17f, 13.5f, 14f, 10f, 14f, 4f)
                line(c, 12f, 13.5f, 12f, 20f)
            }
            Icon.FULLSCREEN -> {
                poly(c, 4f, 9f, 4f, 4f, 9f, 4f); poly(c, 15f, 4f, 20f, 4f, 20f, 9f)
                poly(c, 20f, 15f, 20f, 20f, 15f, 20f); poly(c, 9f, 20f, 4f, 20f, 4f, 15f)
            }
            Icon.FULLSCREEN_EXIT -> {
                poly(c, 4f, 9f, 9f, 9f, 9f, 4f); poly(c, 15f, 4f, 15f, 9f, 20f, 9f)
                poly(c, 20f, 15f, 15f, 15f, 15f, 20f); poly(c, 9f, 20f, 9f, 15f, 4f, 15f)
            }
            Icon.GRIP -> {
                line(c, 10f, 6f, 10f, 18f); line(c, 14f, 6f, 14f, 18f)
            }
            Icon.HISTORY -> {
                // Clock face with a counter-clockwise arrow: going back in time.
                arc(c, 12f, 12f, 8f, 200f, 290f)
                poly(c, 3.2f, 6.8f, 4.6f, 10.4f, 8.2f, 9.2f)
                poly(c, 12f, 7.5f, 12f, 12f, 15f, 14f)
            }
            Icon.RESET -> { arc(c, 12f, 12.5f, 7f, 120f, 300f); poly(c, 4.2f, 8.5f, 5.6f, 12.3f, 9.2f, 10.8f) }
            Icon.BRUSH -> {
                // Round brush: handle, ferrule, tip.
                line(c, 19.5f, 4.5f, 12.5f, 11.5f)
                poly(c, 12.5f, 11.5f, 14.5f, 13.5f, 11.5f, 16.5f, 9.5f, 14.5f, close = true)
                path.reset(); path.moveTo(9.5f, 14.5f); path.cubicTo(6f, 15f, 6.5f, 19f, 4f, 20f)
                path.cubicTo(8f, 20.5f, 11f, 19f, 11.5f, 16.5f); c.drawPath(path, stroke)
            }
        }
    }

    /** PxPaint mark: a rounded "pixel" tile crossed by a sweeping stroke. */
    /** The PxPaint mark: a brush gesture drawing a "P" (stem, one sweep for the bowl, a drop). */
    private fun drawLogo(c: Canvas) {
        path.reset()
        path.moveTo(7.2f, 4.6f); path.cubicTo(6.6f, 10f, 7.4f, 15f, 6.4f, 20.6f)
        c.drawPath(path, stroke)
        path.reset()
        path.moveTo(7.4f, 5f); path.cubicTo(12f, 2.6f, 19.6f, 4f, 19.4f, 9.4f); path.cubicTo(19.2f, 14.2f, 13.6f, 15.6f, 9.4f, 13.8f)
        c.drawPath(path, stroke)
        circle(c, 18f, 18.4f, 1.5f, true)
    }

    override fun setAlpha(alpha: Int) {
        stroke.alpha = alpha
        fill.alpha = alpha
        invalidateSelf()
    }

    override fun setColorFilter(colorFilter: ColorFilter?) {
        stroke.colorFilter = colorFilter
        fill.colorFilter = colorFilter
        invalidateSelf()
    }

    @Deprecated("Deprecated in Java")
    override fun getOpacity(): Int = PixelFormat.TRANSLUCENT
}
