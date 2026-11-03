package com.paintengine.pxpaint.core

import java.io.File
import java.io.IOException
import java.util.Properties
import java.util.UUID

/** Metadata of one project (one canvas). `opCount`: stored content items (tiles since Block 3). */
data class ProjectMeta(
    val id: String,
    val name: String,
    val width: Int,
    val height: Int,
    val paperArgb: Int,
    val transparentBackground: Boolean,
    val createdMs: Long,
    val modifiedMs: Long,
    val opCount: Int,
) {
    val pixelCount: Long get() = width.toLong() * height.toLong()
}

/**
 * File-based project repository. Pure java.io (unit-tested on the JVM):
 *
 *   <root>/<id>/project.properties   metadata
 *   <root>/<id>/content.pxtiles      canvas pixels (TileContent, Block 3+)
 *   <root>/<id>/content.pxops        legacy Block 2 content (ContentLog), read
 *                                    only when no .pxtiles exists yet
 *   <root>/<id>/thumb.png            thumbnail (written by the UI layer)
 *
 * Every write goes to a temporary file first and is then renamed, so a crash
 * never leaves a half-written project. Not thread-safe: the app serialises
 * all calls on one background executor.
 */
class ProjectStore(private val root: File, private val clock: () -> Long = System::currentTimeMillis) {

    class StoreException(message: String, cause: Throwable? = null) : IOException(message, cause)

    init {
        if (!root.isDirectory && !root.mkdirs()) throw StoreException("cannot create $root")
    }

    fun list(): List<ProjectMeta> =
        root.listFiles()?.filter { it.isDirectory }?.mapNotNull { dir -> runCatching { readMeta(dir) }.getOrNull() }
            ?: emptyList()

    /** Null for unknown, malformed or unreadable ids (lookups never throw). */
    fun get(id: String): ProjectMeta? {
        if (!ID_PATTERN.matches(id)) return null
        return dirOf(id).takeIf { it.isDirectory }?.let { runCatching { readMeta(it) }.getOrNull() }
    }

    fun create(name: String, width: Int, height: Int, paperArgb: Int, transparentBackground: Boolean): ProjectMeta {
        require(CanvasSizeRules.validateSide(width) == null && CanvasSizeRules.validateSide(height) == null) {
            "invalid canvas size ${width}x$height"
        }
        val now = clock()
        val meta = ProjectMeta(
            id = newId(), name = uniqueName(ProjectNames.clean(name), names()), width = width, height = height,
            paperArgb = paperArgb, transparentBackground = transparentBackground,
            createdMs = now, modifiedMs = now, opCount = 0,
        )
        val dir = dirOf(meta.id)
        if (!dir.mkdirs()) throw StoreException("cannot create project directory")
        writeMeta(meta)
        writeAtomically(tilesFile(meta.id), TileContent(width, height).encode())
        return meta
    }

    fun rename(id: String, newName: String): ProjectMeta {
        val meta = require(id)
        val cleaned = ProjectNames.clean(newName)
        if (cleaned == meta.name) return meta
        val updated = meta.copy(name = uniqueName(cleaned, names() - meta.name), modifiedMs = clock())
        writeMeta(updated)
        return updated
    }

    fun duplicate(id: String, copySuffix: String = "copy"): ProjectMeta {
        val meta = require(id)
        val now = clock()
        val copy = meta.copy(
            id = newId(), name = uniqueName(ProjectNames.copyName(meta.name, copySuffix), names()),
            createdMs = now, modifiedMs = now,
        )
        val dir = dirOf(copy.id)
        if (!dir.mkdirs()) throw StoreException("cannot create project directory")
        contentFile(id).takeIf { it.isFile }?.let { writeAtomically(contentFile(copy.id), it.readBytes()) }
        tilesFile(id).takeIf { it.isFile }?.let { writeAtomically(tilesFile(copy.id), it.readBytes()) }
        thumbnailFile(id).takeIf { it.isFile }?.let { writeAtomically(thumbnailFile(copy.id), it.readBytes()) }
        writeMeta(copy)
        return copy
    }

    fun delete(id: String) {
        val dir = dirOf(id)
        if (dir.exists() && !dir.deleteRecursively()) throw StoreException("cannot delete project $id")
    }

    fun loadContent(id: String): IntArray {
        val f = contentFile(id)
        if (!f.isFile) return IntArray(0)
        return try {
            ContentLog.decode(f.readBytes())
        } catch (e: IOException) {
            throw StoreException("content of '$id' is damaged: ${e.message}", e)
        }
    }

    fun saveContent(id: String, ops: IntArray): ProjectMeta {
        val meta = require(id)
        val clipped = ContentLog.clip(ops, meta.width, meta.height)
        writeAtomically(contentFile(id), ContentLog.encode(clipped))
        val updated = meta.copy(modifiedMs = clock(), opCount = ContentLog.opCount(clipped))
        writeMeta(updated)
        return updated
    }

    /** Pixel content, or null for a legacy project that has none yet (use [loadContent]). */
    fun loadTiles(id: String): TileContent? {
        val meta = require(id)
        val f = tilesFile(id)
        if (!f.isFile) return null
        val content = try {
            TileContent.decode(f.readBytes())
        } catch (e: IOException) {
            throw StoreException("content of '$id' is damaged: ${e.message}", e)
        }
        if (content.width != meta.width || content.height != meta.height) {
            throw StoreException("content of '$id' is ${content.width}x${content.height}, project is ${meta.width}x${meta.height}")
        }
        return content
    }

    /** Writes the pixel content atomically. The legacy file, if any, is kept but no longer read. */
    fun saveTiles(id: String, content: TileContent): ProjectMeta {
        val meta = require(id)
        if (content.width != meta.width || content.height != meta.height) throw StoreException("content size mismatch")
        writeAtomically(tilesFile(id), content.encode())
        val updated = meta.copy(modifiedMs = clock(), opCount = content.tileCount)
        writeMeta(updated)
        return updated
    }

    fun hasTiles(id: String): Boolean = tilesFile(id).isFile

    fun updateAppearance(id: String, paperArgb: Int, transparentBackground: Boolean): ProjectMeta {
        val updated = require(id).copy(paperArgb = paperArgb, transparentBackground = transparentBackground, modifiedMs = clock())
        writeMeta(updated)
        return updated
    }

    fun thumbnailFile(id: String): File = File(dirOf(id), "thumb.png")

    /** Writes the thumbnail bytes atomically (PNG produced by the UI layer). */
    fun saveThumbnail(id: String, png: ByteArray) {
        require(id)
        writeAtomically(thumbnailFile(id), png)
    }

    // ------------------------------------------------------------------ internals
    private fun names(): Set<String> = list().map { it.name }.toSet()

    private fun require(id: String): ProjectMeta = get(id) ?: throw StoreException("project $id not found")

    private fun dirOf(id: String): File {
        if (!ID_PATTERN.matches(id)) throw StoreException("invalid project id '$id'")
        return File(root, id)
    }

    private fun contentFile(id: String) = File(dirOf(id), "content.pxops")
    private fun tilesFile(id: String) = File(dirOf(id), "content.pxtiles")
    private fun metaFile(id: String) = File(dirOf(id), "project.properties")

    private fun newId(): String = UUID.randomUUID().toString().replace("-", "").substring(0, 16)

    private fun writeMeta(meta: ProjectMeta) {
        val p = Properties()
        p["format"] = FORMAT_VERSION.toString()
        p["name"] = meta.name
        p["width"] = meta.width.toString()
        p["height"] = meta.height.toString()
        p["paper"] = Integer.toHexString(meta.paperArgb)
        p["transparent"] = meta.transparentBackground.toString()
        p["created"] = meta.createdMs.toString()
        p["modified"] = meta.modifiedMs.toString()
        p["ops"] = meta.opCount.toString()
        val bytes = java.io.ByteArrayOutputStream().also { p.store(it, "PxPaint project") }.toByteArray()
        writeAtomically(metaFile(meta.id), bytes)
    }

    private fun readMeta(dir: File): ProjectMeta {
        val id = dir.name
        val p = Properties()
        metaFile(id).inputStream().use { p.load(it) }
        fun int(key: String) = p.getProperty(key)?.toIntOrNull() ?: throw StoreException("$id: bad '$key'")
        fun long(key: String) = p.getProperty(key)?.toLongOrNull() ?: throw StoreException("$id: bad '$key'")
        val format = int("format")
        if (format > FORMAT_VERSION) throw StoreException("$id: project format $format is newer than this app")
        return ProjectMeta(
            id = id,
            name = p.getProperty("name")?.takeIf { it.isNotBlank() } ?: throw StoreException("$id: missing name"),
            width = int("width"),
            height = int("height"),
            paperArgb = java.lang.Long.parseLong(p.getProperty("paper") ?: "ffffffff", 16).toInt(),
            transparentBackground = p.getProperty("transparent") == "true",
            createdMs = long("created"),
            modifiedMs = long("modified"),
            opCount = p.getProperty("ops")?.toIntOrNull() ?: 0,
        )
    }

    private fun writeAtomically(target: File, bytes: ByteArray) {
        val tmp = File(target.parentFile, target.name + ".tmp")
        tmp.writeBytes(bytes)
        if (!tmp.renameTo(target)) {
            tmp.delete()
            throw StoreException("cannot write ${target.name}")
        }
    }

    companion object {
        const val FORMAT_VERSION = 1
        private val ID_PATTERN = Regex("[0-9a-f]{16}")

        /** "Name", "Name 2", "Name 3"... first one not in [taken]. */
        fun uniqueName(base: String, taken: Set<String>): String {
            if (base !in taken) return base
            var n = 2
            while (true) {
                val candidate = ProjectNames.clean("$base $n")
                if (candidate !in taken) return candidate
                n++
            }
        }
    }
}

object ProjectNames {
    const val MAX_LENGTH = 60

    /** Trims, collapses whitespace, removes control characters, limits length. Never empty. */
    fun clean(raw: String, fallback: String = "Untitled"): String {
        val collapsed = raw.filter { !it.isISOControl() }.trim().replace(Regex("\\s+"), " ")
        val limited = if (collapsed.length > MAX_LENGTH) collapsed.take(MAX_LENGTH).trimEnd() else collapsed
        return limited.ifEmpty { fallback }
    }

    fun copyName(name: String, suffix: String): String = clean("$name $suffix")
}
