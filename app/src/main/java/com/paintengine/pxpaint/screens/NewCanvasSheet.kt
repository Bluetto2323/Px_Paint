package com.paintengine.pxpaint.screens

import android.app.ActivityManager
import android.content.Context
import android.text.Editable
import android.text.InputType
import android.text.TextWatcher
import android.view.Gravity
import android.view.View
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import com.paintengine.pxpaint.core.CanvasSizeRules
import com.paintengine.pxpaint.core.Format
import com.paintengine.pxpaint.core.ProjectMeta
import com.paintengine.pxpaint.core.S
import com.paintengine.pxpaint.core.tr
import com.paintengine.pxpaint.ui.CheckerDrawable
import com.paintengine.pxpaint.ui.FlowRow
import com.paintengine.pxpaint.ui.Icon
import com.paintengine.pxpaint.ui.IconButton
import com.paintengine.pxpaint.ui.IconDrawable
import com.paintengine.pxpaint.ui.PxTheme
import com.paintengine.pxpaint.ui.Widgets
import com.paintengine.pxpaint.ui.column
import org.json.JSONObject

/**
 * "New canvas": name, size (presets or custom), background, and a live
 * explanation of how the canvas will be stored on this device.
 */
class NewCanvasSheet(private val host: PxHost, private val onCreated: (ProjectMeta) -> Unit) {
    private val theme: PxTheme get() = host.theme
    private val context: Context get() = host.context

    private data class Paper(val label: S, val argb: Int, val transparent: Boolean)

    private val papers = listOf(
        Paper(S.BG_WHITE, 0xFFFFFFFF.toInt(), false),
        Paper(S.BG_PAPER, 0xFFF3EBDD.toInt(), false),
        Paper(S.BG_GREY, 0xFF8A8D91.toInt(), false),
        Paper(S.BG_BLACK, 0xFF111111.toInt(), false),
        Paper(S.BG_TRANSPARENT, 0xFFFFFFFF.toInt(), true),
    )

    private val metrics = context.resources.displayMetrics
    private val presets = CanvasSizeRules.presets(metrics.widthPixels, metrics.heightPixels)
    private var preset: CanvasSizeRules.PresetKind? = CanvasSizeRules.PresetKind.SCREEN
    private var paper = papers[0]
    private var width = presets[0].width
    private var height = presets[0].height
    private var layer: View? = null
    private var creating = false

    private lateinit var presetRow: FlowRow
    private lateinit var paperRow: LinearLayout
    private lateinit var widthField: EditText
    private lateinit var heightField: EditText
    private lateinit var nameField: EditText
    private lateinit var info: TextView
    private lateinit var note: TextView
    private lateinit var createButton: TextView
    private var updatingFields = false

    private val budget: Long by lazy {
        val mi = ActivityManager.MemoryInfo()
        (context.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager).getMemoryInfo(mi)
        CanvasSizeRules.budgetForRam(mi.totalMem)
    }

    fun show() {
        val content = column(context, theme) {
            nameField = field(tr(S.UNTITLED), InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_CAP_SENTENCES).apply {
                hint = tr(S.NAME)
                contentDescription = tr(S.NAME)
            }
            addView(label(tr(S.NAME)))
            addView(nameField)

            addView(label(tr(S.SIZE)))
            presetRow = FlowRow(context, theme.dp(8))
            addView(presetRow)

            val sizeRow = LinearLayout(context).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                setPadding(0, theme.dp(12), 0, 0)
            }
            widthField = field(width.toString(), InputType.TYPE_CLASS_NUMBER).apply { contentDescription = tr(S.WIDTH) }
            heightField = field(height.toString(), InputType.TYPE_CLASS_NUMBER).apply { contentDescription = tr(S.HEIGHT) }
            sizeRow.addView(labeled(tr(S.WIDTH), widthField), LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            sizeRow.addView(IconButton(context, theme, Icon.SWAP, tr(S.SWAP_SIDES)).apply {
                setOnClickListener { setSize(height, width, keepPreset = false) }
            }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                topMargin = theme.dp(18); marginStart = theme.dp(4); marginEnd = theme.dp(4)
            })
            sizeRow.addView(labeled(tr(S.HEIGHT), heightField), LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            addView(sizeRow)

            addView(label(tr(S.BACKGROUND)))
            paperRow = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL }
            addView(paperRow)

            info = theme.text(context, "", PxTheme.Type.CAPTION, theme.textSecondary).apply { setPadding(0, theme.dp(18), 0, 0) }
            note = theme.text(context, "", PxTheme.Type.CAPTION, theme.textTertiary).apply {
                setPadding(0, theme.dp(4), 0, 0)
                accessibilityLiveRegion = View.ACCESSIBILITY_LIVE_REGION_POLITE
            }
            addView(info)
            addView(note)
            createButton = Widgets.pill(context, theme, tr(S.ACTION_CREATE), true) { create() }
            addView(host.overlays.buttonBar(listOf(
                Widgets.pill(context, theme, tr(S.ACTION_CANCEL), false) { layer?.let { host.overlays.dismiss(it) } },
                createButton,
            )))
        }
        val watcher = object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
            override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
            override fun afterTextChanged(s: Editable?) {
                if (updatingFields) return
                width = widthField.text.toString().toIntOrNull() ?: 0
                height = heightField.text.toString().toIntOrNull() ?: 0
                preset = presets.firstOrNull { it.width == width && it.height == height }?.kind
                renderPresets()
                validate()
            }
        }
        widthField.addTextChangedListener(watcher)
        heightField.addTextChangedListener(watcher)
        renderPresets()
        renderPapers()
        validate()
        layer = host.overlays.sheet(content, tr(S.NEW_CANVAS))
    }

    private fun label(text: String) = theme.text(context, text.uppercase(), PxTheme.Type.MICRO, theme.textSecondary).apply {
        setPadding(0, theme.dp(18), 0, theme.dp(8))
    }

    private fun labeled(text: String, field: View) = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        addView(theme.text(context, text, PxTheme.Type.CAPTION, theme.textSecondary).apply { setPadding(theme.dp(4), 0, 0, theme.dp(4)) })
        addView(field)
    }

    private fun field(initial: String, type: Int) = EditText(context).apply {
        setText(initial)
        isSingleLine = true
        inputType = type
        setSelectAllOnFocus(true)
        theme.style(this, PxTheme.Type.BODY)
        setHintTextColor(theme.textTertiary)
        background = theme.rounded(theme.sunken, theme.radiusS, theme.outline)
        setPadding(theme.dp(14), theme.dp(12), theme.dp(14), theme.dp(12))
        minHeight = theme.touchTarget
    }

    private fun presetName(kind: CanvasSizeRules.PresetKind) = when (kind) {
        CanvasSizeRules.PresetKind.SCREEN -> tr(S.PRESET_SCREEN)
        CanvasSizeRules.PresetKind.SQUARE -> tr(S.PRESET_SQUARE)
        CanvasSizeRules.PresetKind.A4 -> tr(S.PRESET_A4)
        CanvasSizeRules.PresetKind.A5 -> tr(S.PRESET_A5)
        CanvasSizeRules.PresetKind.FULL_HD -> tr(S.PRESET_FULL_HD)
        CanvasSizeRules.PresetKind.UHD_4K -> tr(S.PRESET_4K)
        CanvasSizeRules.PresetKind.PORTRAIT_POST -> tr(S.PRESET_POST)
    }

    private fun renderPresets() {
        presetRow.removeAllViews()
        for (p in presets) {
            presetRow.addView(Widgets.chip(context, theme, presetName(p.kind), preset == p.kind) {
                setSize(p.width, p.height, keepPreset = true)
                preset = p.kind
                renderPresets()
            }.apply { contentDescription = "${presetName(p.kind)}, ${Format.dimensions(p.width, p.height)}" })
        }
        presetRow.addView(Widgets.chip(context, theme, tr(S.PRESET_CUSTOM), preset == null) {
            preset = null
            renderPresets()
            widthField.requestFocus()
        })
    }

    private fun renderPapers() {
        paperRow.removeAllViews()
        for (p in papers) {
            val selected = p == paper
            val swatch = View(context).apply {
                background = if (p.transparent) CheckerDrawable(theme.dp(5), false) else theme.oval(p.argb, theme.outline)
                outlineProvider = object : android.view.ViewOutlineProvider() {
                    override fun getOutline(v: View, o: android.graphics.Outline) = o.setOval(0, 0, v.width, v.height)
                }
                clipToOutline = true
            }
            val ring = android.widget.FrameLayout(context).apply {
                background = theme.oval(0, if (selected) theme.accent else theme.outline)
                val inset = theme.dp(if (selected) 5 else 3)
                addView(swatch, android.widget.FrameLayout.LayoutParams(android.widget.FrameLayout.LayoutParams.MATCH_PARENT, android.widget.FrameLayout.LayoutParams.MATCH_PARENT).apply {
                    setMargins(inset, inset, inset, inset)
                })
                contentDescription = tr(p.label)
                isSelected = selected
                isClickable = true
                setOnClickListener {
                    paper = p
                    renderPapers()
                }
            }
            paperRow.addView(ring, LinearLayout.LayoutParams(theme.touchTarget, theme.touchTarget).apply { marginEnd = theme.dp(10) })
        }
        paperRow.addView(theme.text(context, tr(paper.label), PxTheme.Type.CAPTION, theme.textSecondary).apply {
            gravity = Gravity.CENTER_VERTICAL
        }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, theme.touchTarget))
    }

    private fun setSize(w: Int, h: Int, keepPreset: Boolean) {
        width = w
        height = h
        updatingFields = true
        widthField.setText(w.toString())
        heightField.setText(h.toString())
        updatingFields = false
        if (!keepPreset) {
            preset = presets.firstOrNull { it.width == w && it.height == h }?.kind
            renderPresets()
        }
        validate()
    }

    /** Engine answer when a device exists (real GPU limits), otherwise the local estimate. */
    private fun validate() {
        var ok: Boolean
        var tiled: Boolean
        var maxBytes: Long
        var fromDevice = false
        var reason: String? = null
        val engineJson = host.engineIfStarted?.checkCanvasSize(width, height)
        val parsed = engineJson?.let { runCatching { JSONObject(it) }.getOrNull() }
        val deviceReady = parsed != null && (parsed.optBoolean("ok") || !parsed.optString("reason").contains("not ready"))
        val est = CanvasSizeRules.estimate(width, height, 2048, budget)
        if (deviceReady && parsed != null && CanvasSizeRules.validateSide(width) == null && CanvasSizeRules.validateSide(height) == null) {
            ok = parsed.optBoolean("ok")
            tiled = parsed.optBoolean("tiled")
            maxBytes = parsed.optLong("maxBytes")
            fromDevice = true
            if (!ok) reason = parsed.optString("reason")
        } else {
            ok = est.ok
            tiled = est.tiled
            maxBytes = est.maxBytes
        }
        val error = when (est.error) {
            CanvasSizeRules.SizeError.TOO_SMALL -> tr(S.SIZE_TOO_SMALL)
            CanvasSizeRules.SizeError.TOO_LARGE -> tr(S.SIZE_TOO_LARGE, CanvasSizeRules.MAX_SIDE)
            CanvasSizeRules.SizeError.OVER_BUDGET -> tr(S.SIZE_OVER_BUDGET, CanvasSizeRules.largestSquareSide(2048, budget))
            null -> if (!ok) reason ?: tr(S.SIZE_OVER_BUDGET, CanvasSizeRules.largestSquareSide(2048, budget)) else null
        }
        if (est.error == CanvasSizeRules.SizeError.TOO_SMALL || est.error == CanvasSizeRules.SizeError.TOO_LARGE) ok = false
        if (ok) {
            info.setTextColor(theme.textSecondary)
            info.text = if (tiled) tr(S.STORAGE_TILED, est.tileColumns, est.tileRows, Format.bytes(maxBytes))
            else tr(S.STORAGE_SINGLE, Format.bytes(maxBytes))
            note.text = if (fromDevice) tr(S.SIZE_CHECKED_ON_DEVICE) else tr(S.SIZE_ESTIMATE)
        } else {
            info.setTextColor(theme.danger)
            info.text = error
            note.text = ""
        }
        createButton.isEnabled = ok && !creating
        createButton.alpha = if (createButton.isEnabled) 1f else 0.45f
        info.setCompoundDrawablesRelativeWithIntrinsicBounds(
            IconDrawable(if (ok) Icon.PIXELS else Icon.INFO, if (ok) theme.textSecondary else theme.danger).apply {
                setBounds(0, 0, theme.dp(16), theme.dp(16))
            }, null, null, null,
        )
        info.compoundDrawablePadding = theme.dp(8)
    }

    private fun create() {
        if (creating) return
        creating = true
        validate()
        val name = nameField.text.toString()
        val w = width
        val h = height
        val p = paper
        host.io.execute {
            val r = runCatching { host.store.create(name.ifBlank { tr(S.UNTITLED) }, w, h, p.argb, p.transparent) }
            host.runOnMain {
                creating = false
                r.onSuccess { meta ->
                    layer?.let { host.overlays.dismiss(it) }
                    onCreated(meta)
                }.onFailure {
                    validate()
                    host.overlays.message(tr(S.STORE_ERROR, it.message ?: "?"))
                }
            }
        }
    }
}
