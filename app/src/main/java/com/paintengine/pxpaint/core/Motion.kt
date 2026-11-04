package com.paintengine.pxpaint.core

import kotlin.math.cos
import kotlin.math.exp
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * PxPaint motion language (pure, unit-tested). The view layer (ui/Motion.kt)
 * turns these tokens into native animators; nothing else hard-codes a
 * duration, a curve or a distance.
 *
 * Principles: motion explains a change (where something came from, where it
 * went), never decorates the canvas; enter = decelerate, exit = accelerate
 * and shorter; small distances; springs only for small "settle" moves (list
 * reorder, selection); nothing runs inside a stroke.
 */
enum class MotionLevel {
    /** Everything: fades, movement, scale, springs, the splash light. */
    FULL,
    /** Essential feedback only: short fades, no movement / scale / particles. */
    REDUCED,
    /** No animation at all (states change instantly). */
    OFF,
}

object MotionTokens {
    // Durations (ms, before the user's level and the system animator scale).
    const val MICRO = 90L          // press / selection feedback
    const val SHORT = 150L         // small fades, exits
    const val MEDIUM = 240L        // panels, screens
    const val LONG = 360L          // container transform (project opening)
    const val STAGGER = 28L        // between list items, at most [STAGGER_MAX] items
    const val STAGGER_MAX = 6
    const val LOADING_DELAY = 300L // an indicator appears only if the wait is longer
    const val SAVED_HOLD = 1600L   // "Saved" stays visible, then fades

    // Splash (brand intro) timeline, ms from launch.
    const val SPLASH_SYMBOL = 0L       // symbol fades in ...
    const val SPLASH_SYMBOL_END = 320L
    const val SPLASH_STROKE = 220L     // ... the ember stroke draws through it ...
    const val SPLASH_STROKE_END = 820L
    const val SPLASH_WORD = 600L       // ... the wordmark rises in ...
    const val SPLASH_WORD_END = 1000L
    const val SPLASH_LIGHT = 700L      // ... a soft light runs along the stroke.
    const val SPLASH_LIGHT_END = 1200L
    const val SPLASH_END = 1200L
    const val SPLASH_EXIT = 260L

    // Distances (dp).
    const val NUDGE = 4f
    const val SHIFT = 12f
    const val SLIDE = 24f
    const val SHEET = 48f

    // Scales.
    const val PRESS_SCALE = 0.92f
    const val POP_SCALE = 1.10f
    const val ENTER_SCALE = 0.96f

    // Spring (settling moves): damping ratio and settle time are what matter.
    const val SPRING_DAMPING = 0.78f

    // Easing curves (cubic Bezier control points x1, y1, x2, y2).
    val STANDARD = floatArrayOf(0.2f, 0f, 0f, 1f)
    val DECELERATE = floatArrayOf(0.05f, 0.7f, 0.1f, 1f)   // enter
    val ACCELERATE = floatArrayOf(0.3f, 0f, 0.8f, 0.15f)   // exit
}

object MotionPolicy {
    /**
     * Effective level: the user's choice, and OFF when the system disabled
     * animations (developer options / accessibility: animator scale 0).
     */
    fun level(user: MotionLevel, systemAnimatorsEnabled: Boolean): MotionLevel =
        if (!systemAnimatorsEnabled) MotionLevel.OFF else user

    /**
     * Duration for [base] at [level] (the system animator scale is applied
     * by the native animators themselves, not here). REDUCED keeps short
     * fades only.
     */
    fun duration(base: Long, level: MotionLevel): Long = when (level) {
        MotionLevel.FULL -> base
        MotionLevel.REDUCED -> minOf(base, MotionTokens.SHORT) * 2 / 3
        MotionLevel.OFF -> 0L
    }

    /** Movement, scale, springs, particles: nonessential. */
    fun movement(level: MotionLevel): Boolean = level == MotionLevel.FULL

    fun fades(level: MotionLevel): Boolean = level != MotionLevel.OFF

    /** Delay of item [index] in a staggered list (0 beyond [MotionTokens.STAGGER_MAX]). */
    fun stagger(index: Int, level: MotionLevel): Long =
        if (!movement(level) || index >= MotionTokens.STAGGER_MAX) 0L else index * MotionTokens.STAGGER
}

/**
 * Critically-damped-ish spring as a normalised interpolator: value(0) = 0,
 * value(1) = 1, a small overshoot that settles within the duration.
 */
object Spring {
    fun value(t: Float, damping: Float = MotionTokens.SPRING_DAMPING): Float {
        if (t <= 0f) return 0f
        if (t >= 1f) return 1f
        val z = damping.coerceIn(0.3f, 0.99f).toDouble()
        // Natural frequency chosen so the envelope is ~1 % at t = 1.
        val w = 4.6 / z
        val wd = w * sqrt(1 - z * z)
        val tt = t.toDouble()
        val v = 1 - exp(-z * w * tt) * (cos(wd * tt) + z / sqrt(1 - z * z) * sin(wd * tt))
        return v.toFloat()
    }
}

/**
 * Splash / brand intro timeline (pure). The intro never delays the app:
 * the exit starts as soon as the app is ready, whatever the phase; if the
 * app is not ready when the reveal ends, a loading state takes over.
 */
object IntroTimeline {
    enum class Phase { REVEAL, LOADING, EXIT, DONE }

    /** Progress 0..1 of a segment [start, end] at time [t]. */
    fun segment(t: Long, start: Long, end: Long): Float =
        ((t - start).toFloat() / (end - start).coerceAtLeast(1)).coerceIn(0f, 1f)

    /** [readyAt]: ms when the app became ready (null: not yet). */
    fun phase(t: Long, readyAt: Long?, exitMs: Long = MotionTokens.SPLASH_EXIT): Phase = when {
        readyAt != null && t >= readyAt + exitMs -> Phase.DONE
        readyAt != null && t >= readyAt -> Phase.EXIT
        t < MotionTokens.SPLASH_END -> Phase.REVEAL
        else -> Phase.LOADING
    }

    /** Extra time the intro adds after the app is ready: the exit only (never waits for the reveal). */
    fun delayAfterReady(level: MotionLevel): Long = MotionPolicy.duration(MotionTokens.SPLASH_EXIT, level)
}

/** Save status shown in the workspace (pure state machine). */
enum class SaveState { CLEAN, DIRTY, SAVING, SAVED, FAILED }

class SaveTracker(private val holdMs: Long = MotionTokens.SAVED_HOLD) {
    var state = SaveState.CLEAN
        private set
    private var savedAt = 0L

    /** Called on every status poll: [dirty] = content differs from what was saved. */
    fun poll(dirty: Boolean, nowMs: Long) {
        state = when (state) {
            SaveState.CLEAN -> if (dirty) SaveState.DIRTY else SaveState.CLEAN
            SaveState.SAVED -> when {
                nowMs - savedAt >= holdMs -> if (dirty) SaveState.DIRTY else SaveState.CLEAN
                else -> SaveState.SAVED
            }
            SaveState.DIRTY -> if (dirty) SaveState.DIRTY else SaveState.CLEAN
            SaveState.SAVING, SaveState.FAILED -> state   // only a save result changes these
        }
    }

    fun saveStarted() {
        state = SaveState.SAVING
    }

    /** The save did not run (nothing written): still unsaved. */
    fun saveSkipped() {
        state = SaveState.DIRTY
    }

    fun saveFinished(ok: Boolean, nowMs: Long) {
        state = if (ok) SaveState.SAVED else SaveState.FAILED
        savedAt = nowMs
    }
}
