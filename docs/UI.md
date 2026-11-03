# PxPaint workspace UI (Block 8)

The interface is built with native Android views in code. It uses no
WebView, no XML layouts and no UI libraries. It talks to the engine only
through `NativeEngine`. It is split into pure Kotlin logic, which is
unit-tested on the JVM, and views.

| Layer | Files | Role |
|---|---|---|
| Layout policy | `core/LayoutPolicy.kt` | Picks the layout from the current window: kind, toolbar placement, panel style, slider edge, pinning, panel width |
| Design tokens | `core/DesignTokens.kt`, `ui/Theme.kt` | Colours (dark / light / high contrast), spacing, radii, icon, touch and panel sizes, states, motion |
| Gestures | `core/Gestures.kt`, `core/TouchRouter.kt` | Finger-tap shortcuts, quick pinch, stroke / pan / pinch routing |
| Colour | `core/ColorMath.kt` | HSV / RGB / HEX, un-premultiply, WCAG contrast |
| Views | `ui/QuickSlider.kt` (+ value bubble), `ui/ColorWheel.kt`, `screens/ColorPanel.kt`, `screens/LayersPanel.kt`, `screens/BrushLibraryPanel.kt`, `screens/HistoryPanel.kt` | Controls and panels |
| Workspace | `screens/WorkspaceScreen.kt` | Builds the chrome for the spec, presents panels, routes input |

## Layouts

All values are in dp of the **current window**, divided by the interface
size. This covers split screen, freeform windows, foldables and rotation. The
form factor uses the smallest width (≥ 600 dp is a tablet) and is never based
on a device model.

| Layout | When | Tools | Panels | Sliders |
|---|---|---|---|---|
| Phone portrait | default, and any narrow window | top bar (gallery, title, undo, redo, menu) + bottom dock | bottom sheets, at most 55 % of the height, swipe down or ✕ to close | screen edge (setting, left by default) |
| Phone landscape | landscape, height < 480 dp, width ≥ 480 dp | short top bar + side rail | side sheets on the other side, ≤ 45 % of the width | the edge opposite the rail |
| Tablet portrait / landscape | smallest width ≥ 600 dp and the window at least 600 × 480 dp | one top toolbar: actions left, tools right | popovers under the toolbar, or one pinned panel | screen edge |

* **Tools** are the same group in every layout:
  * Brush, Eraser, Move;
  * Brush library, colour, Layers.

  Only implemented tools are shown: no smudge, selection or transform.
* **Pinned panel (tablets).** Brush, Colour or Layers can stay open beside
  the canvas:
  * use 📌 in the panel header, or *Settings → Panel beside the canvas*;
  * it is allowed only if the canvas keeps at least 480 dp of width,
    otherwise the panel floats;
  * a grip outside its inner edge resizes it between 280 and 480 dp
    (accessibility: scroll actions, ±20 dp); the width is saved.
* **Hide interface.** Turn it on with ⋮ → *Hide interface* or a four-finger
  tap. It also hides the system bars. One corner button, the four-finger tap
  or Back brings the interface back.
* **Configuration changes** (rotation, resize, fold) rebuild only the chrome.
  The engine, the canvas, the history and the open panel stay as they are.

## Brush controls

* **Quick sliders** for size and opacity sit on a screen edge:
  * relative drag, so a touch never jumps the value;
  * moving the finger sideways away from the track gives fine adjustment
    (×0.25);
  * a bubble shows the tip at its real on-screen size (canvas size × zoom)
    or the colour at the chosen opacity, with the value;
  * accessibility: SeekBar role, 1 % steps.
* **Brush panel** (tap the active tool):
  * the current brush with a live engine preview;
  * the recently used brushes for quick switching;
  * size and opacity with values;
  * edit brush;
  * pressure / tilt controls when a pressure stylus is in use, otherwise one
    row to the stylus settings.
* **Brush library**: a long press on Brush or Eraser opens it, as does the
  library button. It is the Block 5 panel (categories, search, favourites,
  recent, previews, quick size / opacity), presented by the layout.

## Colour workspace

* A wheel: hue ring plus a saturation / brightness square.
* Previous and current colour; tap previous to go back.
* Values:
  * HSV or RGB sliders with numbers;
  * a HEX field (`#RRGGBB` or `#RGB`; invalid input is explained and not
    applied).
* Colour lists:
  * the 8 recent colours;
  * a saved palette of up to 30 colours: + saves the current colour, a long
    press removes one (confirmed).
* **Eyedropper**: the panel button, or a stylus touching with its side button
  held.
  * It samples the displayed composite (paper included) with
    `NativeEngine.sampleColor`, which does no compositing work.
  * A loupe follows the finger, and the colour is applied on release.
* Every colour is straight sRGB ARGB, exactly what the brush paints. LY12
  checks that an opaque painted colour is picked back bit for bit.

## Gestures

Gestures are configurable in *Settings → Gestures*.

| Gesture | Action |
|---|---|
| 1 finger | paint, pan or nothing (finger mode, Block 4) |
| 2 fingers | pan, pinch zoom, rotate (rotation optional, with snap) |
| 2-finger tap | undo |
| 3-finger tap | redo |
| 4-finger tap | hide / show interface |
| quick pinch-in (< 350 ms, distance to ≤ 60 %) | fit the canvas |
| tap on the canvas with a floating panel open | close the panel (that touch does not paint) |
| stylus with side button held | eyedropper |

Conflicts with painting are avoided:

* A tap must have all fingers down within 160 ms and lifted within 320 ms,
  with no finger moving more than 18 dp.
* A stylus or a committed stroke cancels it. A pinch that had just started a
  stroke cancels that stroke (StrokeGate), so the tap leaves nothing on the
  canvas.
* A slower pinch is an ordinary zoom.

## Visual system

`DesignTokens` defines:

* **Colours.** Dark is the default; light and high-contrast variants exist.
  Contrast is tested on every surface: text ≥ 7:1, secondary text, accent and
  danger ≥ 4.5:1, hints and disabled ≥ 3:1. The tests also check the
  translucent chrome over a white and over a black canvas.
* **Spacing:** 4 / 8 / 12 / 16 / 24 dp.
* **Radii:** 10 / 16 / 24 dp and pill.
* **Icons:** 18 / 22 / 28 dp.
* **Touch targets:** 48 dp, or 56 dp with *Large touch targets*.
* **Panels:** 280–480 dp, default 320 dp; sheets at most 560 dp wide.
* **Sliders:** 168 × 36 dp, inside a 48 dp touch area.
* **States:** disabled 38 %, dimmed 55 %, selected = accent on a soft accent
  fill.
* **Motion:** 140 / 220 ms, or none with *Reduce motion*.
