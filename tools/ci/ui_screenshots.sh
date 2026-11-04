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

start() {
  adb shell am force-stop "$PKG"
  adb shell pm clear "$PKG" > /dev/null
  adb shell am start -W -n "$ACT" "$@" > /dev/null
  sleep 12
}
shot() {
  adb shell am broadcast -a android.intent.action.CLOSE_SYSTEM_DIALOGS > /dev/null || true
  sleep 1
  adb shell dumpsys window | grep -E "mCurrentFocus" | head -1 | sed "s/^/  $1 focus: /"
  adb exec-out screencap -p > "$OUT/$1.png"
  echo "captured $1 ($(stat -c %s "$OUT/$1.png") bytes)"
}

# UI dump of the current screen (for the accessibility audit), with the density in use.
dump() {
  adb shell uiautomator dump /sdcard/ui.xml > /dev/null 2>&1 && adb pull /sdcard/ui.xml "$OUT/$1.xml" > /dev/null 2>&1 || true
  echo "$1 $(adb shell wm density | tail -1 | grep -o '[0-9]*$')" >> "$OUT/dumps.txt"
}
# Centre of the first node whose content-desc starts with $2 in dump $1.
centre() { python3 tools/ci/ui_find.py "$OUT/$1.xml" "$2"; }
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
