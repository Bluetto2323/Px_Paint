package com.paintengine.pxpaint.core

import java.util.Locale
import kotlin.math.roundToInt

/** Library sections. [CUSTOM] is the default for new user brushes. */
enum class BrushCategory(val key: String, val label: S) {
    SKETCHING("sketching", S.CAT_SKETCHING),
    INKING("inking", S.CAT_INKING),
    PAINTING("painting", S.CAT_PAINTING),
    AIRBRUSH("airbrush", S.CAT_AIRBRUSH),
    DRY("dry", S.CAT_DRY),
    CALLIGRAPHY("calligraphy", S.CAT_CALLIGRAPHY),
    WATER("water", S.CAT_WATER),
    CUSTOM("custom", S.CAT_CUSTOM);

    companion object {
        fun of(key: String?): BrushCategory = entries.firstOrNull { it.key == key } ?: CUSTOM
    }
}

/** Dab shape. [code] is the engine value (BrushTip in BrushTypes.h). */
enum class BrushTip(val key: String, val code: Int, val label: S) {
    ROUND("round", 0, S.TIP_ROUND), CHALK("chalk", 1, S.TIP_CHALK), CHARCOAL("charcoal", 2, S.TIP_CHARCOAL),
    BRISTLE("bristle", 3, S.TIP_BRISTLE);

    companion object {
        fun of(key: String?): BrushTip = entries.firstOrNull { it.key == key } ?: ROUND
    }
}

/** Canvas grain (paper texture). [code] = engine BrushGrain. */
enum class BrushGrain(val key: String, val code: Int, val label: S) {
    NONE("none", 0, S.GRAIN_NONE), PAPER("paper", 1, S.GRAIN_PAPER), ROUGH("rough", 2, S.GRAIN_ROUGH),
    CANVAS("canvas", 3, S.GRAIN_CANVAS), WASH("wash", 4, S.GRAIN_WASH);

    companion object {
        fun of(key: String?): BrushGrain = entries.firstOrNull { it.key == key } ?: NONE
    }
}

/** How the dabs of one stroke combine. [code] = engine Accumulation. */
enum class Accumulation(val key: String, val code: Int, val label: S) {
    STROKE("stroke", 0, S.ACCUM_STROKE), BUILDUP("buildup", 1, S.ACCUM_BUILDUP);

    companion object {
        fun of(key: String?): Accumulation = entries.firstOrNull { it.key == key } ?: STROKE
    }
}

enum class AngleMode(val key: String, val label: S) {
    FIXED("fixed", S.ANGLE_FIXED), STROKE("stroke", S.ANGLE_STROKE);

    companion object {
        fun of(key: String?): AngleMode = entries.firstOrNull { it.key == key } ?: FIXED
    }
}

/**
 * Every parameter of a brush. Defaults = the plain round brush of Block 3.
 * [size] is the diameter in canvas pixels (the maximum when pressure drives
 * size); [angle] in degrees, screen space when [angleMode] is FIXED, relative
 * to the stroke direction otherwise. Ranges: [RANGES]; out-of-range or
 * non-finite values are clamped / defaulted by [sanitized].
 */
data class BrushParams(
    val tip: BrushTip = BrushTip.ROUND,
    val size: Float = 24f,
    val opacity: Float = 1f,
    val flow: Float = 1f,
    val spacing: Float = 0.08f,
    val hardness: Float = 1f,
    val roundness: Float = 1f,
    val angle: Float = 0f,
    val angleMode: AngleMode = AngleMode.FIXED,
    val sizeJitter: Float = 0f,
    val scatter: Float = 0f,
    val angleJitter: Float = 0f,
    val flowJitter: Float = 0f,
    val grain: BrushGrain = BrushGrain.NONE,
    val grainDepth: Float = 0f,
    val grainPressure: Float = 0f,
    val accumulation: Accumulation = Accumulation.STROKE,
    val wetEdge: Float = 0f,
    val pressureSize: Boolean = false,
    val minSize: Float = 0.25f,
    val pressureOpacity: Boolean = false,
    val minOpacity: Float = 0.3f,
    val tilt: Boolean = false,
    val tiltStrength: Float = 1f,
    /** Block 14: share of the colour picked up from the canvas in the paint (0 = plain paint). */
    val mix: Float = 0f,
    /** Block 14: how fast the brush takes up canvas colour (mix) / refreshes what it drags (smudge). */
    val pickup: Float = 0.5f,
    /** Block 15: paint loaded on the brush (1 = never runs out). */
    val charge: Float = 1f,
    /** Block 15: paint thinned with water (0 = full body). */
    val dilution: Float = 0f,
    /** Block 15: the stroke starts thin and grows to full size (0 = off). */
    val taper: Float = 0f,
    /** Block 15: each stroke's colour shifts in hue / lightness by up to this much (0 = off). */
    val hueJitter: Float = 0f,
    val lightJitter: Float = 0f,
) {
    /** Valid range and default of a numeric parameter. */
    data class Range(val key: String, val min: Float, val max: Float, val default: Float)

    fun sanitized(): BrushParams {
        fun f(v: Float, r: Range) = if (v.isFinite()) v.coerceIn(r.min, r.max) else r.default
        return copy(
            size = f(size, R_SIZE), opacity = f(opacity, R_OPACITY), flow = f(flow, R_FLOW), spacing = f(spacing, R_SPACING),
            hardness = f(hardness, R_HARDNESS), roundness = f(roundness, R_ROUNDNESS), angle = if (angle.isFinite()) normalizeAngle(angle) else 0f,
            sizeJitter = f(sizeJitter, R_SIZE_JITTER), scatter = f(scatter, R_SCATTER), angleJitter = f(angleJitter, R_ANGLE_JITTER),
            flowJitter = f(flowJitter, R_FLOW_JITTER), grainDepth = f(grainDepth, R_GRAIN_DEPTH),
            grainPressure = f(grainPressure, R_GRAIN_PRESSURE), wetEdge = f(wetEdge, R_WET_EDGE),
            minSize = f(minSize, R_MIN_SIZE), minOpacity = f(minOpacity, R_MIN_OPACITY), tiltStrength = f(tiltStrength, R_TILT_STRENGTH),
            mix = f(mix, R_MIX), pickup = f(pickup, R_PICKUP), charge = f(charge, R_CHARGE), dilution = f(dilution, R_DILUTION),
            taper = f(taper, R_TAPER), hueJitter = f(hueJitter, R_HUE_JITTER), lightJitter = f(lightJitter, R_LIGHT_JITTER),
        )
    }

    /** Problems of the raw values (empty = valid as is). */
    fun problems(): List<String> {
        val out = ArrayList<String>()
        for ((r, v) in numeric()) if (!v.isFinite() || v < r.min || v > r.max) out += "${r.key}=$v outside ${r.min}..${r.max}"
        return out
    }

    private fun numeric(): List<Pair<Range, Float>> = listOf(
        R_SIZE to size, R_OPACITY to opacity, R_FLOW to flow, R_SPACING to spacing, R_HARDNESS to hardness,
        R_ROUNDNESS to roundness, R_ANGLE to angle, R_SIZE_JITTER to sizeJitter, R_SCATTER to scatter,
        R_ANGLE_JITTER to angleJitter, R_FLOW_JITTER to flowJitter, R_GRAIN_DEPTH to grainDepth,
        R_GRAIN_PRESSURE to grainPressure, R_WET_EDGE to wetEdge, R_MIN_SIZE to minSize, R_MIN_OPACITY to minOpacity,
        R_TILT_STRENGTH to tiltStrength, R_MIX to mix, R_PICKUP to pickup, R_CHARGE to charge, R_DILUTION to dilution,
        R_TAPER to taper, R_HUE_JITTER to hueJitter, R_LIGHT_JITTER to lightJitter,
    )

    /** key = value lines (all parameters, stable order). */
    fun entries(): List<Pair<String, String>> = listOf(
        "tip" to tip.key, "size" to num(size), "opacity" to num(opacity), "flow" to num(flow), "spacing" to num(spacing),
        "hardness" to num(hardness), "roundness" to num(roundness), "angle" to num(angle), "angle_mode" to angleMode.key,
        "size_jitter" to num(sizeJitter), "scatter" to num(scatter), "angle_jitter" to num(angleJitter),
        "flow_jitter" to num(flowJitter), "grain" to grain.key, "grain_depth" to num(grainDepth),
        "grain_pressure" to num(grainPressure), "accumulation" to accumulation.key, "wet_edge" to num(wetEdge),
        "pressure_size" to pressureSize.toString(), "min_size" to num(minSize), "pressure_opacity" to pressureOpacity.toString(),
        "min_opacity" to num(minOpacity), "tilt" to tilt.toString(), "tilt_strength" to num(tiltStrength),
        "mix" to num(mix), "pickup" to num(pickup), "charge" to num(charge), "dilution" to num(dilution),
        "taper" to num(taper), "hue_jitter" to num(hueJitter), "light_jitter" to num(lightJitter),
    )

    /** Uses the Block 3/4 rasteriser in the engine (BrushSettings::classicRound). */
    val classic: Boolean
        get() = tip == BrushTip.ROUND && hardness >= 1f && flow >= 1f && roundness >= 1f && sizeJitter == 0f &&
            scatter == 0f && angleJitter == 0f && flowJitter == 0f && (grain == BrushGrain.NONE || grainDepth == 0f) &&
            accumulation == Accumulation.STROKE && wetEdge == 0f

    companion object {
        val R_SIZE = Range("size", BrushState.MIN_SIZE, BrushState.MAX_SIZE, 24f)
        val R_OPACITY = Range("opacity", 0.01f, 1f, 1f)
        val R_FLOW = Range("flow", 0.01f, 1f, 1f)
        val R_SPACING = Range("spacing", 0.01f, 2f, 0.08f)
        val R_HARDNESS = Range("hardness", 0f, 1f, 1f)
        val R_ROUNDNESS = Range("roundness", 0.05f, 1f, 1f)
        val R_ANGLE = Range("angle", -360f, 360f, 0f)
        val R_SIZE_JITTER = Range("size_jitter", 0f, 1f, 0f)
        val R_SCATTER = Range("scatter", 0f, 2f, 0f)
        val R_ANGLE_JITTER = Range("angle_jitter", 0f, 1f, 0f)
        val R_FLOW_JITTER = Range("flow_jitter", 0f, 1f, 0f)
        val R_GRAIN_DEPTH = Range("grain_depth", 0f, 1f, 0f)
        val R_GRAIN_PRESSURE = Range("grain_pressure", 0f, 1f, 0f)
        val R_WET_EDGE = Range("wet_edge", 0f, 1f, 0f)
        val R_MIN_SIZE = Range("min_size", BrushState.MIN_PRESSURE_FRACTION, 1f, 0.25f)
        val R_MIN_OPACITY = Range("min_opacity", BrushState.MIN_PRESSURE_FRACTION, 1f, 0.3f)
        val R_TILT_STRENGTH = Range("tilt_strength", 0f, 1f, 1f)
        val R_MIX = Range("mix", 0f, 1f, 0f)
        val R_PICKUP = Range("pickup", 0f, 1f, 0.5f)
        val R_CHARGE = Range("charge", 0f, 1f, 1f)
        val R_DILUTION = Range("dilution", 0f, 1f, 0f)
        val R_TAPER = Range("taper", 0f, 1f, 0f)
        val R_HUE_JITTER = Range("hue_jitter", 0f, 1f, 0f)
        val R_LIGHT_JITTER = Range("light_jitter", 0f, 1f, 0f)

        /** Angle in (-180, 180]. */
        fun normalizeAngle(a: Float): Float {
            var v = a % 360f
            if (v > 180f) v -= 360f
            if (v <= -180f) v += 360f
            return v
        }

        private fun num(v: Float): String {
            val r = (v * 10000f).roundToInt() / 10000f
            return if (r == r.roundToInt().toFloat()) r.roundToInt().toString() else String.format(Locale.US, "%.4f", r).trimEnd('0')
        }

        /** Parameters from key = value entries over [base] (unknown keys ignored, bad values keep the base). */
        fun fromEntries(map: Map<String, String>, base: BrushParams = BrushParams()): BrushParams {
            fun fl(k: String, d: Float) = map[k]?.trim()?.toFloatOrNull()?.takeIf { it.isFinite() } ?: d
            fun bo(k: String, d: Boolean) = when (map[k]?.trim()) { "true" -> true; "false" -> false; else -> d }
            return BrushParams(
                tip = map["tip"]?.let { BrushTip.of(it.trim()) } ?: base.tip,
                size = fl("size", base.size), opacity = fl("opacity", base.opacity), flow = fl("flow", base.flow),
                spacing = fl("spacing", base.spacing), hardness = fl("hardness", base.hardness),
                roundness = fl("roundness", base.roundness), angle = fl("angle", base.angle),
                angleMode = map["angle_mode"]?.let { AngleMode.of(it.trim()) } ?: base.angleMode,
                sizeJitter = fl("size_jitter", base.sizeJitter), scatter = fl("scatter", base.scatter),
                angleJitter = fl("angle_jitter", base.angleJitter), flowJitter = fl("flow_jitter", base.flowJitter),
                grain = map["grain"]?.let { BrushGrain.of(it.trim()) } ?: base.grain,
                grainDepth = fl("grain_depth", base.grainDepth), grainPressure = fl("grain_pressure", base.grainPressure),
                accumulation = map["accumulation"]?.let { Accumulation.of(it.trim()) } ?: base.accumulation,
                wetEdge = fl("wet_edge", base.wetEdge),
                pressureSize = bo("pressure_size", base.pressureSize), minSize = fl("min_size", base.minSize),
                pressureOpacity = bo("pressure_opacity", base.pressureOpacity), minOpacity = fl("min_opacity", base.minOpacity),
                tilt = bo("tilt", base.tilt), tiltStrength = fl("tilt_strength", base.tiltStrength),
                mix = fl("mix", base.mix), pickup = fl("pickup", base.pickup),
                charge = fl("charge", base.charge), dilution = fl("dilution", base.dilution), taper = fl("taper", base.taper),
                hueJitter = fl("hue_jitter", base.hueJitter), lightJitter = fl("light_jitter", base.lightJitter),
            ).sanitized()
        }
    }
}

/**
 * A named brush. Built-in presets ("builtin.*") come from
 * assets/brushes/builtin.pxb and cannot be renamed or deleted (their
 * parameters can be customised and reset); user presets ("user.*") can.
 * [names]: localised names by language ("it"), [name] is the default.
 */
data class BrushPreset(
    val id: String,
    val name: String,
    val category: BrushCategory,
    val params: BrushParams,
    val names: Map<String, String> = emptyMap(),
    val tags: List<String> = emptyList(),
    val baseId: String? = null,
) {
    val builtIn: Boolean get() = id.startsWith(BUILTIN_PREFIX)
    fun displayName(italian: Boolean = S.italian): String = (if (italian) names["it"] else null) ?: name

    companion object {
        const val BUILTIN_PREFIX = "builtin."
        const val USER_PREFIX = "user."
        const val MAX_NAME = 40

        /** Trimmed, single-spaced, at most [MAX_NAME] characters; null if empty. */
        fun cleanName(raw: String): String? {
            val n = raw.replace(Regex("[\\p{Cntrl}]"), " ").trim().replace(Regex("\\s+"), " ")
            return if (n.isEmpty()) null else n.take(MAX_NAME).trim()
        }
    }
}

/**
 * Text format shared by the built-in preset asset and the user library:
 *
 *     pxbrushes 1
 *     [preset builtin.round]
 *     name = Basic Round
 *     name.it = Tondo base
 *     category = painting
 *     size = 24
 *     ...
 *
 * '#' starts a comment line. Unknown keys and sections are ignored (newer
 * files open in older versions), missing keys take their defaults, values
 * are sanitised. Names are written with \n and \\ escaped.
 */
object BrushPresetFormat {
    const val HEADER = "pxbrushes 1"

    data class Section(val kind: String, val id: String, val values: Map<String, String>)

    fun sections(text: String): Pair<String?, List<Section>> {
        var header: String? = null
        val out = ArrayList<Section>()
        var kind: String? = null
        var id = ""
        var values = LinkedHashMap<String, String>()
        fun flush() {
            kind?.let { out += Section(it, id, values) }
        }
        for (raw in text.lineSequence()) {
            val line = raw.trim()
            if (line.isEmpty() || line.startsWith("#")) continue
            if (header == null && kind == null && !line.startsWith("[")) {
                header = line
                continue
            }
            if (line.startsWith("[") && line.endsWith("]")) {
                flush()
                val inner = line.substring(1, line.length - 1).trim()
                val sp = inner.indexOf(' ')
                kind = if (sp < 0) inner else inner.substring(0, sp)
                id = if (sp < 0) "" else inner.substring(sp + 1).trim()
                values = LinkedHashMap()
                continue
            }
            val eq = line.indexOf('=')
            if (eq <= 0 || kind == null) continue
            values[line.substring(0, eq).trim()] = unescape(line.substring(eq + 1).trim())
        }
        flush()
        return header to out
    }

    fun presetFrom(sec: Section, base: BrushParams = BrushParams()): BrushPreset? {
        if (sec.id.isEmpty() || !validId(sec.id)) return null
        val v = sec.values
        val names = v.filterKeys { it.startsWith("name.") }.mapKeys { it.key.removePrefix("name.") }
            .mapNotNull { (k, n) -> BrushPreset.cleanName(n)?.let { k to it } }.toMap()
        return BrushPreset(
            id = sec.id,
            name = BrushPreset.cleanName(v["name"] ?: "") ?: sec.id.substringAfter('.'),
            category = BrushCategory.of(v["category"]),
            params = BrushParams.fromEntries(v, base),
            names = names,
            tags = v["tags"]?.split(' ', ',')?.map { it.trim().lowercase(Locale.ROOT) }?.filter { it.isNotEmpty() } ?: emptyList(),
            baseId = v["base"]?.takeIf { validId(it) },
        )
    }

    /** Built-in preset file: presets in file order (later duplicates ignored). */
    fun parsePresets(text: String): List<BrushPreset> {
        val (header, secs) = sections(text)
        if (header != HEADER) return emptyList()
        val seen = HashSet<String>()
        return secs.filter { it.kind == "preset" }.mapNotNull { presetFrom(it) }.filter { seen.add(it.id) }
    }

    fun writePreset(sb: StringBuilder, p: BrushPreset, kind: String = "preset") {
        sb.append('[').append(kind).append(' ').append(p.id).append("]\n")
        sb.append("name = ").append(escape(p.name)).append('\n')
        for ((k, n) in p.names.toSortedMap()) sb.append("name.").append(k).append(" = ").append(escape(n)).append('\n')
        sb.append("category = ").append(p.category.key).append('\n')
        if (p.tags.isNotEmpty()) sb.append("tags = ").append(p.tags.joinToString(" ")).append('\n')
        p.baseId?.let { sb.append("base = ").append(it).append('\n') }
        writeParams(sb, p.params)
    }

    fun writeParams(sb: StringBuilder, p: BrushParams) {
        for ((k, v) in p.entries()) sb.append(k).append(" = ").append(v).append('\n')
    }

    fun validId(id: String) = id.length in 3..80 && id.all { it.isLetterOrDigit() || it == '.' || it == '_' || it == '-' }

    fun escape(s: String) = s.replace("\\", "\\\\").replace("\n", "\\n")
    fun unescape(s: String): String {
        val sb = StringBuilder()
        var i = 0
        while (i < s.length) {
            val c = s[i]
            if (c == '\\' && i + 1 < s.length) {
                sb.append(if (s[i + 1] == 'n') '\n' else s[i + 1])
                i += 2
            } else {
                sb.append(c)
                i++
            }
        }
        return sb.toString()
    }
}

/**
 * Engine encoding of a brush (NativeEngine.Brush params / flags, decoded by
 * JniBridge decodeBrush): 26 floats.
 */
object BrushEncoding {
    const val PARAMS = 26
    const val FLAG_ERASE = 1
    const val FLAG_PRESSURE_SIZE = 2
    const val FLAG_PRESSURE_OPACITY = 4
    const val FLAG_TILT = 8
    const val FLAG_ANGLE_FOLLOWS_STROKE = 16
    /** Block 14: the smudge tool (drags the pixels; opacity = strength). */
    const val FLAG_SMUDGE = 32

    fun params(p: BrushParams, size: Float = p.size, opacity: Float = p.opacity, pressureSmoothing: Float): FloatArray {
        val s = p.sanitized()
        return floatArrayOf(
            size, opacity, s.spacing, s.minSize, s.minOpacity, s.tiltStrength, pressureSmoothing,
            s.hardness, s.flow, s.roundness, s.angle, s.sizeJitter, s.scatter, s.angleJitter, s.flowJitter,
            s.grainDepth, s.grainPressure, s.wetEdge, s.tip.code.toFloat(), s.grain.code.toFloat(), s.accumulation.code.toFloat(),
            s.mix, s.pickup, s.charge, s.dilution, s.taper,
        )
    }

    fun flags(p: BrushParams, erase: Boolean, smudge: Boolean = false): Int =
        (if (smudge && !erase) FLAG_SMUDGE else 0) or (if (erase) FLAG_ERASE else 0) or (if (p.pressureSize) FLAG_PRESSURE_SIZE else 0) or
            (if (p.pressureOpacity) FLAG_PRESSURE_OPACITY else 0) or (if (p.tilt) FLAG_TILT else 0) or
            (if (p.angleMode == AngleMode.STROKE) FLAG_ANGLE_FOLLOWS_STROKE else 0)
}
