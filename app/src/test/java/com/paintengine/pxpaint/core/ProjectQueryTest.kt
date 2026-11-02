package com.paintengine.pxpaint.core

import org.junit.Assert.assertEquals
import org.junit.Test

class ProjectQueryTest {
    private fun p(id: String, name: String, w: Int, h: Int, created: Long, modified: Long) =
        ProjectMeta(id, name, w, h, 0, false, created, modified, 0)

    private val projects = listOf(
        p("a", "Città di notte", 2048, 2048, 10, 500),
        p("b", "Portrait study", 2480, 3508, 20, 300),
        p("c", "landscape", 3840, 2160, 30, 900),
        p("d", "Ritratto", 1080, 1350, 40, 900),
    )

    private fun ids(list: List<ProjectMeta>) = list.joinToString("") { it.id }

    @Test
    fun sortOrders() {
        assertEquals("cdab", ids(ProjectQuery.apply(projects, "", SortOrder.RECENT)))  // tie 900: by name
        assertEquals("abcd", ids(ProjectQuery.apply(projects, "", SortOrder.OLDEST)))
        assertEquals("acbd", ids(ProjectQuery.apply(projects, "", SortOrder.NAME)))     // case-insensitive
        assertEquals("bcad", ids(ProjectQuery.apply(projects, "", SortOrder.SIZE)))   // 8.70, 8.29, 4.19, 1.46 Mpx
    }

    @Test
    fun searchIgnoresCaseAndAccents() {
        assertEquals("a", ids(ProjectQuery.apply(projects, "citta", SortOrder.NAME)))
        assertEquals("a", ids(ProjectQuery.apply(projects, "  CITTÀ   NOTTE ", SortOrder.NAME)))
        assertEquals("", ids(ProjectQuery.apply(projects, "citta giorno", SortOrder.NAME)))
    }

    @Test
    fun searchMatchesDimensions() {
        assertEquals("a", ids(ProjectQuery.apply(projects, "2048x2048", SortOrder.NAME)))
        assertEquals("b", ids(ProjectQuery.apply(projects, "2480×", SortOrder.NAME)))
    }
}
