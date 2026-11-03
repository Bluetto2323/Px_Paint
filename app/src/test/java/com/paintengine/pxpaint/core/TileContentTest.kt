package com.paintengine.pxpaint.core

import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.file.Files

class TileContentTest {
    private val tmp: File = Files.createTempDirectory("pxtiles").toFile()

    @After
    fun cleanup() {
        tmp.deleteRecursively()
    }

    /** Engine export records: (tx, ty, pixels or null = empty). */
    private fun records(vararg tiles: Triple<Int, Int, ByteArray?>): ByteArray {
        val size = 4 + tiles.sumOf { TileContent.RECORD_HEADER + (it.third?.size ?: 0) }
        val buf = ByteBuffer.allocate(size).order(ByteOrder.LITTLE_ENDIAN)
        buf.putInt(tiles.size)
        for ((tx, ty, px) in tiles) {
            buf.putInt(tx)
            buf.putInt(ty)
            buf.putInt(if (px == null) TileContent.FLAG_EMPTY else 0)
            if (px != null) buf.put(px)
        }
        return buf.array()
    }

    private fun pattern(seed: Int): ByteArray = ByteArray(TileContent.TILE_BYTES) { i ->
        // A painted-looking tile: smooth gradient + a few hard edges.
        (((i / 4) % 256) * (seed + 1) / 3 + (if ((i / 1024) % 7 == 0) 200 else 0)).toByte()
    }

    @Test
    fun exportIsCompressedAndRoundTrips() {
        val c = TileContent(600, 300)  // 3 x 2 tiles
        val a = pattern(1)
        val b = pattern(5)
        assertEquals(2, c.applyExport(records(Triple(0, 0, a), Triple(2, 1, b))))
        assertEquals(2, c.tileCount)
        assertTrue("compressed", c.compressedBytes < 2L * TileContent.TILE_BYTES / 2)
        val back = TileContent.decode(c.encode())
        assertEquals(600, back.width)
        assertEquals(300, back.height)
        assertArrayEquals(a, back.tilePixels(0, 0))
        assertArrayEquals(b, back.tilePixels(2, 1))
        assertNull(back.tilePixels(1, 0))
    }

    @Test
    fun emptyTilesAreDroppedAndChangedTilesReplaced() {
        val c = TileContent(600, 300)
        c.applyExport(records(Triple(0, 0, pattern(1)), Triple(1, 0, pattern(2))))
        c.applyExport(records(Triple(0, 0, null), Triple(1, 0, pattern(3))))
        assertFalse(c.has(0, 0))
        assertArrayEquals(pattern(3), c.tilePixels(1, 0))
        assertEquals(1, c.tileCount)
    }

    @Test
    fun importBatchesReproduceEngineRecords() {
        val c = TileContent(1100, 600)  // 5 x 3 tiles
        val tiles = (0 until 7).map { Triple(it % 5, it / 5, pattern(it)) }
        c.applyExport(records(*tiles.toTypedArray()))
        val batches = c.importBatches(tilesPerBatch = 3)
        assertEquals(3, batches.size)
        var seen = 0
        for (batch in batches) {
            val buf = ByteBuffer.wrap(batch).order(ByteOrder.LITTLE_ENDIAN)
            val n = buf.int
            repeat(n) {
                val tx = buf.int
                val ty = buf.int
                assertEquals(0, buf.int)
                val px = ByteArray(TileContent.TILE_BYTES)
                buf.get(px)
                assertArrayEquals(tiles.first { it.first == tx && it.second == ty }.third, px)
                seen++
            }
            assertEquals(0, buf.remaining())
        }
        assertEquals(7, seen)
    }

    @Test
    fun damagedFilesAreRejected() {
        val c = TileContent(600, 300)
        c.applyExport(records(Triple(1, 1, pattern(4))))
        val good = c.encode()
        assertThrows(IOException::class.java) { TileContent.decode(good.copyOf(good.size - 10)) }
        assertThrows(IOException::class.java) { TileContent.decode(byteArrayOf(1, 2, 3, 4, 5)) }
        val badMagic = good.copyOf().also { it[0] = 'Q'.code.toByte() }
        assertThrows(IOException::class.java) { TileContent.decode(badMagic) }
        // Corrupt the compressed data itself (header 24 bytes + record header 12).
        val badData = good.copyOf().also { for (i in 40 until 60) it[i] = 0x55 }
        assertThrows(IOException::class.java) { TileContent.decode(badData) }
        // Tile outside the canvas in an export.
        assertThrows(IOException::class.java) { TileContent(600, 300).applyExport(records(Triple(3, 0, pattern(1)))) }
        assertThrows(IOException::class.java) { TileContent(600, 300).applyExport(byteArrayOf(1, 0, 0, 0, 0)) }
    }

    @Test
    fun storeSavesTilesAndKeepsLegacyProjectsReadable() {
        var now = 1000L
        val store = ProjectStore(tmp) { now }
        val p = store.create("Tiles", 600, 300, -1, false)
        assertTrue(store.hasTiles(p.id))
        assertNull(store.loadTiles(p.id))                        // new projects are layered (Block 6)
        assertEquals(0, store.loadLayers(p.id)!!.tiles.getValue(1).tileCount)
        val c = TileContent(600, 300).apply { applyExport(records(Triple(2, 1, pattern(7)))) }
        now = 2000L
        val saved = store.saveTiles(p.id, c)
        assertEquals(1, saved.opCount)
        assertEquals(2000L, saved.modifiedMs)
        assertArrayEquals(pattern(7), store.loadTiles(p.id)!!.tilePixels(2, 1))
        val copy = store.duplicate(p.id)
        assertArrayEquals(pattern(7), store.loadTiles(copy.id)!!.tilePixels(2, 1))
        assertThrows(ProjectStore.StoreException::class.java) { store.saveTiles(p.id, TileContent(601, 300)) }
        // A Block 2 project: only content.pxops exists -> loadTiles null, ops still readable.
        val legacy = store.create("Old", 200, 200, -1, false)
        File(File(tmp, legacy.id), "content.pxtiles").delete()
        store.saveContent(legacy.id, intArrayOf(10, 10, 5, 5, 0x7F00FF00))
        assertNull(store.loadTiles(legacy.id))
        assertArrayEquals(intArrayOf(10, 10, 5, 5, 0x7F00FF00), store.loadContent(legacy.id))
        // Damaged tile file -> StoreException (the UI then refuses to overwrite it).
        File(File(tmp, p.id), "content.pxtiles").writeBytes(byteArrayOf(9, 9, 9))
        assertThrows(ProjectStore.StoreException::class.java) { store.loadTiles(p.id) }
    }
}
