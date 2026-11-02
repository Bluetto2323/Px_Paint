package com.paintengine.pxpaint.core

/**
 * Canvas size rules shared by the New Canvas sheet and the tests.
 *
 * [estimate] mirrors the engine's StoragePolicy (canvas/TileGrid.cpp) so the
 * sheet can explain a size before a graphics device exists. Once a device
 * exists, the engine's own answer (NativeEngine.checkCanvasSize) is
 * authoritative: it also knows the real GPU limits of this phone.
 */
object CanvasSizeRules {
    const val MIN_SIDE = 1
    const val MAX_SIDE = 16384
    const val TILE_SIZE = 256
    const val TILE_TEXTURE = TILE_SIZE + 2       // 1 px gutter on each side
    const val SINGLE_TEXTURE_MAX_SIDE = 2048
    private const val MIB = 1024L * 1024L

    enum class PresetKind { SCREEN, SQUARE, A4, A5, FULL_HD, UHD_4K, PORTRAIT_POST }

    data class Preset(val kind: PresetKind, val width: Int, val height: Int)

    /** Presets; SCREEN uses the device's real screen size in pixels (portrait). */
    fun presets(screenWidthPx: Int, screenHeightPx: Int): List<Preset> {
        val w = maxOf(1, minOf(screenWidthPx, screenHeightPx))
        val h = maxOf(1, maxOf(screenWidthPx, screenHeightPx))
        return listOf(
            Preset(PresetKind.SCREEN, minOf(w, MAX_SIDE), minOf(h, MAX_SIDE)),
            Preset(PresetKind.SQUARE, 2048, 2048),
            Preset(PresetKind.A4, 2480, 3508),        // 210 x 297 mm at 300 dpi
            Preset(PresetKind.A5, 1748, 2480),        // 148 x 210 mm at 300 dpi
            Preset(PresetKind.FULL_HD, 1920, 1080),
            Preset(PresetKind.UHD_4K, 3840, 2160),
            Preset(PresetKind.PORTRAIT_POST, 1080, 1350),
        )
    }

    enum class SizeError { TOO_SMALL, TOO_LARGE, OVER_BUDGET }

    data class Estimate(
        val ok: Boolean,
        val tiled: Boolean,
        val tileColumns: Int,
        val tileRows: Int,
        /** Worst case GPU bytes (every tile allocated) incl. gutters. */
        val maxBytes: Long,
        /** Pixels of the canvas itself (w*h*4). */
        val canvasBytes: Long,
        val error: SizeError?,
    )

    fun validateSide(side: Int): SizeError? = when {
        side < MIN_SIDE -> SizeError.TOO_SMALL
        side > MAX_SIDE -> SizeError.TOO_LARGE
        else -> null
    }

    fun estimate(width: Int, height: Int, maxTextureSize: Int, memoryBudgetBytes: Long): Estimate {
        val canvasBytes = width.toLong() * height.toLong() * 4L
        val sideError = validateSide(width) ?: validateSide(height)
        if (sideError != null) return Estimate(false, false, 0, 0, 0, maxOf(0, canvasBytes), sideError)
        val singleMax = minOf(maxTextureSize, SINGLE_TEXTURE_MAX_SIDE)
        val tiled = width > singleMax || height > singleMax
        val cols = if (tiled) (width + TILE_SIZE - 1) / TILE_SIZE else 1
        val rows = if (tiled) (height + TILE_SIZE - 1) / TILE_SIZE else 1
        val maxBytes = if (tiled) cols.toLong() * rows * TILE_TEXTURE * TILE_TEXTURE * 4L else canvasBytes
        val overBudget = memoryBudgetBytes in 1 until maxBytes
        return Estimate(!overBudget, tiled, cols, rows, maxBytes, canvasBytes, if (overBudget) SizeError.OVER_BUDGET else null)
    }

    /**
     * Canvas pixel budget from the device's total RAM: a quarter, clamped to
     * [256 MiB, 1.5 GiB]. Phones share RAM between CPU and GPU.
     */
    fun budgetForRam(totalRamBytes: Long): Long =
        (totalRamBytes / 4).coerceIn(256 * MIB, 1536 * MIB)

    /** Largest square side that fits the budget (tiled worst case), for hints. */
    fun largestSquareSide(maxTextureSize: Int, memoryBudgetBytes: Long): Int {
        var lo = MIN_SIDE
        var hi = MAX_SIDE
        while (lo < hi) {
            val mid = (lo + hi + 1) / 2
            if (estimate(mid, mid, maxTextureSize, memoryBudgetBytes).ok) lo = mid else hi = mid - 1
        }
        return lo
    }
}
