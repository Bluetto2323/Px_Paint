package com.paintengine.pxpaint.core

import java.text.Normalizer
import java.util.Locale

/** The tools that use a brush preset. */
enum class BrushTool(val key: String) { PAINT("paint"), ERASER("eraser") }

/** Library filters (chips). */
enum class BrushFilter(val label: S) {
    ALL(S.FILTER_ALL), FAVORITES(S.FILTER_FAVORITES), RECENT(S.FILTER_RECENT), SKETCHING(S.CAT_SKETCHING),
    INKING(S.CAT_INKING), PAINTING(S.CAT_PAINTING), AIRBRUSH(S.CAT_AIRBRUSH), DRY(S.CAT_DRY), MINE(S.FILTER_MINE),
}

/**
 * The brush library: built-in presets (read-only originals from the asset),
 * the user's customisations of them ([overrides], resettable), the user's
 * own presets, favourites, recently used brushes, the preset selected for
 * each tool and the per-tool quick size / opacity of each preset ([live]).
 *
 * Immutable: every operation returns a new library (pure Kotlin, unit-tested).
 * Persisted by PxPaintActivity as one text file ([encode] / [decode]).
 */
data class BrushLibrary(
    val builtIns: List<BrushPreset>,
    val overrides: Map<String, BrushParams> = emptyMap(),
    val user: List<BrushPreset> = emptyList(),
    val favorites: List<String> = emptyList(),
    val recents: List<String> = emptyList(),
    val selected: Map<BrushTool, String> = emptyMap(),
    val live: Map<String, Live> = emptyMap(),
) {
    /** Quick-control values of a preset for one tool. */
    data class Live(val size: Float, val opacity: Float)

    private val builtInIds: Set<String> by lazy { builtIns.mapTo(HashSet()) { it.id } }

    /** Every preset as the user sees it: built-ins (customised) then user presets. */
    fun all(): List<BrushPreset> = builtIns.map { b -> overrides[b.id]?.let { b.copy(params = it) } ?: b } + user

    fun get(id: String): BrushPreset? =
        builtIns.firstOrNull { it.id == id }?.let { b -> overrides[id]?.let { b.copy(params = it) } ?: b }
            ?: user.firstOrNull { it.id == id }

    fun original(id: String): BrushPreset? = builtIns.firstOrNull { it.id == id }
    fun isBuiltIn(id: String) = id in builtInIds
    fun isModified(id: String) = id in overrides
    fun isFavorite(id: String) = id in favorites

    fun selectedId(tool: BrushTool): String = selected[tool]?.takeIf { get(it) != null } ?: defaultId(tool)
    fun selectedPreset(tool: BrushTool): BrushPreset = get(selectedId(tool)) ?: fallbackPreset()

    private fun defaultId(tool: BrushTool): String {
        val want = if (tool == BrushTool.ERASER) DEFAULT_ERASER else DEFAULT_PAINT
        return if (get(want) != null) want else all().firstOrNull()?.id ?: want
    }

    private fun fallbackPreset() = BrushPreset(DEFAULT_PAINT, "Round", BrushCategory.PAINTING, BrushParams())

    /** What the tool paints with: the preset's parameters + its quick size / opacity for this tool. */
    fun effective(tool: BrushTool): BrushParams {
        val p = selectedPreset(tool)
        val l = live[liveKey(tool, p.id)] ?: return p.params
        return p.params.copy(size = l.size, opacity = l.opacity).sanitized()
    }

    fun select(tool: BrushTool, id: String): BrushLibrary {
        if (get(id) == null) return this
        return copy(selected = selected + (tool to id), recents = (listOf(id) + recents.filter { it != id }).take(MAX_RECENTS))
    }

    /** Quick controls (size / opacity) of the selected preset of [tool]. */
    fun setLive(tool: BrushTool, size: Float? = null, opacity: Float? = null): BrushLibrary {
        val id = selectedId(tool)
        val cur = effective(tool)
        val s = BrushParams(size = size ?: cur.size, opacity = opacity ?: cur.opacity).sanitized()
        return copy(live = live + (liveKey(tool, id) to Live(s.size, s.opacity)))
    }

    fun toggleFavorite(id: String): BrushLibrary = when {
        id in favorites -> copy(favorites = favorites - id)
        get(id) != null -> copy(favorites = favorites + id)
        else -> this
    }

    /**
     * Copy of [id] as a new user preset [newId] (selected for no tool).
     * The copy takes the source's current parameters, quick size / opacity of
     * [tool] included, and a unique name ("Pencil copy", "Pencil copy 2", ...).
     */
    fun duplicate(id: String, newId: String, tool: BrushTool? = null, name: String? = null): BrushLibrary {
        val src = get(id) ?: return this
        if (get(newId) != null || !newId.startsWith(BrushPreset.USER_PREFIX) || !BrushPresetFormat.validId(newId)) return this
        val params = live[tool?.let { liveKey(it, id) } ?: ""]?.let { src.params.copy(size = it.size, opacity = it.opacity) }
            ?: src.params
        val base = BrushPreset.cleanName(name ?: "") ?: uniqueName("${src.displayName()} ${tr(S.COPY_SUFFIX)}")
        val copy = BrushPreset(
            id = newId, name = uniqueName(base), category = src.category,
            params = params.sanitized(), tags = src.tags, baseId = src.baseId ?: src.id.takeIf { src.builtIn },
        )
        return copy(user = user + copy)
    }

    /** User presets only; an empty name is refused. */
    fun rename(id: String, name: String): BrushLibrary {
        val n = BrushPreset.cleanName(name) ?: return this
        if (user.none { it.id == id }) return this
        return copy(user = user.map { if (it.id == id) it.copy(name = n, names = emptyMap()) else it })
    }

    fun canRename(id: String) = user.any { it.id == id }
    fun canDelete(id: String) = user.any { it.id == id }

    /** User presets only. Tools that used it fall back to their default brush. */
    fun delete(id: String): BrushLibrary {
        if (!canDelete(id)) return this
        return copy(
            user = user.filter { it.id != id },
            favorites = favorites - id,
            recents = recents - id,
            selected = selected.filterValues { it != id },
            live = live.filterKeys { !it.endsWith("/$id") },
        )
    }

    /**
     * New parameters for [id]: a built-in gets (or loses, if back to the
     * original) an override. A new size / opacity in the preset itself (the
     * editor) replaces the quick values; other changes keep them.
     */
    fun update(id: String, params: BrushParams): BrushLibrary {
        val p = params.sanitized()
        val old = get(id)?.params ?: return this
        val next = original(id)?.let { orig ->
            copy(overrides = if (p == orig.params) overrides - id else overrides + (id to p))
        } ?: if (user.any { it.id == id }) copy(user = user.map { if (it.id == id) it.copy(params = p) else it }) else return this
        return if (old.size != p.size || old.opacity != p.opacity) next.dropLive(id) else next
    }

    fun setCategory(id: String, category: BrushCategory): BrushLibrary =
        if (user.none { it.id == id }) this else copy(user = user.map { if (it.id == id) it.copy(category = category) else it })

    /** The editor sets size / opacity in the preset itself: quick values follow it. */
    private fun dropLive(id: String) = copy(live = live.filterKeys { !it.endsWith("/$id") })

    /** Built-in back to its shipped configuration (parameters and quick values). */
    fun reset(id: String): BrushLibrary = if (!isBuiltIn(id)) this else copy(overrides = overrides - id).dropLive(id)

    /** Presets under [filter] whose name, category or tags match [query] (accent / case insensitive). */
    fun list(filter: BrushFilter, query: String = ""): List<BrushPreset> {
        val base = when (filter) {
            BrushFilter.ALL -> all()
            BrushFilter.FAVORITES -> favorites.mapNotNull { get(it) }
            BrushFilter.RECENT -> recents.mapNotNull { get(it) }
            BrushFilter.MINE -> user
            BrushFilter.SKETCHING -> all().filter { it.category == BrushCategory.SKETCHING }
            BrushFilter.INKING -> all().filter { it.category == BrushCategory.INKING }
            BrushFilter.PAINTING -> all().filter { it.category == BrushCategory.PAINTING }
            BrushFilter.AIRBRUSH -> all().filter { it.category == BrushCategory.AIRBRUSH }
            BrushFilter.DRY -> all().filter { it.category == BrushCategory.DRY }
        }
        val terms = fold(query).split(' ').filter { it.isNotEmpty() }
        if (terms.isEmpty()) return base
        return base.filter { p ->
            val hay = fold(
                listOf(p.name, p.displayName(true), p.displayName(false), tr(p.category.label), p.category.key, p.category.label.en,
                    p.category.label.it).joinToString(" ") + " " + p.tags.joinToString(" "),
            )
            terms.all { hay.contains(it) }
        }
    }

    fun uniqueName(base: String): String {
        val names = all().flatMap { listOf(it.name, it.displayName(true), it.displayName(false)) }.map { it.lowercase(Locale.ROOT) }.toSet()
        val b = BrushPreset.cleanName(base) ?: "Brush"
        if (b.lowercase(Locale.ROOT) !in names) return b
        var i = 2
        while (true) {
            val suffix = " $i"
            val n = b.take(BrushPreset.MAX_NAME - suffix.length) + suffix
            if (n.lowercase(Locale.ROOT) !in names) return n
            i++
        }
    }

    // ------------------------------------------------------------ persistence
    fun encode(): String {
        val sb = StringBuilder()
        sb.append(LIB_HEADER).append('\n')
        sb.append("[state]\n")
        for (t in BrushTool.entries) selected[t]?.let { sb.append("selected.").append(t.key).append(" = ").append(it).append('\n') }
        sb.append("favorites = ").append(favorites.joinToString(",")).append('\n')
        sb.append("recents = ").append(recents.joinToString(",")).append('\n')
        for ((k, l) in live.toSortedMap()) {
            sb.append("\n[live ").append(k).append("]\n")
            sb.append("size = ").append(l.size).append('\n').append("opacity = ").append(l.opacity).append('\n')
        }
        for ((id, p) in overrides.toSortedMap()) {
            sb.append("\n[override ").append(id).append("]\n")
            BrushPresetFormat.writeParams(sb, p)
        }
        for (p in user) {
            sb.append('\n')
            BrushPresetFormat.writePreset(sb, p)
        }
        return sb.toString()
    }

    companion object {
        const val LIB_HEADER = "pxbrushlib 1"
        const val MAX_RECENTS = 12
        const val DEFAULT_PAINT = "builtin.round"
        const val DEFAULT_ERASER = "builtin.hard"
        /** The Block 3/4 brush (classic rasteriser): where existing users' setup goes. */
        const val LEGACY_PRESET = "builtin.hard"

        private fun liveKey(tool: BrushTool, id: String) = "${tool.key}/$id"

        /**
         * Library from its file. Damaged or unknown parts are skipped; references
         * to presets that no longer exist are dropped. Null if [text] is not a
         * library file at all.
         */
        fun decode(text: String, builtIns: List<BrushPreset>): BrushLibrary? {
            val (header, secs) = BrushPresetFormat.sections(text)
            if (header != LIB_HEADER) return null
            var lib = BrushLibrary(builtIns)
            val ids = builtIns.mapTo(HashSet()) { it.id }
            val user = ArrayList<BrushPreset>()
            val overrides = LinkedHashMap<String, BrushParams>()
            val live = LinkedHashMap<String, Live>()
            var state: Map<String, String> = emptyMap()
            for (s in secs) when (s.kind) {
                "state" -> state = s.values
                "override" -> builtIns.firstOrNull { it.id == s.id }?.let { b ->
                    val p = BrushParams.fromEntries(s.values, b.params)
                    if (p != b.params) overrides[s.id] = p
                }
                "preset" -> BrushPresetFormat.presetFrom(s)?.takeIf {
                    it.id.startsWith(BrushPreset.USER_PREFIX) && it.id !in ids
                }?.let {
                    ids += it.id
                    user += it
                }
                "live" -> {
                    val size = s.values["size"]?.toFloatOrNull()
                    val opacity = s.values["opacity"]?.toFloatOrNull()
                    if (size != null && opacity != null && size.isFinite() && opacity.isFinite()) {
                        val c = BrushParams(size = size, opacity = opacity).sanitized()
                        live[s.id] = Live(c.size, c.opacity)
                    }
                }
            }
            lib = lib.copy(user = user, overrides = overrides)
            val exists = { id: String -> lib.get(id) != null }
            val selected = HashMap<BrushTool, String>()
            for (t in BrushTool.entries) state["selected.${t.key}"]?.takeIf(exists)?.let { selected[t] = it }
            fun list(k: String) = state[k]?.split(',')?.map { it.trim() }?.filter { it.isNotEmpty() && exists(it) }?.distinct() ?: emptyList()
            return lib.copy(
                selected = selected,
                favorites = list("favorites"),
                recents = list("recents").take(MAX_RECENTS),
                live = live.filterKeys { k -> k.substringAfter('/').let(exists) && BrushTool.entries.any { k.startsWith(it.key + "/") } },
            )
        }

        /**
         * First start with Block 5. A user of the Block 3/4 brushes ([legacy]
         * non-null) keeps exactly the brush they had: both tools on the classic
         * Hard Brush with their sizes / opacities, and their spacing and stylus
         * settings as a (resettable) customisation of it. New users start on
         * Basic Round.
         */
        fun migrate(builtIns: List<BrushPreset>, legacy: BrushState?): BrushLibrary {
            val lib = BrushLibrary(builtIns)
            if (legacy == null || lib.get(LEGACY_PRESET) == null) return lib
            val hard = lib.get(LEGACY_PRESET)!!.params
            val m = legacy.marker
            val custom = hard.copy(
                spacing = m.spacing, pressureSize = m.pressureSize, minSize = m.minSize,
                pressureOpacity = m.pressureOpacity, minOpacity = m.minOpacity, tilt = m.tilt,
            )
            return lib.update(LEGACY_PRESET, custom)
                .select(BrushTool.PAINT, LEGACY_PRESET)
                .select(BrushTool.ERASER, LEGACY_PRESET)
                .copy(recents = listOf(LEGACY_PRESET))
                .let {
                    it.copy(live = it.live +
                        (liveKey(BrushTool.PAINT, LEGACY_PRESET) to Live(m.size, m.opacity)) +
                        (liveKey(BrushTool.ERASER, LEGACY_PRESET) to Live(legacy.eraser.size, legacy.eraser.opacity)))
                }
        }

        private fun fold(s: String): String =
            Normalizer.normalize(s.lowercase(Locale.ROOT), Normalizer.Form.NFD).replace(Regex("\\p{M}+"), "")
    }
}
