package com.paintengine.pxpaint.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class BrushLibraryTest {
    private val builtIns = BrushPresetsTest.builtIns()
    private fun fresh() = BrushLibrary.migrate(builtIns, null)

    @Test
    fun newUsersStartOnBasicRoundAndTheHardEraser() {
        val lib = fresh()
        assertEquals("builtin.round", lib.selectedId(BrushTool.PAINT))
        assertEquals("builtin.hard", lib.selectedId(BrushTool.ERASER))
        assertEquals(builtIns.first { it.id == "builtin.round" }.params, lib.effective(BrushTool.PAINT))
        assertEquals(28, lib.all().size)
    }

    @Test
    fun switchingBrushesKeepsEachBrushsQuickSizePerTool() {
        var lib = fresh().select(BrushTool.PAINT, "builtin.pencil").setLive(BrushTool.PAINT, size = 9f, opacity = 0.5f)
        lib = lib.select(BrushTool.PAINT, "builtin.marker")
        assertEquals(28f, lib.effective(BrushTool.PAINT).size, 0f)                 // marker default
        lib = lib.select(BrushTool.PAINT, "builtin.pencil")
        assertEquals(9f, lib.effective(BrushTool.PAINT).size, 0f)                  // pencil remembered
        assertEquals(0.5f, lib.effective(BrushTool.PAINT).opacity, 0f)
        lib = lib.select(BrushTool.ERASER, "builtin.pencil")
        assertEquals(5f, lib.effective(BrushTool.ERASER).size, 0f)                 // per tool
        assertEquals(BrushGrain.PAPER, lib.effective(BrushTool.PAINT).grain)       // every other parameter from the preset
        assertNull(lib.select(BrushTool.PAINT, "nope").takeIf { it != lib })        // unknown id: unchanged
    }

    @Test
    fun recentsAreMostRecentFirstUniqueAndBounded() {
        var lib = fresh()
        for (id in BrushPresetsTest.REQUIRED) lib = lib.select(BrushTool.PAINT, id)
        lib = lib.select(BrushTool.PAINT, "builtin.ink")
        assertEquals("builtin.ink", lib.recents.first())
        assertEquals(lib.recents.toSet().size, lib.recents.size)
        assertEquals(BrushLibrary.MAX_RECENTS, lib.recents.size)
        assertEquals(lib.recents, lib.list(BrushFilter.RECENT).map { it.id })
    }

    @Test
    fun favorites() {
        var lib = fresh().toggleFavorite("builtin.chalk").toggleFavorite("builtin.ink")
        assertEquals(listOf("builtin.chalk", "builtin.ink"), lib.list(BrushFilter.FAVORITES).map { it.id })
        lib = lib.toggleFavorite("builtin.chalk")
        assertEquals(listOf("builtin.ink"), lib.favorites)
        assertEquals(lib, lib.toggleFavorite("builtin.missing"))
    }

    @Test
    fun duplicateMakesAUniqueUserPresetWithTheCurrentSettings() {
        var lib = fresh().select(BrushTool.PAINT, "builtin.pencil").setLive(BrushTool.PAINT, size = 11f)
        lib = lib.duplicate("builtin.pencil", "user.a1", BrushTool.PAINT)
        val a = lib.get("user.a1")!!
        assertFalse(a.builtIn)
        assertEquals("Pencil copy", a.displayName(italian = false))
        assertEquals(11f, a.params.size, 0f)
        assertEquals(BrushGrain.PAPER, a.params.grain)
        assertEquals("builtin.pencil", a.baseId)
        lib = lib.duplicate("builtin.pencil", "user.a2", BrushTool.PAINT)
        assertEquals("Pencil copy 2", lib.get("user.a2")!!.name)
        lib = lib.duplicate("user.a1", "user.a3")
        assertEquals("builtin.pencil", lib.get("user.a3")!!.baseId)
        // Refused: existing id, non-user id, unknown source.
        assertEquals(lib, lib.duplicate("builtin.ink", "user.a1"))
        assertEquals(lib, lib.duplicate("builtin.ink", "builtin.ink2"))
        assertEquals(lib, lib.duplicate("missing", "user.z9"))
        assertEquals(listOf("user.a1", "user.a2", "user.a3"), lib.list(BrushFilter.MINE).map { it.id })
    }

    @Test
    fun renameOnlyUserPresetsWithACleanName() {
        var lib = fresh().duplicate("builtin.ink", "user.b1")
        lib = lib.rename("user.b1", "  Comic   inks ")
        assertEquals("Comic inks", lib.get("user.b1")!!.name)
        assertEquals(lib, lib.rename("user.b1", "   "))
        assertEquals(lib, lib.rename("builtin.ink", "Mine"))
        assertTrue(lib.canRename("user.b1"))
        assertFalse(lib.canRename("builtin.ink"))
    }

    @Test
    fun deleteOnlyUserPresetsAndCleanUpReferences() {
        var lib = fresh().duplicate("builtin.chalk", "user.c1").select(BrushTool.PAINT, "user.c1").select(BrushTool.ERASER, "user.c1")
            .toggleFavorite("user.c1").setLive(BrushTool.PAINT, size = 70f)
        lib = lib.delete("user.c1")
        assertNull(lib.get("user.c1"))
        assertFalse("user.c1" in lib.favorites)
        assertFalse("user.c1" in lib.recents)
        assertTrue(lib.live.keys.none { it.endsWith("user.c1") })
        assertEquals("builtin.round", lib.selectedId(BrushTool.PAINT))
        assertEquals("builtin.hard", lib.selectedId(BrushTool.ERASER))
        assertEquals(lib, lib.delete("builtin.round"))     // built-ins cannot be deleted
        assertEquals(28, lib.all().size)
    }

    @Test
    fun customisingAndResettingABuiltIn() {
        val orig = fresh().get("builtin.chalk")!!.params
        var lib = fresh().update("builtin.chalk", orig.copy(grainDepth = 0.2f, spacing = 0.3f))
        assertTrue(lib.isModified("builtin.chalk"))
        assertEquals(0.2f, lib.get("builtin.chalk")!!.params.grainDepth, 0f)
        assertEquals(orig, lib.original("builtin.chalk")!!.params)          // the shipped original is untouched
        lib = lib.update("builtin.chalk", orig)                              // back to the original: no override left
        assertFalse(lib.isModified("builtin.chalk"))
        lib = lib.update("builtin.chalk", orig.copy(hardness = 0.1f)).select(BrushTool.PAINT, "builtin.chalk").setLive(BrushTool.PAINT, size = 80f)
        lib = lib.reset("builtin.chalk")
        assertEquals(orig, lib.effective(BrushTool.PAINT))
        assertEquals(lib, lib.reset("user.none"))
    }

    @Test
    fun editingKeepsQuickValuesUnlessSizeOrOpacityChange() {
        var lib = fresh().select(BrushTool.PAINT, "builtin.ink").setLive(BrushTool.PAINT, size = 4f)
        lib = lib.update("builtin.ink", lib.get("builtin.ink")!!.params.copy(pressureSize = false))
        assertEquals(4f, lib.effective(BrushTool.PAINT).size, 0f)
        lib = lib.update("builtin.ink", lib.get("builtin.ink")!!.params.copy(size = 15f))
        assertEquals(15f, lib.effective(BrushTool.PAINT).size, 0f)
    }

    @Test
    fun libraryFileRoundTrip() {
        var lib = fresh().duplicate("builtin.watercolor", "user.w1").rename("user.w1", "Wash = soft\nedges")
            .update("builtin.pencil", fresh().get("builtin.pencil")!!.params.copy(hardness = 0.5f))
            .select(BrushTool.PAINT, "user.w1").select(BrushTool.ERASER, "builtin.chalk")
            .setLive(BrushTool.PAINT, size = 77f, opacity = 0.4f)
            .toggleFavorite("builtin.ink").toggleFavorite("user.w1")
        lib = lib.setCategory("user.w1", BrushCategory.PAINTING)
        val back = BrushLibrary.decode(lib.encode(), builtIns)!!
        assertEquals(lib.user, back.user)
        assertEquals(lib.overrides, back.overrides)
        assertEquals(lib.favorites, back.favorites)
        assertEquals(lib.recents, back.recents)
        assertEquals(lib.selected, back.selected)
        assertEquals(lib.live, back.live)
        assertEquals(lib.effective(BrushTool.PAINT), back.effective(BrushTool.PAINT))
        assertEquals(back.encode(), BrushLibrary.decode(back.encode(), builtIns)!!.encode())
    }

    @Test
    fun damagedOrForeignFilesAreHandled() {
        assertNull(BrushLibrary.decode("not a library", builtIns))
        val text = """
            pxbrushlib 1
            [state]
            selected.paint = user.gone
            selected.eraser = builtin.ink
            favorites = builtin.ink,user.gone,,builtin.ink
            recents = user.gone,builtin.chalk
            [live paint/user.gone]
            size = 10
            opacity = 1
            [live paint/builtin.ink]
            size = abc
            opacity = 1
            [override builtin.nothere]
            size = 3
            [preset builtin.round]
            name = Fake built-in
            [preset user.ok]
            name = Fine
            size = 9999
        """.trimIndent()
        val lib = BrushLibrary.decode(text, builtIns)!!
        assertEquals("builtin.round", lib.selectedId(BrushTool.PAINT))   // missing preset -> default
        assertEquals("builtin.ink", lib.selectedId(BrushTool.ERASER))
        assertEquals(listOf("builtin.ink"), lib.favorites)
        assertEquals(listOf("builtin.chalk"), lib.recents)
        assertTrue(lib.live.isEmpty())
        assertTrue(lib.overrides.isEmpty())
        assertEquals(listOf("user.ok"), lib.user.map { it.id })            // a user file cannot replace a built-in
        assertEquals(BrushState.MAX_SIZE, lib.get("user.ok")!!.params.size, 0f)
        assertEquals("Basic Round", lib.get("builtin.round")!!.name)
    }

    @Test
    fun block4UsersKeepTheirBrush() {
        val legacy = BrushState(
            marker = ToolBrush(17f, 0.6f, 0.2f, pressureSize = true, minSize = 0.4f, pressureOpacity = true, minOpacity = 0.5f),
            eraser = ToolBrush(90f, 0.9f, 0.08f),
        )
        val lib = BrushLibrary.migrate(builtIns, legacy)
        assertEquals("builtin.hard", lib.selectedId(BrushTool.PAINT))
        assertEquals("builtin.hard", lib.selectedId(BrushTool.ERASER))
        val p = lib.effective(BrushTool.PAINT)
        assertTrue(p.classic)                     // the same rasteriser as Block 3/4: same pixels
        assertEquals(17f, p.size, 0f)
        assertEquals(0.6f, p.opacity, 0f)
        assertEquals(0.2f, p.spacing, 0f)
        assertTrue(p.pressureOpacity)
        assertEquals(0.4f, p.minSize, 0f)
        assertEquals(90f, lib.effective(BrushTool.ERASER).size, 0f)
        assertTrue(lib.isModified("builtin.hard"))   // resettable
        // Untouched Block 4 defaults: nothing to customise.
        assertFalse(BrushLibrary.migrate(builtIns, BrushState()).isModified("builtin.hard"))
    }

    @Test
    fun searchByNameTranslationCategoryAndTag() {
        val lib = fresh()
        fun ids(q: String, f: BrushFilter = BrushFilter.ALL) = lib.list(f, q).map { it.id }
        assertEquals(listOf("builtin.charcoal"), ids("CHARC"))
        assertEquals(listOf("builtin.watercolor"), ids("acquerello"))       // Italian name
        assertEquals(listOf("builtin.chalk"), ids("gessetto"))
        assertTrue(ids("inking").containsAll(listOf("builtin.ink", "builtin.fineliner", "builtin.marker")))
        assertTrue("builtin.textured" in ids("canvas"))                     // tag
        assertEquals(listOf("builtin.pencil", "builtin.graphite", "builtin.6b"), ids("pencil", BrushFilter.SKETCHING))
        assertEquals(listOf("builtin.graphite"), ids("pencil shading"))    // every term must match
        assertEquals(listOf("builtin.round"), ids("Tondo"))
        assertTrue(ids("zzz").isEmpty())
        assertEquals(2, lib.list(BrushFilter.DRY).size)
        assertEquals(1, lib.list(BrushFilter.AIRBRUSH).size)
    }

    @Test
    fun uniqueNames() {
        val lib = fresh()
        assertEquals("Pencil 2", lib.uniqueName("Pencil"))
        assertEquals("Matita 2", lib.uniqueName("matita".replaceFirstChar { it.uppercase() }))
        assertNotNull(lib.uniqueName("x".repeat(60)).takeIf { it.length <= BrushPreset.MAX_NAME })
    }
}
