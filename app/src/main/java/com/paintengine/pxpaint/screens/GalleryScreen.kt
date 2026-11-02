package com.paintengine.pxpaint.screens

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Bitmap
import android.text.Editable
import android.text.TextWatcher
import android.view.Gravity
import android.view.View
import android.view.inputmethod.EditorInfo
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import com.paintengine.pxpaint.core.Format
import com.paintengine.pxpaint.core.LayoutPolicy
import com.paintengine.pxpaint.core.ProjectMeta
import com.paintengine.pxpaint.core.ProjectQuery
import com.paintengine.pxpaint.core.S
import com.paintengine.pxpaint.core.SortOrder
import com.paintengine.pxpaint.core.tr
import com.paintengine.pxpaint.ui.CheckerDrawable
import com.paintengine.pxpaint.ui.GridLayoutView
import com.paintengine.pxpaint.ui.Icon
import com.paintengine.pxpaint.ui.IconButton
import com.paintengine.pxpaint.ui.IconDrawable
import com.paintengine.pxpaint.ui.OverlayHost
import com.paintengine.pxpaint.ui.PxTheme
import com.paintengine.pxpaint.ui.Thumbnails
import com.paintengine.pxpaint.ui.Widgets
import com.paintengine.pxpaint.ui.markHeading
import java.text.DateFormat
import java.util.Date

/** Home: the project gallery. */
class GalleryScreen(host: PxHost) : Screen(host) {
    private var projects: List<ProjectMeta> = emptyList()
    private var loaded = false
    private var search = ""
    private var sort = SortOrder.RECENT
    private var grid: GridLayoutView? = null
    private var stateArea: LinearLayout? = null
    private var countText: TextView? = null
    private var sortChip: TextView? = null

    override fun createView(): View {
        val root = FrameLayout(context).apply { setBackgroundColor(theme.bg) }
        val scroll = ScrollView(context).apply { isFillViewport = true; clipToPadding = false }
        val page = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(theme.pad + host.insets.left, host.insets.top + theme.dp(8), theme.pad + host.insets.right, host.insets.bottom + theme.dp(24))
        }
        page.addView(header())
        page.addView(searchRow(), LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
            topMargin = theme.dp(12); bottomMargin = theme.dp(16)
        })
        val columns = LayoutPolicy.galleryColumns(host.windowWidthDp, theme.scale)
        grid = GridLayoutView(context, columns, theme.dp(12))
        page.addView(grid)
        stateArea = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL; gravity = Gravity.CENTER_HORIZONTAL }
        page.addView(stateArea)
        scroll.addView(page)
        root.addView(scroll)
        render()
        return root
    }

    override fun onShown() = reload()

    fun reload() {
        host.io.execute {
            val list = runCatching { host.store.list() }
            host.runOnMain {
                list.onSuccess { projects = it }.onFailure { host.overlays.message(tr(S.STORE_ERROR, it.message ?: "?")) }
                loaded = true
                render()
            }
        }
    }

    private fun header(): View = LinearLayout(context).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        minimumHeight = theme.touchTarget + theme.dp(8)
        addView(View(context).apply { background = IconDrawable(Icon.LOGO, theme.accent, 2f) }, LinearLayout.LayoutParams(theme.dp(30), theme.dp(30)))
        val titles = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(theme.dp(12), 0, 0, 0)
            addView(theme.text(context, tr(S.APP_NAME), PxTheme.Type.DISPLAY).apply { markHeading() })
            countText = theme.text(context, "", PxTheme.Type.CAPTION, theme.textSecondary)
            addView(countText)
        }
        addView(titles, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        addView(IconButton(context, theme, Icon.SLIDERS, tr(S.SETTINGS)).apply {
            setOnClickListener { host.push(SettingsScreen(host)) }
        })
    }

    private fun searchRow(): View = LinearLayout(context).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        val field = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            background = theme.rounded(theme.surface, theme.dpF(999f), theme.outline)
            minimumHeight = theme.touchTarget
            setPadding(theme.dp(14), 0, theme.dp(8), 0)
            addView(View(context).apply { background = IconDrawable(Icon.SEARCH, theme.textSecondary) }, LinearLayout.LayoutParams(theme.dp(18), theme.dp(18)))
            val edit = EditText(context).apply {
                hint = tr(S.SEARCH_HINT)
                setText(search)
                isSingleLine = true
                imeOptions = EditorInfo.IME_ACTION_SEARCH
                theme.style(this, PxTheme.Type.BODY)
                setHintTextColor(theme.textTertiary)
                background = null
                setPadding(theme.dp(10), theme.dp(10), theme.dp(4), theme.dp(10))
                contentDescription = tr(S.SEARCH_HINT)
                addTextChangedListener(object : TextWatcher {
                    override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
                    override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
                    override fun afterTextChanged(s: Editable?) {
                        search = s?.toString() ?: ""
                        render()
                    }
                })
            }
            addView(edit, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        }
        addView(field, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        sortChip = Widgets.chip(context, theme, sortLabel(), false) { showSortMenu() }.apply {
            minHeight = theme.touchTarget
            setCompoundDrawablesRelativeWithIntrinsicBounds(IconDrawable(Icon.SORT, theme.textSecondary).apply {
                setBounds(0, 0, theme.dp(16), theme.dp(16))
            }, null, null, null)
            compoundDrawablePadding = theme.dp(6)
            contentDescription = "${tr(S.SORT)}: ${sortLabel()}"
        }
        addView(sortChip, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
            marginStart = theme.dp(8)
        })
    }

    private fun sortLabel() = when (sort) {
        SortOrder.RECENT -> tr(S.SORT_RECENT)
        SortOrder.NAME -> tr(S.SORT_NAME)
        SortOrder.SIZE -> tr(S.SORT_SIZE)
        SortOrder.OLDEST -> tr(S.SORT_OLDEST)
    }

    private fun showSortMenu() {
        val anchor = sortChip ?: return
        host.overlays.menu(anchor, SortOrder.entries.map { order ->
            OverlayHost.MenuItem(
                when (order) {
                    SortOrder.RECENT -> tr(S.SORT_RECENT)
                    SortOrder.NAME -> tr(S.SORT_NAME)
                    SortOrder.SIZE -> tr(S.SORT_SIZE)
                    SortOrder.OLDEST -> tr(S.SORT_OLDEST)
                },
                if (order == sort) Icon.CHECK else null,
            ) {
                sort = order
                sortChip?.text = sortLabel()
                sortChip?.contentDescription = "${tr(S.SORT)}: ${sortLabel()}"
                render()
            }
        })
    }

    private fun render() {
        val grid = grid ?: return
        val state = stateArea ?: return
        grid.removeAllViews()
        state.removeAllViews()
        countText?.text = when (projects.size) {
            0 -> tr(S.GALLERY_TITLE)
            1 -> tr(S.CANVAS_COUNT_ONE)
            else -> tr(S.CANVAS_COUNT, projects.size)
        }
        if (!loaded) return
        if (projects.isEmpty()) {
            state.addView(emptyState())
            return
        }
        grid.addView(newCanvasCard())
        val shown = ProjectQuery.apply(projects, search, sort)
        for (p in shown) grid.addView(projectCard(p))
        if (shown.isEmpty()) state.addView(theme.text(context, tr(S.NO_RESULTS, search.trim()), PxTheme.Type.BODY, theme.textSecondary).apply {
            gravity = Gravity.CENTER
            setPadding(0, theme.dp(32), 0, 0)
        })
    }

    private fun emptyState(): View = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        gravity = Gravity.CENTER_HORIZONTAL
        setPadding(theme.dp(12), theme.dp(48), theme.dp(12), 0)
        addView(View(context).apply { background = IconDrawable(Icon.LOGO, theme.textTertiary, 1.2f) }, LinearLayout.LayoutParams(theme.dp(96), theme.dp(96)))
        addView(theme.text(context, tr(S.EMPTY_TITLE), PxTheme.Type.TITLE).apply {
            gravity = Gravity.CENTER; setPadding(0, theme.dp(20), 0, theme.dp(8))
        })
        addView(theme.text(context, tr(S.EMPTY_BODY), PxTheme.Type.BODY, theme.textSecondary).apply {
            gravity = Gravity.CENTER; maxWidth = theme.dp(360)
        })
        addView(Widgets.pill(context, theme, tr(S.NEW_CANVAS), true) { newCanvas() }, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT,
        ).apply { topMargin = theme.dp(24) })
    }

    private fun newCanvas() = NewCanvasSheet(host) { meta -> host.push(WorkspaceScreen(host, meta)) }.show()

    private fun newCanvasCard(): View = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        contentDescription = tr(S.NEW_CANVAS)
        val tile = AspectFrame(context, 1f).apply {
            background = theme.pressable(theme.rounded(theme.accentSoft, theme.radiusM, theme.accent, 1.2f), theme.radiusM)
            isClickable = true
            setOnClickListener { newCanvas() }
            addView(View(context).apply { background = IconDrawable(Icon.PLUS, theme.accent, 2f) },
                FrameLayout.LayoutParams(theme.dp(36), theme.dp(36), Gravity.CENTER))
        }
        addView(tile)
        addView(theme.text(context, tr(S.NEW_CANVAS), PxTheme.Type.LABEL, theme.accent, lines = 1).apply {
            setPadding(theme.dp(2), theme.dp(10), 0, 0)
        })
    }

    private fun projectCard(p: ProjectMeta): View {
        val card = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            contentDescription = "${p.name}, ${Format.dimensions(p.width, p.height)}"
        }
        val thumbFrame = AspectFrame(context, 1f).apply {
            background = theme.rounded(theme.sunken, theme.radiusM)
            clipToOutline = true
            foreground = theme.pressable(null, theme.radiusM)
            isClickable = true
            setOnClickListener { host.push(WorkspaceScreen(host, p)) }
            setOnLongClickListener { showProjectMenu(this, p); true }
        }
        val image = ImageView(context).apply {
            scaleType = ImageView.ScaleType.FIT_CENTER
            importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO
        }
        val pad = theme.dp(10)
        thumbFrame.addView(image, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT).apply {
            setMargins(pad, pad, pad, pad)
        })
        card.addView(thumbFrame)
        val meta = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.TOP
            setPadding(theme.dp(2), theme.dp(8), 0, 0)
        }
        val texts = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            addView(theme.text(context, p.name, PxTheme.Type.LABEL, lines = 1))
            addView(theme.text(context, Format.dimensions(p.width, p.height), PxTheme.Type.CAPTION, theme.textSecondary, lines = 1).apply {
                setPadding(0, theme.dp(3), 0, 0)
            })
            addView(theme.text(context, edited(p.modifiedMs), PxTheme.Type.CAPTION, theme.textTertiary, lines = 1))
        }
        meta.addView(texts, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        val more = IconButton(context, theme, Icon.MORE, "${tr(S.MORE_OPTIONS)}: ${p.name}", 32f)
        more.setOnClickListener { showProjectMenu(more, p) }
        meta.addView(more)
        card.addView(meta)
        loadThumbnail(p, image)
        return card
    }

    private fun loadThumbnail(p: ProjectMeta, image: ImageView) {
        fun apply(bmp: Bitmap?) {
            if (bmp != null) image.setImageBitmap(bmp)
            else image.setImageDrawable(
                if (p.transparentBackground) CheckerDrawable(theme.dp(6), false)
                else android.graphics.drawable.ColorDrawable(p.paperArgb or 0xFF000000.toInt()),
            )
        }
        Thumbnails.cached(p.id, p.modifiedMs)?.let { apply(it); return }
        apply(null)
        host.io.execute {
            val bmp = runCatching {
                Thumbnails.load(host.store.thumbnailFile(p.id), p) ?: run {
                    // No thumbnail yet (e.g. created by a previous version): render it once.
                    val ops = host.store.loadContent(p.id)
                    Thumbnails.render(p, ops).also { host.store.saveThumbnail(p.id, Thumbnails.png(it)) }
                }
            }.getOrNull()
            host.runOnMain { if (image.isAttachedToWindow && bmp != null) apply(bmp) }
        }
    }

    private fun edited(ms: Long): String {
        val (bucket, n) = Format.ago(System.currentTimeMillis(), ms)
        return when (bucket) {
            Format.Ago.JUST_NOW -> tr(S.EDITED_JUST_NOW)
            Format.Ago.MINUTES -> tr(S.EDITED_MINUTES, n)
            Format.Ago.HOURS -> tr(S.EDITED_HOURS, n)
            Format.Ago.YESTERDAY -> tr(S.EDITED_YESTERDAY)
            Format.Ago.DAYS -> tr(S.EDITED_DAYS, n)
            Format.Ago.DATE -> tr(S.EDITED_ON, DateFormat.getDateInstance(DateFormat.MEDIUM).format(Date(ms)))
        }
    }

    private fun showProjectMenu(anchor: View, p: ProjectMeta) {
        host.overlays.menu(anchor, listOf(
            OverlayHost.MenuItem(tr(S.ACTION_OPEN), Icon.CHEVRON_RIGHT) { host.push(WorkspaceScreen(host, p)) },
            OverlayHost.MenuItem(tr(S.ACTION_RENAME), Icon.EDIT) { rename(p) },
            OverlayHost.MenuItem(tr(S.ACTION_DUPLICATE), Icon.COPY) { duplicate(p) },
            OverlayHost.MenuItem(tr(S.ACTION_DELETE), Icon.TRASH, destructive = true) { delete(p) },
        ))
    }

    private fun rename(p: ProjectMeta) {
        host.overlays.prompt(tr(S.RENAME_TITLE), p.name, tr(S.ACTION_SAVE), tr(S.ACTION_CANCEL)) { name ->
            storeAction(tr(S.RENAMED)) { host.store.rename(p.id, name) }
        }
    }

    private fun duplicate(p: ProjectMeta) = storeAction(tr(S.DUPLICATED)) { host.store.duplicate(p.id, tr(S.COPY_SUFFIX)) }

    private fun delete(p: ProjectMeta) {
        host.overlays.confirm(tr(S.DELETE_TITLE, p.name), tr(S.DELETE_BODY), tr(S.ACTION_DELETE), tr(S.ACTION_CANCEL), true) {
            storeAction(tr(S.DELETED)) {
                host.store.delete(p.id)
                Thumbnails.invalidate(p.id)
            }
        }
    }

    private fun storeAction(done: String, action: () -> Unit) {
        host.io.execute {
            val r = runCatching(action)
            host.runOnMain {
                r.onSuccess { host.overlays.message(done) }.onFailure { host.overlays.message(tr(S.STORE_ERROR, it.message ?: "?")) }
                reload()
            }
        }
    }
}

/** FrameLayout whose height is width * ratio. */
@SuppressLint("ViewConstructor")
class AspectFrame(context: Context, private val ratio: Float) : FrameLayout(context) {
    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val w = MeasureSpec.getSize(widthMeasureSpec)
        super.onMeasure(MeasureSpec.makeMeasureSpec(w, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec((w * ratio).toInt(), MeasureSpec.EXACTLY))
    }
}
