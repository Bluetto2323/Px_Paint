# PxPaint — Block 4 report: stylus input and pressure

Status: implemented, built, tested in CI on software GPUs (GLES and
Vulkan). **No physical stylus test**: the Galaxy A33 has no active stylus
(KI-8). Finger drawing is unchanged from Block 3.

## 1. Input architecture

```
MotionEvent (UI thread)
  WorkspaceScreen.dispatch ── per action ──> TouchRouter (pure Kotlin, unit-tested)
     tool type per pointer (ToolKind)          decides: stroke / pan / gesture / ignore
     FLAG_CANCELED (API 33)                    by pointer ID, finger mode, tool;
                                               palm rejection, pinch-cancel (StrokeGate)
  RouterActions (executes with the event)
     StylusInput.append(event, pointerId)  -> SampleBuffer: every historical sample,
       axes only if the device reports them   then the current one; 6 floats each:
       (InputDevice.getMotionRange)            x, y, t, pressure, tilt, orientation (NaN = none)
     NativeEngine.strokeBegin / strokeSamples / strokeEnd / strokeCancel
Engine (native)
  screen -> canvas (viewport), orientation -> canvas angle
  BrushEngine queue -> render thread:
     PressureFilter (1-euro, sample time) -> StrokePath (spline; pressure, tilt,
     orientation interpolated along it; spacing from the current dab size)
     -> BrushDynamics (curve LUT, size, opacity, ellipse) -> StrokeCompositor
     -> RasterStore -> dirty regions -> GPU (unchanged Block 3 pipeline)
```

No second rendering path: pressure and tilt change the size, coverage and
shape of the same dabs in the same compositor.

## 2. MotionEvent data used

| Data | Use |
|---|---|
| `getToolType(i)` | FINGER, STYLUS, ERASER (stylus eraser end, always erases), MOUSE (paints like a stylus, no pressure), PALM (treated as a finger, never as a stylus) |
| `getPointerId` / `findPointerIndex` | strokes, pans and gestures follow pointer IDs |
| `getHistorical*(i, h)` | every batched sample, oldest first, with its own pressure/tilt/orientation |
| `AXIS_PRESSURE` | stylus only, if `getMotionRange` exists; normalised with the reported range, clamped 0..1 |
| `AXIS_TILT`, `AXIS_ORIENTATION` | stylus only, if reported; both needed for tilt shaping |
| `AXIS_DISTANCE` | shown in the stylus test area (hover); not used for drawing |
| `ACTION_HOVER_*` | stylus detection and live readings before contact |
| `FLAG_CANCELED` (API 33) | a pointer the system cancelled (palm) cancels its stroke |
| `InputDevice` (`SOURCE_STYLUS`) | list of styluses the system declares, with their axes |

A finger's pressure is never read. Missing axes are NaN end to end.

## 3. Pressure processing

1. **Normalise** (`PressureNormalizer`): `(raw − min) / (max − min)` from the
   device's motion range, clamped to 0..1; nominal 0..1 if the range is
   degenerate.
2. **Smooth** (`PressureFilter`, native, per stroke): 1-euro filter on the
   MotionEvent timestamps. Resting cutoff 30 Hz (smoothing 0) to 1.5 Hz
   (smoothing 1), +1 Hz per unit/s of pressure change, derivative cutoff
   10 Hz. The first sample passes unchanged. Measured: a 0.2 → 0.9 step at
   240 Hz reaches 0.8 within 3 samples (12.5 ms); sigma 0.02 noise drops to
   39 %; a 1 s press lags at most 0.012.
3. **Interpolate** linearly between samples along the spline.
4. **Response**: sensitivity gamma `p^(2^(−1.5·s))`, then a monotone cubic
   (Fritsch–Carlson) through two user points; sent as a 257-entry table.
5. **Map**: size `D × (min + (1 − min)·c)`, dab opacity `min + (1 − min)·c`,
   each optional and independent; floors 2 % / 5 % so no value makes a stroke
   disappear. Spacing follows the current size.

## 4. Tilt

With tilt and orientation reported and the option on, the dab is an
antialiased ellipse along the stylus direction, major axis
`r × (1 + 1.5 · strength · sin tilt)`. Orientation is converted from screen
to canvas space, so the shape follows the pen whatever the view rotation.
Without either axis the dab stays round (nothing simulated). On the A33 the
option is shown as unavailable.

## 5. UI changes

* Brush panel: new *Stylus* section — status line, *Pressure controls
  size* + *Minimum size*, *Pressure controls opacity* + *Minimum opacity*,
  *Tilt shapes the brush* (disabled with a reason when no stylus reports
  tilt), link to the pressure screen. Settings are saved per tool.
* Settings → Gestures: *One finger*: Draws / Moves the canvas / Gestures
  only (replaces Block 3's switch; the old value is migrated).
* Settings → *Stylus & pressure* → new screen: detected styluses and their
  axes, a test area plotting the stylus's reported pressure vs the brush
  response, a curve editor (two draggable points, presets Linear / Soft /
  Firm / S-curve), sensitivity, smoothing, reset.

## 6. Automated tests (CI run 41619421712 + docs commit)

* `pe_brush_test` 113/113: the 79 Block 3 checks unchanged, plus P1–P8.
  P1 is backward compatibility: dynamics with no pressure give
  byte-identical pixels. The others cover pressure → size (0 / 0.25 / 0.5 / 1,
  clamping, curve, invalid curve), never vanishing, interpolation within one
  segment and across dense historical samples, pressure → opacity (with
  self-overlap), smoothing (step, noise, lag), tilt and orientation (round
  when missing, ellipse size and direction, angle wrap), stylus → finger
  transitions, cancellation with pressure and tilt, and batching
  determinism.
* Host `brush` suite 51/51 on GLES and Vulkan, incl. S10. On screen the
  pressure ramp widths are 10 / 24 / 38 px. A finger stroke right after it
  is drawn at the full 40 px. 50 % pressure gives 60 % opacity. A tilted
  dab is 46 × 20 px horizontal with the view rotated 90°.
* All other suites unchanged and passing (`basic`, `lifecycle`, `faults`,
  `canvas`, `perf`, `pe_canvas_test`).
* JVM unit tests 75/75. They include `PressureTest` (normalisation, tool
  types, curves, sensitivity, tables) and `TouchRouterTest` (finger modes,
  stylus and eraser, palm rejection both ways, stylus ↔ finger,
  pinch-cancel, FLAG_CANCELED, Move/Eraser tools, interruption), plus
  migration of the settings.

## 7. Performance

* Brush CPU time per 8-sample frame (CI runner): 4 / 40 / 300 px =
  0.08 / 0.10 / 0.96 ms without dynamics; with pressure + tilt (elliptical
  dabs, smaller dabs → more of them) 0.25 / 0.30 / 1.89 ms.
* Finger drawing latency, same machine, Block 3 vs Block 4 (`perf`,
  llvmpipe, two runs each): 27.6 / 28.5 ms vs 28.2 / 29.3 ms average —
  within run-to-run noise. CI: 23.1 (GLES) / 22.4 (Vulkan) ms.

## 8. Device limitations

See KI-8: no stylus on the A33, so pressure, tilt, eraser end and palm
rejection with a real pen are untested; the A33 checklist verifies finger
behaviour, finger modes and the "unavailable" states.
