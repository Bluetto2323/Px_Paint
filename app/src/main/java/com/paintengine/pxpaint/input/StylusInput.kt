package com.paintengine.pxpaint.input

import android.content.Context
import android.hardware.input.InputManager
import android.os.Handler
import android.os.Looper
import android.view.InputDevice
import android.view.MotionEvent
import com.paintengine.pxpaint.core.AxisSupport
import com.paintengine.pxpaint.core.PressureNormalizer
import com.paintengine.pxpaint.core.SampleBuffer
import com.paintengine.pxpaint.core.ToolKind

/**
 * Reads stylus axes from MotionEvents using only what the device reports
 * (InputDevice.getMotionRange). Pressure / tilt / orientation are read for
 * an active stylus (TOOL_TYPE_STYLUS / ERASER) only: a finger's "pressure" is
 * a contact-area estimate and is never used as stylus pressure. Missing axes
 * are NaN all the way to the engine - nothing is simulated.
 */
object StylusInput {
    private val cache = HashMap<Long, AxisSupport>()

    fun kind(e: MotionEvent, pointerIndex: Int): ToolKind = ToolKind.fromToolType(e.getToolType(pointerIndex))

    /** Axes the event's device reports for its source (cached per device + source). */
    fun axes(e: MotionEvent): AxisSupport {
        val device = e.device ?: return AxisSupport.NONE
        val key = (device.id.toLong() shl 32) or (e.source.toLong() and 0xFFFFFFFFL)
        return cache.getOrPut(key) { axesOf(device, e.source) }
    }

    fun axesOf(device: InputDevice, source: Int): AxisSupport {
        val p = device.getMotionRange(MotionEvent.AXIS_PRESSURE, source)
        return AxisSupport(
            pressure = p != null && p.max > p.min,
            tilt = device.getMotionRange(MotionEvent.AXIS_TILT, source) != null,
            orientation = device.getMotionRange(MotionEvent.AXIS_ORIENTATION, source) != null,
            distance = device.getMotionRange(MotionEvent.AXIS_DISTANCE, source) != null,
            pressureMin = p?.min ?: 0f,
            pressureMax = p?.max ?: 1f,
        )
    }

    fun invalidate() = cache.clear()

    /**
     * Appends every sample of pointer [pointerId] in [e] to [out]: historical
     * samples first (oldest first), then the current one. Times relative to
     * [t0] (ms). Returns false if the pointer is not in the event.
     */
    fun append(e: MotionEvent, pointerId: Int, kind: ToolKind, t0: Long, out: SampleBuffer, includeHistory: Boolean = true): Boolean {
        val i = e.findPointerIndex(pointerId)
        if (i < 0) return false
        val a = if (kind.isStylus) axes(e) else AxisSupport.NONE
        if (includeHistory) {
            for (h in 0 until e.historySize) {
                out.add(
                    e.getHistoricalX(i, h), e.getHistoricalY(i, h), (e.getHistoricalEventTime(h) - t0).toFloat(),
                    if (a.pressure) PressureNormalizer.normalize(e.getHistoricalPressure(i, h), a.pressureMin, a.pressureMax) else Float.NaN,
                    if (a.tilt) e.getHistoricalAxisValue(MotionEvent.AXIS_TILT, i, h) else Float.NaN,
                    if (a.orientation) e.getHistoricalAxisValue(MotionEvent.AXIS_ORIENTATION, i, h) else Float.NaN,
                )
            }
        }
        out.add(
            e.getX(i), e.getY(i), (e.eventTime - t0).toFloat(),
            if (a.pressure) PressureNormalizer.normalize(e.getPressure(i), a.pressureMin, a.pressureMax) else Float.NaN,
            if (a.tilt) e.getAxisValue(MotionEvent.AXIS_TILT, i) else Float.NaN,
            if (a.orientation) e.getAxisValue(MotionEvent.AXIS_ORIENTATION, i) else Float.NaN,
        )
        return true
    }
}

/**
 * What the app knows about styluses: input devices that declare a stylus
 * source (from the system), and the last stylus actually seen in use (from
 * events, incl. hover). Main thread.
 */
class StylusMonitor(context: Context) : InputManager.InputDeviceListener {
    data class DeviceInfo(val name: String, val axes: AxisSupport)

    data class Reading(
        val kind: ToolKind,
        val hovering: Boolean,
        val rawPressure: Float,
        val pressure: Float,
        val tilt: Float,
        val orientation: Float,
        val distance: Float,
        val timeMs: Long,
    )

    private val im = context.getSystemService(Context.INPUT_SERVICE) as InputManager
    var devices: List<DeviceInfo> = emptyList()
        private set
    /** Axes of the last stylus seen drawing or hovering (null: none yet this session). */
    var seen: AxisSupport? = null
        private set
    var lastReading: Reading? = null
        private set
    private val listeners = ArrayList<() -> Unit>()

    init {
        scan()
        im.registerInputDeviceListener(this, Handler(Looper.getMainLooper()))
    }

    fun close() = im.unregisterInputDeviceListener(this)

    fun addListener(l: () -> Unit) { listeners += l }
    fun removeListener(l: () -> Unit) { listeners -= l }

    /** Pressure is available from a stylus the system declared or one seen in use. */
    val pressureAvailable: Boolean get() = devices.any { it.axes.pressure } || seen?.pressure == true
    val tiltAvailable: Boolean get() = devices.any { it.axes.tilt && it.axes.orientation } || (seen?.let { it.tilt && it.orientation } == true)
    val stylusKnown: Boolean get() = devices.isNotEmpty() || seen != null

    /** Feed every canvas / test-pad event (touch and hover). */
    fun onEvent(e: MotionEvent) {
        val i = e.actionIndex.coerceIn(0, e.pointerCount - 1)
        val kind = StylusInput.kind(e, i)
        if (!kind.isStylus) return
        val a = StylusInput.axes(e)
        val firstTime = seen == null
        seen = a
        val hover = e.actionMasked == MotionEvent.ACTION_HOVER_MOVE || e.actionMasked == MotionEvent.ACTION_HOVER_ENTER
        val raw = if (a.pressure) e.getPressure(i) else Float.NaN
        lastReading = Reading(
            kind, hover, raw, PressureNormalizer.normalize(raw, a.pressureMin, a.pressureMax),
            if (a.tilt) e.getAxisValue(MotionEvent.AXIS_TILT, i) else Float.NaN,
            if (a.orientation) e.getAxisValue(MotionEvent.AXIS_ORIENTATION, i) else Float.NaN,
            if (a.distance) e.getAxisValue(MotionEvent.AXIS_DISTANCE, i) else Float.NaN,
            e.eventTime,
        )
        if (firstTime) scan()
        listeners.toList().forEach { it() }
    }

    private fun scan() {
        devices = InputDevice.getDeviceIds().toList().mapNotNull { id ->
            val d = InputDevice.getDevice(id) ?: return@mapNotNull null
            val stylusSource = InputDevice.SOURCE_STYLUS
            if (!d.supportsSource(stylusSource) || d.isVirtual) null
            else DeviceInfo(d.name, StylusInput.axesOf(d, stylusSource))
        }
        StylusInput.invalidate()
    }

    override fun onInputDeviceAdded(deviceId: Int) = changed()
    override fun onInputDeviceRemoved(deviceId: Int) = changed()
    override fun onInputDeviceChanged(deviceId: Int) = changed()

    private fun changed() {
        scan()
        listeners.toList().forEach { it() }
    }
}
