package com.paintengine.pxpaint.core

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder

class LayersTest {
    private val tmp: File = createTempDir("pxlayers")

    private fun pattern(seed: Int) = ByteArray(TileContent.TILE_BYTES) { ((it * 31 + seed) and 0xFF).toByte() }
    private fun records(vararg t: Triple<Int, Int, ByteArray?>): ByteArray {
        val size = 4 + t.sumOf { TileContent.RECORD_HEADER + (it.third?.size ?: 0) }
        val b = ByteBuffer.allocate(size).order(ByteOrder.LITTLE_ENDIAN)
        b.putInt(t.size)
        for ((x, y, px) in t) {
            b.putInt(x); b.putInt(y); b.putInt(if (px == null) TileContent.FLAG_EMPTY else 0)
            if (px != null) b.put(px)
        }
        return b.array()
    }

    private val sample = LayerStack(listOf(
        LayerInfo(1, "Sketch", visible = true, locked = false, opacity = 0.8f, blend = BlendMode.NORMAL),
        LayerInfo(4, "Shadows \\ = \"x\"", visible = false, locked = true, opacity = 0.35f, blend = BlendMode.MULTIPLY),
        LayerInfo(7, "Ink line", opacity = 1f, blend = BlendMode.DIFFERENCE),
    ), activeId = 4)

    @Test
    fun engineTableParsing() {
        val table = "active\t4\n1\t1\t0\t0.800000\t0\t12\t99\tSketch\n4\t0\t1\t0.350000\t1\t0\t3\tA\\tB\\nC\\\\D\n7\t1\t0\t1.000000\t7\t3\t5\tInk\n"
        val s = LayerStack.parse(table)!!
        assertEquals(listOf(1, 4, 7), s.layers.map { it.id })
        assertEquals(4, s.activeId)
        assertEquals("A\tB\nC\\D", s.get(4)!!.name)
        assertEquals(BlendMode.MULTIPLY, s.get(4)!!.blend)
        assertFalse(s.get(4)!!.visible)
        assertTrue(s.get(4)!!.locked)
        assertEquals(12, s.get(1)!!.tiles)
        assertEquals(listOf(7, 4, 1), s.topDown.map { it.id })
        assertNull(LayerStack.parse(""))
        assertEquals(7, LayerStack.parse("active\t99\n7\t1\t0\t1\t0\t0\t0\tX\n")!!.activeId)  // unknown active -> top
    }

    @Test
    fun checksBeforeAskingTheEngine() {
        val s = sample
        assertEquals(LayerError.HIDDEN, s.paintBlocker())            // active = 4, hidden (and locked)
        assertEquals(LayerError.LOCKED, s.copy(layers = s.layers.map { it.copy(visible = true) }).paintBlocker())
        assertNull(s.copy(activeId = 7).paintBlocker())
        assertEquals(LayerError.NO_LAYER_BELOW, s.mergeDownBlocker(1))
        assertEquals(LayerError.LOCKED, s.mergeDownBlocker(7))         // the layer below (4) is locked
        assertEquals(LayerError.NOT_FOUND, s.mergeDownBlocker(99))
        assertEquals(LayerError.LAST_LAYER, LayerStack(listOf(LayerInfo(1, "a")), 1).deleteBlocker(1))
        assertNull(s.deleteBlocker(7))
        assertEquals(1, s.hiddenCount)
        assertNull(s.flattenBlocker())                                 // the locked layer is hidden: deleted by flatten
        assertTrue(s.canMoveUp(1) && !s.canMoveUp(7) && s.canMoveDown(7) && !s.canMoveDown(1))
        assertEquals("Layer 4", s.newLayerName("Layer"))
        assertEquals(null, LayerStack.cleanName("  \n "))
        assertEquals("A b", LayerStack.cleanName(" A \t b "))
        for (e in LayerError.entries) if (e != LayerError.NONE) assertNotNull(e.message)
        assertEquals(LayerError.BAD_VALUE, LayerError.of(1234))
    }

    @Test
    fun manifestRoundTripAndValidation() {
        val text = LayerManifest.encode(sample, 800, 600)
        val back = LayerManifest.decode(text, 800, 600)
        assertEquals(sample.layers.map { it.copy(tiles = 0, version = 0) }, back.layers)
        assertEquals(4, back.activeId)
        assertThrows(LayerManifest.Invalid::class.java) { LayerManifest.decode(text, 801, 600) }
        assertThrows(LayerManifest.Invalid::class.java) { LayerManifest.decode("pxlayers 2\nwidth = 800\nheight = 600\n[layer 1]\n", 800, 600) }
        assertThrows(LayerManifest.Invalid::class.java) { LayerManifest.decode("pxlayers 1\nwidth = 800\nheight = 600\n", 800, 600) }
        assertThrows(LayerManifest.Invalid::class.java) {
            LayerManifest.decode("pxlayers 1\nwidth = 800\nheight = 600\n[layer 1]\n[layer 1]\n", 800, 600)
        }
        assertThrows(LayerManifest.Invalid::class.java) {
            LayerManifest.decode("pxlayers 1\nwidth = 800\nheight = 600\n[layer 1]\nblend = sparkle\n", 800, 600)
        }
        assertThrows(LayerManifest.Invalid::class.java) { LayerManifest.decode("pxlayers 1\nwidth = 800\nheight = 600\n[layer x]\n", 800, 600) }
        // Tolerated: unknown keys, out-of-range opacity (clamped), missing active (top layer).
        val lax = LayerManifest.decode("pxlayers 1\nwidth = 8\nheight = 8\nfuture = 1\n[layer 2]\nopacity = 7\nglow = yes\n", 8, 8)
        assertEquals(1f, lax.layers.single().opacity, 0f)
        assertEquals(2, lax.activeId)
        assertEquals("Layer 2", lax.layers.single().name)
    }

    @Test
    fun layeredSaveReloadAndRemovedLayers() {
        val store = ProjectStore(tmp)
        val p = store.create("L", 600, 300, -1, false)
        val initial = store.loadLayers(p.id)!!
        assertEquals(listOf(1), initial.stack.layers.map { it.id })
        val l1 = TileContent(600, 300).apply { applyExport(records(Triple(0, 0, pattern(1)))) }
        val l4 = TileContent(600, 300).apply { applyExport(records(Triple(2, 1, pattern(4)))) }
        val l7 = TileContent(600, 300)
        store.saveLayers(p.id, sample, mapOf(1 to l1, 4 to l4, 7 to l7), emptyList())
        val back = store.loadLayers(p.id)!!
        assertEquals(sample.layers.map { it.copy(tiles = 0, version = 0) }, back.stack.layers)
        assertEquals(4, back.stack.activeId)
        assertArrayEquals(pattern(4), back.tiles.getValue(4).tilePixels(2, 1))
        assertArrayEquals(pattern(1), back.tiles.getValue(1).tilePixels(0, 0))
        // Layer 4 removed (e.g. merged down): its file goes, the others stay.
        val after = LayerStack(sample.layers.filter { it.id != 4 }, 7)
        store.saveLayers(p.id, after, emptyMap(), listOf(4))
        assertFalse(File(File(tmp, p.id), "layer-4.pxtiles").exists())
        assertEquals(listOf(1, 7), store.loadLayers(p.id)!!.stack.layers.map { it.id })
        // A new layer that never had pixels still gets a file.
        val withNew = LayerStack(after.layers + LayerInfo(9, "New"), 9)
        store.saveLayers(p.id, withNew, emptyMap(), emptyList())
        assertEquals(0, store.loadLayers(p.id)!!.tiles.getValue(9).tileCount)
        // Duplicate copies every layer.
        val copy = store.duplicate(p.id)
        assertArrayEquals(pattern(1), store.loadLayers(copy.id)!!.tiles.getValue(1).tilePixels(0, 0))
    }

    @Test
    fun oldProjectsMigrateWithoutLosingAnything() {
        val store = ProjectStore(tmp)
        val p = store.create("Old", 600, 300, -1, false)
        val dir = File(tmp, p.id)
        // Turn it into a Block 3-5 project: single raster in content.pxtiles, no manifest.
        File(dir, LayerManifest.FILE).delete()
        File(dir, "layer-1.pxtiles").delete()
        store.saveTiles(p.id, TileContent(600, 300).apply { applyExport(records(Triple(1, 0, pattern(9)))) })
        assertNull(store.loadLayers(p.id))                          // not layered yet
        val single = store.loadTiles(p.id)!!                        // still readable as before
        assertArrayEquals(pattern(9), single.tilePixels(1, 0))
        // First save after opening: layer 1 = the old raster; the old file is replaced.
        store.saveLayers(p.id, LayerStack(listOf(LayerInfo(1, "Layer 1")), 1), mapOf(1 to single), emptyList())
        assertFalse(File(dir, "content.pxtiles").exists())
        assertArrayEquals(pattern(9), store.loadLayers(p.id)!!.tiles.getValue(1).tilePixels(1, 0))
    }

    @Test
    fun damagedLayeredProjectsRefuseToOpen() {
        val store = ProjectStore(tmp)
        val p = store.create("D", 600, 300, -1, false)
        val dir = File(tmp, p.id)
        store.saveLayers(p.id, sample, mapOf(1 to TileContent(600, 300), 4 to TileContent(600, 300), 7 to TileContent(600, 300)), emptyList())
        File(dir, "layer-7.pxtiles").delete()
        assertThrows(ProjectStore.StoreException::class.java) { store.loadLayers(p.id) }   // missing layer
        File(dir, "layer-7.pxtiles").writeBytes(byteArrayOf(1, 2, 3))
        assertThrows(ProjectStore.StoreException::class.java) { store.loadLayers(p.id) }   // damaged layer
        File(dir, LayerManifest.FILE).writeText("garbage")
        assertThrows(ProjectStore.StoreException::class.java) { store.loadLayers(p.id) }   // damaged manifest
    }
}
