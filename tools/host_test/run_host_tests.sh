#!/usr/bin/env bash
# Host verification of the engine (Linux x86_64, no Android device needed).
#
# Prerequisites (all fetched from GitHub):
#   git clone --depth 1 https://github.com/KhronosGroup/Vulkan-Headers   $DEPS/Vulkan-Headers
#   git clone --depth 1 https://github.com/KhronosGroup/EGL-Registry     $DEPS/EGL-Registry
#   git clone --depth 1 https://github.com/KhronosGroup/OpenGL-Registry  $DEPS/OpenGL-Registry
#   git clone --depth 1 https://github.com/google/swiftshader            $DEPS/swiftshader
#     cmake -S $DEPS/swiftshader -B $DEPS/swiftshader/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
#       -DSWIFTSHADER_BUILD_TESTS=OFF -DSWIFTSHADER_BUILD_PVR=OFF && ninja -C $DEPS/swiftshader/build vk_swiftshader
#   Mesa EGL/GLES runtime (libegl1, libgles2, libegl-mesa0).
#
# usage: [DEPS=/path/to/deps] [ICD=/path/icd.json] tools/host_test/run_host_tests.sh [--asan]
#   DEPS unset -> Khronos headers from the system (libvulkan-dev, libegl-dev,
#                 libgles-dev), as in the CI workflow.
#   ICD        -> Vulkan driver manifest (default: SwiftShader under $DEPS).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEPS="${DEPS:-}"
BUILD="${BUILD:-$ROOT/build-host}"
if [[ -n "$DEPS" ]]; then
  INCLUDES="$DEPS/Vulkan-Headers/include;$DEPS/EGL-Registry/api;$DEPS/OpenGL-Registry/api"
  ICD="${ICD:-$DEPS/swiftshader/build/Linux/vk_swiftshader_icd.json}"
else
  INCLUDES=""
  ICD="${ICD:?set ICD (Vulkan driver manifest) or DEPS}"
fi
EXTRA=()
if [[ "${1:-}" == "--asan" ]]; then
  BUILD="$BUILD-asan"; EXTRA=(-DPE_SANITIZE=ON -DCMAKE_CXX_COMPILER=g++ -DCMAKE_C_COMPILER=gcc)
  export ASAN_OPTIONS=detect_leaks=1
fi
cmake -S "$ROOT/tools/host_test" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  "-DPE_KHRONOS_INCLUDE=$INCLUDES" "${EXTRA[@]}"
ninja -C "$BUILD"
export EGL_PLATFORM=surfaceless
fail=0
run() { local name=$1 icd=$2; shift 2
  if VK_ICD_FILENAMES="$icd" timeout 600 "$BUILD/pe_host_test" "$@" > "$BUILD/$name.log" 2>&1; then
    echo "PASS  $name  ($(grep -o '[0-9]*/[0-9]* checks passed' "$BUILD/$name.log"))"
  else echo "FAIL  $name  ($(grep -o '[0-9]*/[0-9]* checks passed' "$BUILD/$name.log"); see $BUILD/$name.log)"; fail=1; fi; }
# backend selection / fallback matrix
run basic_gles             "$ICD"               gles   gles   --suite basic
run basic_vulkan           "$ICD"               vulkan vulkan --no-fallback --suite basic
run basic_auto_vulkan      "$ICD"               auto   vulkan --suite basic
run basic_auto_no_vulkan   /nonexistent.json    auto   gles   --suite basic
run basic_vulkan_fallback  /nonexistent.json    vulkan gles   --suite basic
run basic_no_backend       /nonexistent.json    vulkan none   --no-fallback --suite basic
# lifecycle / surface protocol
run lifecycle_gles         "$ICD"               gles   gles   --suite lifecycle
run lifecycle_vulkan       "$ICD"               vulkan vulkan --no-fallback --suite lifecycle
# injected faults, recovery and fallback after an error
run faults_gles            "$ICD"               gles   gles   --no-fallback --suite faults
run faults_vulkan_only     "$ICD"               vulkan vulkan --no-fallback --suite faults
run faults_auto_fallback   "$ICD"               auto   vulkan --suite faults
exit $fail
