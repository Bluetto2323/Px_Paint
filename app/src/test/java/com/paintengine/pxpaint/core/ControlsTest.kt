package com.paintengine.pxpaint.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** Block 12.5: hold-to-pick eyedropper and the first-start controls setup settings. */
class ControlsTest {
    @Test
    fun aStillFingerBecomesTheEyedropper() {
        val p = LongPressPick(slopPx = 10f)
        p.down(1000, 100f, 100f)
        p.move(104f, 103f)                                   // within the slop: still a hold
        assertFalse(p.fire(1000 + LongPressPick.HOLD_MS - 1))
        assertTrue(p.fire(1000 + LongPressPick.HOLD_MS))
        assertFalse("fires once", p.fire(5000))
    }

    @Test
    fun movingOrASecondFingerIsNotAHold() {
        val p = LongPressPick(slopPx = 10f)
        p.down(0, 100f, 100f)
        p.move(130f, 100f)                                   // a stroke
        assertFalse(p.fire(10_000))
        p.down(0, 100f, 100f)
        p.cancel()                                           // second finger / lifted
        assertFalse(p.fire(10_000))
        assertTrue(LongPressPick.HOLD_MS in 300..700)
    }

    @Test
    fun settingsRoundTripAndDefaults() {
        val d = AppSettings()
        assertTrue(d.longPressPick)
        assertFalse(d.controlsSetupDone)                      // offered at the first start
        val s = d.copy(longPressPick = false, controlsSetupDone = true)
        assertEquals(s, AppSettings.decode(s.encode()))
        // Settings from before Block 12.5: the setup is offered once, hold-to-pick on.
        val old = AppSettings.decode(mapOf(AppSettings.K_THEME to "DARK"))
        assertFalse(old.controlsSetupDone)
        assertTrue(old.longPressPick)
    }
}
