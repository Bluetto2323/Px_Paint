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

## KI-9 — Brush library: verified by automated tests and an emulator only (Block 5)

* **Not tested by hand on a phone yet.** The 12 presets, the library, the editor
  and the previews are covered by:
  * CPU pixel tests;
  * GPU tests on Mesa GLES and lavapipe Vulkan;
  * JVM unit tests;
  * screenshots from an Android 14 **emulator** (SwiftShader, finger only).

  Look and speed on the Galaxy A33 are still to be checked (DEVICE_VALIDATION
  "Blocco 5").
* **Pressure, tilt and "pressure fills the grain"** of the presets are tested only
  with synthetic samples (KI-8).
* **Fine Liner and Hard Brush give the same dab at equal size without pressure.**
  Both use the classic round dab. They differ by default size, spacing and by
  pressure (Fine Liner keeps a constant width). The other 64 of the 66 preset
  pairs differ even with a finger at equal size (host test L4).
* **The watercolour look is not a fluid simulation.** It comes from a wet-edge
  transfer curve, a soft tip and a granulation texture; there is no flow,
  diffusion or mixing.
* **Grain textures repeat every 512 canvas pixels.** The repeat is hard to see on
  fine grains and can show on large flat areas of the coarse grains.
* **Textured stamps cost more CPU than round dabs**, with the bristle tip the
  heaviest at about 1.4 ms per 8-sample frame at 48 px on the host. Very large
  stamped brushes (300 px and more) can exceed a 16 ms frame on slow phones;
  this is unmeasured on the A33.
* **Size and opacity are remembered per brush and per tool.** Moving the quick
  sliders does not mark a built-in brush as modified; changing any other
  parameter in the editor does, and "Reset to original" undoes both.
* **Migration.** Block 3/4 users land on Hard Brush with their size, opacity,
  spacing and stylus settings. The eraser shares that customisation and keeps
  its own size and opacity.

## KI-10 — Settings rows with a chevron showed no text (fixed in Block 5)

In `Widgets.row`, the trailing chevron was a plain `View` added with
`WRAP_CONTENT`. Android measures such a view at the full available width, so the
row's title and description collapsed to zero width. The affected rows were
*Engine diagnostics* (since Block 2), *Pressure curve and test* (Block 4) and
*Edit brush* (Block 5): they kept their height but showed no text.

A layout dump from the emulator screenshot job found it. `Widgets.row` now keeps
the size the caller gives the trailing view. If *Engine diagnostics* could not
be found on the A33, this was why; the REPORT for KI-1 can now be opened from
*Settings → Engine diagnostics*.

## KI-11 — Layers: limits of the Block 6 implementation

* **Not tested by hand on a phone yet.** Verified by:
  * CPU tests: blend modes against an independent reference, operations,
    persistence, migration;
  * GPU tests on Mesa GLES and lavapipe Vulkan;
  * JVM tests;
  * emulator screenshots.
* **Compositing is on the CPU.** This is deliberate: identical on Vulkan and
  GLES, no per-layer GPU memory, no readbacks. The cost is CPU time:
  * Strokes stay cheap (measured < 1.5 ms per frame at 50 layers on the host).
  * A property change (opacity, visibility, blend, order) recomposites every
    tile where that layer has content, for all layers at those tiles. On a
    4096 × 4096 canvas with 50 overlapping layers that work totals about 1 s on
    the host CI machine. It is spread over frames (3–8 ms each), so the app
    stays responsive but the display catches up visibly.
  * A phone CPU is slower than the host: unmeasured on the A33.
* **Merge down and Flatten are isolated.** With a non-Normal mode the upper
  layer blends only with the layer below where that layer is transparent, and a
  flattened stack no longer blends with the paper. The picture is exact for
  Normal layers and wherever the layers below are opaque, as in other painting
  applications.
* **Flatten deletes hidden layers.** The confirmation says how many.
* **Reordering uses Move up / Move down** (buttons and menu); there is no
  drag-and-drop yet.
* ~~No undo~~: added in Block 7 (KI-12).
* **Memory.** Each layer stores only the tiles it uses (256 KiB per tile).
  Duplicate and merge are refused beyond the canvas memory budget (¼ of RAM).
  Many full layers on very large canvases can still use a lot of RAM:
  50 overlapping layers on 4096² measured about 1.1 GiB.
* **Blending is done on sRGB values**, as in common painting apps, not in linear
  light.

## KI-12 — Undo / redo: limits of the Block 7 implementation

* **Not tested by hand on a phone yet.** It is verified by:
  * the CPU history suite (exact restoration, memory, saves, interruptions);
  * GPU tests on Mesa GLES and lavapipe Vulkan (S13: the frame after undo /
    redo equals the earlier frames bit for bit);
  * JVM tests;
  * emulator screenshots.

  Undo latency on the A33 is unmeasured.
* **The history lasts for the session.** It is lost when the canvas is
  closed, the app is killed, or the project is reloaded. The saved project
  is always the current picture. Persistence across restarts was examined and
  left out (see `docs/HISTORY.md`).
* **Recording a stroke costs time when the stroke ends**, on the render
  thread (the hashes and XOR of the touched tiles). On the host it averages
  0.6 ms, and a 500 px stroke across a 4096² canvas takes about 11 ms (one
  frame). It costs nothing while drawing. The phone figure is unmeasured.
* **Old steps are dropped beyond the budget** (1/32 of RAM, 32–192 MiB,
  100 steps). Big steps hold a lot of memory:
  * a deleted, merged or flattened layer keeps its tiles in the history;
  * a flatten of 50 full layers on 2048×1536 holds about 250 MiB.

  The newest step is always kept even beyond the budget.
* **A step can be refused.** If pixels changed outside the history, the step
  is refused and the history cleared, with a message; the picture is not
  changed. No path in the app does that today (imports clear the history),
  and the check is the safety net.
* **Not recorded:**
  * selecting a layer;
  * the paper / background colour (a project setting, not layer content);
  * the view (zoom, rotation).
* **Opacity drags are merged into one step**, but separate drags are separate
  steps.
* **The history list is not live while drawing.** It refreshes after each
  stroke and operation. The buttons follow within about 50 ms of a stroke's
  end.

## KI-13 — Workspace redesign: limits of the Block 8 implementation

* **No physical test yet.** Verified by:
  * JVM tests of the layout policy (15 window shapes), gestures, colour math
    and contrast tokens;
  * emulator screenshots.

  The phone layouts were captured on a Pixel 6 emulator profile. The tablet,
  small and large layouts are **emulated windows** (`wm size` / `wm density`
  on the same emulator), **not tablet hardware**. Touch on the emulator is a
  single synthetic pointer: multi-finger taps and quick pinch are tested only
  by the JVM tests.
* **The stylus button eyedropper** uses `BUTTON_STYLUS_PRIMARY`. It is
  untested with a real S Pen or USI stylus.
* **Panel transitions** are short fades / slides (none with *Reduce motion*).
  There are no shared-element animations between layouts: a rotation
  rebuilds the chrome.
* **Reordering layers** still uses Move up / Move down (no drag and drop).
* **Accessibility audit.** It checks only the states captured. Rows cut off at
  the edge of a scrolling panel can show up as "small targets" in the dump.
* **Colour handling.** The colour panel works in sRGB, like the engine. There
  is no wide-gamut (P3) colour picking.
* **One pinned panel** at a time, on tablets only. Phones use sheets.

## KI-14 — Motion: limits of the Block 9 implementation

* **Emulator only.** The startup, transitions and frame times were recorded
  and measured on the CI emulator (SwiftShader, no GPU, debug build). There
  are no measurements on the A33 or on a low-end phone. Emulator frame times
  are pessimistic and noisy.
* **Fast starts.** When the app is ready almost at once, the intro is cut
  short by design: only the mark and the exit show. The full reveal
  (≈1.2 s) is seen only on slow starts.
* **Container transform.** It lands on an estimated canvas area (the engine
  fits the canvas after loading), so the last frames can be off by a few
  percent before the cross-fade.
* **Project closing** is a fade over the workspace, not a shrink back into the
  card: the canvas is a SurfaceView, which cannot be captured without a GPU
  readback.
* **Layer animations** run when the Layers panel rebuilds after an operation.
  A deleted row disappears at once; the rows around it close the gap.
* **Not animated:**
  * rotation or resize transitions of the chrome (it is rebuilt);
  * selection and transform tools (not implemented);
  * import and export (not implemented).

