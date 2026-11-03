package com.paintengine.pxpaint.screens

import android.content.Context
import android.graphics.Rect
import android.view.View
import com.paintengine.engine.NativeEngine
import com.paintengine.pxpaint.core.AppSettings
import com.paintengine.pxpaint.core.BrushState
import com.paintengine.pxpaint.core.ProjectStore
import com.paintengine.pxpaint.ui.OverlayHost
import com.paintengine.pxpaint.ui.PxTheme
import java.util.concurrent.ExecutorService

/** What screens may use from the activity (PxPaintActivity implements it). */
interface PxHost {
    val context: Context
    val theme: PxTheme
    val settings: AppSettings
    val overlays: OverlayHost
    /** System bar / cutout insets of the window, in px. */
    val insets: Rect
    /** Window size in dp (current orientation, multi-window aware). */
    val windowWidthDp: Int
    val windowHeightDp: Int
    val smallestWidthDp: Int

    /** Project storage; call ONLY on [io]. */
    val store: ProjectStore
    /** Single background thread for all disk work (serialises store access). */
    val io: ExecutorService
    fun runOnMain(action: () -> Unit)

    /** The process' native engine (created on first use, one per process). */
    fun engine(): NativeEngine?
    /** Engine if it already exists (never creates it). */
    val engineIfStarted: NativeEngine?

    fun push(screen: Screen)
    fun pop()
    fun replaceTop(screen: Screen)
    fun updateSettings(newSettings: AppSettings)
    /** Brush setup (sizes, opacities, spacing, colour), persisted across sessions. */
    val brush: BrushState
    fun updateBrush(state: BrushState)
    fun openDiagnostics()
    fun tipsShown(id: String): Boolean
    fun markTipShown(id: String)
    fun resetTips()
}

/**
 * One screen of the app. Views are created by [createView] and may be
 * re-created at any time (theme or layout change), so screens keep their
 * state in fields, not in views.
 */
abstract class Screen(protected val host: PxHost) {
    protected val theme: PxTheme get() = host.theme
    protected val context: Context get() = host.context

    abstract fun createView(): View

    /** Became the visible screen (also after a rebuild). */
    open fun onShown() {}
    /** Leaves the screen stack. */
    open fun onRemoved() {}
    /** Back pressed (overlays already handled). True if consumed. */
    open fun onBack(): Boolean = false
    open fun onPause() {}
    open fun onResume() {}
    /** Window size / orientation / insets changed; default rebuilds the view. */
    open fun onLayoutChanged(): Boolean = true
}
