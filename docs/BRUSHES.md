# Brush library (Block 5)

## Model

A brush is a set of parameters (`BrushParams` in Kotlin, `BrushSettings` in the
engine). Every parameter has a default that gives the Block 3 round brush; when
a brush uses none of the Block 5 features (`classic`), the engine runs the Block
3/4 rasteriser unchanged (pixel-identical, tested: L2).

| Key | Range | Meaning |
|---|---|---|
| `tip` | round, chalk, charcoal, bristle | dab shape: analytic round/ellipse, or a procedural 128×128 stamp |
| `size` | 1–500 px | diameter on the canvas (maximum when pressure drives size) |
| `opacity` | 0.01–1 | stroke ceiling |
| `flow` | 0.01–1 | coverage of one dab (with `buildup`, how fast paint accumulates) |
| `spacing` | 0.01–2 | dab distance / diameter (from the thinner side for flat dabs) |
| `hardness` | 0–1 | round tip: 1 = 1 px antialiased edge, 0 = fully soft falloff |
| `roundness` | 0.05–1 | minor/major axis (flat nib, chisel) |
| `angle` | degrees | major-axis direction; screen space (`fixed`) or relative to the stroke (`stroke`) |
| `angle_mode` | fixed, stroke | stroke = the dab turns with the stroke direction |
| `size_jitter`, `angle_jitter`, `flow_jitter` | 0–1 | per-dab variation |
| `scatter` | 0–2 | dab centre displaced by up to scatter × diameter |
| `grain` | none, paper, rough, canvas, wash | 512×512 tileable texture fixed to the canvas pixels |
| `grain_depth` | 0–1 | how much coverage the grain can remove |
| `grain_pressure` | 0–1 | stylus pressure fills the grain (stylus strokes only) |
| `accumulation` | stroke, buildup | stroke = a stroke never darkens itself (max); buildup = overlaps add up |
| `wet_edge` | 0–1 | coverage → alpha transfer that darkens the stroke border (watercolour look) |
| `pressure_size`, `min_size`, `pressure_opacity`, `min_opacity`, `tilt`, `tilt_strength` | | Block 4 stylus dynamics |

Jitter is deterministic: a hash of the stroke seed (from its first sample) and
the dab index. Replaying the same input gives the same pixels, whatever the
batching, and the provisional tail matches what gets committed (L4, L5).

Textures are generated on the CPU from fixed seeds, once per process, on a
background thread at engine start (about 45 ms on the CI/host machine, 1.3 MiB).
No asset images are loaded, and the GPU never sees them: the result is in the
raster tiles, which are uploaded as before (dirty rectangles).

## Files

**Built-in presets**: `app/src/main/assets/brushes/builtin.pxb`. The app parses
it; the engine tests (`tools/host_test/PresetFile.h`) read the same file.

```
pxbrushes 1
[preset builtin.round]
name = Basic Round
name.it = Tondo base
category = painting          # sketching | inking | painting | airbrush | dry | custom
tags = round soft general
size = 24
...
```

Lines starting with `#` are comments. Unknown keys and sections are ignored,
missing keys take their defaults, and every value is clamped.

**User library**: `filesDir/brushes/library.pxb`. It is written atomically
(temp file + rename), 400 ms after the last change and when the app pauses.

```
pxbrushlib 1
[state]
selected.paint = builtin.round
selected.eraser = builtin.hard
favorites = builtin.ink,user.lq3x9a
recents = ...
[live paint/builtin.pencil]   # quick size / opacity of a preset for one tool
size = 9
opacity = 0.5
[override builtin.chalk]      # customised built-in (all parameters)
...
[preset user.lq3x9a]          # user preset
name = My chalk
base = builtin.chalk
...
```

A damaged file is kept as `library.damaged.pxb`, and the library restarts from
the built-ins.

**First start with Block 5 (migration).** If the Block 3/4 brush preferences
exist, both tools select **Hard Brush**, which is the classic rasteriser and so
gives the same pixels. They keep their sizes and opacities. Spacing and stylus
settings that differ from the preset are stored as a resettable customisation.
New users start on **Basic Round** for painting and **Hard Brush** for erasing.

Projects contain pixels only, so presets never affect how a canvas opens.

## Built-in presets

| Preset | Category | Tip | Notable parameters |
|---|---|---|---|
| Basic Round | painting | round | hardness 0.7 (soft edge), pressure → size |
| Hard Brush | painting | round | classic Block 3/4 dab (1 px AA edge) |
| Soft Airbrush | airbrush | round | hardness 0, flow 0.12, **buildup**, pressure → opacity |
| Pencil | sketching | round | 5 px, **paper grain** 0.55, pressure → size + opacity, pressure fills grain |
| Graphite Pencil | sketching | round | flat lead (roundness 0.55, **follows stroke**), soft, buildup, strong paper grain |
| Ink Pen | inking | round | **flat nib** roundness 0.4 at 35°, pressure → size (min 10 %) |
| Fine Liner | inking | round | 3 px, constant width (no pressure), classic dab |
| Marker | inking | round | chisel 0.6 at 30°, 55 % opacity, light wet edge, no pressure |
| Charcoal | dry | **charcoal stamp** | rough grain, angle/size jitter, scatter, buildup |
| Chalk | dry | **chalk stamp** | rough grain 0.7, angle jitter, wide spacing |
| Textured Brush | painting | **bristle stamp** | follows stroke (bristle streaks), canvas grain |
| Watercolor | painting | round | soft, **wet edge** 0.9, wash granulation, 60 % opacity |

The watercolour look comes from a wet-edge transfer curve and a granulation
texture. It is not a fluid simulation.
