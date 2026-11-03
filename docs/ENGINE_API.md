# PxPaint — UI ↔ engine interface

The Android UI (Kotlin) talks to the native engine (C++) **only** through
`com.paintengine.engine.NativeEngine` (JNI facade) and `EngineSurfaceView`.
No Kotlin code calls a graphics API, and the engine knows nothing about the
UI. This document is the contract between the two.

## Threading

| Who | Rule |
|---|---|
| UI thread | Calls every control method. Each one posts a command to the engine's render thread and returns at once. |
| `surfaceDestroyed()` | The only blocking call. It returns after the swapchain/EGLSurface is destroyed and the `ANativeWindow` is released. There is no timeout, by design: the render thread never waits on the UI thread, so this cannot deadlock. |
| Viewport (`viewport`, `screenToCanvas`, `canvasToScreen`, `stamp`) | Applied **synchronously** under a mutex, so touch mapping is exact for the event being handled. The render thread copies the viewport once per frame. |
| Strokes (`strokeBegin/Samples/End/Cancel`) | UI thread. Samples are mapped to canvas pixels with the viewport **at the time of the call** and appended to the brush engine's input queue (never blocks on rendering). The render thread consumes the queue at the next frame. |
| Persistence (`exportCanvasTiles`, `importCanvasTiles`, `canvasThumbnail`) | Any thread (PxPaint: its io thread). They share a lock with `close()`. Exports first apply queued stroke input, and lock the document per tile, so drawing is never held up by a save. |
| Queries (`statusJson`, `statusText`, `canvasContentRevision`) | Thread-safe snapshots that may be called from any thread. They share a lock with `close()`. |
| Render thread | Owns every GPU object. Only the engine runs code on it. |

## Lifecycle

```
NativeEngine(config)            // starts the render thread, no GPU yet
surfaceCreated / surfaceChanged // device selected on the first surface (Vulkan → GLES fallback)
pause / resume                  // Activity onPause/onResume
surfaceDestroyed                // blocking; the device and the canvas survive
close()                         // ordered teardown, blocking, idempotent
```

The device and all canvas data survive the loss of the surface. The canvas
pixels live in a CPU raster (see below); a device loss, a GLES context loss
or a backend switch simply re-uploads every painted tile from it.

Only **one** `NativeEngine` may exist per process, because the Vulkan
function pointers are process-wide. PxPaint therefore runs the Block 1
diagnostics tools (`DiagnosticsActivity`) in a separate process
(`:diagnostics`).

## Config

| Field | Meaning |
|---|---|
| `backend` | `auto` (Vulkan, then OpenGL ES) · `vulkan` · `gles` |
| `allowFallback` | when a backend is forced, the other one may still be tried |
| `renderMode` | `ON_DEMAND` (a frame only when something changes) · `CONTINUOUS` (every vsync) |
| `diagnostics` | periodic performance log plus automatic self-test. It **never** forces continuous rendering. |
| `scene` | `DIAGNOSTIC` (Block 1 triangle) · `CANVAS` (PxPaint) |
| `canvasMemoryBudgetBytes` | budget for canvas pixels, **CPU raster + GPU copy**. 0 = derived from the GPU report (Vulkan) or a fixed 512 MiB (GLES). PxPaint passes ¼ of device RAM, clamped to 256 MiB–1.5 GiB. |

## Canvas

| Call | Notes |
|---|---|
| `createCanvas(w, h, paperArgb, transparent)` | Returns `null` if accepted, otherwise a reason. Sides 1…16384 px. GPU storage is created on the render thread; the result appears in `statusJson` (`canvasGpuReady`, `canvasStorage`, `canvasError`). |
| `checkCanvasSize(w, h)` | JSON `{ok, tiled, maxBytes, reason}` for the **current device**. Nothing is allocated. Not ok if no device exists yet. |
| `canvasContentRevision()` | Changes on every content change request (stroke input, load, clear, new canvas). Use it to know whether a save is needed. |
| `stamp(screenX, screenY, size, argb)` | Diagnostic square fill (replace semantics) under a screen point. Not used by the app's tools. |
| `loadCanvasContent(ops)` | **Legacy import** of Block 2 projects: flat `{x, y, w, h, argb}` rectangle ops, rasterised (replace semantics) into the canvas. Call right after `createCanvas`. |
| `clearCanvas()` / `closeCanvas()` | Clear the content / close the document. |
| `setCanvasColors(workspace, paper, transparent)` | Workspace colour around the canvas; paper colour or checkerboard. |
| `runCanvasSelfTest()` | Renders known content (tiles, seams, partial updates, rotation) and compares the pixels against a CPU model. Result in `canvasSelfTest*`. |

## Brush strokes (Blocks 3-5)

| Call | Notes |
|---|---|
| `strokeBegin(sample, Brush)` | Starts a stroke. `sample` = `SAMPLE_FLOATS` (6) floats: x, y in **screen** pixels (mapped through the current viewport), t (ms), pressure (0..1, normalised by the app with the device's range), tilt (rad, 0 = perpendicular), orientation (rad, screen space; the engine converts it to canvas space). **NaN = the device does not report the axis.** `Brush`: `diameter` in **canvas** pixels (the maximum when pressure controls size), `opacity`, `argb`, `spacing`, `erase`, and the dynamics `pressureSize` + `minSizeFraction`, `pressureOpacity` + `minOpacityFraction`, `tiltShape` + `tiltStrength`, `pressureSmoothing`, `pressureCurve` (`PRESSURE_CURVE_SIZE` = 257 response values, monotonic 0→1; invalid tables fall back to linear). |
| `strokeSamples(samples, count)` | `count` samples of 6 floats, **oldest first** (historical MotionEvent samples, then the current one). Every sample is kept. |
| `strokeEnd()` | Commits the stroke, ending exactly on the last sample. |
| `strokeCancel()` | Removes the stroke in progress: the canvas returns to its previous state bit for bit. |

Dynamics act only on strokes whose **first sample** carries the axis: a
stroke without pressure (finger, mouse, passive stylus) is drawn exactly as
in Block 3 (full size and opacity, round dabs) even if the brush enables
pressure. Pressure is smoothed per stroke with a 1-euro filter on sample
time (adaptive: slow jitter smoothed, fast presses pass within 1-3 samples),
interpolated linearly between samples together with tilt and orientation
(shorter arc), then mapped through the curve: size =
`diameter × (min + (1 − min) × c)`, dab opacity = `min + (1 − min) × c`, with
floors of 2 % (size) and 5 % (opacity) so no pressure value makes a stroke
vanish. Dab spacing follows the current dab size. Tilt needs both tilt and
orientation; the dab becomes an antialiased ellipse along the stylus
(major axis × (1 + 1.5 · strength · sin tilt)); without them it stays round.

The engine joins samples with a centripetal Catmull-Rom spline (through
every sample), places antialiased round dabs at constant arc-length spacing,
accumulates them in a 16-bit coverage mask with `max` (so a stroke's opacity
is exact) and composites the stroke over the canvas as it was before the
stroke (premultiplied source-over / erase). The newest spline segment is
drawn provisionally at once, so the stroke tip follows the finger without a
one-sample lag. Below `spacing` 0.25 the engine also limits the dab distance
so the edge ripple of big brushes stays under 0.1 px. See
`app/src/main/cpp/brush/` and `docs/BLOCK3_REPORT.md`.

### Brush model and previews (Block 5)

`NativeEngine.Brush(params, flags, argb, pressureCurve)` carries the brush
already encoded (`pxpaint.core.BrushEncoding`). `params` = 21 floats:

| # | Field | # | Field |
|---|---|---|---|
| 0 | diameter (canvas px) | 11 | size jitter |
| 1 | opacity | 12 | scatter |
| 2 | spacing | 13 | angle jitter |
| 3 | min size fraction | 14 | flow jitter |
| 4 | min opacity fraction | 15 | grain depth |
| 5 | tilt strength | 16 | grain pressure |
| 6 | pressure smoothing | 17 | wet edge |
| 7 | hardness | 18 | tip (0 round, 1 chalk, 2 charcoal, 3 bristle) |
| 8 | flow | 19 | grain (0 none, 1 paper, 2 rough, 3 canvas, 4 wash) |
| 9 | roundness | 20 | accumulation (0 stroke max, 1 buildup) |
| 10 | angle (degrees) | | |

`flags`: 1 erase, 2 pressure → size, 4 pressure → opacity, 8 tilt, 16 angle
follows the stroke. A 7-float array (Block 4) is still accepted and gives
the classic round brush. A fixed angle is screen space: the engine
subtracts the view rotation, as it does for the stylus orientation.
Parameters, textures and file formats are described in `docs/BRUSHES.md`.

`NativeEngine.renderBrushPreview(brush, width, height, strokes, background)`
(static, any thread, no engine instance) renders preview strokes with the same
StrokePath / StrokeCompositor code as the canvas into a private CPU raster.
It returns opaque ARGB. `strokes` = `[n, then per stroke: count, diameter (≤ 0:
brush), opacity (< 0: brush), count × 6 floats in preview pixels]`. Host test
L14 checks that the preview equals the canvas stroke for all 12 presets.

Status fields: `brushStrokes`, `brushCancelled`, `brushSamplesReceived`,
`brushSamplesProcessed`, `brushDabs`, `brushPending`, `brushStrokeTiles`,
`brushLastMs`, `brushMaxMs` (CPU time to apply queued input in one frame),
`inputLatencyLastMs/AvgMs/MaxMs/Frames` (oldest sample queued → frame
presented).

## Canvas content and persistence

The canvas pixels are a **CPU raster** (`canvas/RasterStore`): premultiplied
RGBA8 in 256 × 256 tiles allocated on first write. It is the source of
truth. The GPU storage is its copy, refreshed from **dirty rectangles**
(merged per GPU texture, one upload per texture per frame; never the whole
canvas). No GPU readback is used anywhere.

| Call | Notes |
|---|---|
| `exportCanvasTiles(changedOnly, maxTiles)` | Records, little-endian: `int count`, then per tile `int tx, int ty, int flags` (1 = empty, can be dropped) and, unless empty, 256·256·4 bytes of premultiplied RGBA. `changedOnly`: tiles changed since their last export (they are marked saved); otherwise every allocated tile. |
| `importCanvasTiles(records)` | Imports records in the same format (after `createCanvas`). Returns the tiles imported, or -1 if the data is malformed. |
| `canvasThumbnail(maxSide)` | `[w, h, pixels…]`, straight ARGB, box-filtered, transparent where nothing is painted. |

PxPaint stores the compressed tiles in `content.pxtiles` (`TileContent.kt`;
only changed tiles are recompressed at each save, the file is replaced
atomically). Block 2 projects (`content.pxops`, rectangle ops) are imported
with `loadCanvasContent` and written as tiles at the first save.

### Storage strategy

`StoragePolicy::choose(w, h, {maxTextureSize, memoryBudget})` picks:

* **single texture** when both sides are ≤ min(GPU max texture size, 2048), and
* **tiles** otherwise: 256 px tiles plus a 1 px gutter (258² texels),
  allocated where the CPU raster has content, with partial uploads and
  per-frame culling.

The worst case counted against the budget is the GPU copy plus the CPU
raster (whole 256 px tiles), i.e. about twice the pixel data. On Vulkan,
uploads made while a frame is being prepared are copied into a per-frame
staging ring and recorded in the frame's command buffer (no submit-and-wait
per upload); new tile textures are cleared the same way.

Canvases whose worst case exceeds the budget are rejected, with the reason.
`CanvasSizeRules.kt` mirrors this policy so the New Canvas sheet can explain
sizes before a device exists. When a device does exist, the engine's answer
(`checkCanvasSize`) is the one that counts.

## Viewport

`viewport(op, args…)` returns a `ViewportInfo` (zoom, rotation, centre, fitted,
min/max zoom, canvas and screen size). Arguments are in **view pixels** and
**radians**.

| Op | Args |
|---|---|
| `QUERY`, `FIT`, `CENTER` | — |
| `PAN_BY` | dx, dy |
| `ZOOM_AT` / `SET_ZOOM_AT` | factor or zoom, focalX, focalY |
| `ROTATE_AT` / `SET_ROTATION_AT` | radians, focalX, focalY |
| `GESTURE` | prevX, prevY, newX, newY, scale, radians (two-finger pan/zoom/rotate in one step; the canvas point under the fingers follows them exactly) |

Invariants, checked by `pe_canvas_test`:
* `screen→canvas→screen` is exact;
* the canvas aspect ratio is preserved for every screen size, zoom, and rotation;
* fit keeps every corner on screen at any rotation;
* zoom, rotation, and gestures keep the focal point fixed;
* a fitted view is refitted when the screen size changes.

## Status

`statusJson()` returns one flat JSON object. Engine fields are listed in
`diagnostics::statusJson`. Canvas fields: `canvasOpen`, `canvasGpuReady`,
`canvasWidth/Height`, `canvasStorage`, `canvasError`, `canvasTileSize`,
`canvasTilesTotal/Allocated`, `canvasBytesAllocated`, `canvasWrites`,
`canvasTextureUploads`, `canvasBytesUploaded`, `canvasLastWriteMs`,
`canvasMaxWriteMs`, `canvasRestores`, `canvasRasterTiles`, `canvasRasterBytes`, `canvasRevision`,
`canvasTilesVisible`, `canvasPiecesDrawn`, `canvasDrawCalls`, `canvasZoom`,
`canvasRotationDeg`, `canvasMaxTextureSize`, `canvasMemoryBudgetBytes`,
`canvasSelfTest`, `canvasSelfTestRuns`, `canvasSelfTestDetail`.

## Reserved for later blocks

* Pressure / stylus (Block 4): `StrokePoint` already carries the sample time; pressure and tilt join it, and `BrushSettings` gains dynamics. The path and compositor do not change.
* Layers: one `RasterStore` + `CanvasSurface` per layer, plus a compositor.
* Undo: the stroke compositor already keeps the pre-stroke tiles (the "base"); an undo history keeps them after commit instead of recycling them.

The UI already reserves the entry points (Undo/Redo, Layers, tool settings),
shown disabled with an explanation.
