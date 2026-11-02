package com.paintengine.pxpaint.core

import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.IOException

/**
 * Persisted canvas content (Block 2 format, `content.pxops`).
 *
 * Until raster layers exist, a canvas's content is the ordered list of
 * rectangle fills produced by the preview tools: 5 ints per op
 * {x, y, w, h, argb} with *replace* semantics (see docs/ENGINE_API.md).
 * This is the same flat layout as NativeEngine.canvasContent().
 *
 * Binary layout (big-endian): "PXOP" | version:int | count:int | count*5 ints.
 */
object ContentLog {
    const val VERSION = 1
    const val INTS_PER_OP = 5
    const val MAX_OPS = 100_000   // engine journal limit
    private val MAGIC = byteArrayOf('P'.code.toByte(), 'X'.code.toByte(), 'O'.code.toByte(), 'P'.code.toByte())

    fun encode(ops: IntArray): ByteArray {
        require(ops.size % INTS_PER_OP == 0) { "ops size ${ops.size} is not a multiple of $INTS_PER_OP" }
        val count = ops.size / INTS_PER_OP
        require(count <= MAX_OPS) { "too many ops: $count" }
        val bytes = ByteArrayOutputStream(12 + ops.size * 4)
        DataOutputStream(bytes).use { out ->
            out.write(MAGIC)
            out.writeInt(VERSION)
            out.writeInt(count)
            for (v in ops) out.writeInt(v)
        }
        return bytes.toByteArray()
    }

    /** Throws IOException on a corrupt or unsupported file (never returns partial data). */
    fun decode(data: ByteArray): IntArray {
        if (data.size < 12) throw IOException("content file too short (${data.size} bytes)")
        DataInputStream(ByteArrayInputStream(data)).use { input ->
            val magic = ByteArray(4)
            input.readFully(magic)
            if (!magic.contentEquals(MAGIC)) throw IOException("not a PxPaint content file")
            val version = input.readInt()
            if (version != VERSION) throw IOException("unsupported content version $version")
            val count = input.readInt()
            if (count < 0 || count > MAX_OPS) throw IOException("invalid op count $count")
            val expected = 12L + count.toLong() * INTS_PER_OP * 4
            if (data.size.toLong() != expected) throw IOException("content size ${data.size} != expected $expected")
            return IntArray(count * INTS_PER_OP) { input.readInt() }
        }
    }

    fun opCount(ops: IntArray): Int = ops.size / INTS_PER_OP

    /** Ops clipped to the canvas, invalid ones dropped (same rule as the engine). */
    fun clip(ops: IntArray, width: Int, height: Int): IntArray {
        val out = ArrayList<Int>(ops.size)
        var i = 0
        while (i + INTS_PER_OP <= ops.size) {
            val l = maxOf(0, ops[i])
            val t = maxOf(0, ops[i + 1])
            val r = minOf(width.toLong(), ops[i].toLong() + ops[i + 2]).toInt()
            val b = minOf(height.toLong(), ops[i + 1].toLong() + ops[i + 3]).toInt()
            if (r > l && b > t) {
                out += l; out += t; out += r - l; out += b - t; out += ops[i + 4]
            }
            i += INTS_PER_OP
        }
        return out.toIntArray()
    }
}
