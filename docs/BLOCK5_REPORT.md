# Block 5 — Professional brush library: report

## 1. Brush architecture

```
assets/brushes/builtin.pxb ──► BrushPresetFormat (Kotlin) ──► BrushLibrary (immutable state)
filesDir/brushes/library.pxb ◄──── encode/decode ───────────┘      │ effective(tool)
                                                                    ▼
                         BrushEncoding.params/flags (21 floats + bits) ──► JNI decodeBrush
                                                                    ▼
      BrushEngine ─► StrokePath (spline, spacing, stroke direction) ─► StrokeCompositor
                                                   │ classic round? ─► Block 3/4 rasteriser (unchanged)
                                                   └ otherwise ──────► rasterGeneric: hardness falloff,
                                                       roundness/angle, tip stamps, canvas grain, seeded
                                                       jitter, max | buildup mask; wet-edge transfer at composite
      RasterStore tiles ─► dirty-rect upload ─► Vulkan / GLES display (unchanged)
      BrushPreview (same StrokePath + StrokeCompositor, private CPU raster) ─► library / editor previews
```

* **Presets are data.** The 12 built-ins live in an asset text file, and user
  presets, customisations, favourites, recents, selection and quick values live
  in one library file. The UI only lists what the library returns.
* **One rendering path.** There is no second pipeline: the Block 3/4 stroke
  path and compositor gained a generic dab rasteriser, and the GPU side is
  untouched. A brush with no Block 5 feature uses the old rasteriser
  byte-for-byte.
* **Textures.** Four grains (512², tileable, fixed to canvas pixels) and three
  tip stamps (128²) are generated procedurally once per process. This happens
  on a background thread at engine start, takes about 45 ms on the host and
  uses 1.3 MiB. There are no texture assets to load and no GPU resources.
* **Jitter is deterministic** (hash of stroke seed + dab index): replays,
  batching and the provisional tail give identical pixels.

## 2. Presets (12)

Basic Round · Hard Brush · Soft Airbrush · Pencil · Graphite Pencil · Ink Pen ·
Fine Liner · Marker · Charcoal · Chalk · Textured Brush · Watercolor.
Parameters: `docs/BRUSHES.md`.

## 3. How they differ

| Preset | What makes it different (finger) | With a stylus |
|---|---|---|
| Basic Round | soft edge (hardness 0.7) | size |
| Hard Brush | exact Block 3/4 crisp dab | size |
| Soft Airbrush | fully soft, 12 % flow, **builds up** where the stroke overlaps itself | opacity |
| Pencil | thin, **paper grain**, slightly soft | size + opacity, pressure fills the grain |
| Graphite Pencil | flat lead turned across the stroke, soft, grain, **buildup** (layering) | size + opacity + grain |
| Ink Pen | **flat nib at 35°**: thick/thin with direction (8.4 vs 20.4 px, test L11) | strong taper |
| Fine Liner | 3 px, constant width | none (constant) |
| Marker | chisel nib, 55 % transparent, light wet edge; strokes darken over each other, not over themselves | none |
| Charcoal | irregular **charcoal stamp**, rough grain, jitter, scatter, buildup | size + opacity + grain |
| Chalk | speckled **chalk stamp**, strong rough grain, wide spacing | opacity + grain |
| Textured Brush | **bristle stamp following the stroke**: streaks parallel to the stroke in any direction (L10), canvas grain | size |
| Watercolor | soft, **wet edge** (border 0.55 vs centre 0.33, L8), wash granulation | size + opacity |

At equal size with a finger, 65 of the 66 preset pairs render differently.
Hard Brush and Fine Liner render the same dab and differ by default size,
spacing and pressure behaviour (host test L4, KI-9).

## 4. Customisation and persistence

* **Brush editor:** every parameter of the model, with built-ins kept as resettable
  overrides; the shipped original stays untouched.
* **Library actions:** duplicate (unique names, "copy 2"), rename (user presets,
  cleaned, 40 characters), delete (user presets only; references cleaned),
  reset to original, favourites, recents (12), search by name in both languages,
  category and tags.
* **Quick values:** size and opacity per brush and per tool.
* **Saving:** atomic file writes, debounced and flushed on pause. Damaged files
  are kept aside, and the formats are forward- and backward-tolerant.
* **Migration:** Block 3/4 users land on Hard Brush (the classic rasteriser)
  with their settings. Projects are pixels, so presets cannot change them.

## 5. UI screenshots

From an Android 14 **emulator**, Pixel 6 profile at 1080×2400 (CI job "UI
screenshots", SwiftShader, finger only). The ten screens cover:

* the tool panel;
* the library (all, scrolled, dry media);
* the watercolour editor (preview and try pad, parameters, texture);
* the textured brush editor;
* Settings.

They are on the `ci-screenshots` branch.

The layout dump from this job found a bug that predates Block 5: in rows with a
trailing chevron, a plain `View` measured `WRAP_CONTENT` took the whole width
and hid the text. This affected "Edit brush" and, since Blocks 2 and 4,
"Engine diagnostics" and "Pressure curve and test" in Settings. `Widgets.row`
now keeps the trailing view's size, and the screenshots confirm the fix.

## 6. Automated tests

* `pe_brush_test` (CPU): 159/159 — 113 from Blocks 3–4 plus 46 new checks (L1–L16):
  * L1: the shipped file;
  * L2: classic pixel identity;
  * L3: hardness;
  * L4: determinism and distinctness;
  * L5: batching with jitter;
  * L6: cancellation of all 12 presets, paint and erase;
  * L7: buildup;
  * L8: wet edge;
  * L9: grain anchoring and pressure;
  * L10: stroke-following bristles;
  * L11: nib;
  * L12: pressure for every preset, never vanishing;
  * L13: textured eraser;
  * L14: preview == canvas;
  * L15: textures;
  * L16: timings.
* JVM unit tests: 100/100 — 25 new, covering:
  * preset file, serialisation round trips, validation and clamping, engine encoding;
  * library: selection and switching, recents, favourites, duplicate, rename, delete, override/reset;
  * file round trip, damaged files, Block 4 migration;
  * search, preview stroke specs.
* GPU brush suite: S11 draws all 12 presets through the engine and compares the
  displayed frame with the CPU raster.

## 7. Vulkan and GLES

CI (Mesa llvmpipe GLES + lavapipe Vulkan): every suite passed on both backends.

| Suite | GLES | Vulkan |
|---|---|---|
| basic / lifecycle | 33/33 · 42/42 | 31/31 · 42/42 |
| canvas | 66/66 | 66/66 |
| brush (incl. S11) | 57/57 | 57/57 |
| faults / perf | 15/15 · 3/3 | 15/15 · 3/3 |

In S11 the displayed ink matched the CPU raster with a 0.000 % difference for
all 12 presets on both backends, and the chalk grain is visible on screen.

## 8. Performance (host / CI, informational)

* CI per 8-sample frame at default sizes:
  * round 0.15 ms, hard 0.07, airbrush 1.28, pencil 0.13, graphite 0.22, ink 0.36;
  * fine liner 0.11, marker 0.30, charcoal 0.42, chalk 0.81, textured 1.66, watercolor 0.64.
* CI input → present (perf suite): GLES 20.5 / 22.0 ms and Vulkan 18.5 / 20.7 ms on average, from two runs.
  Block 4 runs on CI machines measured 16.8–23.1 ms, so this is in the same range.

* CPU per 8-sample frame at default sizes (host):
  * round 0.15 ms, hard 0.07, airbrush 1.02, pencil 0.12, graphite 0.21, ink 0.28;
  * fine liner 0.08, marker 0.26, charcoal 0.35, chalk 0.66, textured 1.38, watercolor 0.55.
* Classic brushes: unchanged from Block 4, since they use the same code.
* Input → present on llvmpipe, same machine (GLES perf suite): Block 4
  28.2 / 29.3 ms, Block 5 28.6 / 28.1 ms, within noise.
* Brush switching only selects data. Previews render on a background thread
  and are cached. Textures are built once.

## 9. APK and Actions

All three jobs are green: Debug APK (`PxPaint-block5-debug-<sha>.apk`,
0.5.0-block5), Engine host tests, and UI screenshots (emulator).

## 10. Known limitations

See `docs/KNOWN_ISSUES.md` KI-9 (no hand test on a phone yet, stylus behaviour
synthetic only, Fine Liner = Hard Brush shape at equal size without pressure, no
fluid simulation, 512 px grain repeat, CPU cost of large stamped brushes).
