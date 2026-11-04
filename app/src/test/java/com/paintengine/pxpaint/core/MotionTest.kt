package com.paintengine.pxpaint.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.abs

/** Block 9: motion tokens, levels, spring, startup timeline, save status. */
class MotionTest {
    @Test
    fun levelsAndTheSystemAnimatorScale() {
        assertEquals(MotionLevel.FULL, MotionPolicy.level(MotionLevel.FULL, systemAnimatorsEnabled = true))
        // Animations disabled by the system (animator scale 0): off whatever the user chose.
        for (l in MotionLevel.entries) assertEquals(MotionLevel.OFF, MotionPolicy.level(l, systemAnimatorsEnabled = false))
        assertEquals(MotionLevel.REDUCED, MotionPolicy.level(MotionLevel.REDUCED, true))
    }

    @Test
    fun durations() {
        assertEquals(MotionTokens.MEDIUM, MotionPolicy.duration(MotionTokens.MEDIUM, MotionLevel.FULL))
        assertEquals(0L, MotionPolicy.duration(MotionTokens.LONG, MotionLevel.OFF))
        // Reduced: short fades only (never longer than 2/3 of SHORT).
        for (d in listOf(MotionTokens.MICRO, MotionTokens.SHORT, MotionTokens.MEDIUM, MotionTokens.LONG, MotionTokens.SPLASH_END)) {
            val r = MotionPolicy.duration(d, MotionLevel.REDUCED)
            assertTrue("$d -> $r", r in 1..MotionTokens.SHORT * 2 / 3)
        }
        assertTrue(MotionPolicy.movement(MotionLevel.FULL))
        assertFalse(MotionPolicy.movement(MotionLevel.REDUCED))
        assertFalse(MotionPolicy.movement(MotionLevel.OFF))
        assertTrue(MotionPolicy.fades(MotionLevel.REDUCED))
        assertFalse(MotionPolicy.fades(MotionLevel.OFF))
    }

    @Test
    fun responsiveTokens() {
        // "Responsive rather than slow": UI feedback well under a third of a second, exits shorter than entries.
        assertTrue(MotionTokens.MICRO < 120)
        assertTrue(MotionTokens.SHORT < MotionTokens.MEDIUM)
        assertTrue(MotionTokens.MEDIUM <= 250)
        assertTrue(MotionTokens.LONG <= 400)
        assertTrue(MotionTokens.SPLASH_END in 1000..1500)
        assertTrue(MotionTokens.SPLASH_EXIT <= 300)
        // Stagger stops after a few items (long lists never wait).
        assertEquals(0L, MotionPolicy.stagger(MotionTokens.STAGGER_MAX, MotionLevel.FULL))
        assertEquals(0L, MotionPolicy.stagger(2, MotionLevel.REDUCED))
        assertEquals(2 * MotionTokens.STAGGER, MotionPolicy.stagger(2, MotionLevel.FULL))
        assertTrue(MotionPolicy.stagger(MotionTokens.STAGGER_MAX - 1, MotionLevel.FULL) <= 150)
        for (c in listOf(MotionTokens.STANDARD, MotionTokens.DECELERATE, MotionTokens.ACCELERATE)) {
            assertEquals(4, c.size)
            assertTrue(c[0] in 0f..1f && c[2] in 0f..1f)   // valid cubic-bezier time control points
        }
    }

    @Test
    fun springStartsAtZeroEndsAtOneAndSettles() {
        assertEquals(0f, Spring.value(0f), 0f)
        assertEquals(1f, Spring.value(1f), 0f)
        var max = 0f
        var t = 0f
        while (t <= 1f) {
            max = maxOf(max, Spring.value(t))
            t += 0.005f
        }
        // A small overshoot only (no bouncy wobble), settled at the end.
        assertTrue("overshoot $max", max in 1.0f..1.05f)
        assertTrue(abs(Spring.value(0.95f) - 1f) < 0.02f)
        assertTrue(Spring.value(0.3f) > 0.6f)               // responsive: most of the move happens early
    }

    @Test
    fun introNeverDelaysTheApp() {
        // Ready early: the exit starts at once (no waiting for the reveal to finish).
        assertEquals(IntroTimeline.Phase.EXIT, IntroTimeline.phase(150, readyAt = 120))
        assertEquals(IntroTimeline.Phase.DONE, IntroTimeline.phase(120 + MotionTokens.SPLASH_EXIT, readyAt = 120))
        // Not ready yet: the reveal plays, then a loading state.
        assertEquals(IntroTimeline.Phase.REVEAL, IntroTimeline.phase(500, readyAt = null))
        assertEquals(IntroTimeline.Phase.LOADING, IntroTimeline.phase(MotionTokens.SPLASH_END + 1, readyAt = null))
        assertEquals(IntroTimeline.Phase.EXIT, IntroTimeline.phase(2600, readyAt = 2500))
        // The only time the intro adds after the app is ready is its exit (short; none when off).
        assertEquals(MotionTokens.SPLASH_EXIT, IntroTimeline.delayAfterReady(MotionLevel.FULL))
        assertEquals(0L, IntroTimeline.delayAfterReady(MotionLevel.OFF))
        assertEquals(0f, IntroTimeline.segment(100, 200, 400), 0f)
        assertEquals(0.5f, IntroTimeline.segment(300, 200, 400), 0f)
        assertEquals(1f, IntroTimeline.segment(900, 200, 400), 0f)
        // The phases overlap in order: symbol, stroke, wordmark, light.
        assertTrue(MotionTokens.SPLASH_SYMBOL < MotionTokens.SPLASH_STROKE && MotionTokens.SPLASH_STROKE < MotionTokens.SPLASH_WORD)
        assertTrue(MotionTokens.SPLASH_WORD < MotionTokens.SPLASH_LIGHT && MotionTokens.SPLASH_LIGHT_END <= MotionTokens.SPLASH_END)
    }

    @Test
    fun saveStatus() {
        val t = SaveTracker(holdMs = 1000)
        t.poll(dirty = false, nowMs = 0)
        assertEquals(SaveState.CLEAN, t.state)
        t.poll(dirty = true, nowMs = 10)
        assertEquals(SaveState.DIRTY, t.state)
        t.saveStarted()
        t.poll(dirty = true, nowMs = 20)                 // polling does not hide a running save
        assertEquals(SaveState.SAVING, t.state)
        t.saveFinished(ok = true, nowMs = 100)
        t.poll(dirty = false, nowMs = 500)
        assertEquals(SaveState.SAVED, t.state)           // "Saved" shows for a moment ...
        t.poll(dirty = false, nowMs = 1200)
        assertEquals(SaveState.CLEAN, t.state)           // ... then fades away
        t.poll(dirty = true, nowMs = 1300)
        t.saveStarted()
        t.saveFinished(ok = false, nowMs = 1400)
        t.poll(dirty = true, nowMs = 5000)
        assertEquals(SaveState.FAILED, t.state)          // a failure stays until a save succeeds
        t.saveStarted()
        t.saveFinished(ok = true, nowMs = 6000)
        t.poll(dirty = true, nowMs = 7100)               // edited again during the hold
        assertEquals(SaveState.DIRTY, t.state)
        t.saveStarted()
        t.saveSkipped()
        assertEquals(SaveState.DIRTY, t.state)
    }

    @Test
    fun settingMigratesFromTheOldSwitch() {
        assertEquals(MotionLevel.REDUCED, AppSettings.decode(mapOf(AppSettings.K_REDUCE_MOTION to "true")).motionLevel)
        assertEquals(MotionLevel.FULL, AppSettings.decode(mapOf(AppSettings.K_REDUCE_MOTION to "false")).motionLevel)
        assertEquals(MotionLevel.OFF, AppSettings.decode(mapOf(AppSettings.K_MOTION to "OFF", AppSettings.K_REDUCE_MOTION to "false")).motionLevel)
        val s = AppSettings(motionLevel = MotionLevel.OFF)
        assertEquals(s, AppSettings.decode(s.encode()))
        assertTrue(s.reduceMotion)
        assertFalse(AppSettings().reduceMotion)
    }
}
