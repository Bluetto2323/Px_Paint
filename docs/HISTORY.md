# Undo / redo history (Block 7)

## Where it lives

* **Engine** (`app/src/main/cpp/engine/History.h/.cpp`): the history itself.
  It is pure C++ with no UI and no GPU. It works on the document through a
  narrow interface (`HistoryTarget`): find, insert, remove and move a layer;
  set the active layer; report "pixels / properties / stack changed". Only
  `CanvasDocument` implements that interface.
* **`CanvasDocument`**: records every completed operation, under the
  document's lock, in the same call that performs it.
* **Engine / JNI / `NativeEngine`**: undo, redo, clear, counts, the list,
  and the budget.
* **UI** (`WorkspaceScreen`, `HistoryPanel`): the buttons, the panel and
  the messages.

## Commands

Each step is a `HistoryCommand` with explicit `undo()` and `redo()`. A command
returns false when the document is not in the state it recorded; it then
changes nothing.

| Kind (code) | Command | What it keeps |
|---|---|---|
| Stroke (0), Erase (1) | `PixelCommand` | Layer id + `PixelPatch` of the tiles the stroke touched |
| Clear (2), Fill (3) | `PixelCommand` | Same (clear: every tile of the layer) |
| AddLayer (4), DuplicateLayer (6) | `InsertLayerCommand` | Undo removes the layer and keeps it (same id, pixels); redo re-inserts it at its index |
| DeleteLayer (5) | `DeleteLayerCommand` | The deleted layer itself (`LayerData`: id, name, properties, tiles) and its index |
| MoveLayer (7) | `MoveLayerCommand` | id, index before / after |
| RenameLayer (8), Visibility (9), Lock (10), Opacity (11), Blend (12) | `PropertyCommand` | `LayerInfo` before / after |
| MergeDown (13) | `MergeDownCommand` | The upper layer itself, and the lower layer's pixels and opacity before the merge |
| Flatten (14) | `FlattenCommand` | Every layer before (hidden ones too), the active id, and the flattened layer |

These are not recorded:

* selecting the active layer, which is navigation and not an edit;
* operations that fail (locked, hidden, last layer, memory, and so on);
* cancelled strokes;
* strokes still in progress;
* strokes that change no pixel (for example entirely outside the canvas).

## Order

Steps are recorded in the order of the operations. Every recorded operation
first commits the strokes that have already ended and are still queued for
the render thread, so a stroke drawn just before an opacity, blend or rename
change is the step before it. A stroke still in progress is not cut: it
becomes a step when it ends.

## Grouping

* **A stroke is one step.** The brush engine already keeps a copy of each
  touched tile from before the stroke, for exact cancel. When the stroke is
  committed, a hook gives the history those tiles and the rectangle the stroke
  actually touched in each one. The patch is built once, at the end, not per
  dab.
* **A slider drag is one step.** Consecutive opacity changes of the same layer
  are absorbed into one step, which keeps the value from before the drag. They
  are absorbed only while nothing is redoable, so a change after an undo is
  always a new step.
* **Interruptions.** If an operation arrives while a stroke is in progress (a
  layer change, an undo, the pause-save of a backgrounded app), the stroke is
  first committed with what was drawn so far. It becomes one complete step, and
  the rest of that gesture paints nothing. A step is never half of an
  operation.

## Pixel restoration: `PixelPatch`

Each touched tile stores, for the touched rectangle only:

* `before XOR after`, as 32-bit words (premultiplied RGBA8, so alpha
  included), run-length coded as `[zeros, n, n literal words]…`. Pixels the
  stroke did not change are zero after the XOR and cost almost nothing.
* Whether the tile existed before and after: undoing a stroke on empty canvas
  releases the tile again, so memory goes back too.
* FNV-1a 64-bit hashes of the rectangle before and after.

Applying the patch:

1. **Verify every tile first.** The rectangle's hash must equal the recorded
   "after" hash for undo, or the "before" hash for redo.
2. **Only then XOR.** XOR is its own inverse, so the same data restores the
   pixels exactly in both directions. There are no approximations: the result
   is bit-identical (colour and alpha) to the recorded state.

If any tile does not match, the step is **refused**:

* nothing is changed, the history is cleared, and the user is told;
* the canvas is never "restored" into something wrong.

This can only happen if pixels were changed outside the history, which the
code paths prevent: an import clears the history.

Why not the alternatives:

* *Full snapshots* cost the whole canvas per step.
* *Re-rendering strokes* is not exact (randomised grains, wet edges, history
  of the tile) and gets slower as the history grows.
* *Before-only tile copies* are 2–10 times larger than XOR+RLE for typical
  strokes.

The patch works on the CPU tiles, which are the authoritative content. The
GPU copy follows through the usual dirty-region upload. The history therefore
holds **no GPU resources** and never needs a GPU readback.

## Layers

Layer commands move the real `LayerData` objects between the stack and the
history. They never copy them and never re-create them:

* a deleted layer comes back with the same id, name, properties and tiles;
* undoing a merge puts the upper layer back and swaps the lower layer's
  original tiles back in;
* undoing a flatten restores every layer, hidden ones included, and the active
  layer.

After an undo or redo:

* every tile of a restored layer is marked unsaved, so the next incremental
  save rewrites its file exactly;
* the composite is refreshed: changed rectangles at once on the active layer,
  whole tiles progressively on others (Block 6).

## Memory

* **Budget.** The default is 128 MiB and 100 steps. The app sets the byte
  budget to 1/32 of device RAM, clamped to 32–192 MiB: 64 MiB on a 2 GB phone,
  128 MiB at 4 GB, 192 MiB from 6 GB. This is on top of the canvas budget (¼
  of RAM).
* **Accounting.** `bytes()` of each step counts:
  * the compressed patch;
  * kept layers' tiles (256 KiB each);
  * merge / flatten copies.

  The total is in the status (`historyBytes`) and in the panel.
* **Trimming.** When the budget or the step limit is exceeded, the oldest
  steps are dropped (`historyDropped`). The newest step is always kept,
  however large, so the last action can always be undone. Trimming only
  releases history data and never touches the canvas.
* **Typical sizes** (host benchmarks, see the report):
  * a 24-sample stroke of 8–56 px on 2048×1536: about 120 KiB;
  * a 48 px stroke across 32 tiles of an 8192² canvas: 2.2 MiB (the canvas is
    256 MiB);
  * a 500 px stroke across a 4096² canvas: 7.8 MiB.

## History lifecycle

The history is **per session**, and it is cleared when:

* a canvas is opened, closed or loaded;
* a layer's pixels are replaced from outside (import);
* the user chooses *Clear history*;
* a step is refused.

Saving never depends on the history: the project file is always the current
picture.

**Persistence across restarts was examined and not implemented.** Restoring
it would mean:

* writing the patches and the kept layers to disk next to the project;
* versioning them with the project files;
* proving after every reload that the files and the history describe the same
  pixels.

The hashes would make a wrong restore impossible, but a mismatch would
silently lose the history. Saving and loading would also take longer and use
more storage. That is not worth the risk to project integrity in this block.
The format allows it later (patches are position-independent byte arrays).

## UI

* **Undo / Redo** in the top bar:
  * tap: one step;
  * disabled (dimmed) when there is nothing to undo or redo;
  * long-press: the History panel.

  The states come from a lock-free counter (`historyCounts`). It is polled
  every 250 ms and 50 ms after each stroke.
* **The step undone or redone is announced**, for example "Undone: Stroke ·
  Layer 2".
* **History panel** (long-press, or ⋮ → History):
  * the steps, newest first, with the layer they applied to;
  * the current step is highlighted, and undone steps are dimmed and marked;
  * "Start" is the canvas before the oldest kept step;
  * tapping a step undoes or redoes up to it;
  * steps / memory used / budget, how many old steps were dropped, and *Clear
    history* (confirmed).
* **Threading.** Undo, redo and the list run on the layer-operation thread, in
  order with layer operations. They never run on the UI thread.
