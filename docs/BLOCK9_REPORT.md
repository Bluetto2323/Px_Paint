# Block 9 — Motion design system: report

Details: `docs/MOTION.md`.

## 1. Architecture

* **`core/Motion.kt`** is pure Kotlin, tested on the JVM. It holds:
  * the tokens (durations, curves, distances, scales, spring);
  * the levels Full / Reduced / Off, combined with the system animator scale;
  * the startup timeline, which never delays the app;
  * the save-status machine.
* **`ui/Motion.kt`** provides `enter`, `exit`, `pop`, `nudge`, `settle`
  (FLIP) and `fadeTo`. They use native ViewPropertyAnimator / ValueAnimator /
  StateListAnimator only. Nothing new is added: no dependency, no WebView.
  * Each helper ends in the exact final state.
  * Animating a property cancels only that property's previous animation, so
    a pulse never freezes a move.
* **`ui/Indicators.kt`** contains the one loading ring and the save indicator.
  **`ui/BrandIntro.kt`** is the startup animation.
* **Engine:** the painting engine is untouched except for one status field,
  `compositePending`, used by the heavy-compositing indicator.

## 2. Splash

* **Android 12+.** The system splash shows the official mark (the launcher
  foreground vector, unchanged) on the app background. Android 13+ uses a
  light variant on the next start when the app is light.
* **Hand-over.** The intro draws the same mark at the same size and place, and
  the system splash view is removed at once. The CI recording first showed a
  dip there: the default fade, plus the mark redrawn from nothing. Both are
  fixed.
* **Brand intro (≈1.2 s), then the gallery:**
  * a soft ember light and six sparks run along the stroke;
  * the wordmark rises;
  * before Android 12 there is no system splash, so the tile fades in and the
    stroke draws first.
* **No artificial delay.** When the gallery has its data, the intro leaves at
  once, in any phase. After that point it adds only its 260 ms exit.
* **Slow starts.** If the reveal ends before the app is ready, a loading ring
  appears.
* **Measured on the CI emulator** (cold start, debug):
  * activity start (TotalTime): 1.96–2.20 s;
  * normal start: ready 1.19–1.23 s after the intro began, exit at once;
  * simulated 2.5 s start: loading ring at 1.6 s, ready at 2.65 s, then the
    exit.

## 3. Animation inventory

| Area | Animation |
|---|---|
| Startup | Splash hand-over, light and sparks, wordmark, loading ring, exit; then gallery cards rise in, staggered |
| Navigation | Settings / editor / pressure: shared axis X, forward and back |
| Project opening / closing | Container transform: the card thumbnail grows to the canvas area and cross-fades to the canvas. Back: the gallery fades over, the project card pulses |
| Workspace | Chrome entrance once: top bar, dock / rail, sliders, pinned panel from its side |
| Toolbar | Press scale; selected tool pop; undo / redo icon nudge |
| Panels | Bottom sheet, side sheet and popover each enter and leave their own way; swipe-down spring-back; only newly opened panels animate |
| Brushes | Category list fade (staggered), selected row pop |
| Colour | Swatch pop on a new colour, eyedropper loupe pop, palette via the panel |
| Size / opacity | Value bubble fades in with the drag; sliders fade back after a sheet closes |
| Layers | FLIP: insert / duplicate grow in; reorder and the gaps left by delete / merge spring into place; visibility fade; active row pulse; merged layer pulse; thumbnail cross-fade |
| Canvas | Fit and rotation snap pulse the zoom chip; the snap gives a haptic tick. The canvas content and strokes are never animated |
| Loading / saving | Ring (determinate "layer n of m" when real); save dot → ring → check → fade; failure nudge, tap to retry; compositing ring |
| Messages | Rise in, fade out; replacements swap |

Not animated, because they don't exist: selection and transform tools, and
import / export.

## 4. Recordings and screenshots

All from the CI emulator, on the `ci-screenshots` branch:

* `60-startup.mp4`, `62-startup-slow.mp4`, `63-startup-loading-state.png`;
* `65-project-open-close.mp4`, `66-project-opened.png`;
* `68-panel-transitions.mp4`;
* `69-reduced-motion-color.png`;
* the screenshots of Block 8, retaken.

The workflow keeps system animations off for stable screenshots. The motion
section turns them on at 1× for its own recordings.

## 5. Performance

* **Drawing.** The input path and the render thread are untouched, and
  nothing animates inside a stroke.
  * Engine perf suite (CI host, this run): input → present 19.2 ms GLES,
    18.4 ms Vulkan.
  * Block 8 runs measured 16.6–25.8 ms depending on the runner.
* **UI frames** (`dumpsys gfxinfo`, CI emulator with SwiftShader software
  rendering): 15 rapid open / close cycles of Layers, Colour and Library
  each, with motion off and then full.

  | Motion | Frames | p50 | p90 | p95 | p99 |
  |---|---|---|---|---|---|
  | Off | 303 | 69 ms | 150 ms | 250 ms | 450 ms |
  | Full | 293 | 85 ms | 200 ms | 350 ms | 850 ms |

  The emulator renders the UI in software, so most frames are slow with or
  without motion. The animations add drawing work on it. These numbers are not
  representative of a phone; on the A33 they are unmeasured.
* **Allocations.**
  * The intro preallocates its paths, paints and shader.
  * The indicators' animators exist only while visible and attached.
  * Only transform and alpha properties are animated, so there are no layout
    passes during animations.

## 6. Reduced motion

*Settings → Accessibility → Animations*:

* **Full.**
* **Reduced:** fades of at most 100 ms only. No movement, scale, springs or
  sparks; the startup shows still frames that fade.
* **Off:** instant changes, still indicators.

Behaviour:

* The old "Reduce motion" switch migrates to Reduced.
* A system animator scale of 0 forces Off.
* The system scale applies to every animation.
* There is no flashing: no animation exceeds 3 changes per second, and fades
  never alternate.

## 7. Automated tests

* **JVM 137/137**, including the new `MotionTest` (7 tests):
  * levels and the system scale;
  * durations of every level, responsive tokens, stagger limits, curve
    validity;
  * spring endpoints, overshoot under 5 %, settling;
  * the startup timeline never waiting for the reveal;
  * the save-status machine (unsaved, saving, saved hold, failed until a
    success, edited during the hold, skipped save);
  * migration of the old switch.
* **Engine:** all 21 host suites on GLES and Vulkan, including
  `pe_layer_test` 81/81, `pe_history_test` 63/63 and `pe_brush_test`
  159/159.
* **Emulator:**
  * startup, normal and slow;
  * project opening and closing;
  * 30 rapid panel open / close cycles twice (no stuck panel, no crash);
  * the reduced-motion state;
  * accessibility audit: 325 controls, all labelled; the only small-target
    hits are 4 rows cut off at a panel edge.

## 8. APK and Actions

`PxPaint-block9-debug-<sha>.apk`, version 0.9.0-block9. See the chat report
for the final run.

## 9. Known limitations

See `docs/KNOWN_ISSUES.md` KI-14:

* emulator only; no measurements on a phone or a low-end device;
* on fast starts the intro is cut short by design;
* the container transform lands on an estimated canvas area;
* project closing is a fade, not a shrink back into the card;
* a deleted layer row disappears at once, while the rows around it close the
  gap.
