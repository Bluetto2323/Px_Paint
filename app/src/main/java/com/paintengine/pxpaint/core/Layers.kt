package com.paintengine.pxpaint.core

import java.util.Locale

/** Layer blend modes; [code] = engine canvas::BlendMode. */
enum class BlendMode(val code: Int, val key: String, val label: S) {
    NORMAL(0, "normal", S.BLEND_NORMAL),
    MULTIPLY(1, "multiply", S.BLEND_MULTIPLY),
    SCREEN(2, "screen", S.BLEND_SCREEN),
    OVERLAY(3, "overlay", S.BLEND_OVERLAY),
    DARKEN(4, "darken", S.BLEND_DARKEN),
    LIGHTEN(5, "lighten", S.BLEND_LIGHTEN),
    ADD(6, "add", S.BLEND_ADD),
    DIFFERENCE(7, "difference", S.BLEND_DIFFERENCE);

    companion object {
        fun ofCode(c: Int): BlendMode? = entries.firstOrNull { it.code == c }
        fun ofKey(k: String?): BlendMode? = entries.firstOrNull { it.key == k }
    }
}

/** One layer as the engine reports it. [version] changes with its pixels; [tiles] = content tiles. */
data class LayerInfo(
    val id: Int,
    val name: String,
    val visible: Boolean = true,
    val locked: Boolean = false,
    val opacity: Float = 1f,
    val blend: BlendMode = BlendMode.NORMAL,
    val tiles: Int = 0,
    val version: Long = 0,
) {
    val empty: Boolean get() = tiles == 0
}

/** Engine LayerError codes, with the message the user sees. */
enum class LayerError(val code: Int, val message: S?) {
    NONE(0, null), NO_CANVAS(1, S.LAYER_ERR_NO_CANVAS), NOT_FOUND(2, S.LAYER_ERR_NOT_FOUND),
    LAST_LAYER(3, S.LAYER_ERR_LAST), NO_LAYER_BELOW(4, S.LAYER_ERR_NO_BELOW), LOCKED(5, S.LAYER_ERR_LOCKED),
    HIDDEN(6, S.LAYER_ERR_HIDDEN), TOO_MANY(7, S.LAYER_ERR_TOO_MANY), MEMORY(8, S.LAYER_ERR_MEMORY),
    BAD_VALUE(9, S.LAYER_ERR_BAD_VALUE);

    companion object {
        fun of(code: Int): LayerError = entries.firstOrNull { it.code == code } ?: BAD_VALUE
    }
}

/**
 * The layer stack (bottom first) and the active layer. Pure Kotlin: parsing
 * of the engine's layer table, the checks the UI does before asking the
 * engine (the engine checks again), and the project manifest format.
 */
data class LayerStack(val layers: List<LayerInfo>, val activeId: Int) {
    val active: LayerInfo? get() = layers.firstOrNull { it.id == activeId }
    fun indexOf(id: Int): Int = layers.indexOfFirst { it.id == id }
    fun get(id: Int): LayerInfo? = layers.firstOrNull { it.id == id }
    /** Top first, as the panel lists them. */
    val topDown: List<LayerInfo> get() = layers.asReversed()

    /** Why painting is impossible on the active layer (null: it is possible). */
    fun paintBlocker(): LayerError? {
        val a = active ?: return LayerError.NOT_FOUND
        return when {
            !a.visible -> LayerError.HIDDEN
            a.locked -> LayerError.LOCKED
            else -> null
        }
    }

    fun mergeDownBlocker(id: Int): LayerError? {
        val i = indexOf(id)
        if (i < 0) return LayerError.NOT_FOUND
        if (i == 0) return LayerError.NO_LAYER_BELOW
        val up = layers[i]
        val low = layers[i - 1]
        return when {
            up.locked || low.locked -> LayerError.LOCKED
            !up.visible || !low.visible -> LayerError.HIDDEN
            else -> null
        }
    }

    fun deleteBlocker(id: Int): LayerError? = when {
        indexOf(id) < 0 -> LayerError.NOT_FOUND
        layers.size <= 1 -> LayerError.LAST_LAYER
        else -> null
    }

    fun flattenBlocker(): LayerError? = if (layers.any { it.visible && it.locked }) LayerError.LOCKED else null
    fun canMoveUp(id: Int) = indexOf(id).let { it >= 0 && it < layers.size - 1 }
    fun canMoveDown(id: Int) = indexOf(id) > 0
    val hiddenCount: Int get() = layers.count { !it.visible }

    /** Name for a new layer: "Layer n" with the first free n. */
    fun newLayerName(prefix: String): String {
        val names = layers.map { it.name }.toSet()
        var n = layers.size + 1
        while ("$prefix $n" in names) n++
        return "$prefix $n"
    }

    companion object {
        const val MAX_LAYERS = 100
        const val MAX_NAME = 60

        /**
         * Engine table (NativeEngine.layers): "active\t<id>" then per layer
         * "id\tvisible\tlocked\topacity\tblend\ttiles\tversion\tname" (name escaped).
         * Malformed lines are skipped; null if nothing usable.
         */
        fun parse(table: String?): LayerStack? {
            if (table.isNullOrEmpty()) return null
            var active = 0
            val out = ArrayList<LayerInfo>()
            for (line in table.split('\n')) {
                if (line.isEmpty()) continue
                val f = line.split('\t')
                if (f[0] == "active") {
                    active = f.getOrNull(1)?.toIntOrNull() ?: 0
                    continue
                }
                if (f.size < 8) continue
                val id = f[0].toIntOrNull() ?: continue
                out += LayerInfo(
                    id = id, name = unescape(f.subList(7, f.size).joinToString("\t")), visible = f[1] == "1", locked = f[2] == "1",
                    opacity = f[3].toFloatOrNull()?.takeIf { it.isFinite() }?.coerceIn(0f, 1f) ?: 1f,
                    blend = BlendMode.ofCode(f[4].toIntOrNull() ?: 0) ?: BlendMode.NORMAL,
                    tiles = f[5].toIntOrNull() ?: 0, version = f[6].toLongOrNull() ?: 0,
                )
            }
            if (out.isEmpty()) return null
            return LayerStack(out, if (out.any { it.id == active }) active else out.last().id)
        }

        fun unescape(s: String): String {
            val sb = StringBuilder()
            var i = 0
            while (i < s.length) {
                val c = s[i]
                if (c == '\\' && i + 1 < s.length) {
                    when (s[i + 1]) {
                        't' -> sb.append('\t'); 'n' -> sb.append('\n'); 'r' -> sb.append('\r'); else -> sb.append(s[i + 1])
                    }
                    i += 2
                } else {
                    sb.append(c)
                    i++
                }
            }
            return sb.toString()
        }

        /** Trimmed, single-spaced, at most [MAX_NAME] characters; null if empty. */
        fun cleanName(raw: String): String? {
            val n = raw.replace(Regex("\\p{Cntrl}"), " ").trim().replace(Regex("\\s+"), " ")
            return if (n.isEmpty()) null else n.take(MAX_NAME).trim()
        }
    }
}

/**
 * Project layer manifest (layers.pxlayers), format 1:
 *
 *     pxlayers 1
 *     width = 2048
 *     height = 1536
 *     active = 3
 *     [layer 1]
 *     name = Sketch
 *     visible = true
 *     locked = false
 *     opacity = 0.8
 *     blend = multiply
 *     [layer 3]
 *     ...
 *
 * Sections bottom first; each layer's pixels are in layer-<id>.pxtiles.
 * Unknown keys are ignored; anything structurally wrong (bad header, no
 * layer, duplicate or invalid ids, unknown blend mode, wrong size) makes the
 * whole manifest invalid: the project then refuses to open instead of
 * showing (and later saving) a partial picture.
 */
object LayerManifest {
    const val HEADER = "pxlayers 1"
    const val FILE = "layers.pxlayers"
    fun layerFile(id: Int) = "layer-$id.pxtiles"

    class Invalid(message: String) : Exception(message)

    fun encode(stack: LayerStack, width: Int, height: Int): String {
        val sb = StringBuilder(HEADER).append('\n')
        sb.append("width = ").append(width).append('\n')
        sb.append("height = ").append(height).append('\n')
        sb.append("active = ").append(stack.activeId).append('\n')
        for (l in stack.layers) {
            sb.append("[layer ").append(l.id).append("]\n")
            sb.append("name = ").append(BrushPresetFormat.escape(l.name)).append('\n')
            sb.append("visible = ").append(l.visible).append('\n')
            sb.append("locked = ").append(l.locked).append('\n')
            sb.append("opacity = ").append(String.format(Locale.US, "%.4f", l.opacity)).append('\n')
            sb.append("blend = ").append(l.blend.key).append('\n')
        }
        return sb.toString()
    }

    /** @throws Invalid */
    fun decode(text: String, width: Int, height: Int): LayerStack {
        val (header, secs) = BrushPresetFormat.sections(text)
        if (header != HEADER) throw Invalid("not a layer manifest (version?)")
        // Top-level keys come before the first section: re-read them.
        val top = text.lineSequence().map { it.trim() }.takeWhile { !it.startsWith("[") }
            .mapNotNull { l -> l.indexOf('=').takeIf { it > 0 }?.let { l.substring(0, it).trim() to l.substring(it + 1).trim() } }.toMap()
        if (top["width"]?.toIntOrNull() != width || top["height"]?.toIntOrNull() != height) throw Invalid("canvas size mismatch")
        val layers = ArrayList<LayerInfo>()
        for (s in secs) {
            if (s.kind != "layer") continue
            val id = s.id.toIntOrNull()?.takeIf { it > 0 } ?: throw Invalid("bad layer id '${s.id}'")
            if (layers.any { it.id == id }) throw Invalid("duplicate layer id $id")
            val blend = BlendMode.ofKey(s.values["blend"] ?: "normal") ?: throw Invalid("unknown blend mode ${s.values["blend"]}")
            layers += LayerInfo(
                id = id, name = LayerStack.cleanName(s.values["name"] ?: "") ?: "Layer $id",
                visible = s.values["visible"] != "false", locked = s.values["locked"] == "true",
                opacity = s.values["opacity"]?.toFloatOrNull()?.takeIf { it.isFinite() }?.coerceIn(0f, 1f) ?: 1f,
                blend = blend,
            )
        }
        if (layers.isEmpty()) throw Invalid("no layer")
        if (layers.size > LayerStack.MAX_LAYERS) throw Invalid("too many layers")
        val active = top["active"]?.toIntOrNull()?.takeIf { a -> layers.any { it.id == a } } ?: layers.last().id
        return LayerStack(layers, active)
    }
}
