package com.paintengine.pxpaint.ui

import android.animation.ValueAnimator
import android.animation.TimeInterpolator
import android.view.View
import android.view.animation.PathInterpolator
import com.paintengine.pxpaint.core.MotionLevel
import com.paintengine.pxpaint.core.MotionPolicy
import com.paintengine.pxpaint.core.MotionTokens
import com.paintengine.pxpaint.core.Spring

/**
 * The motion system on views: native ViewPropertyAnimator / ValueAnimator
 * only (they follow the system animator duration scale), tokens from
 * core/Motion.kt, and the effective level (user setting, OFF when the system
 * disabled animations). Every helper is safe to call repeatedly: it cancels
 * the view's running animation and ends in the exact final state, so rapid
 * interactions never leave a view half-faded or offset.
 */
class Motion(userLevel: MotionLevel, private val density: Float) {
    val level: MotionLevel = MotionPolicy.level(userLevel, ValueAnimator.areAnimatorsEnabled())
    val enabled: Boolean get() = MotionPolicy.fades(level)
    val movement: Boolean get() = MotionPolicy.movement(level)

    fun d(base: Long): Long = MotionPolicy.duration(base, level)
    private fun px(dp: Float) = dp * density

    // --- the system ----------------------------------------------------------
    fun enter(v: View, dyDp: Float = MotionTokens.SHIFT, dxDp: Float = 0f, delay: Long = 0L, duration: Long = MotionTokens.MEDIUM,
              then: (() -> Unit)? = null) {
        val a = v.animate()
        a.cancel()
        if (!enabled) {
            v.alpha = 1f; v.translationX = 0f; v.translationY = 0f; v.scaleX = 1f; v.scaleY = 1f
            then?.invoke()
            return
        }
        // (End actions are captured when the animation starts: pass them here, never afterwards.)
        a.withEndAction(then?.let { Runnable(it) })
        v.alpha = 0f
        if (movement) {
            v.translationX = px(dxDp)
            v.translationY = px(dyDp)
        }
        a.alpha(1f).translationX(0f).translationY(0f).setStartDelay(delay).setDuration(d(duration))
            .setInterpolator(decelerate).withLayer().start()
    }

    /** Fades [v] out (moving [dyDp] if movement is on), then [then] (e.g. removes it). */
    fun exit(v: View, dyDp: Float = MotionTokens.SHIFT, dxDp: Float = 0f, duration: Long = MotionTokens.SHORT, then: () -> Unit) {
        val a = v.animate()
        a.cancel()
        if (!enabled) {
            then()
            return
        }
        a.alpha(0f).translationX(if (movement) px(dxDp) else 0f).translationY(if (movement) px(dyDp) else 0f)
            .setStartDelay(0).setDuration(d(duration)).setInterpolator(accelerate).withLayer()
            .withEndAction(then).start()
    }

    /** Small "this changed" pulse (selection, colour, saved): scale up and settle with a spring. */
    fun pop(v: View, scale: Float = MotionTokens.POP_SCALE) {
        if (!movement) return
        val a = v.animate()
        v.scaleX = scale
        v.scaleY = scale
        a.scaleX(1f).scaleY(1f).setStartDelay(0).setDuration(d(MotionTokens.MEDIUM)).setInterpolator(spring).start()
    }

    /** Rotation nudge (undo / redo icon): turns by [degrees] and springs back. */
    fun nudge(v: View, degrees: Float) {
        if (!movement) return
        val a = v.animate()
        v.rotation = degrees
        a.rotation(0f).setStartDelay(0).setDuration(d(MotionTokens.MEDIUM)).setInterpolator(spring).start()
    }

    /** Moves [v] from an old offset back to its place (list reorder: FLIP). */
    fun settle(v: View, fromDy: Float, delay: Long = 0L) {
        if (!movement || fromDy == 0f) return
        v.translationY = fromDy
        v.animate().translationY(0f).setStartDelay(delay).setDuration(d(MotionTokens.MEDIUM)).setInterpolator(spring).start()
    }

    /** Alpha change (visibility toggles, dimmed rows). */
    fun fadeTo(v: View, alpha: Float, from: Float? = null) {
        val a = v.animate()
        if (!enabled) {
            v.alpha = alpha
            return
        }
        from?.let { v.alpha = it }
        a.alpha(alpha).setStartDelay(0).setDuration(d(MotionTokens.SHORT)).setInterpolator(standard).start()
    }

    companion object {
        private fun bezier(p: FloatArray) = PathInterpolator(p[0], p[1], p[2], p[3])
        val standard: TimeInterpolator = bezier(MotionTokens.STANDARD)
        val decelerate: TimeInterpolator = bezier(MotionTokens.DECELERATE)
        val accelerate: TimeInterpolator = bezier(MotionTokens.ACCELERATE)
        val spring: TimeInterpolator = TimeInterpolator { t -> Spring.value(t) }
    }
}
