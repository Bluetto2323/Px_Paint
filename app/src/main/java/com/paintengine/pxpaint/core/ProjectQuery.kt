package com.paintengine.pxpaint.core

import java.text.Normalizer

enum class SortOrder { RECENT, NAME, SIZE, OLDEST }

/** Gallery search and sorting (pure, unit-tested). */
object ProjectQuery {
    fun apply(projects: List<ProjectMeta>, search: String, sort: SortOrder): List<ProjectMeta> {
        val tokens = normalize(search).split(' ').filter { it.isNotEmpty() }
        val filtered = if (tokens.isEmpty()) projects else projects.filter { p ->
            val haystack = normalize(p.name) + " " + "${p.width}x${p.height}" + " " + "${p.width}×${p.height}"
            tokens.all { haystack.contains(it) }
        }
        val comparator: Comparator<ProjectMeta> = when (sort) {
            SortOrder.RECENT -> compareByDescending<ProjectMeta> { it.modifiedMs }.thenBy { normalize(it.name) }
            SortOrder.OLDEST -> compareBy<ProjectMeta> { it.createdMs }.thenBy { normalize(it.name) }
            SortOrder.NAME -> compareBy<ProjectMeta> { normalize(it.name) }.thenByDescending { it.modifiedMs }
            SortOrder.SIZE -> compareByDescending<ProjectMeta> { it.pixelCount }.thenBy { normalize(it.name) }
        }
        return filtered.sortedWith(comparator.thenBy { it.id })
    }

    /** Lower case, accents removed ("Città" matches "citta"), single spaces. */
    fun normalize(s: String): String =
        Normalizer.normalize(s.lowercase(), Normalizer.Form.NFD)
            .replace(Regex("\\p{Mn}+"), "")
            .replace(Regex("\\s+"), " ")
            .trim()
}
