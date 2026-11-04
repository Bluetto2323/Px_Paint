# PxPaint interface redesign (Block 11)

Minimal like a canvas-first painting app, practical like a phone-first one,
and drawn with PxPaint's own tokens and icons. The painting engine,
rendering, input path, projects and settings are unchanged.

## Architecture

| Part | File | Role |
|---|---|---|
| Themes | `core/DesignTokens.kt`, `core/AppSettings.kt` (`ThemeMode`) | Three complete palettes: Deep Blue, Pure Black, Warm Beige. Glass alphas, high-contrast variants, migration of the old names |
| View theme | `ui/Theme.kt` (`PxTheme`) | Tokens → pixels; `glass()`, `float()` (translucent surface, hairline edge, tinted soft shadow) |
| Workspace | `screens/WorkspaceScreen.kt` | Floating capsules instead of the bar; transient zoom chip; *More tools* menu |
| Brush selector | `screens/BrushLibraryPanel.kt`, `ui/Widgets.kt` (`TileGrid`) | Grid of real previews, filters, search, recents, favourites |
| Layers | `screens/LayersPanel.kt`, `screens/LayerList.kt`, `core/LayerDrag.kt` | Tap / hold-and-drag rows, landing slot, auto-scroll |
| Settings | `screens/SettingsScreen.kt`, `ui/ThemePreview.kt` | Three previews drawn from each theme's tokens |
| Theme switch | `PxPaintActivity.retheme()` | Rebuild with the new tokens; the old interface fades out over it |

Every colour comes from `ColorTokens`. No screen hard-codes a colour, so a
theme reaches every part of the interface:

* toolbar capsules, floating panels, the brush selector, the layer panel;
* buttons, dialogs, menus, sliders, colour controls;
* gallery, settings, text, borders, selection indicators (marching-ants
  handles, accent states).

The canvas paper, the artwork and brush previews (rendered on their own
paper) never change with the theme.

## Themes

| Token | Deep Blue | Pure Black | Warm Beige |
|---|---|---|---|
| Background | #101A2B | #08090B | #D8C8A8 |
| Surface | #17263B | #111317 | #E6D9BF |
| Floating panel (glass) | #20344E | #1B1E23 | #F0E6D3 |
| Accent | #6FA8E8 | #A6B7D4 | **#6F4F2C** (suggested #9B7850) |
| Primary text | #E6EDF7 | #F3F4F6 | #30291F |
| Secondary text | #A9BAD0 | #A2A7B0 | **#5C4F40** (suggested #766957) |
| Border | #354A65 | #30343B | #C6B697 |

* **Contrast.** The suggested beige accent and secondary text fail WCAG AA on
  the beige background (3.0:1 and 3.4:1). They are darkened in the same hue,
  and `WorkspaceUiTest` checks every theme:
  * text ≥ 7:1, secondary ≥ 4.5:1, hints ≥ 3:1, accent ≥ 4.5:1 on the
    background, surface and panel;
  * text ≥ 7:1 and secondary ≥ 4.5:1 on the translucent glass over a white
    or a black canvas.
* **Glass.**
  * Controls over the canvas: the panel colour at 90 %.
  * Panels: 95 %.
  * A hairline edge: a light rim on the dark themes, the border colour on
    beige.
  * A soft shadow, tinted on Android 9+.
* **No blur.** Android can blur what is under a view only for whole windows.
  The canvas is a GPU SurfaceView, and blurring the artwork would cost GPU
  time while painting, so the brief's fallback is used: a translucent surface
  with a subtle edge.
* **Choosing a theme:** *Settings → Appearance → Theme*, three previews.
  * Applied at once, no restart; the previous interface fades out over the
    new one.
  * Saved in the settings.
  * Old values migrate: dark / system → Deep Blue, light → Warm Beige.

## Workspace

**Phone portrait:**

* **Top-left capsule:** gallery, compositing and save state.
* **Top-right capsule:** undo, redo. Long press opens History.
* **Bottom capsule, under the thumb:** Brush, Eraser, Colour, Layers, More
  tools. No labels unless *Tool labels* is on.
* **Edges:** the size / opacity sliders.
* **The rest is canvas.**
  * The zoom / rotation chip appears only while the view changes, then fades
    (tap: fit; long press: canvas menu).
  * The selection bar appears only with a selection.

**Phone landscape:**

* the tools become a vertical capsule on the rail side, centred;
* gallery and history stay at the top corners;
* the sliders take the other edge.

**Tablets:**

* one top-right capsule: undo / redo, then the tools;
* the project name beside the gallery button;
* popovers, or a pinned side panel. The pinned Brushes panel is now the brush
  selector.

**Tool buttons:**

* **Brush / Eraser:** a tap selects the tool. A tap on the active one opens
  its brush selector. A long press opens its settings (size, opacity, editor,
  pressure).
* **More tools:**
  * Select (again: its options), Move canvas, Eyedropper;
  * Brush settings, History, Hide interface;
  * Canvas options: fit, actual pixels, rotation, info, test, clear,
    settings.

  The button shows the extra tool in use (select / move).
* **Distraction-free canvas:** *More → Hide interface*, or a four-finger tap.
  Only a small corner button remains.

## Brush selector

1. Tap the active brush (or the active eraser): a compact floating panel.
2. It shows:
   * search;
   * filters: all, favourites, recent, the categories, mine;
   * a strip of recently used brushes;
   * a grid of tiles with three columns on a phone and more on wider panels.
3. Each tile shows the brush's real stroke, rendered by the engine's brush
   code and cached by `BrushPreviews`, plus its name. The current brush is
   outlined in the accent colour; the star marks favourites.
4. One tap activates the brush. The panel closes by itself (90 ms later, so
   the choice is seen) unless pinned. Drawing continues at once.
5. A long press on a tile opens edit, duplicate, rename, reset, delete.

Presets and settings are the same library as before: no duplicate
definitions, no pictures standing in for previews.

## Layer interactions

| Gesture | Effect |
|---|---|
| Tap a row (thumbnail or name) | Select the layer |
| Tap the selected row | Its options menu (rename, duplicate, move up / down, merge down, lock, delete) |
| Press and hold ~0.28 s, without moving | The row lifts (haptic), follows the finger; the other rows open a gap; an outlined slot shows where it lands; near the panel edges the list scrolls by itself; each new slot ticks |
| Release | The layer moves there: one undoable *Move layer* step; the rebuilt list animates from where the rows were |
| Move before the hold | A normal scroll (never a drag) |
| Eye / ⋯ / lock | Their own action; a press on them never drags |

Opacity, blend mode, merge, duplicate, rename, delete and flatten keep their
controls under the list. Move up / down stay in the menu for accessibility
services.

## Performance

* The canvas, render thread and input path are untouched. UI surfaces are
  ordinary views over the SurfaceView, with no blur.
* Previews are cached (LruCache 16 MiB) and rendered off the UI thread.
* Dragging only changes translations: no layout pass per move. Auto-scroll
  runs on animation frames only while a row is held.
* Measurements: see `docs/BLOCK11_REPORT.md`.
