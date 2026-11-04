package com.paintengine.pxpaint.core

/**
 * Reordering layers by dragging their rows (Block 11), pure arithmetic
 * (unit-tested); ui code in screens/LayersPanel. The list shows the top
 * layer first; the engine counts from the bottom (index 0).
 *
 *  * A press becomes a drag after [LONG_PRESS_MS] without moving more than the
 *    touch slop; moving earlier is a scroll (or a tap on release).
 *  * While dragging, the row follows the finger; the other rows open a gap
 *    where it would land ([target], [shift]); near the list's edges the list
 *    scrolls by itself ([autoScroll]).
 *  * Releasing moves the layer there (one undoable "move layer" step).
 */
object LayerDrag {
    /** Shorter than the system long press (~400-500 ms): the row lifts quickly, but a tap never drags. */
    const val LONG_PRESS_MS = 280L

    /**
     * List position (0 = top row) where the dragged row lands: the number of
     * other rows whose middle is above the dragged row's middle [centerY].
     * [tops] / [heights]: the rows' resting layout (list order).
     */
    fun target(tops: IntArray, heights: IntArray, from: Int, centerY: Float): Int {
        var pos = 0
        for (i in tops.indices) {
            if (i == from) continue
            if (tops[i] + heights[i] / 2f < centerY) pos++
        }
        return pos
    }

    /**
     * Offset of row [i] while row [from] is dragged towards position [to]: the
     * rows between open the gap by the dragged row's height plus the spacing.
     */
    fun shift(i: Int, from: Int, to: Int, step: Int): Int = when {
        i == from -> 0
        from < to && i in (from + 1)..to -> -step
        from > to && i in to until from -> step
        else -> 0
    }

    /** List position (top first) -> engine stack index (bottom = 0). */
    fun engineIndex(position: Int, count: Int): Int = count - 1 - position

    /**
     * Auto-scroll speed (px per frame, negative = up) for a finger at [y] in a
     * viewport [top]..[bottom]: zero outside the [zone] at each edge, growing
     * linearly to [max] at the edge itself.
     */
    fun autoScroll(y: Float, top: Float, bottom: Float, zone: Float, max: Float): Float {
        if (zone <= 0f || bottom - top <= 2 * zone) return 0f
        return when {
            y < top + zone -> -max * ((top + zone - y) / zone).coerceIn(0f, 1f)
            y > bottom - zone -> max * ((y - (bottom - zone)) / zone).coerceIn(0f, 1f)
            else -> 0f
        }
    }

    /**
     * Top (list coordinates, resting layout) of the slot where the dragged
     * row lands at position [to]: the insertion indicator is drawn there.
     */
    fun landingTop(tops: IntArray, heights: IntArray, from: Int, to: Int): Int = when {
        tops.isEmpty() -> 0
        to <= from -> tops[to.coerceIn(0, tops.size - 1)]
        else -> { val t = to.coerceAtMost(tops.size - 1); tops[t] + heights[t] - heights[from] }
    }
}
