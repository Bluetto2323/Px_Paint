# PxPaint selection engine (Block 10)

A selection limits where painting and editing act. It is a coverage mask the
size of the canvas, separate from the layers: 0 = not selected, 255 =
selected, values in between on antialiased edges. Without a selection,
everything is editable, exactly as before Block 10.

## Architecture

| Part | File | Role |
|---|---|---|
| Mask | `cpp/canvas/SelectionMask.{h,cpp}` | Tiled 8-bit coverage, shape rasteriser, combine modes, invert, translate, outline, preview, history encoding |
| Brush clip | `cpp/brush/StrokeCompositor.cpp` | Each stroke / eraser pixel's coverage is multiplied by the mask |
| Document | `cpp/engine/CanvasSelection.cpp` (`CanvasDocument`) | Selection operations, pixel operations, clipboard, moving content, history |
| History | `cpp/engine/History.{h,cpp}` | `SelectionCommand`, `GroupCommand`, kinds 15–20 |
| Engine / JNI | `Engine::selectShape / selectionOp / selectionMoveCanvas / viewMatrix`, `jni/JniBridge.cpp` | Screen → canvas mapping, render requests |
| Kotlin API | `engine/NativeEngine.kt` | `select`, `selectionOp`, `selectionMove`, `selectionInfo`, `selectionVersion`, `selectionContours`, `selectionPreview`, `viewMatrix` |
| Tool logic | `pxpaint/core/Selection.kt` (pure, JVM-tested) | Gesture state machine, geometry, outline format, contextual actions, error messages |
| Feedback | `pxpaint/ui/SelectionOverlay.kt` | Marching ants, dim of the unselected area, shape preview, handles |
| Workspace | `pxpaint/screens/WorkspaceScreen.kt` | Select tool, options panel, contextual action bar, touches |

Every engine call goes through the document lock, after queued strokes (the
order of input is kept), on the single "layers" worker thread of the
workspace, never on the UI thread. While content is dragged, offsets are
coalesced: the worker applies only the latest one.

## The mask

* **Tiles of 256 × 256**, like the raster. A tile is EMPTY or FULL with no
  memory, or PARTIAL with 64 KiB of coverage. A partial tile that becomes
  uniform is turned back into EMPTY / FULL.
* **Shapes are polygons.**
  * The rectangle is snapped to whole pixels: crisp edges, exact area.
  * The ellipse is a polygon with a chord error below 0.1 px.
  * Lasso and polygon use the touch points.
* **Rasterisation:** exact horizontal coverage on 4 sub-scanlines per row,
  with nonzero winding, so a self-crossing lasso stays filled.
  * Rows are written band by band. A tile whose rows are all 0 or all 255 is
    never allocated, so memory follows the outline, not the area.
  * Example: a 0.4-canvas ellipse on 8192² uses 3.25 MiB instead of 64 MiB.
* **Modes:**
  * replace;
  * add = max;
  * subtract = min(a, 1 − b);
  * intersect = min.

  Subtract or intersect with nothing selected stays nothing. A result that
  selects nothing deactivates the selection.
* **Select from content:** the union of the visible layers' alpha, with layer
  opacity: 1 − ∏(1 − αᵢ·opacityᵢ).
* **Outline:** pixel-edge loops at 50 % coverage, with straight runs merged.
  FULL tiles are visited only on their border, next to a non-full tile.
* **Preview:** inverse coverage, at most 512 px on the longer side, for
  dimming.
* **History encoding:** tile states, plus RLE of partial tiles. A
  rectangle's state is a few hundred bytes.
* **Version:** bumped by every change; the UI polls it (lock-only read) to
  notice undo / redo.

## Painting with a selection

`StrokeCompositor::composite` multiplies each pixel's stroke coverage by the
mask:

* EMPTY tiles copy the base row and stop;
* FULL tiles skip the multiply.

The eraser uses the same path, so it erases `a·(1 − s)` on edges. Outside the
selection, every pixel stays bit-for-bit identical. This is tested on every
pixel of the canvas.

## Operations and history

| Operation | Pixels | History step |
|---|---|---|
| Rectangle / ellipse / lasso / polygon (replace, add, subtract, intersect) | — | `selection` |
| Select all, deselect, invert, select from content | — | `selection` |
| Clear selected | active layer × (1 − s) | `clear_selection` (pixel patch) |
| Copy / copy merged | none (clipboard) | none |
| Cut | copy, then clear | `cut` |
| Paste | new layer above the active one | `paste` (insert layer) |
| Duplicate selection | new layer with the selected part | `duplicate_selection` |
| Move (drag, drop) | lifted pixels moved on the active layer | `move_selection` = group (pixel patch + selection) |

* Re-selecting an identical selection adds no step.
* Clearing never deletes the layer.
* Without a selection, clear / copy / cut / move are refused, so a whole layer
  is never cleared by mistake.
* Locked or hidden layers refuse pixel edits; the UI then hides those
  actions.
* Pixel steps are XOR-RLE patches of the tiles touched, as in Block 7. There
  are no full-canvas snapshots: the selection state is the compact encoding
  above.
* Undo / redo restores pixels and selection together for moves. Other steps
  touch only what they changed.

## Clipboard and layers

* **Copy** reads the active layer: its pixels times the coverage, in
  premultiplied RGBA, so transparency and partial edges are kept.
* **Copy merged** composites the visible layers (opacity and blend modes) over
  transparency, without the paper. The UI says that it merged all visible
  layers.
* **Paste** inserts a new layer above the active one, with the pixels where
  they were copied from. A paste into a canvas of another size keeps what
  fits. The clipboard lasts for the app session; it is not the system
  clipboard.

## Moving selected content (and Block 11)

1. **Begin.** The selected pixels of the active layer are lifted into
   `move_.floating` (pixels × coverage, at offset 0). Nothing changes on
   screen.
2. **Offset.** At an offset (dx, dy), each affected tile of the active layer is
   recomposed as `original × (1 − s)` with the shifted floating pixels drawn
   over it (premultiplied src-over).
   * Only those tiles change, and they are uploaded like brush tiles.
   * The layer's own opacity and blend mode apply.
   * What the drag shows is exactly what the drop records.
   * Pixels moved beyond the canvas edge are dropped.
3. **Commit** builds one patch from the originals of every touched tile,
   translates the selection, and pushes `GroupCommand(MoveSelection,
   {PixelCommand, SelectionCommand})`.
4. **Cancel** restores the originals.

A stroke, a layer operation, undo, an export or a save during a move commits
the move first, so it lands before them in the history.

**Block 11 integration point:** `CanvasDocument::Move` holds the lifted
pixels, the selection before, the original tiles and the tiles currently
shown. A transform engine replaces the integer offset in
`composeMoveTileLocked` with a resampled affine transform of `floating`. There
is no separate floating layer or GPU path to reconcile.

## Interface

* **Select tool** in the dock / rail / toolbar: dashed rectangle icon.
  * With it, one pointer selects: a finger whatever the finger mode, a stylus
    or a mouse.
  * Two fingers still move, zoom and rotate the view. A second finger right
    after the first cancels the shape (a pinch).
  * Taps with two / three fingers still undo / redo.
* **Options** (tap the tool again):
  * shape: Rectangle, Ellipse, Lasso, Polygon;
  * mode: New, Add, Subtract, Intersect;
  * Smooth edges (antialiasing);
  * Select all, Select content, Invert, Deselect.
* **Gestures:**
  * Rectangle / ellipse: drag corner to corner. They are axis-aligned on the
    canvas, so a rotated view shows them rotated.
  * Lasso: draw around the area.
  * Polygon: each tap is a corner. Tapping the first corner (ring), or *Close
    shape*, closes it; *Remove point* and *Cancel* are in the bar.
  * A tap without a drag deselects in New mode.
  * Back cancels a shape in progress or a move.
* **Contextual bar** under the zoom chip. It shows only actions that can act
  now:
  * Move, Copy, Copy merged, Cut, Paste, Duplicate, Clear, Invert, Deselect;
  * Done / Cancel while moving;
  * the polygon actions while drawing one;
  * Paste / Select all / Select content without a selection.
* **Feedback:**
  * Marching ants: a white line under black dashes, 1.25 dp at every zoom,
    stepping 10 times a second. They stand still while a stroke is painted.
  * With the Select tool, the unselected area is dimmed.
  * Previews match what the engine selects.
  * Corner handles; vertex dots and the closing ring for polygons.
  * The overlay is an ordinary View above the SurfaceView. It never redraws
    the canvas and does not touch the input path.

## Persistence

The selection is not saved in the project. It is editing state, like the
history. Projects stay byte-compatible with Block 9.

A move in progress is committed before any export, so a saved file never
contains lifted content.

## Performance (CPU host, this machine; see the report for CI numbers)

| Canvas | Rectangle | Ellipse | 20 000-point lasso | Invert | Outline | Move step | Mask (0.4-canvas ellipse) |
|---|---|---|---|---|---|---|---|
| 4096² | 49 ms | 58 ms | 52 ms | 6 ms | 33 ms | ≤ 7 ms | 1.5 MiB |
| 8192² | 142 ms | 209 ms | 194 ms | 13 ms | 74 ms | ≤ 18 ms | 3.25 MiB |

Brush cost with a selection: 99 % (select all) and 97 % (antialiased ellipse)
of the cost without one, over 150 strokes.
