package com.paintengine.pxpaint.screens

import android.content.Context
import android.text.Editable
import android.text.InputType
import android.text.TextWatcher
import android.view.Gravity
import android.view.View
import android.view.inputmethod.EditorInfo
import android.widget.EditText
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.TextView
import com.paintengine.pxpaint.core.BrushFilter
import com.paintengine.pxpaint.core.BrushLibrary
import com.paintengine.pxpaint.core.BrushPreset
import com.paintengine.pxpaint.core.BrushState
import com.paintengine.pxpaint.core.BrushTool
import com.paintengine.pxpaint.core.S
import com.paintengine.pxpaint.core.tr
import com.paintengine.pxpaint.ui.BrushPreviewView
import com.paintengine.pxpaint.ui.Icon
import com.paintengine.pxpaint.ui.IconButton
import com.paintengine.pxpaint.ui.OverlayHost
import com.paintengine.pxpaint.ui.PxSlider
import com.paintengine.pxpaint.ui.PxTheme
import com.paintengine.pxpaint.ui.Widgets
import kotlin.math.roundToInt

/**
 * The brush library: a floating panel over the workspace (the canvas stays
 * visible around it). Filters (all, favourites, recent, categories, the
 * user's own), search, quick size / opacity of the selected brush, and one
 * row per brush with a live preview rendered by the engine, a favourite star
 * and a menu (edit, duplicate, rename, reset, delete).
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
    private var list: LinearLayout? = null
    private var chips: LinearLayout? = null
    private var sizeSlider: PxSlider? = null
    private var opacitySlider: PxSlider? = null
    private var sizeValue: TextView? = null
    private var opacityValue: TextView? = null
    private val rows = HashMap<String, View>()

    fun view(): View = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        // Header: title, search, new brush from the current one.
        addView(LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(theme.text(context, tr(S.BRUSHES), PxTheme.Type.HEADLINE), LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            addView(IconButton(context, theme, Icon.SEARCH, tr(S.SEARCH_BRUSHES)).apply {
                isSelected = state.searching
                setOnClickListener {
                    state.searching = !state.searching
                    if (!state.searching) state.query = ""
                    onChanged()
                }
            })
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
        addView(quickControls())
        addView(LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            list = this
        }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = theme.dp(10) })
        buildList()
    }

    private fun searchField(): View = EditText(context).apply {
        setText(state.query)
        hint = tr(S.SEARCH_BRUSHES)
        isSingleLine = true
        inputType = InputType.TYPE_CLASS_TEXT
        imeOptions = EditorInfo.IME_ACTION_SEARCH
        theme.style(this, PxTheme.Type.BODY)
        setHintTextColor(theme.textTertiary)
        background = theme.rounded(theme.sunken, theme.radiusS, theme.outline)
        setPadding(theme.dp(12), theme.dp(8), theme.dp(12), theme.dp(8))
        contentDescription = tr(S.SEARCH_BRUSHES)
        addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {}
            override fun afterTextChanged(s: Editable?) {
                state.query = s?.toString() ?: ""
                buildList()
            }
        })
        post { if (state.query.isEmpty()) requestFocus() }
    }

    private fun buildChips() {
        val c = chips ?: return
        c.removeAllViews()
        for (f in BrushFilter.entries) {
            c.addView(Widgets.chip(context, theme, tr(f.label), f == state.filter) {
                state.filter = f
                buildChips()
                buildList()
            }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                marginEnd = theme.dp(6)
            })
        }
    }

    private fun quickControls(): View = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(0, theme.dp(6), 0, 0)
        val p = lib.effective(tool)
        addView(slider(tr(S.SIZE), BrushState.sizeToSlider(p.size), { BrushState.sizeLabel(BrushState.sliderToSize(it)) }, true) {
            host.updateBrushes(lib.setLive(tool, size = BrushState.sliderToSize(it)))
        })
        addView(slider(tr(S.OPACITY), p.opacity, { "${(BrushState.sliderToOpacity(it) * 100).roundToInt()}%" }, false) {
            host.updateBrushes(lib.setLive(tool, opacity = BrushState.sliderToOpacity(it)))
        })
    }

    private fun slider(label: String, initial: Float, format: (Float) -> String, size: Boolean, apply: (Float) -> Unit): View =
        LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(theme.text(context, label, PxTheme.Type.LABEL, theme.textSecondary, lines = 1), LinearLayout.LayoutParams(theme.dp(72), LinearLayout.LayoutParams.WRAP_CONTENT))
            val value = theme.text(context, format(initial), PxTheme.Type.LABEL, theme.textSecondary, lines = 1).apply { gravity = Gravity.END }
            val s = PxSlider(context, theme, label).apply {
                valueText = format
                setValue(initial)
                onChange = { v, fromUser ->
                    value.text = format(v)
                    if (fromUser) apply(v)
                }
            }
            addView(s, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            addView(value, LinearLayout.LayoutParams(theme.dp(56), LinearLayout.LayoutParams.WRAP_CONTENT))
            if (size) { sizeSlider = s; sizeValue = value } else { opacitySlider = s; opacityValue = value }
        }

    private fun refreshQuick() {
        val p = lib.effective(tool)
        sizeSlider?.setValue(BrushState.sizeToSlider(p.size))
        sizeValue?.text = BrushState.sizeLabel(p.size)
        opacitySlider?.setValue(p.opacity)
        opacityValue?.text = "${(p.opacity * 100).roundToInt()}%"
    }

    private fun buildList() {
        val l = list ?: return
        l.removeAllViews()
        rows.clear()
        val items = lib.list(state.filter, state.query)
        if (items.isEmpty()) {
            val msg = when {
                state.query.isNotBlank() -> tr(S.NO_BRUSHES, state.query.trim())
                state.filter == BrushFilter.FAVORITES -> tr(S.NO_FAVORITES)
                state.filter == BrushFilter.RECENT -> tr(S.NO_RECENT)
                state.filter == BrushFilter.MINE -> tr(S.NO_MINE)
                else -> tr(S.NO_BRUSHES, "")
            }
            l.addView(theme.text(context, msg, PxTheme.Type.CAPTION, theme.textSecondary).apply { setPadding(0, theme.dp(12), 0, theme.dp(12)) })
            return
        }
        for (p in items) {
            val r = row(p)
            rows[p.id] = r
            l.addView(r, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                bottomMargin = theme.dp(6)
            })
        }
    }

    private fun row(p: BrushPreset): View = LinearLayout(context).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        minimumHeight = theme.touchTarget + theme.dp(8)
        setPadding(theme.dp(6), theme.dp(4), 0, theme.dp(4))
        val selected = lib.selectedId(tool) == p.id
        background = theme.pressable(theme.rounded(if (selected) theme.accentSoft else theme.surface, theme.radiusS,
            if (selected) theme.accent else theme.outline), theme.radiusS)
        isSelected = selected
        isClickable = true
        contentDescription = p.displayName()
        setOnClickListener { select(p.id) }
        setOnLongClickListener { menu(this, p); true }
        val preview = BrushPreviewView(context, theme, host.previews, false) { host.pressureTable() }
        addView(preview, LinearLayout.LayoutParams(theme.dp(104), theme.dp(40)))
        preview.show(p.params)
        addView(LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(theme.dp(10), 0, theme.dp(4), 0)
            addView(theme.text(context, p.displayName(), PxTheme.Type.LABEL, lines = 1))
            val sub = buildString {
                append(tr(p.category.label))
                if (lib.isModified(p.id)) append(" · ").append(tr(S.BRUSH_MODIFIED))
            }
            addView(theme.text(context, sub, PxTheme.Type.CAPTION, if (lib.isModified(p.id)) theme.accent else theme.textSecondary, lines = 1))
        }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        val fav = lib.isFavorite(p.id)
        addView(IconButton(context, theme, if (fav) Icon.STAR_FILLED else Icon.STAR, tr(if (fav) S.FAVORITE_REMOVE else S.FAVORITE_ADD), sizeDp = 40f).apply {
            isSelected = fav
            setOnClickListener {
                host.updateBrushes(lib.toggleFavorite(p.id))
                replaceRow(p.id)
                if (state.filter == BrushFilter.FAVORITES) buildList()
            }
        })
        val more = IconButton(context, theme, Icon.MORE, tr(S.MORE_OPTIONS), sizeDp = 40f)
        more.setOnClickListener { menu(more, p) }
        addView(more)
    }

    private fun replaceRow(id: String) {
        val l = list ?: return
        val old = rows[id] ?: return
        val i = l.indexOfChild(old)
        val p = lib.get(id) ?: return
        val r = row(p)
        rows[id] = r
        l.removeViewAt(i)
        l.addView(r, i, old.layoutParams)
    }

    private fun select(id: String) {
        val prev = lib.selectedId(tool)
        host.updateBrushes(lib.select(tool, id))
        replaceRow(prev)
        replaceRow(id)
        refreshQuick()
        onSelected?.invoke()
    }

    /** Called after a brush is chosen (the workspace refreshes its brush row). */
    var onSelected: (() -> Unit)? = null

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
