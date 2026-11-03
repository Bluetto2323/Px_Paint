# Block 8 — Adaptive workspace redesign: report

Details of the design: `docs/UI.md`.

## 1. UI architecture

* **Native views only:** no WebView and no new dependencies. The engine is
  untouched except for one read-only call, `sampleColor`, used by the
  eyedropper.
* **Pure logic** lives in Kotlin and is tested on the JVM:
  * `LayoutPolicy`: four layouts from the real window size, panel style,
    pinning, panel width;
  * `DesignTokens`: colours, spacing, radii, icon / touch / panel sizes,
    states, motion;
  * `Gestures`: finger taps, quick pinch;
  * `ColorMath`.
* **Views:**
  * `QuickSlider` with its value bubble;
  * `ColorWheel` and `ColorPanel`;
  * the existing Layers, Library and History panels, presented by the new
    panel host in `WorkspaceScreen`.

  The panel host presents a panel as a bottom sheet, side sheet, popover or
  pinned resizable panel, depending on the layout.
* **Configuration changes** rebuild the chrome only. The engine, the canvas,
  the history and the open panel stay.

## 2–5. Screenshots

The screenshots come from the CI emulator: Android 14, Pixel 6 profile,
SwiftShader. They are on the `ci-screenshots` branch.

| Layout | Files |
|---|---|
| Phone portrait | `01` canvas, `02` brush panel, `03` library, `04`/`05` colour, `06` layers, `07` history, `08` interface hidden, `10`–`12` stroke → undo → redo (toolbar buttons tapped on the emulator) |
| Phone landscape | `20` canvas (rail + sliders), `21` colour, `22` layers (side sheets) |
| Tablet portrait | `30` canvas, `31` colour popover, `32` pinned Layers |
| Tablet landscape | `40` canvas, `41` library popover + pinned Layers, `42` brush popover + pinned Colour |
| Other windows | `50` small window (360 × 640 dp), `51` large display (1920 × 1080 dp) |

The tablet and other-window layouts are **emulated windows**: `wm size` and
`wm density` on the phone emulator, giving 800 × 1280 dp, 1280 × 800 dp and so
on. They are not tablet hardware.

## 6. Adaptive layout logic

Everything is computed in dp of the current window, divided by the interface
size. The form factor uses the smallest width (≥ 600 dp is a tablet). Device
names are never used.

| Layout | Condition | Toolbar | Panels |
|---|---|---|---|
| Phone portrait | the default | bottom dock | bottom sheet, ≤ 55 % of the height |
| Phone landscape | landscape, height < 480 dp | side rail | side sheet, ≤ 45 % of the width |
| Tablet portrait / landscape | smallest width ≥ 600 dp and the window ≥ 600 × 480 dp | top toolbar | popover, or one pinned panel |

* A panel is pinned only if the canvas keeps at least 480 dp of width.
* The pinned panel can be resized between 280 and 480 dp.
* 15 window shapes are tested, including:
  * small and large phones;
  * a foldable, folded and unfolded;
  * tablets and a large display;
  * split screen (half and third);
  * short and tiny freeform windows;
  * extra-large interface size.

## 7. Gesture behaviour

| Gesture | Action |
|---|---|
| 2-finger tap | undo |
| 3-finger tap | redo |
| 4-finger tap | hide / show the interface (with the system bars) |
| quick pinch-in | fit the canvas |
| stylus with its side button held | eyedropper |
| tap on the canvas while a panel floats | closes the panel |

All of them are configurable in *Settings → Gestures*.

* **No conflict with painting.** A tap needs every finger down within 160 ms
  and lifted within 320 ms, with no finger moving more than 18 dp. A stylus
  or a committed stroke cancels it. A stroke started by the first finger is
  cancelled by StrokeGate, so the tap leaves nothing on the canvas.
* **Pan, zoom, rotate and snap** behave as in Blocks 2–4.
* **Verification:** the JVM tests cover taps, rejections and the quick pinch.
  The emulator's input is a single synthetic pointer, so multi-finger gestures
  are not verified on a device.

## 8. Accessibility results

* **Contrast** (`WorkspaceUiTest`, every surface, dark, light and high
  contrast):
  * text ≥ 7:1;
  * secondary text, accent and danger ≥ 4.5:1;
  * hints and disabled ≥ 3:1;
  * the translucent chrome over a white and over a black canvas keeps text
    ≥ 7:1.

  The light accent, the light danger colour and both tertiary colours were
  darkened or lightened to meet these thresholds.
* **Touch targets and labels.** The CI audits the UI dumps of 14 screens
  across all layouts:
  * 293 interactive nodes, every one labelled;
  * 289 of them at least 48 × 48 dp;
  * the 4 remaining are rows cut off at the bottom of a scrolling panel in
    the dump, not real small targets.

  The first run found 44 problems: the zoom chip, the 36 dp chips, the colour
  swatches and the squeezed landscape rail. All were fixed.
* **Screen readers:**
  * the quick sliders and the colour wheel are SeekBars with ranges and step
    actions;
  * the pinned panel's resize grip responds to scroll actions;
  * the value bubble is hidden from screen readers, since the slider
    announces the value.
* **Settings** respect large touch targets (56 dp), reduce motion (no
  animations) and high contrast.

## 9. Performance

* **Drawing latency:** the engine's input path is unchanged.
  * CI perf suite in the final pre-report run: input → present 16.8 ms GLES,
    16.6 ms Vulkan.
  * The same machine runs local GPU suites unchanged: all pass.
  * UI work never runs inside a stroke. Slider changes go to the brush
    settings (debounced save), and the eyedropper reads the displayed
    composite without compositing.
* **Chrome rebuild** happens on a rotation, a resize or a pin, never while
  drawing. On the CI emulator (debug build, SwiftShader, logcat):

  | Layout | Median | Max |
  |---|---|---|
  | Phone portrait | 17 ms | 72 ms |
  | Phone landscape | 11 ms | 19 ms |
  | Tablet portrait, Layers panel pinned | 29 ms | 94 ms |
  | Tablet landscape | 29–36 ms | 117 ms (1920 dp, pinned Layers with thumbnails) |

  Unmeasured on the A33.

## 10. Automated tests

* **JVM: 130/130.**
  * The layout policy was rewritten: 10 tests, including the 15 window
    shapes.
  * `WorkspaceUiTest` (new, 14 tests):
    * exact HSV round trip over every 5th byte value;
    * HEX parsing, un-premultiply;
    * the saved palette and its migration;
    * taps and their rejections, quick pinch;
    * contrast, tokens, new settings and their migration.
  * `AppSettingsTest`: the stable key list is updated.
* **CPU:**
  * `pe_layer_test` 81/81, with the new LY12 eyedropper (paper, opaque
    colour picked back exactly, layers / opacity equal the composite, outside
    the canvas, transparent background, semi-transparent paint
    un-premultiplied);
  * `pe_history_test` 63/63, `pe_brush_test` 159/159, `pe_canvas_test` 37/37.
* **GPU** (Mesa GLES and lavapipe Vulkan): basic, lifecycle, faults, canvas,
  brush 79/79 and perf all pass. The brush suite includes the undo / redo
  frame comparisons of Block 7.
* **Emulator:**
  * every layout above;
  * drawing, then undo and redo through the toolbar buttons (`10`–`12`);
  * the accessibility audit;
  * no crash in logcat.

## 11. APK and Actions

`PxPaint-block8-debug-<sha>.apk`, version 0.8.0-block8. See the chat report
for the final run.

## 12. Known limitations

See `docs/KNOWN_ISSUES.md` KI-13:

* not tested on the A33 or on tablet hardware; tablets are emulated windows;
* multi-finger gestures and the stylus button eyedropper are verified only by
  JVM tests;
* reordering layers still uses buttons, not drag and drop;
* one pinned panel at a time;
* sRGB only;
* the audit covers only the states captured;
* the first time the interface is hidden, Android shows its own
  "Viewing full screen" notice.
