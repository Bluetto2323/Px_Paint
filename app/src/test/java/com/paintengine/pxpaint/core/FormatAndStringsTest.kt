package com.paintengine.pxpaint.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.Locale

class FormatAndStringsTest {
    @Test
    fun formats() {
        assertEquals("2048 × 1024 px", Format.dimensions(2048, 1024))
        assertEquals("512 B", Format.bytes(512))
        assertEquals("16 MB", Format.bytes(16L * 1024 * 1024))
        assertEquals("1.5 MB", Format.bytes(1536L * 1024))
        assertEquals("1.5 GB", Format.bytes(1536L * 1024 * 1024))
        assertEquals("48%", Format.zoomPercent(0.48))
        assertEquals("4.5%", Format.zoomPercent(0.045))
        assertEquals("90°", Format.degrees(Math.PI / 2))
        assertEquals("180°", Format.degrees(-Math.PI))
        assertEquals("-45°", Format.degrees(-Math.PI / 4))
    }

    @Test
    fun relativeAge() {
        val now = 100L * 86_400_000
        assertEquals(Format.Ago.JUST_NOW to 0L, Format.ago(now, now - 5_000))
        assertEquals(Format.Ago.MINUTES to 5L, Format.ago(now, now - 5 * 60_000))
        assertEquals(Format.Ago.HOURS to 3L, Format.ago(now, now - 3 * 3_600_000))
        assertEquals(Format.Ago.YESTERDAY to 1L, Format.ago(now, now - 30 * 3_600_000))
        assertEquals(Format.Ago.DAYS to 4L, Format.ago(now, now - 4 * 86_400_000))
        assertEquals(Format.Ago.DATE, Format.ago(now, now - 30L * 86_400_000).first)
        assertEquals(Format.Ago.JUST_NOW, Format.ago(now, now + 60_000).first)   // clock skew
    }

    private val placeholder = Regex("%[sd]")

    @Test
    fun translationsHaveTheSamePlaceholders() {
        for (s in S.entries) {
            assertTrue("${s.name} is blank", s.en.isNotBlank() && s.it.isNotBlank())
            assertEquals(s.name, placeholder.findAll(s.en).map { it.value }.toList(),
                placeholder.findAll(s.it).map { it.value }.toList())
        }
    }

    @Test
    fun languageSelection() {
        S.applyLanguage(LanguagePref.SYSTEM, Locale.ITALY)
        assertEquals("Nuova tela", tr(S.NEW_CANVAS))
        assertEquals("Modificata 5 min fa", tr(S.EDITED_MINUTES, 5))
        S.applyLanguage(LanguagePref.ENGLISH, Locale.ITALY)
        assertEquals("New canvas", tr(S.NEW_CANVAS))
        S.applyLanguage(LanguagePref.SYSTEM, Locale.US)
        assertEquals("Edited 5 min ago", tr(S.EDITED_MINUTES, 5))
    }
}
