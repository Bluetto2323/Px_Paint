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

[to be completed with the CI numbers]

## 7. Automated test results

[to be completed]

## 8. Screenshots and recordings of real selection operations

[to be completed]

## 9. APK and GitHub Actions status

[to be completed]

## 10. Known limitations

See KI-15.
