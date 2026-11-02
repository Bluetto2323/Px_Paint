package com.paintengine.pxpaint.core

import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import java.io.File
import java.nio.file.Files

class ProjectStoreTest {
    private lateinit var root: File
    private var now = 1_000_000L
    private lateinit var store: ProjectStore

    @Before
    fun setUp() {
        root = Files.createTempDirectory("pxpaint-store").toFile()
        store = ProjectStore(root) { now }
    }

    @After
    fun tearDown() {
        root.deleteRecursively()
    }

    @Test
    fun createListGet() {
        val p = store.create("  My   sketch ", 2048, 1024, 0xFFFFFFFF.toInt(), false)
        assertEquals("My sketch", p.name)
        assertEquals(listOf(p), store.list())
        assertEquals(p, store.get(p.id))
        assertEquals(0, store.loadContent(p.id).size)
    }

    @Test
    fun namesAreUnique() {
        val a = store.create("Study", 100, 100, 0, false)
        val b = store.create("Study", 100, 100, 0, false)
        val c = store.create("Study", 100, 100, 0, false)
        assertEquals(listOf("Study", "Study 2", "Study 3"), listOf(a.name, b.name, c.name))
        now++
        val renamed = store.rename(c.id, "Study")
        assertEquals("Study 3", renamed.name)   // its own name does not count, others do
        assertEquals("Fresh", store.rename(c.id, "Fresh").name)
        assertEquals("Untitled", store.rename(c.id, "   ").name)
    }

    @Test
    fun saveAndLoadContentClipsAndUpdatesMeta() {
        val p = store.create("A", 100, 50, 0, false)
        now += 5000
        val saved = store.saveContent(p.id, intArrayOf(90, 40, 20, 20, 7, -10, -10, 5, 5, 8))
        assertEquals(1, saved.opCount)
        assertEquals(now, saved.modifiedMs)
        assertArrayEquals(intArrayOf(90, 40, 10, 10, 7), store.loadContent(p.id))
        assertEquals(saved, store.get(p.id))
    }

    @Test
    fun duplicateCopiesContentAndThumbnail() {
        val p = store.create("Portrait", 300, 400, 0xFF102030.toInt(), true)
        store.saveContent(p.id, intArrayOf(1, 1, 2, 2, 9))
        store.saveThumbnail(p.id, byteArrayOf(1, 2, 3))
        now += 10
        val d = store.duplicate(p.id, "copy")
        assertNotEquals(p.id, d.id)
        assertEquals("Portrait copy", d.name)
        assertEquals(p.width to p.height, d.width to d.height)
        assertTrue(d.transparentBackground)
        assertArrayEquals(store.loadContent(p.id), store.loadContent(d.id))
        assertArrayEquals(byteArrayOf(1, 2, 3), store.thumbnailFile(d.id).readBytes())
        assertEquals("Portrait copy 2", store.duplicate(p.id, "copy").name)
    }

    @Test
    fun deleteRemovesEverything() {
        val p = store.create("Gone", 10, 10, 0, false)
        store.delete(p.id)
        assertNull(store.get(p.id))
        assertTrue(store.list().isEmpty())
        assertTrue(root.listFiles()!!.isEmpty())
    }

    @Test
    fun invalidIdsAreRejected() {
        assertThrows(ProjectStore.StoreException::class.java) { store.loadContent("../../etc") }
        assertThrows(ProjectStore.StoreException::class.java) { store.delete("..") }
        assertNull(store.get("nothere"))
    }

    @Test
    fun corruptProjectsAreSkippedNotFatal() {
        val ok = store.create("Fine", 10, 10, 0, false)
        val broken = File(root, "0123456789abcdef").apply { mkdirs() }
        File(broken, "project.properties").writeText("name=\nwidth=x")
        assertEquals(listOf(ok), store.list())
        File(root, "fedcba9876543210").mkdirs()            // no metadata at all
        assertEquals(listOf(ok), store.list())
    }

    @Test
    fun damagedContentIsReportedNotSilentlyEmptied() {
        val p = store.create("Damaged", 10, 10, 0, false)
        File(File(root, p.id), "content.pxops").writeBytes(byteArrayOf(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13))
        assertThrows(ProjectStore.StoreException::class.java) { store.loadContent(p.id) }
    }

    @Test
    fun writesLeaveNoTemporaryFiles() {
        val p = store.create("Clean", 10, 10, 0, false)
        store.saveContent(p.id, intArrayOf(0, 0, 1, 1, 1))
        store.rename(p.id, "Cleaner")
        val leftovers = File(root, p.id).listFiles()!!.filter { it.name.endsWith(".tmp") }
        assertTrue(leftovers.toString(), leftovers.isEmpty())
    }

    @Test
    fun newerFormatIsNotMisread() {
        val p = store.create("Future", 10, 10, 0, false)
        val meta = File(File(root, p.id), "project.properties")
        meta.writeText(meta.readText().replace("format=1", "format=99"))
        assertNull(store.get(p.id))
        assertFalse(store.list().any { it.id == p.id })
    }

    @Test
    fun invalidSizesAreRefused() {
        assertThrows(IllegalArgumentException::class.java) { store.create("X", 0, 10, 0, false) }
        assertThrows(IllegalArgumentException::class.java) { store.create("X", 10, 20000, 0, false) }
    }

    @Test
    fun namesAreCleaned() {
        assertEquals("a b", ProjectNames.clean("  a \t\n b  "))
        assertEquals("Untitled", ProjectNames.clean("\u0000\u0007"))
        assertEquals(ProjectNames.MAX_LENGTH, ProjectNames.clean("x".repeat(200)).length)
    }
}
