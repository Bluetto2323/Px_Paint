# Layers and compositing (Block 6)

## Architecture

```
CanvasDocument
  layers_      bottom → top; each: LayerInfo (id, name, visible, locked, opacity, blend) + its own RasterStore
               (256 px tiles, premultiplied sRGB RGBA8, sparse: only tiles with content exist)
  composite_   visible layers composited over the backdrop (paper or transparency)
               → uploaded to the GPU by dirty regions, exactly as the single raster before
  BrushEngine  strokes go into the ACTIVE layer's RasterStore only
```

**Compositing runs on the CPU** (`canvas/Compositor`) in a single place:

* Vulkan and OpenGL ES show bit-identical images.
* No per-layer GPU textures: the GPU resource count does not depend on the
  number of layers (S12: 6 resources before and after 9 layers).
* No GPU readback, ever.

**Composite cache and dirty regions:**

* **Strokes** on the active layer recomposite exactly their dirty rectangles in
  the same frame.
* **Property changes** (visibility, opacity, blend, move, delete, merge) mark
  only the tiles where that layer has content. Imports, duplicates and paper
  changes mark the tiles concerned.
* **Stale tiles** are recomposited by the render thread within a time budget:
  about 3 ms per frame while painting, 8 ms otherwise. Large documents stay
  responsive and the display catches up over a few frames.
* **Empty tiles:** where no layer has content, the composite tile is released,
  and the renderer's paper is exactly what shows there.

**Ordered with strokes.** Layer operations first apply queued stroke input and
commit a stroke still in progress. A stroke always lands on the layer it
started on.

## Colour space and blend formula

* **Storage:** 8-bit sRGB-encoded, premultiplied alpha.
* **Blending:** on the sRGB values, as Photoshop and Procreate do by default, in
  32-bit float with one rounding per output pixel.

The formula is W3C Compositing and Blending Level 1. With `cs, as` the layer
pixel (premultiplied, × layer opacity) and `cb, ab` the backdrop:

    co = cs·(1 − ab) + cb·(1 − as) + as·ab·B(Cb, Cs)      Cs = cs/as, Cb = cb/ab
    ao = as + ab·(1 − as)

| Mode | B(Cb, Cs) |
|---|---|
| Normal | Cs |
| Multiply | Cb·Cs |
| Screen | Cb + Cs − Cb·Cs |
| Overlay | Cb ≤ ½ ? 2·Cb·Cs : 1 − 2·(1−Cb)·(1−Cs) |
| Darken | min(Cb, Cs) |
| Lighten | max(Cb, Cs) |
| Add | min(1, Cb + Cs) |
| Difference | abs(Cb − Cs) |

**Transparent pixels:**

* A transparent layer pixel leaves the backdrop unchanged.
* Over a transparent backdrop every mode shows the layer as is: Multiply over
  nothing is not black, and there are no halos.

**Background:** the bottom layer's backdrop is the paper colour when the canvas
background is opaque, so Multiply and the other modes blend with the paper
exactly as displayed. With a transparent background the backdrop is
transparent.

**Layer opacity** multiplies the layer's alpha at compositing time; it is
independent of the brush opacity, which is stored in the layer's pixels.

## Operations

* **Basic:** add (above the active layer), delete (never the last one),
  duplicate (pixels and properties, unlocked copy), rename, move to an index,
  visibility, lock, opacity, blend mode, active layer, clear (active layer).
* **Merge down:** the upper layer is composited onto the lower one as an
  isolated pair.
  * The result keeps the lower layer's id, name and blend mode, with both
    opacities baked in (opacity 100 %).
  * For a Normal upper layer the picture is unchanged.
  * For other modes it is unchanged wherever the lower layer is opaque.
    Elsewhere the upper layer now blends with the lower one only, as in every
    painting application.
* **Flatten image:** the visible layers are composited into one layer, which
  keeps the bottom layer's id; the UI asks for confirmation and says how many
  hidden layers will be deleted.
  * The result is composited over transparency: the paper stays the background.
  * The picture is unchanged with Normal layers. A non-Normal layer that blended
    with the paper changes where nothing opaque is under it.
* **Refused, with a message:**
  * painting on a hidden or locked layer;
  * merge down of the bottom layer, or with a hidden or locked layer involved;
  * flatten with a visible locked layer;
  * more than 100 layers;
  * operations beyond the memory budget (¼ of RAM, the canvas budget);
  * unknown ids, bad values.

## Project format (version: `pxlayers 1`)

```
<project>/project.properties      unchanged (size, paper, name, dates)
<project>/layers.pxlayers         manifest: width, height, active, then [layer <id>] sections bottom → top
                                  (name, visible, locked, opacity, blend)
<project>/layer-<id>.pxtiles      each layer's tiles (the Block 3 tile format, compressed)
<project>/thumb.png               composite thumbnail (gallery)
```

**Saving:**

* Only layers whose tiles changed are rewritten, each file atomically
  (temp file + rename).
* A new layer without pixels still gets its (empty) file.
* The manifest is written last, atomically: until it is replaced the previous
  manifest and files form a complete project.
* Files of removed layers are deleted after it.

**Migration:**

* A project without a manifest opens as before, from `content.pxtiles`
  (Blocks 3–5) or `content.pxops` (Block 2), as the single layer "Layer 1".
* The first save writes `layer-1.pxtiles` and the manifest. Only then are the
  old content files deleted.
* Nothing is flattened or lost. New projects are layered from creation.

**Damaged files:** a damaged or inconsistent manifest (version, size, ids,
blend modes), or a missing or damaged layer file, makes the project refuse to
open with a message. A partial picture is never shown and then saved over the
original.

## Thumbnails

Layer thumbnails are box-filtered downsamples of that layer's own tiles (CPU,
visibility ignored, transparency preserved). They are rendered on a worker
thread and cached until the layer's pixel version changes: about 0.3 ms for a
full 2048² layer at 96 px on the host.
