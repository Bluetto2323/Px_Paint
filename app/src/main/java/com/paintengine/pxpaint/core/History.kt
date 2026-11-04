package com.paintengine.pxpaint.core

/** Engine HistoryKind codes, with the name shown in the history list. */
enum class HistoryKind(val code: Int, val label: S) {
    STROKE(0, S.HIST_STROKE),
    ERASE(1, S.HIST_ERASE),
    CLEAR(2, S.HIST_CLEAR),
    FILL(3, S.HIST_FILL),
    ADD_LAYER(4, S.HIST_ADD_LAYER),
    DELETE_LAYER(5, S.HIST_DELETE_LAYER),
    DUPLICATE_LAYER(6, S.HIST_DUPLICATE_LAYER),
    MOVE_LAYER(7, S.HIST_MOVE_LAYER),
    RENAME_LAYER(8, S.HIST_RENAME_LAYER),
    VISIBILITY(9, S.HIST_VISIBILITY),
    LOCK(10, S.HIST_LOCK),
    OPACITY(11, S.HIST_OPACITY),
    BLEND(12, S.HIST_BLEND),
    MERGE_DOWN(13, S.HIST_MERGE_DOWN),
    FLATTEN(14, S.HIST_FLATTEN),
    SELECTION(15, S.HIST_SELECTION),
    CLEAR_SELECTED(16, S.HIST_CLEAR_SELECTED),
    CUT(17, S.HIST_CUT),
    PASTE(18, S.HIST_PASTE),
    DUPLICATE_SELECTION(19, S.HIST_DUPLICATE_SELECTION),
    MOVE_SELECTION(20, S.HIST_MOVE_SELECTION),
    GRADIENT(21, S.HIST_GRADIENT),
    SMUDGE(22, S.HIST_SMUDGE),
    OTHER(-1, S.HIST_OTHER);

    companion object {
        fun of(code: Int): HistoryKind = entries.firstOrNull { it.code == code } ?: OTHER
    }
}

/** One history step; [label] = the layer it applied to; [undone] = in the redo part. */
data class HistoryStep(val kind: HistoryKind, val label: String, val undone: Boolean)

/**
 * The engine's undo / redo history as the UI shows it (pure Kotlin: parsing
 * of NativeEngine.historyTable and the position arithmetic of the list).
 * [steps]: oldest first; the first [undo] are done, the rest are undone
 * (next redo first).
 */
data class HistoryState(
    val undo: Int,
    val redo: Int,
    val bytes: Long = 0,
    val budget: Long = 0,
    val maxSteps: Int = 0,
    val dropped: Long = 0,
    val refused: Long = 0,
    val steps: List<HistoryStep> = emptyList(),
) {
    val canUndo: Boolean get() = undo > 0
    val canRedo: Boolean get() = redo > 0

    /**
     * Steps to move so that the document is in the state right after step
     * [index] (-1: before every recorded step). Negative = undo that many,
     * positive = redo that many, 0 = already there.
     */
    fun moveTo(index: Int): Int {
        val target = (index + 1).coerceIn(0, undo + redo)
        return target - undo
    }

    companion object {
        val EMPTY = HistoryState(0, 0)

        /**
         * Engine table: "history\tundo\tredo\tbytes\tbudget\tmaxSteps\tdropped\trefused"
         * then per step "kind\tundone\tlabel" (label escaped). Null if the header is missing.
         */
        fun parse(table: String?): HistoryState? {
            if (table.isNullOrEmpty()) return null
            val lines = table.split('\n')
            val h = lines[0].split('\t')
            if (h.size < 8 || h[0] != "history") return null
            val undo = h[1].toIntOrNull() ?: return null
            val redo = h[2].toIntOrNull() ?: return null
            if (undo < 0 || redo < 0) return null
            val steps = ArrayList<HistoryStep>()
            for (line in lines.drop(1)) {
                if (line.isEmpty()) continue
                val f = line.split('\t')
                if (f.size < 3) continue
                val kind = HistoryKind.of(f[0].toIntOrNull() ?: -1)
                steps += HistoryStep(kind, LayerStack.unescape(f.subList(2, f.size).joinToString("\t")), f[1] == "1")
            }
            // The list must agree with the counts (else show the counts only).
            val consistent = steps.size == undo + redo && steps.withIndex().all { (i, s) -> s.undone == (i >= undo) }
            return HistoryState(
                undo, redo, h[3].toLongOrNull() ?: 0, h[4].toLongOrNull() ?: 0, h[5].toIntOrNull() ?: 0,
                h[6].toLongOrNull() ?: 0, h[7].toLongOrNull() ?: 0, if (consistent) steps else emptyList(),
            )
        }

        /** Engine counts (NativeEngine.historyCounts) as a state without the list. */
        fun counts(undo: Int, redo: Int) = HistoryState(undo.coerceAtLeast(0), redo.coerceAtLeast(0))

        private const val MiB = 1024L * 1024L

        /**
         * History memory budget for a device: 1/32 of its RAM, clamped to
         * 32 … 192 MiB (2 GB → 64 MiB, 4 GB → 128 MiB, 6 GB and up → 192 MiB).
         * On top of the canvas budget (CanvasSizeRules.budgetForRam).
         */
        fun budgetForRam(totalRamBytes: Long): Long =
            if (totalRamBytes <= 0) 64 * MiB else (totalRamBytes / 32).coerceIn(32 * MiB, 192 * MiB)

        /** Maximum number of steps kept (also bounded by the memory budget). */
        const val MAX_STEPS = 100
    }
}
