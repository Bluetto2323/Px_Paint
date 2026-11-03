#!/usr/bin/env bash
# UI screenshots of the brush library on an Android EMULATOR (CI job
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

start --es pxpaint.demo workspace
shot 01-workspace-brush-panel
adb shell uiautomator dump /sdcard/ui01.xml > /dev/null 2>&1 && adb pull /sdcard/ui01.xml "$OUT/01-layout.xml" > /dev/null 2>&1 || true
start --es pxpaint.demo library
shot 02-library-all
adb shell input swipe 540 1700 540 1000 600; sleep 2
shot 03-library-all-scrolled
start --es pxpaint.demo library --es pxpaint.filter dry
shot 04-library-dry-media
start --es pxpaint.demo editor --es pxpaint.brush builtin.watercolor
shot 05-editor-watercolor
adb shell input swipe 540 1900 540 500 600; sleep 3
shot 06-editor-parameters
adb shell input swipe 540 1900 540 500 600; sleep 3
shot 07-editor-texture
start --es pxpaint.demo editor --es pxpaint.brush builtin.textured
shot 08-editor-textured
adb logcat -d -s PxPaint:* PaintEngine:* AndroidRuntime:E > "$OUT/logcat.txt" 2>&1 || true
ls -la "$OUT"
