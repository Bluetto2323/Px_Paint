package com.paintengine.pxpaint.screens

import android.content.Context
import android.text.Editable
import android.text.InputType
import android.text.TextWatcher
import android.view.Gravity
import android.view.View
import android.view.inputmethod.EditorInfo
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import com.paintengine.pxpaint.core.BrushFilter
import com.paintengine.pxpaint.core.BrushLibrary
import com.paintengine.pxpaint.core.BrushPreset
import com.paintengine.pxpaint.core.BrushTool
import com.paintengine.pxpaint.core.MotionPolicy
import com.paintengine.pxpaint.core.MotionTokens
import com.paintengine.pxpaint.core.S
import com.paintengine.pxpaint.core.tr
import com.paintengine.pxpaint.ui.BrushPreviewView
import com.paintengine.pxpaint.ui.Icon
import com.paintengine.pxpaint.ui.IconButton
import com.paintengine.pxpaint.ui.OverlayHost
import com.paintengine.pxpaint.ui.PxTheme
import com.paintengine.pxpaint.ui.TileGrid
import com.paintengine.pxpaint.ui.Widgets

/**
 * The brush selector (Block 11): a compact floating panel over the
 * workspace. Search, filters (all, recent, favourites, categories, the
 * user's own), a strip of the recently used brushes, then a grid of tiles:
 * each one the brush's real stroke (rendered by the engine's brush code,
 * cached by BrushPreviews) and its name; the current brush is outlined in
 * the accent colour. One tap selects the brush and closes the panel (unless
 * it is pinned); the star marks a favourite; a long press opens the brush's
 * menu (edit, duplicate, rename, reset, delete).
 *
 * State that must survive a rebuild (filter, query) lives in [State], owned
 * by the workspace.
 */
class BrushLibraryPanel(
    private val host: PxHost,
    private val tool: BrushTool,
    private val state: State,
    private val onEdit: (BrushTool) -> Unit,
    private val onChanged: () -> Unit,
) {
    class State {
        var filter = BrushFilter.ALL
        var query = ""
        var searching = false
    }

    private val context: Context get() = host.context
    private val theme: PxTheme get() = host.theme
    private val lib: BrushLibrary get() = host.brushes
    private var grid: TileGrid? = null
    private var chips: LinearLayout? = null
    private val tiles = HashMap<String, View>()

    /** Called after a brush is chosen (the workspace closes the panel unless it is pinned). */
    var onSelected: (() -> Unit)? = null
    /** Opens the tool's settings (size, opacity, editor, pressure). */
    var onSettings: (() -> Unit)? = null

    fun view(): View = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        // Header: title (which tool), search, settings, new brush from the current one.
        addView(LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(theme.text(context, tr(if (tool == BrushTool.ERASER) S.ERASER_BRUSHES else S.BRUSHES), PxTheme.Type.HEADLINE),
                LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            addView(IconButton(context, theme, Icon.SEARCH, tr(S.SEARCH_BRUSHES)).apply {
                isSelected = state.searching
                setOnClickListener {
                    state.searching = !state.searching
                    if (!state.searching) state.query = ""
                    onChanged()
                }
            })
            onSettings?.let { open -> addView(IconButton(context, theme, Icon.SLIDERS, tr(S.BRUSH_SETTINGS)).apply { setOnClickListener { open() } }) }
            addView(IconButton(context, theme, Icon.PLUS, tr(S.SAVE_AS_NEW)).apply {
                setOnClickListener { duplicate(lib.selectedId(tool)) }
            })
        })
        if (state.searching) addView(searchField(), LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
            topMargin = theme.dp(6)
        })
        addView(HorizontalScrollView(context).apply {
            isHorizontalScrollBarEnabled = false
            addView(LinearLayout(context).apply {
                orientation = LinearLayout.HORIZONTAL
                chips = this
            })
        }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = theme.dp(8) })
        buildChips()
        // Quick access: the recently used brushes (all brushes view, no search).
        val recent = if (state.filter == BrushFilter.ALL && state.query.isBlank())
            lib.recents.mapNotNull { lib.get(it) }.filter { it.id != lib.selectedId(tool) }.take(RECENT_STRIP) else emptyList()
        if (recent.isNotEmpty()) {
            addView(theme.text(context, tr(S.RECENT_BRUSHES), PxTheme.Type.LABEL, theme.textSecondary).apply { setPadding(0, theme.dp(10), 0, theme.dp(6)) })
            addView(HorizontalScrollView(context).apply {
                isHorizontalScrollBarEnabled = false
                addView(LinearLayout(context).apply {
                    orientation = LinearLayout.HORIZONTAL
                    for (p in recent) addView(tile(p, compact = true), LinearLayout.LayoutParams(theme.dp(96), LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                        marginEnd = theme.dp(8)
                    })
                })
            })
        }
        addView(TileGrid(context, theme.dp(TILE_MIN_DP), theme.dp(8)).also { grid = it },
            LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = theme.dp(10) })
        buildGrid()
    }

    private fun searchField(): View = EditText(context).apply {
        setText(state.query)
        hint = tr(S.SEARCH_BRUSHES)
        isSingleLine = true
        inputType = InputType.TYPE_CLASS_TEXT
        imeOptions = EditorInfo.IME_ACTION_SEARCH
        theme.style(this, PxTheme.Type.BODY)
        setHintTextColor(theme.textTertiary)
        background = theme.rounded(theme.sunken, theme.radiusPill, theme.outline)
        setPadding(theme.dp(14), theme.dp(8), theme.dp(14), theme.dp(8))
        contentDescription = tr(S.SEARCH_BRUSHES)
        addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {}
            override fun afterTextChanged(s: Editable?) {
                state.query = s?.toString() ?: ""
                buildGrid()
            }
        })
        post { if (state.query.isEmpty()) requestFocus() }
    }

    private fun buildChips() {
        val c = chips ?: return
        c.removeAllViews()
        for (f in BrushFilter.entries) {
            c.addView(Widgets.chip(context, theme, tr(f.label), f == state.filter) {
                if (state.filter == f) return@chip
                state.filter = f
                onChanged()   // rebuilt (the recent strip shows only in "all")
            }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                marginEnd = theme.dp(6)
            })
        }
    }

    private fun buildGrid() {
        val g = grid ?: return
        g.removeAllViews()
        tiles.clear()
        val items = lib.list(state.filter, state.query)
        if (items.isEmpty()) {
            val msg = when {
                state.query.isNotBlank() -> tr(S.NO_BRUSHES, state.query.trim())
                state.filter == BrushFilter.FAVORITES -> tr(S.NO_FAVORITES)
                state.filter == BrushFilter.RECENT -> tr(S.NO_RECENT)
                state.filter == BrushFilter.MINE -> tr(S.NO_MINE)
                else -> tr(S.NO_BRUSHES, "")
            }
            g.addView(theme.text(context, msg, PxTheme.Type.CAPTION, theme.textSecondary).apply { setPadding(0, theme.dp(12), 0, theme.dp(12)) })
            return
        }
        for ((i, p) in items.withIndex()) {
            val t = tile(p, compact = false)
            tiles[p.id] = t
            g.addView(t)
            theme.motion.enter(t, dyDp = MotionTokens.NUDGE * 2, delay = MotionPolicy.stagger(i, theme.motion.level), duration = MotionTokens.SHORT)
        }
    }

    /** One brush: its real stroke, its name; the current brush outlined; a star for favourites. */
    private fun tile(p: BrushPreset, compact: Boolean): View = FrameLayout(context).apply {
        val selected = lib.selectedId(tool) == p.id
        val fav = lib.isFavorite(p.id)
        background = theme.pressable(theme.rounded(if (selected) theme.accentSoft else theme.surface, theme.radiusM,
            if (selected) theme.accent else theme.outline, if (selected) 2f else 1f), theme.radiusM)
        isSelected = selected
        isClickable = true
        contentDescription = p.displayName() + (if (selected) ", " + tr(S.BRUSH_CURRENT) else "") + (if (fav) ", " + tr(S.FILTER_FAVORITES) else "")
        setOnClickListener { select(p.id) }
        setOnLongClickListener { menu(this, p); true }
        val col = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(theme.dp(6), theme.dp(6), theme.dp(6), theme.dp(6))
            val preview = BrushPreviewView(context, theme, host.previews, false) { host.pressureTable() }
            addView(preview, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, theme.dp(if (compact) 36 else 46)))
            preview.show(p.params)
            addView(theme.text(context, p.displayName(), PxTheme.Type.CAPTION, if (selected) theme.accent else theme.text, lines = 1).apply {
                setPadding(theme.dp(2), theme.dp(6), theme.dp(if (compact) 2 else 22), 0)
            })
        }
        addView(col, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.WRAP_CONTENT))
        if (!compact) addView(IconButton(context, theme, if (fav) Icon.STAR_FILLED else Icon.STAR, tr(if (fav) S.FAVORITE_REMOVE else S.FAVORITE_ADD), sizeDp = 30f).apply {
            isSelected = fav
            minimumWidth = theme.dp(36)
            minimumHeight = theme.dp(36)
            setOnClickListener {
                host.updateBrushes(lib.toggleFavorite(p.id))
                if (state.filter == BrushFilter.FAVORITES) buildGrid() else replaceTile(p.id)
            }
        }, FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM or Gravity.END))
    }

    private fun replaceTile(id: String) {
        val g = grid ?: return
        val old = tiles[id] ?: return
        val i = g.indexOfChild(old)
        val p = lib.get(id) ?: return
        val t = tile(p, compact = false)
        tiles[id] = t
        g.removeViewAt(i)
        g.addView(t, i)
    }

    private fun select(id: String) {
        val prev = lib.selectedId(tool)
        host.updateBrushes(lib.select(tool, id))
        replaceTile(prev)
        replaceTile(id)
        tiles[id]?.let { theme.motion.pop(it, 1.04f) }   // the chosen brush answers
        onSelected?.invoke()
    }

    private fun menu(anchor: View, p: BrushPreset) {
        val items = ArrayList<OverlayHost.MenuItem>()
        items += OverlayHost.MenuItem(tr(S.EDIT_BRUSH), Icon.SLIDERS) {
            host.updateBrushes(lib.select(tool, p.id))
            onEdit(tool)
        }
        items += OverlayHost.MenuItem(tr(S.ACTION_DUPLICATE), Icon.COPY) { duplicate(p.id) }
        if (lib.canRename(p.id)) items += OverlayHost.MenuItem(tr(S.ACTION_RENAME), Icon.EDIT) { rename(p) }
        if (lib.isModified(p.id)) items += OverlayHost.MenuItem(tr(S.RESET_BRUSH), Icon.RESET) { reset(p) }
        if (lib.canDelete(p.id)) items += OverlayHost.MenuItem(tr(S.ACTION_DELETE), Icon.TRASH, destructive = true) { delete(p) }
        host.overlays.menu(anchor, items)
    }

    private fun duplicate(id: String) {
        val newId = newUserId(lib)
        val next = lib.duplicate(id, newId, tool)
        if (next.get(newId) == null) return
        host.updateBrushes(next.select(tool, newId))
        host.overlays.message(tr(S.BRUSH_DUPLICATED))
        onChanged()
    }

    private fun rename(p: BrushPreset) {
        host.overlays.prompt(tr(S.RENAME_BRUSH), p.name, tr(S.ACTION_SAVE), tr(S.ACTION_CANCEL)) { name ->
            host.updateBrushes(lib.rename(p.id, name))
            onChanged()
        }
    }

    private fun reset(p: BrushPreset) {
        host.overlays.confirm(tr(S.RESET_BRUSH), tr(S.RESET_BRUSH_BODY, p.displayName()), tr(S.RESET_BRUSH), tr(S.ACTION_CANCEL), false) {
            host.updateBrushes(lib.reset(p.id))
            host.overlays.message(tr(S.BRUSH_RESET))
            onChanged()
        }
    }

    private fun delete(p: BrushPreset) {
        host.overlays.confirm(tr(S.DELETE_TITLE, p.displayName()), tr(S.DELETE_BRUSH_BODY), tr(S.ACTION_DELETE), tr(S.ACTION_CANCEL), true) {
            host.updateBrushes(lib.delete(p.id))
            host.overlays.message(tr(S.BRUSH_DELETED))
            onChanged()
        }
    }

    companion object {
        const val RECENT_STRIP = 6
        /** Narrowest grid tile (3 columns in a phone sheet, more on tablets). */
        const val TILE_MIN_DP = 104

        /** A fresh user preset id (time + random, checked against the library). */
        fun newUserId(lib: BrushLibrary): String {
            while (true) {
                val id = BrushPreset.USER_PREFIX + java.lang.Long.toString(System.currentTimeMillis(), 36) +
                    java.lang.Integer.toString((Math.random() * 1296).toInt(), 36)
                if (lib.get(id) == null) return id
            }
        }
    }
}
