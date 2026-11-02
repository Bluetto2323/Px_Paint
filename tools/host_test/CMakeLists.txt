# Host verification build (Linux desktop). NOT part of the Android app.
#   cmake -S tools/host_test -B build-host -DPE_KHRONOS_INCLUDE="<Vulkan-Headers/include>;<EGL api>;<GLES api>"
cmake_minimum_required(VERSION 3.22)
project(pe_host_test LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(${CMAKE_CURRENT_LIST_DIR}/../../app/src/main/cpp/EngineSources.cmake)

set(PE_KHRONOS_INCLUDE "" CACHE STRING "Khronos header include dirs (Vulkan, EGL, GLES)")
option(PE_SANITIZE "Build with AddressSanitizer + UBSan" OFF)

add_executable(pe_host_test host_test.cpp ${PE_ENGINE_SOURCES})
target_include_directories(pe_host_test PRIVATE ${PE_CPP_DIR} ${PE_KHRONOS_INCLUDE})
# EGL_NO_X11: plain integer native types (we only use pbuffers on host).
target_compile_definitions(pe_host_test PRIVATE EGL_NO_X11 PE_ENABLE_TEST_HOOKS=1)
target_compile_options(pe_host_test PRIVATE ${PE_WARNING_FLAGS} -Werror)
find_library(EGL_LIB NAMES EGL libEGL.so.1 REQUIRED)
find_library(GLES_LIB NAMES GLESv2 libGLESv2.so.2 REQUIRED)
find_package(Threads REQUIRED)
target_link_libraries(pe_host_test PRIVATE ${EGL_LIB} ${GLES_LIB} Threads::Threads dl)
if(PE_SANITIZE)
  target_compile_options(pe_host_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -g)
  target_link_options(pe_host_test PRIVATE -fsanitize=address,undefined)
endif()
