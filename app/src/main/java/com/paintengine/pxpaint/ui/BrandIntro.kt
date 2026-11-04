package com.paintengine.pxpaint.ui

import android.animation.ValueAnimator
import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.graphics.PathMeasure
import android.graphics.RadialGradient
import android.graphics.RectF
import android.graphics.Shader
import android.graphics.Typeface
import android.os.SystemClock
import android.view.Gravity
import android.view.View
import android.view.animation.LinearInterpolator
import android.widget.FrameLayout
import com.paintengine.pxpaint.core.IntroTimeline
import com.paintengine.pxpaint.core.MotionTokens
import com.paintengine.pxpaint.core.S
import com.paintengine.pxpaint.core.tr

/**
 * PxPaint startup animation, drawn over the gallery while the app starts.
 *
 * It continues Android's own splash screen seamlessly: same background and
 * the official mark (res/drawable/ic_launcher_foreground.xml: the rounded
 * "pixel" tile, the ember stroke, its dot) at the same size and place as the
 * system splash icon (240 dp for the 108-unit vector). Timeline
 * (core/IntroTimeline, ~1.2 s): the tile fades in, the stroke draws through
 * it, the wordmark rises, a soft light runs along the stroke with a few
 * sparks. It never holds the app back: [ready] starts the exit at once,
 * whatever the phase; if the app is not ready when the reveal ends, a
 * loading ring appears. Motion OFF: a still frame, removed when ready.
 * Drawing allocates nothing per frame (paths, paints, shader made once).
 */
@SuppressLint("ViewConstructor")
class BrandIntro(context: Context, private val theme: PxTheme, private val onDone: () -> Unit) : FrameLayout(context) {
    private val m = theme.motion
    private val startMs = SystemClock.uptimeMillis()
    private var readyAt: Long? = null
    private var finished = false
    private val art = Art()
    private val ring = ProgressIndicator(context, theme, sizeDp = 22f, strokeDp = 2.5f).apply { alpha = 0f; visibility = INVISIBLE }
    private val clock = ValueAnimator.ofFloat(0f, 1f).apply {
        duration = MotionTokens.SPLASH_END
        interpolator = LinearInterpolator()
        addUpdateListener { art.invalidate() }
        addListener(object : android.animation.AnimatorListenerAdapter() {
            override fun onAnimationEnd(animation: android.animation.Animator) {
                if (readyAt == null && !finished) showLoading()
            }
        })
    }

    init {
        setBackgroundColor(theme.bg)
        isClickable = true            // the gallery under it is not touched while it shows
        importantForAccessibility = IMPORTANT_FOR_ACCESSIBILITY_NO_HIDE_DESCENDANTS
        addView(art, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT))
        addView(ring, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT, Gravity.CENTER).apply {
            topMargin = theme.dp(170)
        })
        if (m.enabled) clock.start()
    }

    /** Elapsed time of the reveal (the end state when motion is off). */
    private fun t(): Long = if (!m.enabled) MotionTokens.SPLASH_END else SystemClock.uptimeMillis() - startMs

    private fun showLoading() {
        ring.visibility = VISIBLE
        m.fadeTo(ring, 1f, from = 0f)
        android.util.Log.i("PxPaint", "Intro: app not ready after ${t()} ms, loading state shown")
    }

    /** The app can be used: leave now (never waits for the reveal to finish). */
    fun ready() {
        if (readyAt != null) return
        readyAt = t()
        android.util.Log.i("PxPaint", "Intro: app ready at ${readyAt} ms, phase ${IntroTimeline.phase(readyAt!!, null)}")
        clock.cancel()
        if (!m.enabled) {
            finish()
            return
        }
        animate().alpha(0f).setDuration(m.d(MotionTokens.SPLASH_EXIT)).setInterpolator(Motion.accelerate)
            .withEndAction { finish() }.start()
        if (m.movement) art.animate().scaleX(0.94f).scaleY(0.94f).setDuration(m.d(MotionTokens.SPLASH_EXIT)).setInterpolator(Motion.accelerate).start()
    }

    private fun finish() {
        if (finished) return
        finished = true
        ring.visibility = GONE
        (parent as? android.view.ViewGroup)?.removeView(this)
        onDone()
    }

    override fun onDetachedFromWindow() {
        clock.cancel()
        super.onDetachedFromWindow()
    }

    /** The mark and wordmark, drawn from the launcher vector's geometry. */
    private inner class Art : View(context) {
        private val unit = theme.dpF(240f) / 108f      // system splash icon size / vector viewport
        private val tilePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            style = Paint.Style.STROKE; strokeWidth = 4.5f * unit; color = theme.text
        }
        private val strokePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            style = Paint.Style.STROKE; strokeWidth = 5.5f * unit; strokeCap = Paint.Cap.ROUND; color = theme.accent
        }
        private val dotPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = theme.accent }
        private val sparkPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = theme.accent }
        private val lightPaint = Paint(Paint.ANTI_ALIAS_FLAG)
        private val wordPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = theme.text
            textAlign = Paint.Align.CENTER
            textSize = theme.dpF(30f)
            typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
            letterSpacing = 0.02f
        }
        private val tile = RectF()
        private val stroke = Path()
        private val partial = Path()
        private val measure = PathMeasure()
        private val pos = FloatArray(2)
        private var length = 0f
        private var cx = 0f
        private var cy = 0f
        private val lightRadius = theme.dpF(34f)
        private val light = RadialGradient(0f, 0f, lightRadius,
            intArrayOf((theme.accent and 0x00FFFFFF) or 0x66000000, (theme.accent and 0x00FFFFFF) or 0x22000000, 0),
            floatArrayOf(0f, 0.45f, 1f), Shader.TileMode.CLAMP)
        // A few sparks along the stroke: fraction along it, sideways offset (units), size (dp).
        private val sparks = floatArrayOf(0.18f, -6f, 1.6f, 0.34f, 5f, 2.2f, 0.52f, -4f, 1.4f, 0.68f, 6f, 1.9f, 0.84f, -5f, 1.5f, 0.95f, 3f, 2.0f)

        init {
            lightPaint.shader = light
        }

        override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
            cx = w / 2f
            cy = h / 2f
            fun x(u: Float) = cx + (u - 54f) * unit
            fun y(u: Float) = cy + (u - 54f) * unit
            tile.set(x(32.75f), y(32.75f), x(75.25f), y(75.25f))
            stroke.reset()
            stroke.moveTo(x(40.25f), y(65.25f))
            stroke.cubicTo(x(46.5f), y(47.75f), x(57.75f), y(64f), x(67.75f), y(41.5f))
            measure.setPath(stroke, false)
            length = measure.length
        }

        private fun alpha(f: Float) = (f.coerceIn(0f, 1f) * 255).toInt()

        override fun onDraw(canvas: Canvas) {
            val t = t()
            val move = m.movement
            // 1. The tile fades in (and settles from 96 %).
            val sym = IntroTimeline.segment(t, MotionTokens.SPLASH_SYMBOL, MotionTokens.SPLASH_SYMBOL_END)
            tilePaint.alpha = alpha(sym)
            canvas.save()
            if (move) {
                val s = 0.96f + 0.04f * Motion.decelerate.getInterpolation(sym)
                canvas.scale(s, s, cx, cy)
            }
            val r = 12.5f * unit
            canvas.drawRoundRect(tile, r, r, tilePaint)
            // 2. The ember stroke draws through it (reduced motion: fades with the tile).
            val st = if (move) Motion.decelerate.getInterpolation(IntroTimeline.segment(t, MotionTokens.SPLASH_STROKE, MotionTokens.SPLASH_STROKE_END)) else sym
            if (st > 0f) {
                partial.reset()
                measure.getSegment(0f, length * (if (move) st else 1f), partial, true)
                strokePaint.alpha = alpha(if (move) 1f else st)
                canvas.drawPath(partial, strokePaint)
                if (st >= 0.98f || !move) {
                    measure.getPosTan(length, pos, null)
                    dotPaint.alpha = alpha(if (move) (st - 0.98f) / 0.02f else st)
                    canvas.drawCircle(pos[0], pos[1], 4f * unit, dotPaint)
                }
            }
            // 4. A soft light and a few sparks run along the stroke (full motion only).
            if (move) {
                val lt = IntroTimeline.segment(t, MotionTokens.SPLASH_LIGHT, MotionTokens.SPLASH_LIGHT_END)
                if (lt > 0f && lt < 1f) {
                    measure.getPosTan(length * Motion.standard.getInterpolation(lt), pos, null)
                    lightPaint.alpha = alpha(1f - kotlin.math.abs(lt - 0.5f) * 2f)
                    canvas.save()
                    canvas.translate(pos[0], pos[1])
                    canvas.drawCircle(0f, 0f, lightRadius, lightPaint)
                    canvas.restore()
                    var i = 0
                    while (i < sparks.size) {
                        val f = sparks[i]
                        val local = ((lt - f * 0.8f) / 0.35f)
                        if (local in 0f..1f) {
                            measure.getPosTan(length * f, pos, null)
                            sparkPaint.alpha = alpha((1f - local) * 0.8f)
                            canvas.drawCircle(pos[0] + sparks[i + 1] * unit * 0.3f, pos[1] - local * theme.dpF(14f) + sparks[i + 1] * unit * 0.4f,
                                theme.dpF(sparks[i + 2]), sparkPaint)
                        }
                        i += 3
                    }
                }
            }
            canvas.restore()
            // 3. The wordmark rises in under the mark.
            val wd = IntroTimeline.segment(t, MotionTokens.SPLASH_WORD, MotionTokens.SPLASH_WORD_END)
            if (wd > 0f || !m.enabled) {
                val e = if (m.enabled) Motion.decelerate.getInterpolation(wd) else 1f
                wordPaint.alpha = alpha(e)
                val rise = if (move) (1f - e) * theme.dpF(MotionTokens.SHIFT) else 0f
                canvas.drawText(tr(S.APP_NAME), cx, cy + theme.dpF(128f) + rise, wordPaint)
            }
        }
    }
}
