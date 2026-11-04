# Block 10 — Selection engine: report

Details: `docs/SELECTION.md`. Limits: `docs/KNOWN_ISSUES.md` KI-15.

## 1. Selection architecture

| Layer | Contents |
|---|---|
| **Mask** (`canvas/SelectionMask`) | Native, independent from the layer pixels |
| **Brush clip** (`brush/StrokeCompositor`) | Every stroke and eraser pixel is multiplied by the mask |
| **Document** (`engine/CanvasSelection.cpp`) | Selection operations, pixel operations, clipboard, moving content; every operation under the document lock after queued strokes |
| **History** | `SelectionCommand` (mask before / after, compact); `GroupCommand` (move = pixels + selection in one step); kinds 15–20 |
| **Engine / JNI / `NativeEngine`** | Screen → canvas mapping through the current view, render requests, cheap version polling, view matrix for overlays |
| **Kotlin** | `core/Selection.kt` (pure, JVM-tested: gestures, geometry, outline format, contextual actions); `ui/SelectionOverlay.kt` (ants, dim, previews, handles); the Select tool in `WorkspaceScreen` |

* The mask is the only selection representation. There is no second
  "selection layer".
* Engine calls run on the workspace's single worker thread. Drag offsets of a
  move are coalesced.

## 2. Supported selection types

* **Rectangle.** Snapped to whole pixels, so crisp edges and an exact area.
* **Ellipse.** Polygon with a chord error under 0.1 px, antialiased.
* **Freehand lasso.** Every touch sample, nonzero winding.
* **Polygon.** Tapped corners; closed by tapping the first one.
* **From visible content.** The alpha of every visible layer, with layer
  opacity.

Rectangles and ellipses are axis-aligned on the canvas, also in a rotated
view. Antialiasing ("Smooth edges") is on by default and can be turned off.

**Operations:** replace, add, subtract, intersect, invert, select all,
deselect, clear selected content. Clearing changes only the selected pixels
of the active layer; it never deletes the layer.

## 3. Selection-mask implementation

* **Tiles:** 256² tiles of 8-bit coverage. Empty and full tiles hold no
  memory; partial tiles hold 64 KiB.
  * The rasteriser computes exact coverage on 4 sub-scanlines per row.
  * It writes rows band by band and never allocates a tile whose rows are
    all 0 or all 255.
  * So memory follows the outline: a 0.4-canvas ellipse takes 1.5 MiB on
    4096² and 3.25 MiB on 8192², where a full 8-bit mask would take 16 /
    64 MiB.
* **Combine:** add = max, subtract = min(a, 1 − b), intersect = min,
  tile by tile, with uniform tiles short-cut.
* **Outline:** pixel-edge loops at 50 % coverage, straight runs merged. Full
  tiles are visited on their border only.
* **Preview:** inverse coverage, at most 512 px, for dimming.
* **History encoding:** RLE.
* **Dirty regions:** pixel operations mark and upload only the tiles they
  touch, as brush strokes do. The mask carries a version, so outlines and
  previews are rebuilt only when it changes.
* **GPU:** the mask stays on the CPU, where the raster it clips lives. The GPU
  composites the results and, through the hardware-accelerated overlay,
  scales the dim image.

## 4. Layer integration

* **Strokes and eraser:** clipped on the active layer. Transparency and partial
  alpha are kept; at an antialiased edge the eraser removes `a·(1 − s)`.
* **Clear, cut, move:** act on the active layer. They are refused on a locked
  or hidden layer, and the bar does not offer them there.
* **Copy:** copies the active layer.
* **Copy merged:** copies the visible image, with opacity and blend modes, no
  paper. The message says so.
* **Paste / duplicate:** create a new layer above the active one, with
  transparency.
* **Move:**
  * shown live on the active layer with its own opacity and blend mode;
  * dropped exactly as displayed;
  * unrelated pixels untouched (checked pixel by pixel).

  Block 11 extends the same lifted content (`CanvasDocument::Move`).
* **Projects:** files are unchanged. The selection is not saved, by design. A
  move in progress lands before any save or export.

## 5. Undo / redo integration

Each logical operation is one predictable step:

| Step | Operations |
|---|---|
| `selection` | every shape / mode, invert, select all, deselect, from content |
| `clear_selection`, `cut` | pixel patch |
| `paste`, `duplicate_selection` | layer insert |
| `move_selection` | one group: pixels + selection |

* Repeating an identical selection adds no step.
* Copy is not a step.
* Undo / redo restores pixels and selection together. There are no
  full-canvas snapshots: pixel steps are tile delta patches, and the mask
  state is RLE (hundreds of bytes for a rectangle).
* Strokes, layer operations, undo or saving during a move commit the move
  first, in order.

## 6. Performance measurements

All numbers below come from the CI runner (x86-64), host build. The
emulator log adds the app-side timings.

| Canvas | Select rectangle | Ellipse | 20 000-point lasso | Invert | From content | Outline | Copy | Clear | Move begin / step / commit | Undo move |
|---|---|---|---|---|---|---|---|---|---|---|
| 4096² | 39 ms | 56 ms | 50 ms | 6 ms | 39 ms | 33 ms | 6 ms | 15 ms | 6 / ≤ 7 / 13 ms | 4 ms |
| 8192² | 141 ms | 210 ms | 166 ms | 12 ms | 122 ms | 58 ms | 12 ms | 45 ms | 12 / ≤ 13 / 27 ms | 9 ms |

* **Mask memory:** a 0.4-canvas ellipse takes 1.5 MiB on 4096² and 3.25 MiB on
  8192². Select all takes 0 bytes.
* **Brush cost:** 150 strokes take 296.6 ms without a selection, 297.1 ms with
  select all, and 297.0 ms with an antialiased ellipse. That is no measurable
  regression.
* **Drawing latency:** input → present is 19.2 ms on GLES and 18.5 ms on
  Vulkan, in the Block 2–9 range.
* **GPU (S14), select → frame presented:**

  | Measure | GLES | Vulkan |
  |---|---|---|
  | Select → frame | 10.2 ms | 11.0 ms |
  | Drag step | 6.9 ms | 7.2 ms |
  | Drop | 9.2 ms | 12.2 ms |
* **Emulator, select an ellipse in the app:**
  * on a 1600×2400 canvas: 28 ms;
  * copy: 4 ms;
  * paste: 1 ms;
  * move begin: 43 ms.

## 7. Automated test results

* **`pe_selection_test`: 99/99**, also under ASan/UBSan. It covers:
  * every shape and exact areas;
  * antialiased edges;
  * the modes;
  * invert;
  * tile edges;
  * encoding;
  * outside pixels unchanged, checked on every pixel;
  * the eraser edge `a·(1 − s)`;
  * clear, copy, copy merged, cut, paste, duplicate, with transparency;
  * moving, including live composite, unrelated pixels and the canvas edge;
  * painting during a move;
  * 16 mixed operations undone and redone exactly;
  * from content;
  * large canvases;
  * brush cost.
* **Host GPU suites (GLES and Vulkan): 21/21.**
  * S14, 100/100 per backend: on screen the stroke changes only pixels inside
    the selection (492 034 px outside unchanged); lifting is invisible;
    dropping matches the drag; undo / redo of the move, clear and paste are
    exact frames.
  * GLES vs Vulkan selection frame: identical (max channel difference 0).
* **Existing suites:** `pe_layer_test` 81/81, `pe_history_test` 63/63,
  `pe_brush_test` 159/159, `pe_canvas_test` 37/37.
* **JVM 148/148**, including `SelectionTest`, 11 tests:
  * gestures, polygon, lasso;
  * view transform and offsets;
  * preview = engine geometry;
  * outline format;
  * contextual actions;
  * errors;
  * routing.

## 8. Screenshots and recordings of real selection operations

* The emulator recording drives the real app with touches: drag, taps on the
  real controls, lasso with `input motionevent`.
  * Files: `70-selection-ops.mp4`, `71`–`82`; `23` (landscape), `33` / `34`
    (tablet portrait), `43` (tablet landscape).
* **Bug found by the first run (4c823f2):** autosave (every 20 s) landed a move
  that had just begun, so the next drag drew a new ellipse. Fixed in ee3adeb:
  no autosave during a move; leaving the app lands it.
* **Script bug in the same run:** the scripted stroke started on the screen
  edge (the system back gesture). Fixed in ee3adeb.

## 9. APK and GitHub Actions status

* Runs 41814987994 and 41815756331 (commit 4c823f2): all jobs green.
* APK: `PxPaint-block10-debug-4c823f2.apk`.

## 10. Known limitations

See KI-15.
