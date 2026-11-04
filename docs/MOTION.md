# PxPaint motion design (Block 9)

Motion explains a change: where a panel came from, where a layer went, that a
tap was understood. It never decorates the canvas, never delays drawing, and
never waits for an animation to finish.

## Architecture

| Part | File | Role |
|---|---|---|
| Tokens and policy | `core/Motion.kt` (pure, JVM-tested) | Durations, curves, distances, scales, spring; the levels Full / Reduced / Off; startup timeline; save status machine |
| View helpers | `ui/Motion.kt` | `enter`, `exit`, `pop`, `nudge`, `settle` (FLIP), `fadeTo` over native ViewPropertyAnimator / ValueAnimator, with the tokens' curves |
| Indicators | `ui/Indicators.kt` | `ProgressIndicator` (one loading ring, determinate when progress is real) and `SaveIndicator` |
| Startup | `ui/BrandIntro.kt`, `res/values-v31/themes.xml`, `res/drawable/ic_splash_light.xml` | Android splash, then the brand intro |
| Screens | `PxPaintActivity` | Shared-axis navigation; container transform for opening a project |

Every helper cancels the view's running animation and ends in the exact final
state. Rapid repeated taps never leave a view half faded or offset.

## Tokens

| Token | Value | Use |
|---|---|---|
| MICRO | 90 ms | press feedback |
| SHORT | 150 ms | exits, small fades, message out |
| MEDIUM | 240 ms | panels, screens, springs |
| LONG | 360 ms | container transform (project opening) |
| STAGGER | 28 ms, at most 6 items | gallery cards, list rows |
| LOADING_DELAY | 300 ms | an indicator appears only for longer waits |
| SAVED_HOLD | 1.6 s | "Saved" check, then it fades |

* **Curves:**
  * standard (0.2, 0, 0, 1);
  * decelerate (0.05, 0.7, 0.1, 1) for entering;
  * accelerate (0.3, 0, 0.8, 0.15) for leaving;
  * a damped spring (damping 0.78, under 5 % overshoot) for settling moves.
* **Distances:** nudge 4, shift 12, slide 24 and sheet 48 dp.
* **Scales:** press 0.92, pop 1.10, enter 0.96.

## Levels

The level is set in *Settings → Accessibility → Animations*.

| Level | Behaviour |
|---|---|
| Full (default) | Everything: fades, movement, scale, springs, the startup light. |
| Reduced | Short fades only, at most 100 ms. No movement, scale, springs or particles. The startup shows the mark and wordmark fading in. Replaces the old "Reduce motion" switch, which migrates to Reduced. |
| Off | Every state changes instantly. Indicators are drawn still. |

* The system animator duration scale applies to every animation (all of them
  are native animators).
* A system scale of 0 (developer options, accessibility) forces Off.

## Startup

1. **Android 12+ system splash.** It shows the official mark (the launcher
   foreground vector) on the app background, without an icon plate. On
   Android 13+, the next start uses the light variant when the app theme is
   light (`setSplashScreenTheme`).
2. **Brand intro** (`BrandIntro`). It draws the same mark at the same size and
   place, so the hand-over is seamless. Over about 1.2 s:
   * the tile fades in;
   * the ember stroke draws through it (PathMeasure segment);
   * the wordmark rises;
   * a soft light and six sparks run along the stroke.

   Everything is preallocated; nothing is allocated per frame.
3. **Readiness.**
   * When the gallery has its project list (and its first frame), the intro
     leaves at once, whatever the phase. The exit is 260 ms: fade, mark to
     94 %. The gallery cards then rise in.
   * If the app is not ready when the reveal ends, a loading ring appears.
     There is no artificial delay; the only time added after readiness is the
     exit.
4. **When it runs.** Only on a cold start. Scripted debug states skip it.

## Inventory

| Area | What moves |
|---|---|
| Navigation | Settings, editor, pressure: shared axis X (fade + 24 dp from the side; back from the other side). The outgoing screen stays still under the incoming one (a workspace's SurfaceView is never faded). |
| Project opening | Container transform: the card's thumbnail grows from the card to the canvas area (360 ms, decelerate). It then fades when the workspace is ready, or after 300 ms, revealing the loading card. |
| Project closing | The gallery fades in over the workspace; the project's card pulses. |
| Workspace | Its chrome arrives once: the top bar from above, dock and rail from below, sliders from their edge, a pinned panel from its side. Rotations and resizes rebuild without animating. |
| Toolbar | Press scale (StateListAnimator). The selected tool pops; the undo / redo icons turn and spring back. |
| Panels | Bottom sheet up 48 dp; side sheet from its side; popover drops 8 dp and scales from 96 %. Dismissal goes back the way it came. A swipe-down that is not far enough springs back. Only a newly opened panel animates; updates of an open panel just swap. |
| Sliders | The value bubble fades in with the drag and out after it. Sliders fade back after a sheet closes. |
| Brushes | A category change fades the new list in, staggered. The chosen brush row pops. |
| Colour | The toolbar swatch pops on a new colour. The eyedropper loupe grows in. The palette opens as a panel. |
| Layers (FLIP) | New rows (add, duplicate) grow in. Moved rows spring from their old place: reorder, or the gap left by a delete or merge. Visibility fades the row. The newly active row pulses, as does a layer merged into. Thumbnails cross-fade when updated. |
| Canvas | Fit (quick pinch or chip tap) and rotation snap pulse the zoom chip; the snap also gives a haptic tick. The canvas content is never animated. Selection and transform tools do not exist yet. |
| Messages | Rise in, fade out; a replacing message swaps without re-entering. |

## Loading and saving

| Wait | Indicator |
|---|---|
| App start | Intro loading ring, only after the reveal |
| Project opening | Card with a ring. Determinate "Layer *n* of *m*" when there are several layers (real count), otherwise indeterminate. Shown only after 300 ms. |
| Saving | Save indicator in the top bar |
| Thumbnail generation | Gallery thumbnails cross-fade in when generated |
| Heavy layer compositing | Small ring in the top bar while the engine still has stale composite tiles (new status field `compositePending`), after two polls (≥ 250 ms) |
| Large image import / export | Not features of the app yet: no indicator |

Percentages are never invented.

**Save status** (`SaveTracker`, top bar):

* unsaved: a small dot;
* saving: a mini ring;
* saved: a check that pops, then fades after 1.6 s;
* failed: a warning that nudges; tap it to retry.

There are no dialogs; screen readers hear the state through a polite live
region.

## Performance

* Only transform and alpha properties are animated, with hardware layers for
  fades (`withLayer`).
* No layout passes run during animations.
* The animators allocate nothing per frame (the intro's paths, paints and
  shader are made once).
* Nothing animates inside a stroke. The engine's input path and render thread
  are untouched.
* Measurements are in `docs/BLOCK9_REPORT.md`.
