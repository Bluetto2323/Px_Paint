package com.paintengine.pxpaint.ui

import android.animation.ValueAnimator
import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.view.View
import android.view.animation.LinearInterpolator
import com.paintengine.pxpaint.core.MotionTokens
import com.paintengine.pxpaint.core.SaveState
import com.paintengine.pxpaint.core.S
import com.paintengine.pxpaint.core.tr

/**
 * The one loading indicator of PxPaint: a ring. Determinate when real
 * progress is known ([progress] 0..1, e.g. "layer 2 of 5"), otherwise an
 * indeterminate rotating arc. Never a made-up percentage. The animator runs
 * only while the view is attached and visible; with motion OFF the
 * indeterminate ring is drawn still.
 */
@SuppressLint("ViewConstructor")
class ProgressIndicator(context: Context, private val theme: PxTheme, private val sizeDp: Float = 28f, strokeDp: Float = 3f) : View(context) {
    /** null = indeterminate. */
    var progress: Float? = null
        set(value) {
            field = value?.coerceIn(0f, 1f)
            if (android.os.Build.VERSION.SDK_INT >= 30) stateDescription = value?.let { "${(it * 100).toInt()}%" }
            invalidate()
            updateAnimator()
        }
    private val track = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; strokeWidth = theme.dpF(strokeDp); color = theme.outline
    }
    private val arc = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; strokeWidth = theme.dpF(strokeDp); color = theme.accent; strokeCap = Paint.Cap.ROUND
    }
    private val rect = RectF()
    private var angle = 0f
    private val spin = ValueAnimator.ofFloat(0f, 360f).apply {
        duration = 900
        repeatCount = ValueAnimator.INFINITE
        interpolator = LinearInterpolator()
        addUpdateListener { angle = it.animatedValue as Float; invalidate() }
    }

    init {
        importantForAccessibility = IMPORTANT_FOR_ACCESSIBILITY_YES
        contentDescription = tr(S.LOADING)
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val s = theme.dp(sizeDp)
        setMeasuredDimension(s, s)
    }

    override fun onDraw(canvas: Canvas) {
        val inset = arc.strokeWidth / 2 + 1
        rect.set(inset, inset, width - inset, height - inset)
        canvas.drawOval(rect, track)
        val p = progress
        if (p != null) canvas.drawArc(rect, -90f, 360f * p, false, arc)
        else canvas.drawArc(rect, angle - 90f, 100f, false, arc)
    }

    private fun updateAnimator() {
        val run = progress == null && isAttachedToWindow && visibility == VISIBLE && theme.motion.enabled
        if (run && !spin.isStarted) spin.start() else if (!run && spin.isStarted) spin.cancel()
    }

    override fun onAttachedToWindow() { super.onAttachedToWindow(); updateAnimator() }
    override fun onDetachedFromWindow() { spin.cancel(); super.onDetachedFromWindow() }
    override fun onVisibilityChanged(changedView: View, visibility: Int) { super.onVisibilityChanged(changedView, visibility); updateAnimator() }
}

/**
 * Save status in the workspace top bar: unsaved (small dot), saving (mini
 * ring), saved (check, fades after a moment), failed (warning, tap = retry).
 * Never a dialog; state changes cross-fade.
 */
@SuppressLint("ViewConstructor")
class SaveIndicator(context: Context, private val theme: PxTheme, private val onRetry: () -> Unit) : android.widget.FrameLayout(context) {
    var state: SaveState = SaveState.CLEAN
        private set
    private val icon = View(context)
    private val ring = ProgressIndicator(context, theme, sizeDp = 16f, strokeDp = 2f).apply { visibility = GONE }
    private val dot = View(context).apply { background = theme.oval(theme.textSecondary) }

    init {
        minimumWidth = theme.dp(32)
        minimumHeight = theme.touchTarget
        addView(icon, LayoutParams(theme.dp(18), theme.dp(18), android.view.Gravity.CENTER))
        addView(ring, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT, android.view.Gravity.CENTER))
        addView(dot, LayoutParams(theme.dp(7), theme.dp(7), android.view.Gravity.CENTER))
        accessibilityLiveRegion = ACCESSIBILITY_LIVE_REGION_POLITE
        setOnClickListener { if (state == SaveState.FAILED) onRetry() }
        apply(SaveState.CLEAN, animate = false)
    }

    fun show(s: SaveState) {
        if (s == state) return
        apply(s, animate = true)
    }

    private fun apply(s: SaveState, animate: Boolean) {
        val old = state
        state = s
        icon.visibility = if (s == SaveState.SAVED || s == SaveState.FAILED) VISIBLE else GONE
        ring.visibility = if (s == SaveState.SAVING) VISIBLE else GONE
        dot.visibility = if (s == SaveState.DIRTY) VISIBLE else GONE
        when (s) {
            SaveState.SAVED -> icon.background = IconDrawable(Icon.CHECK, theme.accent)
            SaveState.FAILED -> icon.background = IconDrawable(Icon.INFO, theme.danger)
            else -> Unit
        }
        isClickable = s == SaveState.FAILED
        contentDescription = when (s) {
            SaveState.CLEAN -> tr(S.SAVE_STATE_SAVED)
            SaveState.DIRTY -> tr(S.SAVE_STATE_UNSAVED)
            SaveState.SAVING -> tr(S.SAVE_STATE_SAVING)
            SaveState.SAVED -> tr(S.SAVE_STATE_SAVED)
            SaveState.FAILED -> tr(S.SAVE_STATE_FAILED)
        }
        if (!animate) return
        val m = theme.motion
        when {
            s == SaveState.SAVED && old == SaveState.SAVING -> { m.fadeTo(icon, 1f, from = 0f); m.pop(icon, 1.25f) }
            s == SaveState.FAILED -> { m.fadeTo(icon, 1f, from = 0f); m.nudge(icon, 12f) }
            s == SaveState.DIRTY -> m.fadeTo(dot, 1f, from = 0f)
            s == SaveState.SAVING -> m.fadeTo(ring, 1f, from = 0f)
            s == SaveState.CLEAN && old == SaveState.SAVED -> {
                // "Saved" fades away instead of vanishing.
                icon.visibility = VISIBLE
                m.exit(icon, dyDp = 0f) { icon.visibility = GONE; icon.alpha = 1f }
            }
        }
    }

    companion object {
        const val HOLD = MotionTokens.SAVED_HOLD
    }
}
