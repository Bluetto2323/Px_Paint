cmake_minimum_required(VERSION 3.22.1)
project(paintengine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_LIST_DIR}/EngineSources.cmake)

# ---------------------------------------------------------------------------
# pe_engine: the engine (core + graphics abstraction + Vulkan + GLES +
# diagnostics). Static so the shared library only exports JNI symbols.
# ---------------------------------------------------------------------------
add_library(pe_engine STATIC ${PE_ENGINE_SOURCES})
target_include_directories(pe_engine PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_compile_options(pe_engine PRIVATE ${PE_WARNING_FLAGS})
target_compile_definitions(pe_engine PUBLIC $<$<CONFIG:Debug>:PE_DEBUG=1>)

# Vulkan is NOT linked: libvulkan.so is opened with dlopen() at runtime so
# devices without a working driver still start and fall back to GLES.
# GLESv3 (ES 3.0 entry points) is the baseline; newer entry points are
# resolved through eglGetProcAddress.
target_link_libraries(pe_engine PUBLIC EGL GLESv3 log dl)

# The per-pixel paths (brush rasterisation, compositing, tile copies) are
# always optimised, also in the debug APK: at -O0 they would be several
# times slower and the device test would measure the build type, not the
# engine. Everything else keeps the build type's flags.
set_source_files_properties(
    ${PE_CPP_DIR}/brush/StrokePath.cpp
    ${PE_CPP_DIR}/brush/StrokeCompositor.cpp
    ${PE_CPP_DIR}/brush/BrushEngine.cpp
    ${PE_CPP_DIR}/canvas/RasterStore.cpp
    ${PE_CPP_DIR}/canvas/CanvasSurface.cpp
    ${PE_CPP_DIR}/engine/CanvasDocument.cpp
    PROPERTIES COMPILE_OPTIONS "-O2")

# ---------------------------------------------------------------------------
# libpaintengine.so: JNI bridge + Android platform layer.
# ---------------------------------------------------------------------------
add_library(paintengine SHARED
    jni/JniBridge.cpp
    platform/android/AndroidWindow.cpp
)
target_compile_options(paintengine PRIVATE ${PE_WARNING_FLAGS})
target_link_libraries(paintengine PRIVATE pe_engine android log)

# 16 KB page-size compatibility (required for new apps targeting Android 15+).
target_link_options(paintengine PRIVATE "-Wl,-z,max-page-size=16384")
