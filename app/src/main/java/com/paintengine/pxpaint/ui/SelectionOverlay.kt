package com.paintengine.pxpaint.ui

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.DashPathEffect
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.Path
import android.view.View
import com.paintengine.engine.NativeEngine
import com.paintengine.pxpaint.core.Affine
import com.paintengine.pxpaint.core.SelectionGesture
import com.paintengine.pxpaint.core.SelectionGeometry
import com.paintengine.pxpaint.core.SelectionOutline
import com.paintengine.pxpaint.core.SelectionShape
import com.paintengine.pxpaint.core.TransformHandle
import com.paintengine.pxpaint.core.TransformState
import java.nio.ByteBuffer

/**
 * Selection feedback over the GPU canvas (Block 10): a normal hardware-
 * accelerated View between the SurfaceView and the chrome, so it never
 * touches the canvas rendering or the input path.
 *
 *  * Marching ants: the engine's outline (canvas pixels, rebuilt only when
 *    the selection changes) mapped to the screen when the view changes, then
 *    drawn as a white line under black dashes whose phase steps ~10 times a
 *    second. The dash effects are made once; the line width is constant at
 *    every zoom. Paused (still dashes) while a stroke is being painted.
 *  * Selected / unselected: with the selection tool, the unselected area is
 *    dimmed through a small coverage image (<= 512 px) the GPU scales.
 *  * Shape being drawn: rectangle / ellipse as the engine will select them
 *    (canvas-aligned), the lasso path, the polygon with its corner handles
 *    and its closing target.
 *  * Moving content: outline and dim follow the offset.
 */
class SelectionOverlay(context: Context, private val theme: PxTheme) : View(context) {
    var engine: NativeEngine? = null
    /** The shape being drawn (null: none). */
    var gesture: SelectionGesture? = null
    var shape = SelectionShape.RECTANGLE
    /** Dim the unselected area (selection tool active). */
    var showDim = false
        set(v) { if (field != v) { field = v; invalidate() } }
    /** A stroke is being painted: the ants stand still. */
    var paused = false
        set(v) { if (field != v) { field = v; if (!v) invalidate() } }

    private val m = FloatArray(6)
    private val toScreen = Affine()
    private val toCanvas = Affine()
    private val matrix = Matrix()
    private val matrixValues = FloatArray(9)
    private val lastMatrix = FloatArray(6)

    // Outline: canvas space (when the selection changes), screen space (when the view changes).
    private val outlineCanvas = Path()
    private val outlineScreen = Path()
    private var hasOutline = false
    private var outlineDirty = true
    /** Selected content being transformed (Block 12): outline, dim and the box with its handles follow it. */
    var transform: TransformState? = null
        set(v) { field = v; outlineDirty = true; invalidate() }
    private val tfm = DoubleArray(6)
    private val combined = FloatArray(6)
    private var preview: Bitmap? = null
    private var canvasW = 0
    private var canvasH = 0
    private val bitmapMatrix = Matrix()

    // Paints: made once (no allocation while drawing).
    private val antsWidth = theme.dpF(1.25f)
    private val dash = theme.dpF(5f)
    private val phases = Array(8) { DashPathEffect(floatArrayOf(dash, dash), it * dash * 2 / 8) }
    private var phase = 0
    private val white = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; strokeWidth = antsWidth; color = Color.WHITE
    }
    private val black = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; strokeWidth = antsWidth; color = Color.BLACK; pathEffect = phases[0]
    }
    private val dim = Paint(Paint.FILTER_BITMAP_FLAG).apply { color = Color.argb(96, 0, 0, 0) }
    private val handleFill = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL; color = Color.WHITE }
    private val handleRing = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; strokeWidth = theme.dpF(1.5f); color = theme.accent
    }
    private val closeRing = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; strokeWidth = theme.dpF(2f); color = theme.accent
    }
    private val shapePath = Path()
    private val boxPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; strokeWidth = theme.dpF(1.5f); color = theme.accent
    }
    private val box = DoubleArray(4)
    private val ring = FloatArray(128 * 2)
    private val handleR = theme.dpF(5f)
    private val closeR = theme.dpF(14f)

    private val tick = Runnable { advance() }

    init {
        importantForAccessibility = IMPORTANT_FOR_ACCESSIBILITY_NO
        isClickable = false
        isFocusable = false
    }

    /** New selection from the engine: outline loops (canvas px), coverage preview. */
    fun setSelection(contours: FloatArray?, previewPx: ByteArray?, previewW: Int, previewH: Int, cw: Int, ch: Int) {
        outlineCanvas.reset()
        hasOutline = false
        if (contours != null && SelectionOutline.loops(contours) > 0) {
            SelectionOutline.forEachLoop(contours) { start, n ->
                if (n >= 2) {
                    outlineCanvas.moveTo(contours[start], contours[start + 1])
                    for (k in 1 until n) outlineCanvas.lineTo(contours[start + k * 2], contours[start + k * 2 + 1])
                    outlineCanvas.close()
                    hasOutline = true
                }
            }
        }
        preview?.recycle()
        preview = null
        if (hasOutline && previewPx != null && previewW > 0 && previewH > 0 && previewPx.size >= previewW * previewH) {
            preview = Bitmap.createBitmap(previewW, previewH, Bitmap.Config.ALPHA_8).also {
                it.copyPixelsFromBuffer(ByteBuffer.wrap(previewPx, 0, previewW * previewH))
            }
        }
        canvasW = cw
        canvasH = ch
        outlineDirty = true
        removeCallbacks(tick)
        invalidate()
    }


    override fun onDetachedFromWindow() {
        removeCallbacks(tick)
        super.onDetachedFromWindow()
    }

    private fun advance() {
        phase = (phase + 1) % phases.size
        black.pathEffect = phases[phase]
        invalidate()
    }

    override fun onDraw(c: Canvas) {
        val e = engine ?: return
        if (!e.viewMatrix(m)) return
        toScreen.set(m)
        if (!toScreen.invertInto(toCanvas)) return
        // The view moved (pan / zoom / rotation) or the selection changed: map the outline again.
        // Outline matrix: view · transform of the content being transformed (if any).
        val tf = transform
        if (tf != null) {
            tf.matrix(tfm)
            combined[0] = (m[0] * tfm[0] + m[2] * tfm[1]).toFloat(); combined[1] = (m[1] * tfm[0] + m[3] * tfm[1]).toFloat()
            combined[2] = (m[0] * tfm[2] + m[2] * tfm[3]).toFloat(); combined[3] = (m[1] * tfm[2] + m[3] * tfm[3]).toFloat()
            combined[4] = (m[0] * tfm[4] + m[2] * tfm[5] + m[4]).toFloat(); combined[5] = (m[1] * tfm[4] + m[3] * tfm[5] + m[5]).toFloat()
        } else System.arraycopy(m, 0, combined, 0, 6)
        var changed = outlineDirty
        for (i in 0 until 6) if (combined[i] != lastMatrix[i]) { changed = true; lastMatrix[i] = combined[i] }
        if (changed) {
            matrixValues[0] = combined[0]; matrixValues[1] = combined[2]; matrixValues[2] = combined[4]
            matrixValues[3] = combined[1]; matrixValues[4] = combined[3]; matrixValues[5] = combined[5]
            matrixValues[6] = 0f; matrixValues[7] = 0f; matrixValues[8] = 1f
            matrix.setValues(matrixValues)
            outlineCanvas.transform(matrix, outlineScreen)
            outlineDirty = false
        }
        if (hasOutline) {
            val p = preview
            if (showDim && p != null && canvasW > 0) {
                bitmapMatrix.set(matrix)
                bitmapMatrix.preScale(canvasW / p.width.toFloat(), canvasH / p.height.toFloat())
                c.drawBitmap(p, bitmapMatrix, dim)
            }
            c.drawPath(outlineScreen, white)
            c.drawPath(outlineScreen, black)
        }
        drawGesture(c)
        if (tf != null) drawTransformBox(c, tf)
        // Ants: about 10 steps per second, only while there is an outline and nothing is painted.
        removeCallbacks(tick)
        if (hasOutline && !paused && isShown) postDelayed(tick, ANTS_STEP_MS)
    }

    private fun drawGesture(c: Canvas) {
        val g = gesture ?: return
        if (!g.busy) return
        val pts = g.points
        shapePath.reset()
        when (shape) {
            SelectionShape.RECTANGLE, SelectionShape.ELLIPSE -> {
                if (pts.count < 2) return
                val ellipse = shape == SelectionShape.ELLIPSE
                SelectionGeometry.canvasBox(toCanvas, pts.x(0), pts.y(0), pts.x(1), pts.y(1), snap = !ellipse, out = box)
                val n = SelectionGeometry.outline(toScreen, box, ellipse, if (ellipse) ring.size / 2 else 4, ring)
                shapePath.moveTo(ring[0], ring[1])
                for (k in 1 until n) shapePath.lineTo(ring[k * 2], ring[k * 2 + 1])
                shapePath.close()
                c.drawPath(shapePath, white)
                c.drawPath(shapePath, black)
                // Corner handles of the box being dragged.
                if (!ellipse) for (k in 0 until 4) drawHandle(c, ring[k * 2], ring[k * 2 + 1])
                else {
                    SelectionGeometry.outline(toScreen, box, false, 4, ring)
                    for (k in 0 until 4) drawHandle(c, ring[k * 2], ring[k * 2 + 1])
                }
            }
            SelectionShape.LASSO -> {
                if (pts.count < 2) return
                shapePath.moveTo(pts.x(0), pts.y(0))
                for (k in 1 until pts.count) shapePath.lineTo(pts.x(k), pts.y(k))
                c.drawPath(shapePath, white)
                c.drawPath(shapePath, black)
            }
            SelectionShape.POLYGON -> {
                if (pts.count < 1) return
                shapePath.moveTo(pts.x(0), pts.y(0))
                for (k in 1 until pts.count) shapePath.lineTo(pts.x(k), pts.y(k))
                if (!g.cursorX.isNaN()) shapePath.lineTo(g.cursorX, g.cursorY)
                c.drawPath(shapePath, white)
                c.drawPath(shapePath, black)
                for (k in 0 until pts.count) drawHandle(c, pts.x(k), pts.y(k))
                // Closing target on the first corner once the polygon can close.
                if (pts.count >= 3) c.drawCircle(pts.x(0), pts.y(0), closeR, closeRing)
            }
        }
    }

    /** The transform box: outline, 4 corner and 4 edge handles, the rotation handle above the top edge. */
    private fun drawTransformBox(c: Canvas, tf: TransformState) {
        fun sx(x: Double, y: Double) = toScreen.x(x, y).toFloat()
        fun sy(x: Double, y: Double) = toScreen.y(x, y).toFloat()
        val hw = tf.w / 2; val hh = tf.h / 2
        shapePath.reset()
        for (k in 0 until 4) {
            val u = CORNER_U[k] * hw; val v = CORNER_V[k] * hh
            val x = tf.mapX(u, v); val y = tf.mapY(u, v)
            if (k == 0) shapePath.moveTo(sx(x, y), sy(x, y)) else shapePath.lineTo(sx(x, y), sy(x, y))
        }
        shapePath.close()
        c.drawPath(shapePath, boxPaint)
        val rd = theme.dpF(36f) / toScreen.scale
        val topX = tf.handleX(TransformHandle.N, rd); val topY = tf.handleY(TransformHandle.N, rd)
        val rx = tf.handleX(TransformHandle.ROTATE, rd); val ry = tf.handleY(TransformHandle.ROTATE, rd)
        c.drawLine(sx(topX, topY), sy(topX, topY), sx(rx, ry), sy(rx, ry), boxPaint)
        for (hd in TransformHandle.entries) {
            if (hd == TransformHandle.NONE || hd == TransformHandle.MOVE) continue
            val x = tf.handleX(hd, rd); val y = tf.handleY(hd, rd)
            if (hd == TransformHandle.ROTATE) {
                c.drawCircle(sx(x, y), sy(x, y), handleR * 1.4f, handleFill)
                c.drawCircle(sx(x, y), sy(x, y), handleR * 1.4f, closeRing)
            } else drawHandle(c, sx(x, y), sy(x, y))
        }
    }

    private fun drawHandle(c: Canvas, x: Float, y: Float) {
        c.drawCircle(x, y, handleR, handleFill)
        c.drawCircle(x, y, handleR, handleRing)
    }

    companion object {
        /** Marching-ants step (about 10 per second: calm, and cheap on the UI thread). */
        const val ANTS_STEP_MS = 100L
        private val CORNER_U = doubleArrayOf(-1.0, 1.0, 1.0, -1.0)
        private val CORNER_V = doubleArrayOf(-1.0, -1.0, 1.0, 1.0)
    }
}
