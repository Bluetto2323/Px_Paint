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
| Queries (`statusJson`, `statusText`, `canvasContent`, `canvasContentRevision`) | Thread-safe snapshots that may be called from any thread. They share a lock with `close()`. |
| Render thread | Owns every GPU object. Only the engine runs code on it. |

## Lifecycle

```
NativeEngine(config)            // starts the render thread, no GPU yet
surfaceCreated / surfaceChanged // device selected on the first surface (Vulkan → GLES fallback)
pause / resume                  // Activity onPause/onResume
surfaceDestroyed                // blocking; the device and the canvas survive
close()                         // ordered teardown, blocking, idempotent
```

The device and all canvas data survive the loss of the surface. A device
loss, a GLES context loss, or a backend switch rebuilds the GPU storage from
the canvas **content journal**.

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
| `canvasMemoryBudgetBytes` | budget for canvas pixels. 0 = derived from the GPU report (Vulkan) or a fixed 512 MiB (GLES). PxPaint passes ¼ of device RAM, clamped to 256 MiB–1.5 GiB. |

## Canvas

| Call | Notes |
|---|---|
| `createCanvas(w, h, paperArgb, transparent)` | Returns `null` if accepted, otherwise a reason. Sides 1…16384 px. GPU storage is created on the render thread; the result appears in `statusJson` (`canvasGpuReady`, `canvasStorage`, `canvasError`). |
| `checkCanvasSize(w, h)` | JSON `{ok, tiled, maxBytes, reason}` for the **current device**. Nothing is allocated. Not ok if no device exists yet. |
| `loadCanvasContent(ops)` | Replaces the content (call right after `createCanvas`). Ops are clipped to the canvas. Returns the number kept. |
| `canvasContent()` | Content as flat `IntArray` ops `{x, y, w, h, argb}`, in write order. |
| `canvasContentRevision()` | Changes on every content change. Use it to know whether a save is needed. |
| `stamp(screenX, screenY, size, argb)` | **Preview tool** (no brush engine yet): fills a `size`×`size` square centred on the canvas point under the screen point. It *replaces* pixels and does not blend. `argb = 0` erases to transparent, so the paper shows through. |
| `clearCanvas()` / `closeCanvas()` | Clear the content / close the document. |
| `setCanvasColors(workspace, paper, transparent)` | Workspace colour around the canvas; paper colour or checkerboard. |
| `runCanvasSelfTest()` | Renders known content (tiles, seams, partial updates, rotation) and compares the pixels against a CPU model. Result in `canvasSelfTest*`. |

### Content format (temporary)

Until raster layers exist, a canvas's content is the ordered list of
`ContentOp {x, y, w, h, argb}` (straight ARGB, replace semantics). PxPaint
stores it in `content.pxops` (see `ContentLog.kt`). The list is also the
replay journal used after a device loss (up to 100 000 ops). Block 3's brush
engine will replace this format with raster tiles; the app reads its version
header.

### Storage strategy

`StoragePolicy::choose(w, h, {maxTextureSize, memoryBudget})` picks:

* **single texture** when both sides are ≤ min(GPU max texture size, 2048), and
* **tiles** otherwise: 256 px tiles plus a 1 px gutter (258² texels),
  allocated on the first write, with partial uploads and per-frame culling.

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
`canvasMaxWriteMs`, `canvasJournalOps`, `canvasRestores`,
`canvasTilesVisible`, `canvasPiecesDrawn`, `canvasDrawCalls`, `canvasZoom`,
`canvasRotationDeg`, `canvasMaxTextureSize`, `canvasMemoryBudgetBytes`,
`canvasSelfTest`, `canvasSelfTestRuns`, `canvasSelfTestDetail`.

## Reserved for later blocks

* Brush engine: it will write through the same tile storage (`CanvasSurface::write`).
* Layers: one `CanvasSurface` per layer, plus a compositor.
* Undo: the content journal becomes the history source.

The UI already reserves the entry points (Undo/Redo, Layers, tool settings),
shown disabled with an explanation.
