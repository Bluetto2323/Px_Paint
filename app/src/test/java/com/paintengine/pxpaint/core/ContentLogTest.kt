package com.paintengine.pxpaint.core

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test
import java.io.IOException

class ContentLogTest {
    @Test
    fun roundTrip() {
        val ops = intArrayOf(1, 2, 3, 4, 0xFF112233.toInt(), 10, 20, 30, 40, 0)
        assertArrayEquals(ops, ContentLog.decode(ContentLog.encode(ops)))
        assertArrayEquals(IntArray(0), ContentLog.decode(ContentLog.encode(IntArray(0))))
        assertEquals(2, ContentLog.opCount(ops))
    }

    @Test
    fun corruptFilesAreRejected() {
        val good = ContentLog.encode(intArrayOf(1, 2, 3, 4, 5))
        assertThrows(IOException::class.java) { ContentLog.decode(good.copyOf(good.size - 1)) }
        assertThrows(IOException::class.java) { ContentLog.decode(good.copyOf().also { it[0] = 'X'.code.toByte() }) }
        assertThrows(IOException::class.java) { ContentLog.decode(good.copyOf().also { it[7] = 9 }) }   // version
        assertThrows(IOException::class.java) { ContentLog.decode(ByteArray(3)) }
    }

    @Test
    fun clipMatchesTheEngineRule() {
        val ops = intArrayOf(
            -100, -100, 50, 50, 1,        // fully outside: dropped
            7990, 5990, 40, 40, 2,        // clipped to 10x10
            10, 10, 5, 5, 3,              // inside: kept
            20, 20, 0, 5, 4,              // empty: dropped
        )
        assertArrayEquals(intArrayOf(7990, 5990, 10, 10, 2, 10, 10, 5, 5, 3), ContentLog.clip(ops, 8000, 6000))
    }
}
