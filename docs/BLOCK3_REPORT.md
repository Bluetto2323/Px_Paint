# PxPaint — Block 3 report: brush engine

Status: implemented, built and tested in CI on software GPUs. **Not yet
tested on the Galaxy A33** (checklist in `docs/DEVICE_VALIDATION.md`,
"Blocco 3").

## 1. Architecture

```
 UI thread (Kotlin)                 native, any thread             render thread
 ------------------                 ------------------             -------------
 WorkspaceScreen                    Engine::stroke*                 CanvasController::render
   MotionEvent: historical   --->   viewport: screen -> canvas      (after the frame's GPU wait)
   samples + current, in             (under viewMutex_)               CanvasDocument::syncGpu
   order (SampleBuffer)              CanvasDocument                     BrushEngine::process
   StrokeGate (pinch => cancel)        BrushEngine input queue            StrokePath  (spline, spacing)
   brush settings (BrushState)          (mutex, reused vectors)           StrokeCompositor (dabs, mask,
                                                                            base, composite)
                                                                          -> RasterStore (CPU tiles)
                                                                        RasterStore dirty rects
                                                                          -> CanvasSurface::upload
                                                                             (merged per GPU texture)
                                                                        CanvasRenderer draws the tiles
```

* `brush/` is independent of the UI and of the GPU: it only sees canvas
  coordinates and a `RasterStore`.
* `canvas/RasterStore` (CPU, 256 px tiles, premultiplied RGBA8) is the
  source of truth. The GPU storage is a copy fed by dirty rectangles; device
  loss / backend switch re-uploads it. No GPU readback anywhere.
* `engine/CanvasDocument` owns store + brush engine behind one mutex, so the
  render thread (GPU sync) and the io thread (save/thumbnail, locked per
  tile) never race. `generation()` tells the render thread when its GPU copy
  belongs to a replaced document.
* Kotlin talks to it only through `NativeEngine` (`strokeBegin/Samples/End/
  Cancel`, `export/importCanvasTiles`, `canvasThumbnail`). UI code never
  touches GPU resources.

## 2. Rendering pipeline

1. **Input.** Every MotionEvent sample, historical ones first, is sent in one
   JNI call (stack buffer, no allocation). The engine maps screen -> canvas
   with the viewport at that instant and appends to the brush queue, then
   wakes the render thread. `requestUnbufferedDispatch` delivers MOVE events
   as they arrive.
2. **Path.** Centripetal Catmull-Rom through every sample; exact-duplicate
   positions add no geometry. Segments are flattened (<= ~1 px pieces) and
   dabs are placed at constant arc length (`spacing x diameter`, remainder
   carried across segments). The newest segment is drawn provisionally right
   away (no one-sample lag) and committed when the next sample arrives;
   `end` commits it and adds a dab exactly on the last sample.
3. **Dabs.** Round, analytic antialiasing (1 px ramp at the radius; sub-pixel
   brushes keep a 1 px footprint with proportional coverage). Rows are filled
   as spans; only the antialiased ring is evaluated per pixel. For continuous
   strokes (spacing <= 25 %) the dab distance is also capped so the edge
   ripple between dabs stays <= 0.1 px, which keeps 500 px brushes straight.
4. **Stroke compositing.** Dabs accumulate in a 16-bit mask with `max`; the
   tile is recomposited from its pre-stroke copy (premultiplied source-over,
   or erase), so opacity is exact, overlapping dabs never darken, strokes
   stack normally, and cancel restores the pixels bit for bit. Stroke tiles
   are pooled.
5. **GPU.** The store's dirty rectangles (one per touched store tile per
   frame) are merged per GPU texture and uploaded once. A new GPU tile is
   filled completely (own pixels + gutter) so tile borders filter correctly.
   On Vulkan, uploads and texture clears made while a frame is being
   prepared go through a per-frame staging ring into the frame's command
   buffer (Block 2 did a submit + `vkQueueWaitIdle` for every upload).
6. **Persistence.** Saves export only tiles changed since the last export,
   deflate them and rewrite `content.pxtiles` atomically; thumbnails come
   from a box-filtered native preview. Block 2 projects (`content.pxops`)
   are rasterised on open and converted at the first save.

## 3. Performance, before / after

Same scenario on both versions (`host_test --suite perf`; baseline = Block 2
drawing path on branch `perf-baseline-block2`): 1080 x 1600 surface, A4
canvas, 40 fast strokes, a batch of 2 samples every 4 ms (240 Hz), 13 px
per sample, brushes 48 / 6 px. GitHub Actions runners, **software GPUs**
(Mesa llvmpipe for GLES, lavapipe for Vulkan): the GPU work itself runs on
the CPU there, so only the relative numbers mean something.

| | Block 2 (baseline, run 41606710117) | Block 3 (runs 41606696432 / 41607161658) |
|---|---|---|
| input -> frame presented, GLES, avg / max | 27.9 / 72.4 ms | 19.3 / 23.8 ms · 15.3 / 19.8 ms |
| input -> frame presented, Vulkan, avg / max | 29.7 / 52.7 ms | 18.6 / 21.8 ms · 14.8 / 18.7 ms |
| frame CPU work avg (GLES / Vulkan) | 16.0 / 16.7 ms | 11.0 / 10.7 ms · 8.9 / 8.7 ms |
| texture uploads for the 40 strokes | 11 519 (one per stamp) | 916-1 158 |
| bytes uploaded | 11.8 MiB | 17.9-18.7 MiB |

Runner-to-runner variance is visible in the two Block 3 runs; the baseline
was measured once.

Block 3 draws round antialiased dabs with fine spacing instead of Block 2's
square stamps, so it uploads more bytes but in 12x fewer uploads. CPU brush
cost (`pe_brush_test` B15, CI runner): 4 px brush 0.06 ms, 40 px 0.09 ms,
300 px 1.0 ms per frame of fast drawing (worst frame 1.6 ms).

On the A33 these numbers must be measured: the performance overlay shows
input->present latency (avg/last/max) and brush CPU time per frame.

## 4. Automated tests (CI run on the final commit)

* `pe_brush_test` 79/79 (CPU): straight, diagonal (45/30 deg), circular
  (16/48/720 samples per turn), fast vs slow sampling, frame batching and
  provisional tail determinism, taps and tiny strokes, 200 000-sample
  stroke, sizes 0.5-600 px, opacity (exact, no self-overlap darkening,
  source-over stacking, no dark fringes, exact colour), tile boundaries,
  cancellation (bit-exact restore), 40 repeated overlapping strokes,
  eraser, robustness, raster store.
* `pe_canvas_test` 37/37 (viewport, tiles, storage policy incl. CPU raster).
* Host GPU suites on GLES and Vulkan: `basic`, `lifecycle`, `faults`,
  `canvas` 66/66, `brush` 42/42 (screen samples -> raster -> GPU display
  checked on the captured backbuffer: stroke position and width, edge
  antialiasing, 50 % self-crossing stroke, tiles at 300 %, rotated + zoomed
  view, cancel, eraser, save/reload renders identically, 2 440 samples with
  none lost, resources released), `perf`.
* JVM unit tests (JUnit 4.13.2): 56/56, incl. `TileContentTest` (format,
  compression, damaged files, legacy projects) and `BrushStateTest`.
* API-level lint (minSdk 26) and APK inspection.

## 5. Not done / limits

See `docs/KNOWN_ISSUES.md` KI-4 (no pressure, one layer, no undo, memory x2,
stroke working memory, sRGB 8-bit blending) and KI-7 (device numbers
pending). KI-1 (Vulkan resize, Block 1) is still open: no new device report
since the fix.
