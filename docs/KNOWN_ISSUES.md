# Known issues

## KI-1 — Vulkan swapchain does not always follow a SurfaceView resize (OPEN)

**Status:** open. A fix is implemented in Block 2 (see below) but it is **not
verified on the device yet**. Do not mark this resolved until a Galaxy A33
DEVICE TEST report shows section D passing.

### Evidence (Galaxy A33 5G, SM-A336B, Android 16 / API 36, Mali-G68, driver 32.1.0)

Block 1 DEVICE TEST, 2026-11-02 16:04, section D (Vulkan backend):

```
1080x1440 -> surface 1080x2400 (rebuild 22.9 ms) FAILED
 756x1680 -> surface  756x1680 (rebuild 24.4 ms) OK
 540x2400 -> surface  756x1680 (rebuild 18.8 ms) FAILED
full size restored and self-test passes
```

App timeline: Android delivered `surfaceChanged 1080x1440`,
`surfaceChanged 756x1680`, `surfaceChanged 540x2400` (all three). The
self-tests during the failing steps rendered correctly but at the **old**
size (1080x2400, then 756x1680): the swapchain was rebuilt (18-23 ms) with
the previous extent and never caught up within the 5 s window.

Everything else in the run passed (29/30): lifecycle, 10x surface recreation,
pause/resume, render modes, Vulkan <-> OpenGL ES switching with clean
shutdown, 0 engine errors.

### Analysis

1. `surfaceChanged(w,h)` -> `notifySurfaceResized` marked the swapchain dirty.
2. The rebuild used `vkGetPhysicalDeviceSurfaceCapabilitiesKHR().currentExtent`
   which, right after the callback, can still report the previous window
   buffer size (the window's buffer size is updated asynchronously).
3. The dirty flag was cleared, and `VK_SUBOPTIMAL_KHR` from present was
   ignored (it was assumed to mean "rotation without pre-rotation" only), so
   nothing triggered another rebuild once the window reached its new size.

Root cause is consistent with the alternating pass/fail pattern (a race),
but it is an inference from the report, not a driver-confirmed fact.

### Fix implemented (Block 2) — pending device verification

* The size from `surfaceChanged` is kept as the **desired extent**. While the
  swapchain extent differs from it, the device re-queries the surface
  capabilities every frame and rebuilds as soon as `currentExtent` changes
  (bounded: gives up with a logged warning after ~2 s / 120 frames).
* While such a resize is pending the device asks for frames even in
  on-demand mode (`GraphicsDevice::needsFrame()`), otherwise an idle app
  would never catch up.
* `VK_SUBOPTIMAL_KHR` now re-queries the capabilities: a size mismatch marks
  the swapchain dirty; a transform-only mismatch (rotation, pre-rotation not
  implemented) is still accepted without rebuilding in a loop.
* Section D of the DEVICE TEST is unchanged (same strict assertions:
  surface must match the view size, self-test incl. geometry must pass).

### How to verify

Run DEVICE TEST on the A33 and check section D: all three sizes must report
`-> surface <same size> ... OK`.

## KI-2 — "CPU/frame" included the vsync wait (diagnostics, fixed in Block 2)

Block 1 reported `steady CPU avg 16.63 ms` at 59.8 fps: that number included
the time spent waiting for the GPU/vsync (fence wait + acquire). Block 2
splits frame time into *wait* (beginFrame: fence + acquire) and *work*
(scene recording + submit + present call), so CPU cost is no longer
confused with the display refresh interval.

## KI-3 — Vulkan pre-rotation not implemented

The compositor rotates the image in landscape (extra composition pass).
Visual orientation after rotation must be checked by eye; the pixel
self-test reads the swapchain image, not the screen.
