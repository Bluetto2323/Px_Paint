# Known issues

## KI-1 — Vulkan swapchain does not always follow a SurfaceView resize (OPEN)

**Status:** open. A fix is implemented in Block 2 (see below) but it is **not
verified on the device yet**. Do not mark this resolved until a Galaxy A33
DEVICE TEST report shows section D passing.

### Evidence (Galaxy A33 5G, SM-A336B, Android 16 / API 36, Mali-G68, driver 32.1.0)

Block 1 DEVICE TEST, 2026-11-02 16:04, section D (Vulkan backend):

```
1080x1440 -> surface 1080x2400 (rebuild 22.9 ms) FAILED
 756x1680 -> surface  756x1680 (rebuild 24.4 ms) OK
 540x2400 -> surface  756x1680 (rebuild 18.8 ms) FAILED
full size restored and self-test passes
```

App timeline: Android delivered `surfaceChanged 1080x1440`,
`surfaceChanged 756x1680`, `surfaceChanged 540x2400` (all three). The
self-tests during the failing steps rendered correctly but at the **old**
size (1080x2400, then 756x1680): the swapchain was rebuilt (18-23 ms) with
the previous extent and never caught up within the 5 s window.

Everything else in the run passed (29/30): lifecycle, 10x surface recreation,
pause/resume, render modes, Vulkan <-> OpenGL ES switching with clean
shutdown, 0 engine errors.

### Analysis

1. `surfaceChanged(w,h)` -> `notifySurfaceResized` marked the swapchain dirty.
2. The rebuild used `vkGetPhysicalDeviceSurfaceCapabilitiesKHR().currentExtent`
   which, right after the callback, can still report the previous window
   buffer size (the window's buffer size is updated asynchronously).
3. The dirty flag was cleared, and `VK_SUBOPTIMAL_KHR` from present was
   ignored (it was assumed to mean "rotation without pre-rotation" only), so
   nothing triggered another rebuild once the window reached its new size.

Root cause is consistent with the alternating pass/fail pattern (a race),
but it is an inference from the report, not a driver-confirmed fact.

### Fix implemented (Block 2) — pending device verification

* The size from `surfaceChanged` is kept as the **desired extent**. While the
  swapchain extent differs from it, the device re-queries the surface
  capabilities every frame and rebuilds as soon as `currentExtent` changes
  (bounded: gives up with a logged warning after ~2 s / 120 frames).
* While such a resize is pending the device asks for frames even in
  on-demand mode (`GraphicsDevice::needsFrame()`), otherwise an idle app
  would never catch up.
* `VK_SUBOPTIMAL_KHR` now re-queries the capabilities: a size mismatch marks
  the swapchain dirty; a transform-only mismatch (rotation, pre-rotation not
  implemented) is still accepted without rebuilding in a loop.
* Section D of the DEVICE TEST is unchanged (same strict assertions:
  surface must match the view size, self-test incl. geometry must pass).

### How to verify

Run DEVICE TEST on the A33 and check section D: all three sizes must report
`-> surface <same size> ... OK`.

## KI-2 — "CPU/frame" included the vsync wait (diagnostics, fixed in Block 2)

Block 1 reported `steady CPU avg 16.63 ms` at 59.8 fps: that number included
the time spent waiting for the GPU/vsync (fence wait + acquire). Block 2
splits frame time into *wait* (beginFrame: fence + acquire) and *work*
(scene recording + submit + present call), so CPU cost is no longer
confused with the display refresh interval.

## KI-3 — Vulkan pre-rotation not implemented

The compositor rotates the image in landscape (extra composition pass).
Visual orientation after rotation must be checked by eye; the pixel
self-test reads the swapchain image, not the screen.

## KI-4 — Brush engine scope and limits (Block 3)

Block 2's rectangle "preview marker" (and its 100 000-operation cap) is gone:
the canvas is now a raster painted by the brush engine. What is deliberately
not there yet, or bounded:

* **No velocity dynamics.** (Pressure and tilt: Block 4, see KI-8.)
* **One layer, no undo/redo.** The UI entry points stay disabled.
* **Memory:** the canvas exists twice, CPU raster (source of truth) + GPU
  copy, both only where painted. The canvas budget counts both, so the
  largest square canvas a device accepts is about 1/sqrt(2) of what Block 2
  accepted: with the A33's 1.5 GiB budget, up to 14 080 x 14 080 instead of
  16 384 x 16 384 (the New Canvas sheet explains the limit).
* **Stroke working memory:** while a stroke is drawn, every tile it touches
  holds a copy of its previous pixels plus two 16-bit masks (512 KiB per
  tile). A very long stroke with a huge brush on a huge canvas can touch
  hundreds of tiles (~50 MiB per 100 tiles). Up to 48 tiles are kept pooled
  between strokes.
* **Blending is in sRGB space** with 8-bit premultiplied storage, like most
  painting apps. Very transparent paint loses colour precision
  (un-premultiplying at alpha a can be off by up to 255 / (2a)).
* **CPU rasterisation:** dabs are rasterised and composited on the CPU
  (render thread, optimised code) and only the changed rectangles are
  uploaded. Measured on desktop CPUs (`pe_brush_test` B15) a 300 px brush
  costs about 1 ms per frame of fast drawing; on the A33 it must be measured
  (performance overlay, `docs/DEVICE_VALIDATION.md`).
* **Block 2 projects** are imported (rectangles rasterised) and saved in the
  new tile format at the first save; their old `content.pxops` file is kept
  but no longer read.

## KI-5 — PxPaint UI has no automated on-device tests

There is no Android device or emulator in CI. What is automated:
JVM unit tests of the pure-Kotlin core (settings, brush settings, canvas
size rules, tile storage format, project store, search/sort, adaptive layout
decisions, gesture/stroke-gate math, formatting), the CPU brush engine suite
(`pe_brush_test`) and the engine host suites (`canvas`, `brush`, `perf` on
Mesa GLES and lavapipe Vulkan). Those GPUs are **software** renderers: host
timings say nothing about the Mali-G68. Screen
rendering, touch handling, insets and the real Mali-G68 driver are verified
only by the manual A33 procedure in `docs/DEVICE_VALIDATION.md`.

## KI-6 — Diagnostics run a second GPU device while open

`DiagnosticsActivity` runs in its own process (`:diagnostics`) because the
native Vulkan loader state is process-global. While it is open, PxPaint's
engine (if a canvas was opened) still holds its own device in the main
process. Both are released normally; this only matters for memory use while
diagnostics are on screen.

## KI-7 — Block 3 latency/performance on the device is unmeasured

The engine now reports input-to-present latency and brush CPU time
(performance overlay, `inputLatency*` / `brush*Ms` status fields). Host runs
on software GPUs only show the pipeline works and nothing is lost; the real
numbers must come from the A33 (Block 3 checklist).

## KI-8 — Stylus pressure / tilt are not tested on real hardware (Block 4)

The primary test device (Galaxy A33 5G) has **no active stylus support**:
it reports no stylus input device, and its touchscreen "pressure" is a
finger contact estimate, which PxPaint deliberately ignores. Therefore:

* pressure, tilt, orientation, hover distance, the eraser end and palm
  rejection against a real stylus are verified **only by automated tests**
  (synthetic samples through the brush engine and the GPU on GLES and
  Vulkan, and the TouchRouter unit tests) - never on a physical stylus;
* the axis reading follows the Android API (`InputDevice.getMotionRange`,
  `MotionEvent.getHistorical*`) but its behaviour with specific styluses
  (S Pen, USI pens, Bluetooth pens) is unverified;
* pressure smoothing and the default curves are tuned on synthetic 240 Hz
  data; they may need adjusting on a real pen;
* tilt shaping assumes Android's orientation convention (0 = stylus
  pointing to the top of the screen, clockwise positive). A device that
  reports it differently would rotate the ellipse.
