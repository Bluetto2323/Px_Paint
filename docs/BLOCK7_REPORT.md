# Block 7 — Undo / redo history: report

## 1. History architecture

The history is an engine component, `engine/History.h/.cpp`. It does not
depend on the UI or the GPU.

* **`History`** keeps:
  * the undo deque (oldest first) and the redo stack;
  * the byte count, the budget and the step limit;
  * statistics (dropped, refused, timings);
  * a lock-free counter `(undo << 32) | redo` for the UI.
* **`HistoryCommand`** is the step interface: `undo()`, `redo()`, `bytes()`,
  `absorb()` and `label()`. A future tool adds a command class; nothing else
  changes.
* **`HistoryTarget`** is the narrow interface commands act through: find,
  insert, remove and move a layer, set the active layer, and notify that
  pixels, properties or the stack changed. `CanvasDocument` implements it.
* **Recording** happens in `CanvasDocument`, under its lock, in the same call
  as the operation. A stroke is recorded once, when it is committed, through a
  hook of the stroke compositor.
* **Layers above the engine:**
  * `Engine::historyOp` (undo / redo n steps, clear);
  * JNI `nativeHistoryOp`, `nativeHistoryCounts`, `nativeHistory`,
    `nativeSetHistoryBudget`;
  * `NativeEngine.history / historyCounts / historyTable / setHistoryBudget`;
  * `pxpaint.core.HistoryState` (pure Kotlin);
  * the UI: `WorkspaceScreen` and `HistoryPanel`.

Rules:

* a new step clears redo;
* undo and redo are strictly LIFO;
* failed operations, cancelled strokes and strokes that change nothing are not
  recorded;
* a stroke in progress is not a step until it ends. Interruptions such as a
  layer change, an undo or the pause-save commit it first, as one whole step;
* every recorded operation first commits strokes that have already ended, so
  the order is chronological.

Details: `docs/HISTORY.md`.

## 2. Command types

| Command | Operations |
|---|---|
| `PixelCommand` | stroke, eraser, clear layer, fill |
| `InsertLayerCommand` | add layer, duplicate |
| `DeleteLayerCommand` | delete layer: keeps the layer itself (same id, name, properties, tiles) |
| `MoveLayerCommand` | reorder (exact index) |
| `PropertyCommand` | rename, visibility, lock, opacity (one slider drag = one step), blend mode |
| `MergeDownCommand` | merge down: the upper layer, the lower layer's original tiles and opacity |
| `FlattenCommand` | flatten: every layer (hidden ones too), the active layer, the result |

Selecting the active layer is navigation and is not recorded.

## 3. Pixel restoration strategy

The history works on the CPU tiles, which are the authoritative content. The
GPU follows through the existing dirty-region upload, so the history holds no
GPU resources and needs no readback.

For each tile a stroke touched, `PixelPatch` stores only the touched
rectangle:

* `before XOR after` as 32-bit premultiplied RGBA words, so alpha is
  included, run-length coded (unchanged pixels cost almost nothing);
* whether the tile existed before and after;
* FNV-1a 64-bit hashes of the rectangle before and after.

Applying it:

1. Every tile is checked against the expected hash.
2. Only then is the XOR applied. XOR is its own inverse, so one record restores
   both directions bit for bit.
3. Tiles that did not exist are released again, but only if nothing is left in
   them.

If a hash does not match, the step is refused. The canvas is untouched, the
history is cleared and the user is told; a wrong "restore" is impossible.

Layer commands move the real layer objects between the stack and the history.
Nothing is copied or re-created, and nothing is flattened. After an undo,
restored layers are marked unsaved in full, so the incremental save rewrites
them exactly.

## 4. Memory management

* **Budget.** The engine default is 128 MiB and 100 steps. The app uses 1/32
  of RAM, clamped to 32–192 MiB (64 MiB at 2 GB, 128 MiB at 4 GB, 192 MiB
  from 6 GB). This is on top of the canvas budget.
* **Accounting.** Each step reports its bytes (patch, kept tiles). The total
  is in the status and the panel.
* **Trimming.** The oldest steps are dropped beyond the budget or the step
  limit. The newest step is always kept. Trimming releases only history data:
  the canvas is never touched, and no GPU resource is involved.
* **No snapshots.** There are no full-canvas snapshots. Patches cover touched
  rectangles only, and new tiles are released on undo.
* **Measured** (CI, run 41712804298):
  * a 48 px stroke across ~32 tiles of an 8192² canvas: 2.2 MiB (the canvas is
    256 MiB);
  * 100 strokes on 2048×1536: 11.1 MiB;
  * 1000 strokes: 124 MiB;
  * a 3 MiB budget with 30 large strokes keeps 3 steps (2.8 MiB) and drops 27.

## 5. History UI screenshots

These are Android 14 emulator screenshots from CI, on the `ci-screenshots`
branch:

* `13-history-panel.png` and `14-history-panel-scrolled.png`: the demo after 3
  undos.
  * The steps are listed newest first, with the layer each applied to.
  * The current step is highlighted; the undone steps are dimmed and marked
    "undone".
  * The header shows "24 steps · 4.2 MB of 77 MB" and *Clear history*.
* `11-layers-panel.png`: Undo enabled and Redo disabled (dimmed). Nothing has
  been undone yet.
* The UI dumps confirm `enabled=true/false` on the two buttons
  (`01-layout.xml`, `13-layout.xml`).

## 6. Automated tests

* **`pe_history_test` (new, CPU): 63/63.** The tests:
  * H1: stroke and eraser undo / redo, one stroke = one step, cancelled
    strokes not recorded;
  * H2: 10 strokes, chronological undo / redo, undo then a new stroke clears
    redo;
  * H3: add, delete (same id and content), duplicate, move, rename, hide, lock,
    opacity (21 changes = 1 step), blend; selection not recorded; queued
    strokes are recorded before a later property change;
  * H4: merge down, flatten (with a hidden layer), clear layer;
  * H4b: all 12 shipped presets (grain, jitter, buildup, wet edge), queued back
    to back: 12 steps, each undone and redone exactly;
  * H5: tile boundaries, 8192² canvas, 400 px brush;
  * H6: memory budget exhaustion and the step limit; the newest step is kept;
  * H7: 100 full undo / redo cycles without drift;
  * H8: save and reload after undo and after redo;
  * H8b: incremental saves between 19 operations, undos and redos, each
    reloaded;
  * H9: interruption (stroke in progress, pause-save, undo during a stroke,
    pixels replaced from outside);
  * H11: the patch format and refusal of a mismatched store;
  * H12: benchmarks.

  It is also clean under AddressSanitizer and UBSan (GCC).
* **GPU suite S13 (new)** runs on GLES and Vulkan, on a 1200×800 canvas
  (single texture) and on 4096² (tiled GPU storage). It covers stroke, eraser,
  delete of a Multiply layer, and merge down. For each, the captured frame
  after undo equals the frame before the action, and the frame after redo
  equals the frame after it. It also runs 20 × (undo 6 + redo 6) with an
  identical final frame and the same GPU resource count.
* **JVM: 114/114.** This includes 8 new `HistoryTest` cases: table parsing,
  malformed tables, step position arithmetic, the RAM budget, strings.
* **Blocks 1–6** all still pass:
  * canvas 37/37, brush 159/159, layer 75/75;
  * the GPU basic, lifecycle, faults, canvas and perf suites on both backends;
  * brush 79/79 per backend (S1–S13).

## 7. Pixel-level verification

* Every CPU comparison is exact: 64-bit checksums of every layer's pixels and
  of the composite, plus the full stack (ids, order, names, visibility, lock,
  opacity, blend, active layer).
* Every expected restoration matched exactly in all tests, including:
  * partially erased alpha;
  * presets with random grain;
  * merge and flatten of layers with Screen / Multiply modes;
  * 100 cycles.
* On the GPU (S13), every pixel of the captured frame was compared: maximum
  difference **0** in all 16 undo / redo comparisons per backend.

## 8. Vulkan and GLES

Results are identical (CI: Mesa llvmpipe GLES, lavapipe Vulkan): S13 passes
on both, with difference 0 and no GPU resources gained or lost. The history
contains no GPU objects, so both backends receive the same CPU tiles.

## 9. Performance benchmarks (CI host, run 41712804298; informational)

| Case | Result |
|---|---|
| 100 strokes 2048×1536 | history 11.1 MiB; recording at stroke end avg 0.47 ms (max 1.5); undo avg 0.80 ms (max 2.8); redo avg 0.98 ms (max 3.2) |
| 1000 strokes | 124 MiB; recording avg 0.54 ms (max 2.3); undo avg 0.97 ms (max 3.6); redo avg 1.0 ms (max 3.8) |
| 500 px stroke on 4096² | 7.8 MiB; recorded in 11 ms (at stroke end); undo 7.9 ms; redo 26 ms (including recomposite) |
| 20 layers (op / undo) | delete 28 / 30 ms, merge 27 / 52 ms, flatten 77 / 52 ms, redo flatten 20 ms |
| 50 layers (op / undo) | delete 60 / 62 ms, merge 22 / 105 ms, flatten 130 / 106 ms; history 254 MiB |

Undo to the next presented frame (S13), including GPU upload:

| Backend | Canvas | Stroke | Eraser | Delete layer | Merge down |
|---|---|---|---|---|---|
| GLES | 1200×800 | 2.7 ms | 0.7 ms | 7.0 ms | 8.3 ms |
| GLES | 4096² | 10.3 ms | 1.0 ms | 29 ms | 41 ms |
| Vulkan | 1200×800 | 6.7 ms | 4.9 ms | 11 ms | 12 ms |
| Vulkan | 4096² | 15 ms | 3.1 ms | 38 ms | 48 ms |

* **GPU synchronisation:** none added. The history never waits on the GPU,
  and undo uses the existing dirty-tile upload. GPU resources do not change
  (6 → 6 at 1200×800).
* **Drawing latency:** there is no work per dab. Recording happens once, at
  stroke end, when the finger lifts.
  * The perf suite measured 27.6 ms (GLES) and 27.3 ms (Vulkan) input to
    present. Block 6 measured 19.3 / 18.6 ms, but on a slower runner: in this
    run the unchanged Block 6 layer benchmarks are also 30–60 % slower than in
    the Block 6 report.
  * A same-machine comparison of Block 6 and Block 7, alternated 3 times
    locally, gave 34.2 ms and 34.9 ms average (noise).

## 10. Known limitations

See `docs/KNOWN_ISSUES.md` KI-12:

* no test on a phone yet;
* the history lasts for the session only; persistence across restarts was
  examined and left out to protect saving;
* recording costs about 11 ms (one frame) at the end of a huge stroke on the
  host, unmeasured on the A33;
* big layer steps hold a lot of memory, and the newest step is kept even
  beyond the budget;
* a refused step clears the history (safety net, with a message);
* the background colour and the view are not part of the history.

## 11. APK and Actions

Debug APK `PxPaint-block7-debug-<sha>.apk`, version 0.7.0-block7. All three
CI jobs pass (APK + unit tests + lint, host tests on both backends, emulator
screenshots); see the chat report for the final run.
