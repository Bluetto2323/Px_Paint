package com.paintengine.pxpaint.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/** The shipped preset file and the preset format / validation / engine encoding. */
class BrushPresetsTest {
    companion object {
        /** The real asset (unit tests run in the module directory). */
        fun builtInText(): String {
            val candidates = listOf("src/main/assets/brushes/builtin.pxb", "app/src/main/assets/brushes/builtin.pxb")
            return candidates.map { File(it) }.first { it.exists() }.readText()
        }

        fun builtIns(): List<BrushPreset> = BrushPresetFormat.parsePresets(builtInText())

        val REQUIRED = listOf(
            "builtin.round", "builtin.hard", "builtin.airbrush", "builtin.pencil", "builtin.graphite", "builtin.ink",
            "builtin.fineliner", "builtin.marker", "builtin.charcoal", "builtin.chalk", "builtin.textured", "builtin.watercolor",
            "builtin.oil", "builtin.gouache", "builtin.blender",
            "builtin.technical", "builtin.brushpen", "builtin.calligraphy", "builtin.dryink", "builtin.6b", "builtin.softpastel",
            "builtin.flat", "builtin.acrylic", "builtin.drybrush", "builtin.glaze", "builtin.wetwash", "builtin.spray", "builtin.foliage",
        )
    }

    @Test
    fun shippedFileHasTheTwelvePresetsAllValid() {
        val all = builtIns()
        assertEquals(REQUIRED, all.map { it.id })
        for (p in all) {
            assertTrue(p.id, p.builtIn)
            assertTrue("${p.id}: ${p.params.problems()}", p.params.problems().isEmpty())
            assertTrue(p.id, p.name.isNotBlank())
            assertNotNull("${p.id} has an Italian name", p.names["it"])
            assertTrue(p.id, p.tags.isNotEmpty())
        }
        assertEquals(
            setOf(BrushCategory.SKETCHING, BrushCategory.INKING, BrushCategory.PAINTING, BrushCategory.AIRBRUSH, BrushCategory.DRY, BrushCategory.CALLIGRAPHY, BrushCategory.WATER),
            all.map { it.category }.toSet(),
        )
        // No two presets share their parameters (not renamed copies of one brush).
        for (i in all.indices) for (j in i + 1 until all.size) assertNotEquals(all[i].params, all[j].params)
        // Only the hard brush and the fine liner use the classic Block 3/4 rasteriser.
        assertEquals(listOf("builtin.hard", "builtin.fineliner", "builtin.technical"), all.filter { it.params.classic }.map { it.id })
    }

    @Test
    fun presetsUseTheModelFeaturesTheyAreNamedFor() {
        val m = builtIns().associateBy { it.id }
        assertEquals(Accumulation.BUILDUP, m.getValue("builtin.airbrush").params.accumulation)
        assertEquals(0f, m.getValue("builtin.airbrush").params.hardness, 0f)
        assertEquals(BrushGrain.PAPER, m.getValue("builtin.pencil").params.grain)
        assertEquals(BrushTip.CHARCOAL, m.getValue("builtin.charcoal").params.tip)
        assertEquals(BrushTip.CHALK, m.getValue("builtin.chalk").params.tip)
        assertEquals(BrushTip.BRISTLE, m.getValue("builtin.textured").params.tip)
        assertEquals(AngleMode.STROKE, m.getValue("builtin.textured").params.angleMode)
        assertTrue(m.getValue("builtin.watercolor").params.wetEdge > 0.5f)
        assertEquals(BrushGrain.WASH, m.getValue("builtin.watercolor").params.grain)
        assertTrue(m.getValue("builtin.ink").params.roundness < 1f)
        assertFalse(m.getValue("builtin.fineliner").params.pressureSize)
        assertEquals("Acquerello", m.getValue("builtin.watercolor").displayName(italian = true))
        assertEquals("Watercolor", m.getValue("builtin.watercolor").displayName(italian = false))
    }

    @Test
    fun paramsRoundTripThroughTheTextFormat() {
        val rnd = java.util.Random(7)
        repeat(200) {
            val p = BrushParams(
                tip = BrushTip.entries[rnd.nextInt(4)], size = 1f + rnd.nextFloat() * 400f, opacity = rnd.nextFloat(),
                flow = rnd.nextFloat(), spacing = rnd.nextFloat() * 2f, hardness = rnd.nextFloat(), roundness = rnd.nextFloat(),
                angle = rnd.nextFloat() * 720f - 360f, angleMode = AngleMode.entries[rnd.nextInt(2)], sizeJitter = rnd.nextFloat(),
                scatter = rnd.nextFloat() * 2f, angleJitter = rnd.nextFloat(), flowJitter = rnd.nextFloat(),
                grain = BrushGrain.entries[rnd.nextInt(5)], grainDepth = rnd.nextFloat(), grainPressure = rnd.nextFloat(),
                accumulation = Accumulation.entries[rnd.nextInt(2)], wetEdge = rnd.nextFloat(), pressureSize = rnd.nextBoolean(),
                minSize = rnd.nextFloat(), pressureOpacity = rnd.nextBoolean(), minOpacity = rnd.nextFloat(), tilt = rnd.nextBoolean(),
                tiltStrength = rnd.nextFloat(),
            ).sanitized()
            val back = BrushParams.fromEntries(p.entries().toMap())
            // 4 decimals in the file.
            for ((a, b) in p.entries().zip(back.entries())) {
                val fa = a.second.toFloatOrNull()
                if (fa != null) assertEquals(a.first, fa, b.second.toFloat(), 1e-3f) else assertEquals(a.first, a.second, b.second)
            }
            assertEquals(back, BrushParams.fromEntries(back.entries().toMap()))   // stable after one trip
        }
    }

    @Test
    fun presetWithAwkwardNameRoundTrips() {
        val p = BrushPreset("user.k1x", "Ink = \"wet\" \\ test ñ", BrushCategory.CUSTOM, BrushParams(size = 33f, tip = BrushTip.CHALK),
            tags = listOf("a", "b"), baseId = "builtin.chalk")
        val sb = StringBuilder(BrushPresetFormat.HEADER).append('\n')
        BrushPresetFormat.writePreset(sb, p)
        val back = BrushPresetFormat.parsePresets(sb.toString()).single()
        assertEquals(p, back)
    }

    @Test
    fun validationClampsAndDefaults() {
        val bad = BrushParams(size = 9000f, opacity = -1f, flow = 0f, spacing = Float.NaN, hardness = 2f, roundness = 0f,
            angle = 540f, scatter = 5f, grainDepth = Float.POSITIVE_INFINITY, minSize = 0f, tiltStrength = -3f)
        assertTrue(bad.problems().size >= 9)
        val s = bad.sanitized()
        assertTrue(s.problems().isEmpty())
        assertEquals(BrushState.MAX_SIZE, s.size, 0f)
        assertEquals(0.01f, s.opacity, 0f)
        assertEquals(0.01f, s.flow, 0f)
        assertEquals(0.08f, s.spacing, 0f)           // NaN -> default
        assertEquals(1f, s.hardness, 0f)
        assertEquals(0.05f, s.roundness, 0f)
        assertEquals(180f, s.angle, 0f)              // 540 deg = 180 deg
        assertEquals(2f, s.scatter, 0f)
        assertEquals(0f, s.grainDepth, 0f)           // infinite -> default
        assertEquals(BrushState.MIN_PRESSURE_FRACTION, s.minSize, 0f)
        assertEquals(0f, s.tiltStrength, 0f)
        assertEquals(-90f, BrushParams.normalizeAngle(270f), 0f)
    }

    @Test
    fun unknownKeysAndValuesAreTolerated() {
        val text = """
            pxbrushes 1
            # comment
            [preset builtin.x]
            name = X
            future_key = 12
            tip = sponge
            grain = ??
            size = big
            hardness = 0.5
            [unknown-section a]
            size = 3
            [preset bad id with spaces]
            name = nope
        """.trimIndent()
        val p = BrushPresetFormat.parsePresets(text).single()
        assertEquals(BrushTip.ROUND, p.params.tip)
        assertEquals(BrushGrain.NONE, p.params.grain)
        assertEquals(24f, p.params.size, 0f)
        assertEquals(0.5f, p.params.hardness, 0f)
        assertTrue(BrushPresetFormat.parsePresets("pxbrushes 2\n[preset builtin.x]\nname = X").isEmpty())
    }

    @Test
    fun namesAreCleaned() {
        assertEquals("My brush", BrushPreset.cleanName("  My \n  brush  "))
        assertEquals(null, BrushPreset.cleanName("   "))
        assertEquals(BrushPreset.MAX_NAME, BrushPreset.cleanName("x".repeat(100))!!.length)
    }

    @Test
    fun engineEncodingLayout() {
        val p = BrushParams(size = 40f, opacity = 0.5f, spacing = 0.1f, hardness = 0.3f, flow = 0.2f, roundness = 0.4f, angle = 30f,
            angleMode = AngleMode.STROKE, sizeJitter = 0.1f, scatter = 0.2f, angleJitter = 0.3f, flowJitter = 0.4f,
            grain = BrushGrain.CANVAS, grainDepth = 0.6f, grainPressure = 0.7f, wetEdge = 0.8f, tip = BrushTip.BRISTLE,
            accumulation = Accumulation.BUILDUP, pressureSize = true, minSize = 0.3f, pressureOpacity = false, minOpacity = 0.4f,
            tilt = true, tiltStrength = 0.9f)
        val a = BrushEncoding.params(p, size = 12f, opacity = 0.25f, pressureSmoothing = 0.35f)
        assertEquals(BrushEncoding.PARAMS, a.size)
        val expect = floatArrayOf(12f, 0.25f, 0.1f, 0.3f, 0.4f, 0.9f, 0.35f, 0.3f, 0.2f, 0.4f, 30f, 0.1f, 0.2f, 0.3f, 0.4f, 0.6f, 0.7f,
            0.8f, 3f, 3f, 1f)
        for (i in expect.indices) assertEquals("param $i", expect[i], a[i], 1e-6f)
        val f = BrushEncoding.flags(p, erase = true)
        assertEquals(BrushEncoding.FLAG_ERASE or BrushEncoding.FLAG_PRESSURE_SIZE or BrushEncoding.FLAG_TILT or
            BrushEncoding.FLAG_ANGLE_FOLLOWS_STROKE, f)
        // Block 4 layout kept in the first 7 floats.
        val classic = BrushEncoding.params(BrushParams(), pressureSmoothing = 0f)
        assertEquals(24f, classic[0], 0f)
        assertEquals(1f, classic[7], 0f)   // hardness 1 = classic edge
    }

    @Test
    fun wetMediaPresetsAndEncoding() {
        val m = builtIns().associateBy { it.id }
        // Block 14: the existing presets keep plain paint; the wet ones mix, the blender carries only canvas colour.
        for (id in REQUIRED.take(12)) assertEquals(id, 0f, m.getValue(id).params.mix, 0f)
        assertTrue(m.getValue("builtin.oil").params.mix in 0.3f..0.8f)
        assertTrue(m.getValue("builtin.gouache").params.mix in 0.1f..0.6f)
        assertEquals(1f, m.getValue("builtin.blender").params.mix, 0f)
        // Encoded for the engine: mix and pickup after the 21 Block 5 floats; smudge is a flag, never with erase.
        val p = BrushParams(mix = 0.4f, pickup = 0.2f)
        val a = BrushEncoding.params(p, pressureSmoothing = 0f)
        assertEquals(26, a.size)
        // Block 15: charge, dilution, taper follow; colour jitter stays in the app (applied per stroke).
        val q = BrushParams(charge = 0.3f, dilution = 0.6f, taper = 0.5f, hueJitter = 0.2f)
        val e = BrushEncoding.params(q, pressureSmoothing = 0f)
        assertEquals(0.3f, e[23], 0f)
        assertEquals(0.6f, e[24], 0f)
        assertEquals(0.5f, e[25], 0f)
        assertEquals(0xFF336699.toInt(), ColorJitter.apply(0xFF336699.toInt(), 0f, 0f, 7))
        val j1 = ColorJitter.apply(0xFF336699.toInt(), 0.5f, 0.5f, 7)
        assertTrue(j1 != 0xFF336699.toInt() && (j1 ushr 24) == 0xFF)
        assertEquals(j1, ColorJitter.apply(0xFF336699.toInt(), 0.5f, 0.5f, 7))        // same stroke, same colour
        assertTrue(j1 != ColorJitter.apply(0xFF336699.toInt(), 0.5f, 0.5f, 8))
        assertEquals(0.4f, a[21], 0f)
        assertEquals(0.2f, a[22], 0f)
        assertEquals(BrushEncoding.FLAG_SMUDGE, BrushEncoding.flags(p, erase = false, smudge = true) and BrushEncoding.FLAG_SMUDGE)
        assertEquals(0, BrushEncoding.flags(p, erase = true, smudge = true) and BrushEncoding.FLAG_SMUDGE)
        assertEquals(0, BrushEncoding.flags(p, erase = false) and BrushEncoding.FLAG_SMUDGE)
        // Saved and read back; out-of-range values clamped.
        val sb = StringBuilder()
        BrushPresetFormat.writeParams(sb, p)
        val back = BrushParams.fromEntries(sb.lineSequence().filter { '=' in it }.associate { it.substringBefore('=').trim() to it.substringAfter('=').trim() })
        assertEquals(0.4f, back.mix, 0f)
        assertEquals(0.2f, back.pickup, 0f)
        assertEquals(1f, BrushParams(mix = 3f, pickup = -1f).sanitized().mix, 0f)
        assertEquals(0f, BrushParams(mix = 3f, pickup = -1f).sanitized().pickup, 0f)
        // The smudge tool has its own brush (a soft one by default), separate from paint and eraser.
        val lib = BrushLibrary.migrate(builtIns(), null)
        assertEquals(BrushLibrary.DEFAULT_SMUDGE, lib.selectedId(BrushTool.SMUDGE))
        val sized = lib.setLive(BrushTool.SMUDGE, size = 99f)
        assertEquals(99f, sized.effective(BrushTool.SMUDGE).size, 0f)
    }
}
