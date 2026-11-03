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
