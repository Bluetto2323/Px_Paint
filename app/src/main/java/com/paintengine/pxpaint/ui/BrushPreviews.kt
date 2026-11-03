package com.paintengine.pxpaint.ui

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapShader
import android.graphics.Canvas
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Shader
import android.os.Handler
import android.os.Looper
import android.util.LruCache
import android.view.View
import com.paintengine.engine.NativeEngine
import com.paintengine.pxpaint.core.BrushEncoding
import com.paintengine.pxpaint.core.BrushParams
import com.paintengine.pxpaint.core.BrushPreviewSpec
import com.paintengine.pxpaint.core.tr
import java.util.concurrent.Executors

/**
 * Brush previews rendered by the engine's real brush code
 * (NativeEngine.renderBrushPreview) on one low-priority background thread,
 * cached by everything they depend on (parameters, size, colours, pressure
 * curve). Requests for a key already being rendered share one render.
 * Main thread API.
 */
class BrushPreviews {
    private val exec = Executors.newSingleThreadExecutor { r ->
        Thread(r, "PxPaint-previews").apply { priority = Thread.NORM_PRIORITY - 1 }
    }
    private val main = Handler(Looper.getMainLooper())
    private val cache = object : LruCache<String, Bitmap>(16 * 1024 * 1024) {
        override fun sizeOf(key: String, value: Bitmap) = value.byteCount
    }
    private val waiting = HashMap<String, MutableList<(Bitmap?) -> Unit>>()
    @Volatile private var closed = false

    fun cached(key: String): Bitmap? = cache.get(key)

    /** [strokes] in BrushPreviewSpec format; [onReady] on the main thread. */
    fun request(
        key: String, params: BrushParams, w: Int, h: Int, ink: Int, paper: Int, curve: FloatArray?, strokes: () -> FloatArray,
        onReady: (Bitmap?) -> Unit,
    ) {
        cache.get(key)?.let { onReady(it); return }
        waiting[key]?.let { it += onReady; return }
        waiting[key] = mutableListOf(onReady)
        exec.execute {
            val bmp = if (closed) null else render(params, w, h, ink, paper, curve, strokes())
            main.post {
                if (bmp != null) cache.put(key, bmp)
                waiting.remove(key)?.forEach { it(bmp) }
            }
        }
    }

    /** Renders now on the calling thread (the try pad's own worker). */
    fun render(params: BrushParams, w: Int, h: Int, ink: Int, paper: Int, curve: FloatArray?, strokes: FloatArray): Bitmap? {
        if (w <= 0 || h <= 0) return null
        val brush = NativeEngine.Brush(
            BrushEncoding.params(params, pressureSmoothing = 0f), BrushEncoding.flags(params, false), ink, curve,
        )
        val px = NativeEngine.renderBrushPreview(brush, w, h, strokes, paper) ?: return null
        return Bitmap.createBitmap(px, w, h, Bitmap.Config.ARGB_8888)
    }

    fun close() {
        closed = true
        exec.shutdownNow()
    }

    companion object {
        /** Previews show the brush on light paper in dark ink, in both themes. */
        const val PAPER = 0xFFF7F5F0.toInt()
        const val INK = 0xFF1F2733.toInt()
    }
}

/**
 * Shows a brush preview: a thumbnail (one S-curve) or the editor's detail
 * view (stroke, pressure response, sizes, opacities, with labels). Renders
 * when it knows its size; shows the last image until the new one is ready.
 */
@SuppressLint("ViewConstructor")
class BrushPreviewView(
    context: Context,
    private val theme: PxTheme,
    private val previews: BrushPreviews,
    private val detail: Boolean,
    private val curve: () -> FloatArray?,
) : View(context) {
    private var params: BrushParams? = null
    private var bitmap: Bitmap? = null
    private var key: String? = null
    private var labels: List<BrushPreviewSpec.Label> = emptyList()
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG or Paint.FILTER_BITMAP_FLAG)
    private val bg = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = BrushPreviews.PAPER }
    private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = 0xFF6B6F76.toInt()
        textSize = theme.dpF(11f)
    }
    private val rect = RectF()
    private val radius = theme.dpF(if (detail) 12f else 8f)

    init {
        importantForAccessibility = IMPORTANT_FOR_ACCESSIBILITY_NO
    }

    fun show(p: BrushParams) {
        params = p
        requestRender()
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        requestRender()
    }

    private fun requestRender() {
        val p = params ?: return
        val w = width
        val h = height
        if (w <= 0 || h <= 0) return
        val c = curve()
        val k = BrushPreviewSpec.key(p, if (detail) "D" else "T", w, h, BrushPreviews.INK, BrushPreviews.PAPER, c?.contentHashCode() ?: 0)
        if (k == key && bitmap != null) return
        key = k
        val spec = if (detail) BrushPreviewSpec.detail(p, w, h) else null
        labels = spec?.second ?: emptyList()
        previews.cached(k)?.let { set(it); return }
        previews.request(k, p, w, h, BrushPreviews.INK, BrushPreviews.PAPER, c,
            { spec?.first ?: BrushPreviewSpec.thumbnail(p, w, h) }) { b -> if (key == k && b != null) set(b) }
    }

    private fun set(b: Bitmap) {
        bitmap = b
        paint.shader = BitmapShader(b, Shader.TileMode.CLAMP, Shader.TileMode.CLAMP).apply { setLocalMatrix(Matrix()) }
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        rect.set(0f, 0f, width.toFloat(), height.toFloat())
        canvas.drawRoundRect(rect, radius, radius, bg)
        if (bitmap != null) canvas.drawRoundRect(rect, radius, radius, paint)
        for (l in labels) canvas.drawText(tr(l.text), l.x, l.y + text.textSize, text)
    }
}
