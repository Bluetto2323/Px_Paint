#!/usr/bin/env bash
# UI screenshots of every workspace layout on an Android EMULATOR (CI job
# "UI screenshots"). Uses the debug-only DebugDemo route of the app
# (screens/DebugDemo.kt): every state starts from cleared app data.
# This is not a physical-device test: the emulator has no stylus pressure
# and renders with SwiftShader.
#   usage: tools/ci/ui_screenshots.sh <debug.apk> <output dir>
set -uo pipefail
APK="$1"
OUT="$2"
PKG=com.paintengine
ACT="$PKG/.pxpaint.PxPaintActivity"
mkdir -p "$OUT"
adb install -r -g "$APK"
# The freshly booted emulator's launcher often raises an "isn't responding"
# dialog over everything: hide error dialogs, let the system settle, close
# system dialogs before every capture, and warm the app up once.
adb shell settings put global hide_error_dialogs 1 || true
adb shell input keyevent KEYCODE_HOME
sleep 30
adb shell am broadcast -a android.intent.action.CLOSE_SYSTEM_DIALOGS > /dev/null || true
adb shell am start -W -n "$ACT" > /dev/null
sleep 15
adb shell wm size > "$OUT/device.txt"
adb shell getprop ro.build.version.release >> "$OUT/device.txt"

start() {   # every adb call is bounded: one stuck launch must not stall the whole job
  timeout 30 adb shell am force-stop "$PKG" || echo "  start: force-stop timed out"
  timeout 60 adb shell pm clear "$PKG" > /dev/null || echo "  start: pm clear timed out"
  timeout 60 adb shell am start -W -n "$ACT" "$@" > /dev/null || echo "  start: launch not reported within 60 s"
  sleep 12
  # A stuck launch leaves the launcher in front: launch once more.
  if ! adb shell dumpsys window | grep -m1 mCurrentFocus | grep -q "$PKG"; then
    echo "  start: app not in front, launching again"
    timeout 30 adb shell am force-stop "$PKG"
    timeout 60 adb shell am start -W -n "$ACT" "$@" > /dev/null || echo "  start: second launch not reported"
    sleep 12
  fi
}
shot() {
  adb shell am broadcast -a android.intent.action.CLOSE_SYSTEM_DIALOGS > /dev/null || true
  sleep 1
  adb shell dumpsys window | grep -E "mCurrentFocus" | head -1 | sed "s/^/  $1 focus: /"
  timeout 30 adb exec-out screencap -p > "$OUT/$1.png" || echo "  shot $1: screencap timed out"
  echo "captured $1 ($(stat -c %s "$OUT/$1.png") bytes)"
}

# UI dump of the current screen (for the accessibility audit), with the density in use.
dump() {
  # uiautomator waits for an idle UI: never let one screen stall the whole job.
  adb shell rm -f /sdcard/ui.xml > /dev/null 2>&1
  if ! timeout 40 adb shell uiautomator dump /sdcard/ui.xml > /dev/null 2>&1; then
    echo "  dump $1: UI not idle within 40 s (accessibility events below)"
    timeout 4 adb shell uiautomator events 2>/dev/null | head -20 | sed 's/^/    /' || true
  fi
  adb pull /sdcard/ui.xml "$OUT/$1.xml" > /dev/null 2>&1 || true
  echo "$1 $(adb shell wm density | tail -1 | grep -o '[0-9]*$')" >> "$OUT/dumps.txt"
}
# Centre of the first node whose content-desc starts with $2 (equals it if $3 = exact) in dump $1.
centre() { python3 tools/ci/ui_find.py "$OUT/$1.xml" "$2" ${3:-}; }
display() {   # wm size WxH, density D (an emulated phone / tablet window on the same emulator)
  if [ "$1" = reset ]; then adb shell wm size reset; adb shell wm density reset
  else adb shell wm size "$1"; adb shell wm density "$2"; fi
  sleep 4
}

# ---------------- Phone portrait (Pixel 6 profile: 1080x2400, ~411x914 dp)
start --es pxpaint.demo canvas
shot 01-phone-portrait-canvas
dump 01-phone-portrait-canvas
start --es pxpaint.demo workspace
shot 02-phone-portrait-brush-panel
dump 02-phone-portrait-brush-panel
start --es pxpaint.demo library
shot 03-phone-portrait-library
start --es pxpaint.demo color
shot 04-phone-portrait-color
dump 04-phone-portrait-color
adb shell input swipe 540 1900 540 900 500; sleep 2
shot 05-phone-portrait-color-scrolled
start --es pxpaint.demo layers
shot 06-phone-portrait-layers
dump 06-phone-portrait-layers
start --es pxpaint.demo history
shot 07-phone-portrait-history
start --es pxpaint.demo fullscreen
shot 08-phone-portrait-fullscreen
dump 08-phone-portrait-fullscreen
start --es pxpaint.demo editor --es pxpaint.brush builtin.watercolor
shot 09-editor-watercolor
tapc 09-editor-watercolor "Wet mix" exact
shot 09b-studio-wet-mix
# Drawing, then undo / redo with the toolbar buttons (emulator touch input).
start --es pxpaint.demo canvas
adb shell input swipe 300 1500 800 1250 400; sleep 2
shot 10-phone-drawn
dump 10-phone-drawn
if xy=$(centre 10-phone-drawn "Undo"); then adb shell input tap $xy; sleep 2; shot 11-phone-after-undo; dump 11-phone-after-undo; fi
if xy=$(centre 11-phone-after-undo "Redo"); then adb shell input tap $xy; sleep 2; shot 12-phone-after-redo; fi
start --es pxpaint.demo settings
shot 13-settings
adb shell input swipe 540 1900 540 600 600; sleep 2
shot 14-settings-gestures

# ---------------- Motion (Block 9): startup animation, project opening, panel frame times
record() {   # record <name> <seconds>: screen recording in the background (pulled by pull_rec)
  adb shell screenrecord --time-limit "$2" --bit-rate 4000000 "/sdcard/$1.mp4" > /dev/null 2>&1 &
  sleep 1
}
pull_rec() {
  sleep "${2:-1}"
  adb pull "/sdcard/$1.mp4" "$OUT/$1.mp4" > /dev/null 2>&1 || echo "no recording $1"
  if command -v ffmpeg > /dev/null && [ -f "$OUT/$1.mp4" ]; then
    ffmpeg -loglevel error -y -i "$OUT/$1.mp4" -vf "fps=6,scale=200:-1,tile=10x1" -frames:v 1 "$OUT/$1-strip.png" || true
  fi
}
# The emulator action disables animations (scale 0, which PxPaint treats as "motion off"):
# turn them on for this section only, at the normal 1x scale.
for k in window_animation_scale transition_animation_scale animator_duration_scale; do adb shell settings put global $k 1; done
# Cold start, normal (no demo state: the startup animation runs).
adb shell am force-stop "$PKG"; adb shell pm clear "$PKG" > /dev/null
adb logcat -c
record 60-startup 5
adb shell am start -W -n "$ACT" > "$OUT/60-startup-am.txt"
pull_rec 60-startup 5
shot 61-gallery-after-startup
# Cold start with a simulated slow initialisation (2.5 s): loading state, then the exit.
adb shell am force-stop "$PKG"
record 62-startup-slow 6
adb shell am start -n "$ACT" --el pxpaint.slowinit 2500 > /dev/null
sleep 1.9
shot 63-startup-loading-state
pull_rec 62-startup-slow 5
adb logcat -d -s PxPaint:I | grep -E "Intro:" > "$OUT/startup-intro-log.txt" || true
# Project opening (container transform from the gallery card) and closing.
start --es pxpaint.demo canvas
adb shell input keyevent KEYCODE_BACK; sleep 3
dump 64-gallery-with-project
record 65-project-open-close 7
if xy=$(centre 64-gallery-with-project "Brush demo"); then
  adb shell input tap $xy; sleep 3
  shot 66-project-opened
  adb shell input keyevent KEYCODE_BACK; sleep 2
fi
pull_rec 65-project-open-close 3
# Rapid repeated panel interactions: UI frame times with motion full, then off.
for level in full off; do
  start --es pxpaint.demo canvas --es pxpaint.motion "$level"
  dump "67-panels-$level"
  adb shell dumpsys gfxinfo "$PKG" reset > /dev/null
  [ "$level" = full ] && record 68-panel-transitions 8
  for i in 1 2 3 4 5; do
    for b in "Layers" "Color" "Brush library"; do
      if xy=$(centre "67-panels-$level" "$b"); then adb shell input tap $xy; sleep 0.45; adb shell input tap $xy; sleep 0.35; fi
    done
  done
  [ "$level" = full ] && pull_rec 68-panel-transitions 1
  adb shell dumpsys gfxinfo "$PKG" > "$OUT/gfxinfo-panels-$level.txt"
done
python3 tools/ci/motion_perf.py "$OUT" "panels motion full=gfxinfo-panels-full.txt" "panels motion off=gfxinfo-panels-off.txt" \
  | tee "$OUT/motion-perf.txt" || true
start --es pxpaint.demo color --es pxpaint.motion reduced
shot 69-reduced-motion-color
for k in window_animation_scale transition_animation_scale animator_duration_scale; do adb shell settings put global $k 0; done

# ---------------- Selection (Block 10): real selection operations, driven by touches
# (adb input: a single-finger drag, taps on the real controls found in UI dumps).
tapc() {   # tapc <dump> <content-desc> [exact]: dump the screen, tap that control
  dump "$1"
  if xy=$(centre "$1" "$2" "${3:-}"); then adb shell input tap $xy; sleep 2; else echo "  no control '$2' in $1"; fi
}
lasso() {  # lasso cx cy r: a closed freehand loop with one finger (input motionevent)
  adb shell input motionevent DOWN $(( $1 + $3 )) $2
  for a in 15 30 45 60 75 90 105 120 135 150 165 180 195 210 225 240 255 270 285 300 315 330 345 360; do
    x=$(python3 -c "import math;print(int($1+$3*math.cos(math.radians($a))*(1+0.15*math.sin(math.radians(3*$a)))))")
    y=$(python3 -c "import math;print(int($2+$3*math.sin(math.radians($a))))")
    adb shell input motionevent MOVE $x $y
  done
  adb shell input motionevent UP $(( $1 + $3 )) $2
  sleep 2
}
start --es pxpaint.demo selection --es pxpaint.selshape ellipse
adb logcat -c
record 70-selection-ops 180
adb shell input swipe 250 700 850 1450 700; sleep 3
shot 71-selection-ellipse
tapc 71-selection-ellipse "Transform" exact
adb shell input swipe 550 1075 760 1300 900; sleep 2
shot 72-selection-moving
# Transform box: drag the bottom-right corner handle outwards (scale), then the content was moved too.
dump 72b-transform

tapc 72-selection-moving "Done" exact
shot 73-selection-moved
tapc 73-selection-moved "Copy" exact
tapc 73b-selection-copied "Paste" exact
shot 74-selection-pasted
tapc 74-selection-pasted "Clear" exact
shot 75-selection-cleared
# A brush stroke across the whole canvas stays inside the selection.
tapc 75-selection-cleared "Brush" exact
adb shell input swipe 150 1100 930 1180 800; sleep 3   # (not from the screen edge: that is the system back gesture)
shot 76-selection-clipped-stroke
# Lasso, added to the selection; then inverted; then undo.
tapc 76-selection-clipped-stroke "More tools" exact
tapc 76b-more-tools "Select" exact
tapc 76c-select-tool "More tools" exact
tapc 76d-more-tools "Select" exact
tapc 76e-selection-options "Lasso" exact
tapc 76f-selection-options "Add" exact
shot 77-selection-options
adb shell input keyevent KEYCODE_BACK; sleep 1
lasso 560 1750 260
shot 78-selection-lasso-add
tapc 78-selection-lasso-add "More selection actions" exact
tapc 78b-selection-overflow "Invert" exact
shot 79-selection-inverted
tapc 79-selection-inverted "Undo"
shot 80-selection-undo-invert
# Polygon: three corners, then the first corner again closes it.
tapc 80-selection-undo-invert "More tools" exact
tapc 80a-more-tools "Select" exact
tapc 80b-selection-options "Polygon" exact
tapc 80c-selection-options "New" exact
adb shell input keyevent KEYCODE_BACK; sleep 1
for xy in "300 600" "800 650" "700 1000"; do adb shell input tap $xy; sleep 1; done
shot 81-selection-polygon-building
adb shell input tap 302 602; sleep 3
shot 82-selection-polygon
# Ants animating, nothing else: UI frame times (the canvas itself is not redrawn by them).
adb shell dumpsys gfxinfo "$PKG" reset > /dev/null
sleep 6
adb shell dumpsys gfxinfo "$PKG" > "$OUT/gfxinfo-selection-ants.txt"
pull_rec 70-selection-ops 1
adb logcat -d -s PxPaint:I | grep -E "Selection:" > "$OUT/selection-log.txt" || true
python3 tools/ci/motion_perf.py "$OUT" "selection ants=gfxinfo-selection-ants.txt" | tee "$OUT/selection-perf.txt" || true

# ---------------- Redesign (Block 11): themes, brush selector, layer drag, distraction-free canvas
for t in deep_blue pure_black warm_beige; do
  start --es pxpaint.demo canvas --es pxpaint.theme "$t"
  shot "90-workspace-$t"
  dump "90-workspace-$t"
done
start --es pxpaint.demo library --es pxpaint.theme deep_blue
shot 91-brush-selector
tapc 91-brush-selector "Inking" exact
tapc 91a-brush-set-inking "Ink Pen"
shot 92-brush-selected-panel-closed
adb shell input swipe 300 900 800 1100 400; sleep 2
shot 92b-drawing-with-selected-brush
start --es pxpaint.demo library --es pxpaint.theme warm_beige
shot 91w-brush-selector-warm-beige
start --es pxpaint.demo library --es pxpaint.theme pure_black
shot 91b-brush-selector-pure-black
# Layers: press and hold a row, drag it down (finger still down for the screenshot), release, undo.
start --es pxpaint.demo layers --es pxpaint.theme deep_blue
shot 93-layers-panel
dump 93-layers-panel
if xy=$(centre 93-layers-panel "Glow"); then
  set -- $xy
  record 94-layer-drag 12
  adb shell "input motionevent DOWN $1 $2; sleep 0.6; for d in 20 40 60 80 100 120 140 160 180 200; do input motionevent MOVE $1 \$(( $2 + d )); done"
  sleep 1
  shot 94-layer-dragging
  adb shell input motionevent UP "$1" "$(( $2 + 200 ))"; sleep 2
  shot 95-layer-dropped
  tapc 95-layer-dropped "Undo"
  shot 96-layer-reorder-undone
  pull_rec 94-layer-drag 1
else echo "  no layer row 'Glow'"; fi
start --es pxpaint.demo layers --es pxpaint.theme warm_beige
shot 93w-layers-warm-beige
start --es pxpaint.demo layers --es pxpaint.theme pure_black
shot 93b-layers-pure-black
start --es pxpaint.demo fullscreen --es pxpaint.theme deep_blue
shot 97-distraction-free
start --es pxpaint.demo canvas --es pxpaint.theme deep_blue
tapc 98-workspace "More tools" exact
shot 98-more-tools-menu
# Settings: the three theme previews; switching applies at once.
start --es pxpaint.demo settings
shot 99-settings-themes
dump 99-settings-themes
if xy=$(centre 99-settings-themes "Warm Beige" exact); then adb shell input tap $xy; sleep 3; shot 99b-settings-warm-beige; fi
dump 99b-settings-warm-beige

# ---------------- Fill and gradient (Block 13): real taps / drags
start --es pxpaint.demo canvas
tapc 100-fill "More tools" exact
tapc 100b-more-tools "Fill" exact
adb shell input tap 540 1500; sleep 3
shot 101-bucket-fill
tapc 101-bucket-fill "More tools" exact
tapc 101b-more-tools "Gradient" exact
adb shell input swipe 200 600 900 1400 700; sleep 3
shot 102-gradient-preview
tapc 102-gradient-preview "Done" exact
shot 103-gradient-done
adb logcat -d -s PxPaint:I | grep -E "Selection: fill" >> "$OUT/selection-log.txt" || true

# ---------------- Smudge (Block 14): the tool in the dock, a real drag across the demo strokes
start --es pxpaint.demo canvas
tapc 104-smudge-tool "Smudge" exact
adb shell input swipe 420 560 470 1450 1200; sleep 3
shot 104-smudged

# ---------------- ColorDrop (Block 15): drag the colour chip onto the canvas, hold, slide for the threshold
if xy=$(centre 104-smudge-tool "Color" exact); then
  set -- $xy
  adb shell input motionevent DOWN $1 $2; sleep 0.2
  adb shell input motionevent MOVE $1 $(( $2 - 200 )); sleep 0.1
  adb shell input motionevent MOVE 700 1700; sleep 0.1
  adb shell input motionevent MOVE 702 1702; sleep 1.2      # held still: fills, threshold mode
  shot 105-colordrop-hold
  adb shell input motionevent MOVE 950 1702; sleep 1.5
  shot 105b-colordrop-threshold
  adb shell input motionevent UP 950 1702; sleep 2
  shot 105c-colordrop-done
else echo "  no colour chip for ColorDrop"; fi

# ---------------- Actions (Procreate's wrench menu) and its Prefs tab
tapc 105c-colordrop-done "More tools"
shot 106-actions-tools
tapc 106-actions-tools "Prefs" exact
shot 107-actions-prefs

# ---------------- Phone landscape (2400x1080 -> ~914x411 dp)
display 2400x1080 420
start --es pxpaint.demo canvas
shot 20-phone-landscape-canvas
dump 20-phone-landscape-canvas
start --es pxpaint.demo color
shot 21-phone-landscape-color
start --es pxpaint.demo layers
shot 22-phone-landscape-layers
dump 22-phone-landscape-layers
start --es pxpaint.demo selection --es pxpaint.selshape rectangle
adb shell input swipe 900 300 1500 800 600; sleep 3
shot 23-phone-landscape-selection
dump 23-phone-landscape-selection
start --es pxpaint.demo library --es pxpaint.theme warm_beige
shot 24-phone-landscape-brush-selector-warm-beige
start --es pxpaint.demo layers --es pxpaint.theme pure_black
shot 25-phone-landscape-layers-pure-black

# ---------------- Tablet portrait (emulated window 1600x2560 @320 -> 800x1280 dp)
display 1600x2560 320
start --es pxpaint.demo canvas
shot 30-tablet-portrait-canvas
dump 30-tablet-portrait-canvas
start --es pxpaint.demo color
shot 31-tablet-portrait-color-popover
start --es pxpaint.demo layers --es pxpaint.pin layers
shot 32-tablet-portrait-layers-pinned
dump 32-tablet-portrait-layers-pinned
start --es pxpaint.demo selection --es pxpaint.selshape ellipse
adb shell input swipe 400 900 1200 1700 700; sleep 3
shot 33-tablet-portrait-selection
tapc 33-tablet-portrait-selection "More tools" exact
tapc 33b-more-tools "Select" exact
shot 34-tablet-portrait-selection-options

# ---------------- Tablet landscape (2560x1600 @320 -> 1280x800 dp)
display 2560x1600 320
start --es pxpaint.demo canvas
shot 40-tablet-landscape-canvas
dump 40-tablet-landscape-canvas
start --es pxpaint.demo library --es pxpaint.pin layers
shot 41-tablet-landscape-library-and-pinned-layers
dump 41-tablet-landscape-library-and-pinned-layers
start --es pxpaint.demo workspace --es pxpaint.pin color
shot 42-tablet-landscape-brush-and-pinned-color
start --es pxpaint.demo selection --es pxpaint.selshape lasso
lasso 1280 800 380
shot 43-tablet-landscape-selection
dump 43-tablet-landscape-selection

# ---------------- Small window (720x1280 @320 -> 360x640 dp) and large display (1920x1080 dp)
display 720x1280 320
start --es pxpaint.demo workspace
shot 50-small-window-brush-panel
dump 50-small-window-brush-panel
display 3840x2160 320
start --es pxpaint.demo layers --es pxpaint.pin layers
shot 51-large-display-layers-pinned
display reset

python3 tools/ci/a11y_audit.py "$OUT" | tee "$OUT/accessibility.txt" || true
adb logcat -d -s PxPaint:* PaintEngine:* AndroidRuntime:E > "$OUT/logcat.txt" 2>&1 || true
ls -la "$OUT"
