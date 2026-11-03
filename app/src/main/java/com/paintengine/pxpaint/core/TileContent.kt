package com.paintengine.pxpaint.core

import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.EOFException
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.TreeMap
import java.util.zip.DataFormatException
import java.util.zip.Deflater
import java.util.zip.Inflater

/**
 * Canvas pixels as stored in a project (Block 3+): the engine's 256 x 256
 * raster tiles, premultiplied RGBA8, each deflate-compressed. Empty tiles
 * are not stored.
 *
 * The app keeps one instance per open canvas: a save exports only the tiles
 * the engine changed since the last export ([applyExport]), compresses just
 * those, and writes the whole (mostly unchanged, already compressed) set
 * atomically. Pure JVM, unit-tested.
 *
 * File (`content.pxtiles`, big-endian):
 *   "PXTL", int version, int width, int height, int tileSize, int count,
 *   count x { int tx, int ty, int length, length bytes of deflate data }
 *
 * Engine records (NativeEngine.exportCanvasTiles / importCanvasTiles,
 * little-endian): int count, count x { int tx, int ty, int flags,
 * TILE_BYTES unless flags and FLAG_EMPTY }.
 */
class TileContent(val width: Int, val height: Int) {
    private val tiles = TreeMap<Long, ByteArray>()

    val columns: Int get() = (width + TILE - 1) / TILE
    val rows: Int get() = (height + TILE - 1) / TILE
    val tileCount: Int get() = tiles.size
    val compressedBytes: Long get() = tiles.values.sumOf { it.size.toLong() }
    fun has(tx: Int, ty: Int): Boolean = tiles.containsKey(key(tx, ty))
    fun clear() = tiles.clear()

    /**
     * Merges engine records: changed tiles are recompressed, tiles reported
     * empty are dropped. Returns the number of records read.
     */
    fun applyExport(records: ByteArray): Int {
        val buf = ByteBuffer.wrap(records).order(ByteOrder.LITTLE_ENDIAN)
        if (buf.remaining() < 4) throw IOException("export too short")
        val count = buf.int
        if (count < 0) throw IOException("bad export count $count")
        val deflater = Deflater(Deflater.BEST_SPEED)
        val pixels = ByteArray(TILE_BYTES)
        try {
            repeat(count) {
                if (buf.remaining() < RECORD_HEADER) throw IOException("truncated export")
                val tx = buf.int
                val ty = buf.int
                val flags = buf.int
                checkTile(tx, ty)
                if (flags and FLAG_EMPTY != 0) {
                    tiles.remove(key(tx, ty))
                } else {
                    if (buf.remaining() < TILE_BYTES) throw IOException("truncated tile $tx,$ty")
                    buf.get(pixels)
                    tiles[key(tx, ty)] = compress(deflater, pixels)
                }
            }
        } finally {
            deflater.end()
        }
        return count
    }

    /**
     * Engine records for [NativeEngine.importCanvasTiles], in batches of at
     * most [tilesPerBatch] tiles (bounded memory for big canvases).
     */
    fun importBatches(tilesPerBatch: Int = 32): List<ByteArray> {
        require(tilesPerBatch > 0)
        val out = ArrayList<ByteArray>()
        val inflater = Inflater()
        try {
            val entries = tiles.entries.toList()
            for (start in entries.indices step tilesPerBatch) {
                val chunk = entries.subList(start, minOf(entries.size, start + tilesPerBatch))
                val buf = ByteBuffer.allocate(4 + chunk.size * (RECORD_HEADER + TILE_BYTES)).order(ByteOrder.LITTLE_ENDIAN)
                buf.putInt(chunk.size)
                for ((k, data) in chunk) {
                    buf.putInt(tx(k))
                    buf.putInt(ty(k))
                    buf.putInt(0)
                    inflateInto(inflater, data, buf.array(), buf.position())
                    buf.position(buf.position() + TILE_BYTES)
                }
                out += buf.array()
            }
        } finally {
            inflater.end()
        }
        return out
    }

    /** Raw (premultiplied) pixels of one tile, or null if it is empty. */
    fun tilePixels(tx: Int, ty: Int): ByteArray? {
        val data = tiles[key(tx, ty)] ?: return null
        val out = ByteArray(TILE_BYTES)
        val inflater = Inflater()
        try {
            inflateInto(inflater, data, out, 0)
        } finally {
            inflater.end()
        }
        return out
    }

    fun encode(): ByteArray {
        val bytes = ByteArrayOutputStream(64 + compressedBytes.toInt())
        DataOutputStream(bytes).use { out ->
            out.write(MAGIC)
            out.writeInt(VERSION)
            out.writeInt(width)
            out.writeInt(height)
            out.writeInt(TILE)
            out.writeInt(tiles.size)
            for ((k, data) in tiles) {
                out.writeInt(tx(k))
                out.writeInt(ty(k))
                out.writeInt(data.size)
                out.write(data)
            }
        }
        return bytes.toByteArray()
    }

    private fun checkTile(tx: Int, ty: Int) {
        if (tx !in 0 until columns || ty !in 0 until rows) throw IOException("tile $tx,$ty outside a ${width}x$height canvas")
    }

    companion object {
        const val TILE = 256
        const val TILE_BYTES = TILE * TILE * 4
        const val VERSION = 1
        const val FLAG_EMPTY = 1
        const val RECORD_HEADER = 12
        private val MAGIC = byteArrayOf('P'.code.toByte(), 'X'.code.toByte(), 'T'.code.toByte(), 'L'.code.toByte())
        private const val MAX_COMPRESSED = TILE_BYTES + 4096

        fun key(tx: Int, ty: Int): Long = (tx.toLong() shl 32) or (ty.toLong() and 0xFFFFFFFFL)
        private fun tx(key: Long): Int = (key shr 32).toInt()
        private fun ty(key: Long): Int = key.toInt()

        /** Parses a `content.pxtiles` file; throws IOException if it is damaged. */
        fun decode(bytes: ByteArray): TileContent {
            try {
                DataInputStream(ByteArrayInputStream(bytes)).use { input ->
                    val magic = ByteArray(4)
                    input.readFully(magic)
                    if (!magic.contentEquals(MAGIC)) throw IOException("not a PxPaint tile file")
                    val version = input.readInt()
                    if (version != VERSION) throw IOException("unsupported tile format version $version")
                    val w = input.readInt()
                    val h = input.readInt()
                    val tile = input.readInt()
                    if (CanvasSizeRules.validateSide(w) != null || CanvasSizeRules.validateSide(h) != null) {
                        throw IOException("bad canvas size ${w}x$h")
                    }
                    if (tile != TILE) throw IOException("unsupported tile size $tile")
                    val content = TileContent(w, h)
                    val count = input.readInt()
                    if (count < 0 || count > content.columns * content.rows) throw IOException("bad tile count $count")
                    val inflater = Inflater()
                    val check = ByteArray(TILE_BYTES)
                    try {
                        repeat(count) {
                            val tx = input.readInt()
                            val ty = input.readInt()
                            val len = input.readInt()
                            content.checkTile(tx, ty)
                            if (len <= 0 || len > MAX_COMPRESSED) throw IOException("bad tile length $len")
                            val data = ByteArray(len)
                            input.readFully(data)
                            inflateInto(inflater, data, check, 0)  // verifies the data now, not when drawing
                            content.tiles[key(tx, ty)] = data
                        }
                    } finally {
                        inflater.end()
                    }
                    return content
                }
            } catch (e: EOFException) {
                throw IOException("tile file truncated", e)
            }
        }

        private fun compress(deflater: Deflater, pixels: ByteArray): ByteArray {
            deflater.reset()
            deflater.setInput(pixels)
            deflater.finish()
            val out = ByteArrayOutputStream(16 * 1024)
            val chunk = ByteArray(32 * 1024)
            while (!deflater.finished()) {
                val n = deflater.deflate(chunk)
                out.write(chunk, 0, n)
            }
            return out.toByteArray()
        }

        private fun inflateInto(inflater: Inflater, data: ByteArray, dst: ByteArray, offset: Int) {
            inflater.reset()
            inflater.setInput(data)
            try {
                var done = 0
                while (done < TILE_BYTES) {
                    val n = inflater.inflate(dst, offset + done, TILE_BYTES - done)
                    if (n == 0 && (inflater.finished() || inflater.needsInput())) break
                    done += n
                }
                if (done != TILE_BYTES || !inflater.finished()) throw IOException("damaged tile data ($done bytes)")
            } catch (e: DataFormatException) {
                throw IOException("damaged tile data", e)
            }
        }
    }
}
