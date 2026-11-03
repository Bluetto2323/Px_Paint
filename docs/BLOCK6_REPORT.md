# Block 6 — Layers and compositing: report

## 1. Architecture

`CanvasDocument` holds the layer stack:

* **Each layer** has its own tiled `RasterStore`: 256 px tiles, premultiplied
  sRGB RGBA8, sparse. Its properties are a persistent id, name, visible,
  locked, opacity and blend mode.
* **The composite** of the visible layers is what the GPU displays. The
  Vulkan / GLES upload path of Blocks 2–5 is unchanged, with partial uploads by
  dirty region.
* **The brush engine** writes into the active layer only.

Compositing runs on the CPU in one place (`canvas/Compositor`):

* Bit-identical on Vulkan and GLES (S11/S12 show a difference of 0).
* No per-layer GPU textures: 6 GPU resources with 1 layer and with 9.
* No GPU readback.

The composite is a cache:

* **Strokes** recomposite exactly their dirty rectangles in the same frame.
* **Property changes** mark only the tiles where that layer has content.
  Stale tiles are recomposited within 3–8 ms per frame.
* **Empty tiles** stay unallocated, and the renderer's paper shows there.

Details: `docs/LAYERS.md`.

## 2–3. Compositing and blend modes

The formula is W3C Compositing and Blending Level 1, computed in float on sRGB
values with one rounding per pixel. The modes are Normal, Multiply, Screen,
Overlay, Darken, Lighten, Add and Difference.

* **Backdrop:** with an opaque background the bottom layer blends with the
  paper, so Multiply over cream paper darkens it as shown.
* **Transparency:** transparent layer pixels change nothing, and over
  transparency every mode shows the layer as is (no darkening, no halos).
* **Opacity:** layer opacity is applied at composite time, independent of the
  brush opacity (50 % × 50 % = 25 %, LY3).

The reference image `PxPaint-blend-modes-reference.png` is rendered by the
same compositor. LY1 compares all 8 modes against an independent
double-precision implementation on 128 cases each: maximum error 1/255.

## 4. UI screenshots

Taken on an Android 14 emulator (CI) and published on the `ci-screenshots`
branch: the Layers panel with five layers (modes, opacity, one hidden, one
locked), plus the screens from earlier blocks.

## 5. Project format and migration

* **Files:**
  * `layers.pxlayers` manifest (`pxlayers 1`): size, active layer, layers
    bottom → top with name, visible, locked, opacity and blend.
  * `layer-<id>.pxtiles`: each layer's tiles.
* **Saving:** only changed layer files are written, each atomically; the
  manifest is written last, atomically; removed layers' files are deleted
  afterwards.
* **Migration:** Block 3–5 (`content.pxtiles`) and Block 2 (`content.pxops`)
  projects open as one layer. The first save writes the layered format and only
  then deletes the old file. New projects are layered from creation.
* **Damaged files:** a damaged manifest or a missing/damaged layer file makes
  the project refuse to open (message) rather than show and save a partial
  picture.

## 6. Automated tests

* `pe_layer_test` (new, CPU): 75/75.
  * LY1 blend modes against the reference.
  * LY2 transparency.
  * LY3 opacity independence.
  * LY4 add / delete / duplicate / rename / move / visibility / lock / active.
  * LY5 painting only the active layer; switching mid-stroke; hidden / locked refused; clear.
  * LY6 merge down and flatten keep the picture; invalid merges, last layer, 100-layer limit.
  * LY7 dirty-region compositing bit-identical to a full recomposite; tile corners.
  * LY8 per-layer thumbnails (hidden layers too, transparency kept).
  * LY9 save / reload of every property, bit-identical picture; invalid stacks and data refused.
  * LY10 migration of Block 2 and Block 3–5 content.
  * LY11 benchmarks.
* GPU brush suite (incl. S12): 67/67 on GLES and on Vulkan.
  * Display equals the composite for all modes, over the colour ramp and over the paper.
  * Opacity 0 and hidden layers disappear.
  * No GPU resources per layer.
  * A stroke at zoom 2, rotated, across a tile corner lands on the active layer only.
  * A locked layer refuses painting.
* JVM: 106/106 (6 new tests on the layer table, checks, manifest, layered save / reload / removal, migration, damaged projects).
* Everything from Blocks 1–5 still passes: canvas, brush, basic, lifecycle,
  faults, perf, pe_brush_test 159/159, pe_canvas_test 37/37.

## 7. Vulkan and GLES

Identical results on both (CI: Mesa llvmpipe GLES and lavapipe Vulkan):
display versus composite difference 0, and the same counts in every suite.

## 8. Benchmarks (CI host, CPU; informational)

Each layer covers a third of the canvas, semi-transparent, modes mixed.

| Canvas | Layers | Full composite | Stroke frame avg / worst | Layer switch | Opacity change | Layer thumb | Export all | Import + composite | CPU memory |
|---|---|---|---|---|---|---|---|---|---|
| 2048×1536 (single texture) | 5 | 94 ms | 0.24 / 0.88 ms | 0.015 ms | 38 ms | 0.2 ms | 22 ms | 86 ms | 41 MiB |
| | 10 | 130 ms | 0.28 / 0.94 ms | 0.015 ms | 63 ms | 0.3 ms | 33 ms | 144 ms | 61 MiB |
| | 20 | 222 ms | 0.39 / 1.03 ms | 0.017 ms | 120 ms | 0.3 ms | 66 ms | 274 ms | 117 MiB |
| | 50 | 507 ms | 0.75 / 1.53 ms | 0.018 ms | 264 ms | 0.3 ms | 152 ms | 658 ms | 267 MiB |
| 4096×4096 (tiled) | 5 | 437 ms | 0.13 / 0.29 ms | 0.015 ms | 166 ms | 0.4 ms | 70 ms | 381 ms | 185 MiB |
| | 10 | 628 ms | 0.21 / 0.83 ms | 0.015 ms | 272 ms | 0.5 ms | 161 ms | 652 ms | 297 MiB |
| | 20 | 1157 ms | 0.26 / 0.89 ms | 0.015 ms | 426 ms | 0.5 ms | 282 ms | 1303 ms | 525 MiB |
| | 50 | 2372 ms | 0.42 / 1.09 ms | 0.016 ms | 1141 ms | 0.5 ms | 634 ms | 2876 ms | 1157 MiB |

* **Full composite, opacity change and import+composite** are total work. In the
  app it runs on the render thread in 3–8 ms slices per frame, never in one go.
* **Export** is the raw tile export; compression and writing are on the io
  thread.
* **GPU memory** does not grow with layers.
* **Input latency (CI perf suite):** GLES 19.3 ms and Vulkan 18.6 ms on average,
  in the range of Blocks 4–5. The same-machine local run gave 28.5 ms, against
  28.1–28.6 ms for Block 5.

## 9. Known limitations

See `docs/KNOWN_ISSUES.md` KI-11:

* no hand test on a phone yet;
* CPU compositing cost of property changes on huge, deep documents (spread over
  frames; unmeasured on the A33);
* merge and flatten are isolated (exact for Normal);
* flatten deletes hidden layers, after confirmation;
* move up / down instead of drag and drop;
* no undo;
* blending in sRGB.

## 10. APK and Actions

Debug APK `PxPaint-block6-debug-<sha>.apk`, version 0.6.0-block6. Engine host
tests and APK are green; see the chat report for the run.
