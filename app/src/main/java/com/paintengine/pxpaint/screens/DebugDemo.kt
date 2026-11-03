package com.paintengine.pxpaint.screens

import android.content.Intent
import com.paintengine.BuildConfig
import com.paintengine.pxpaint.core.BrushFilter
import com.paintengine.pxpaint.core.BrushTool

/**
 * DEBUG BUILDS ONLY: opens a scripted state for automated UI screenshots
 * (CI emulator job, tools/ci/ui_screenshots.sh). Never reachable from the UI:
 * only through `adb shell am start ... --es pxpaint.demo <state>`:
 *
 *   workspace       demo canvas, one stroke per built-in preset, tool panel open
 *   library         + brush library panel (optional --es pxpaint.filter dry|inking|...)
 *   editor          brush editor of --es pxpaint.brush <preset id>
 *   settings        settings screen
 *   layers          five layers (modes, opacity, hidden, locked) + the Layers panel
 *   history         the layers demo (all recorded), 3 steps undone, the History panel
 *
 * The strokes are drawn through the normal engine stroke API with finger-like
 * samples (no pressure: nothing is simulated).
 */
object DebugDemo {
    data class Script(val panel: String, val filter: BrushFilter?, val brush: String?)

    fun from(intent: Intent?): Script? {
        if (!BuildConfig.DEBUG || intent == null) return null
        val state = intent.getStringExtra("pxpaint.demo") ?: return null
        val filter = intent.getStringExtra("pxpaint.filter")?.let { f -> BrushFilter.entries.firstOrNull { it.name.equals(f, true) } }
        return Script(state, filter, intent.getStringExtra("pxpaint.brush"))
    }

    fun start(host: PxHost, s: Script) {
        host.markTipShown(WorkspaceScreen.TIP_GESTURES)   // the first-run tip would cover the panels
        if (s.panel == "settings") {
            host.push(SettingsScreen(host))
            return
        }
        host.io.execute {
            val meta = runCatching { host.store.create("Brush demo", 1600, 2400, 0xFFFFFFFF.toInt(), false) }.getOrNull() ?: return@execute
            host.runOnMain {
                s.brush?.let { id -> host.updateBrushes(host.brushes.select(BrushTool.PAINT, id)) }
                val ws = WorkspaceScreen(host, meta)
                ws.demo = s
                host.push(ws)
            }
        }
    }
}
