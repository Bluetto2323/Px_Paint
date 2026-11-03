package com.paintengine.pxpaint.ui

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.PorterDuff
import android.graphics.PorterDuffXfermode
import android.util.LruCache
import com.paintengine.pxpaint.core.ContentLog
import com.paintengine.pxpaint.core.ProjectMeta
import com.paintengine.pxpaint.core.TileContent
import android.graphics.Rect
import java.nio.ByteBuffer
import java.io.ByteArrayOutputStream
import java.io.File
import kotlin.math.max
import kotlin.math.roundToInt

/**
 * Gallery thumbnails, drawn on the CPU from the persisted content ops with
 * the same semantics as the engine (rectangles *replace* pixels, the paper
 * shows through transparent ones). Runs on the io thread.
 */
object Thumbnails {
    const val MAX_SIDE = 480

    private val cache = object : LruCache<String, Bitmap>(24 * 1024 * 1024) {
        override fun sizeOf(key: String, value: Bitmap) = value.byteCount
    }

    fun cached(id: String, modifiedMs: Long): Bitmap? = cache.get("$id@$modifiedMs")
    fun invalidate(id: String) {
        for (key in cache.snapshot().keys) if (key.startsWith("$id@")) cache.remove(key)
    }

    fun load(file: File, meta: ProjectMeta): Bitmap? {
        cached(meta.id, meta.modifiedMs)?.let { return it }
        if (!file.isFile) return null
        val bmp = BitmapFactory.decodeFile(file.path) ?: return null
        cache.put("${meta.id}@${meta.modifiedMs}", bmp)
        return bmp
    }

    /**
     * From the engine's preview (NativeEngine.canvasThumbnail: straight ARGB
     * [w, h, pixels...]) over the paper / checkerboard.
     */
    fun fromEngine(meta: ProjectMeta, preview: IntArray?): Bitmap {
        val (w, h, bmp, canvas) = base(meta)
        if (preview != null && preview.size >= 2) {
            val pw = preview[0]
            val ph = preview[1]
            if (pw > 0 && ph > 0 && preview.size >= 2 + pw * ph) {
                val content = Bitmap.createBitmap(preview, 2, pw, pw, ph, Bitmap.Config.ARGB_8888)
                canvas.drawBitmap(content, null, Rect(0, 0, w, h), Paint(Paint.FILTER_BITMAP_FLAG))
                content.recycle()
            }
        }
        cache.put("${meta.id}@${meta.modifiedMs}", bmp)
        return bmp
    }

    /** From stored tiles (gallery, when a project has no thumbnail file). */
    fun fromTiles(meta: ProjectMeta, tiles: TileContent): Bitmap {
        val (w, h, bmp, canvas) = base(meta)
        val sx = w.toFloat() / meta.width
        val sy = h.toFloat() / meta.height
        val tile = Bitmap.createBitmap(TileContent.TILE, TileContent.TILE, Bitmap.Config.ARGB_8888)
        val paint = Paint(Paint.FILTER_BITMAP_FLAG)
        val dst = android.graphics.RectF()
        for (ty in 0 until tiles.rows) for (tx in 0 until tiles.columns) {
            // ARGB_8888 bitmaps hold premultiplied RGBA bytes: the store's layout.
            val px = tiles.tilePixels(tx, ty) ?: continue
            tile.copyPixelsFromBuffer(ByteBuffer.wrap(px))
            val x0 = tx * TileContent.TILE
            val y0 = ty * TileContent.TILE
            dst.set(x0 * sx, y0 * sy, (x0 + TileContent.TILE) * sx, (y0 + TileContent.TILE) * sy)
            canvas.drawBitmap(tile, null, dst, paint)
        }
        tile.recycle()
        cache.put("${meta.id}@${meta.modifiedMs}", bmp)
        return bmp
    }

    private data class Base(val w: Int, val h: Int, val bmp: Bitmap, val canvas: Canvas)

    private fun base(meta: ProjectMeta): Base {
        val s = MAX_SIDE.toFloat() / max(meta.width, meta.height)
        val scale = minOf(1f, s)
        val w = max(1, (meta.width * scale).roundToInt())
        val h = max(1, (meta.height * scale).roundToInt())
        val bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bmp)
        if (meta.transparentBackground) drawChecker(canvas, w, h) else canvas.drawColor(meta.paperArgb or 0xFF000000.toInt())
        return Base(w, h, bmp, canvas)
    }

    /** Legacy (Block 2) content: rectangle ops over the paper. */
    fun render(meta: ProjectMeta, ops: IntArray): Bitmap {
        val s = MAX_SIDE.toFloat() / max(meta.width, meta.height)
        val scale = minOf(1f, s)
        val w = max(1, (meta.width * scale).roundToInt())
        val h = max(1, (meta.height * scale).roundToInt())
        val bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bmp)
        if (meta.transparentBackground) drawChecker(canvas, w, h) else canvas.drawColor(meta.paperArgb or 0xFF000000.toInt())
        // Content on its own layer (replace semantics), then over the paper.
        val layer = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        val lc = Canvas(layer)
        lc.scale(w.toFloat() / meta.width, h.toFloat() / meta.height)
        val paint = Paint().apply { xfermode = PorterDuffXfermode(PorterDuff.Mode.SRC); isAntiAlias = false }
        var i = 0
        while (i + ContentLog.INTS_PER_OP <= ops.size) {
            paint.color = ops[i + 4]
            lc.drawRect(ops[i].toFloat(), ops[i + 1].toFloat(), (ops[i] + ops[i + 2]).toFloat(), (ops[i + 1] + ops[i + 3]).toFloat(), paint)
            i += ContentLog.INTS_PER_OP
        }
        canvas.drawBitmap(layer, 0f, 0f, null)
        layer.recycle()
        cache.put("${meta.id}@${meta.modifiedMs}", bmp)
        return bmp
    }

    /** Neutral light checkerboard (the usual "transparent" convention). */
    private fun drawChecker(canvas: Canvas, w: Int, h: Int) {
        canvas.drawColor(0xFFFFFFFF.toInt())
        val p = Paint().apply { color = 0xFFDCDCDC.toInt() }
        val cell = 12
        for (y in 0 until (h + cell - 1) / cell) for (x in 0 until (w + cell - 1) / cell)
            if ((x + y) % 2 == 1) canvas.drawRect((x * cell).toFloat(), (y * cell).toFloat(), ((x + 1) * cell).toFloat(), ((y + 1) * cell).toFloat(), p)
    }

    fun png(bmp: Bitmap): ByteArray = ByteArrayOutputStream().also { bmp.compress(Bitmap.CompressFormat.PNG, 100, it) }.toByteArray()
}
