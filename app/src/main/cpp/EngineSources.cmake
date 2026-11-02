# Platform-independent engine sources, shared by the Android build
# (CMakeLists.txt next to this file) and the host verification build
# (tools/host_test). Android-only files (JNI, ANativeWindow) are NOT here.
set(PE_CPP_DIR ${CMAKE_CURRENT_LIST_DIR})

set(PE_ENGINE_SOURCES
    # core
    ${PE_CPP_DIR}/core/Log.cpp
    ${PE_CPP_DIR}/core/TaskQueue.cpp
    ${PE_CPP_DIR}/core/TestHooks.cpp        # empty unless PE_ENABLE_TEST_HOOKS
    ${PE_CPP_DIR}/core/ShutdownReport.cpp
    # canvas core (pure math, no GPU)
    ${PE_CPP_DIR}/canvas/Viewport.cpp
    ${PE_CPP_DIR}/canvas/TileGrid.cpp
    # canvas GPU (backend-neutral graphics API only)
    ${PE_CPP_DIR}/canvas/CanvasSurface.cpp
    ${PE_CPP_DIR}/canvas/CanvasRenderer.cpp
    ${PE_CPP_DIR}/canvas/CanvasSelfTest.cpp
    # graphics abstraction (API independent)
    ${PE_CPP_DIR}/graphics/abstraction/GraphicsTypes.cpp
    ${PE_CPP_DIR}/graphics/abstraction/GraphicsCapabilities.cpp
    ${PE_CPP_DIR}/graphics/abstraction/ResourceManager.cpp
    ${PE_CPP_DIR}/graphics/abstraction/FrameScheduler.cpp
    ${PE_CPP_DIR}/graphics/BackendSelector.cpp
    # Vulkan backend
    ${PE_CPP_DIR}/graphics/vulkan/VulkanLoader.cpp
    ${PE_CPP_DIR}/graphics/vulkan/VulkanCommon.cpp
    ${PE_CPP_DIR}/graphics/vulkan/VulkanBackend.cpp
    ${PE_CPP_DIR}/graphics/vulkan/VulkanDevice.cpp
    ${PE_CPP_DIR}/graphics/vulkan/VulkanCapabilities.cpp
    ${PE_CPP_DIR}/graphics/vulkan/VulkanSwapchain.cpp
    ${PE_CPP_DIR}/graphics/vulkan/VulkanResources.cpp
    ${PE_CPP_DIR}/graphics/vulkan/VulkanRenderingContext.cpp
    # OpenGL ES backend
    ${PE_CPP_DIR}/graphics/opengles/GLESCommon.cpp
    ${PE_CPP_DIR}/graphics/opengles/GLESBackend.cpp
    ${PE_CPP_DIR}/graphics/opengles/GLESDevice.cpp
    ${PE_CPP_DIR}/graphics/opengles/GLESCapabilities.cpp
    ${PE_CPP_DIR}/graphics/opengles/GLESResources.cpp
    ${PE_CPP_DIR}/graphics/opengles/GLESRenderingContext.cpp
    # diagnostics + engine
    ${PE_CPP_DIR}/diagnostics/Diagnostics.cpp
    ${PE_CPP_DIR}/engine/TestScene.cpp
    ${PE_CPP_DIR}/engine/CanvasController.cpp
    ${PE_CPP_DIR}/engine/Engine.cpp
)

set(PE_CANVAS_CORE_SOURCES
    ${PE_CPP_DIR}/canvas/Viewport.cpp
    ${PE_CPP_DIR}/canvas/TileGrid.cpp
)

set(PE_WARNING_FLAGS -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter)
