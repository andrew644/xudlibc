cmake_minimum_required(VERSION 3.25)
project(xudlibc LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS OFF)          # -std=c11, never -std=gnu11

# Debug/dev build by default: XUD_VALIDATE=1 turns validation layers on. Flip with
# -DXUD_DEBUG=OFF for a release build (-O2, validation off).
option(XUD_DEBUG "Debug build: validation layers on, -g" ON)
option(XUD_SANITIZERS "Build every target with ASan + UBSan (tests/ only in practice)" OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Werror)

if (XUD_DEBUG)
    add_compile_options(-g -DXUD_DEBUG=1)
else()
    add_compile_options(-O2)
endif()

if (XUD_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLFW_WAYLAND REQUIRED
    wayland-client>=0.2.7
    wayland-cursor>=0.2.7
    wayland-egl>=0.2.7
    xkbcommon>=0.5.0)

find_program(GLFW_WAYLAND_SCANNER NAMES wayland-scanner)
if (NOT GLFW_WAYLAND_SCANNER)
    message(FATAL_ERROR "wayland-scanner not found; the vendored GLFW Wayland backend cannot be generated")
endif()

set(GLFW_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/external/glfw")
set(GLFW_WAYLAND_PROTO_DIR "${CMAKE_CURRENT_BINARY_DIR}/glfw-wayland")

foreach(GLFW_WAYLAND_PROTO wayland.xml viewporter.xml xdg-shell.xml idle-inhibit-unstable-v1.xml
                          pointer-constraints-unstable-v1.xml relative-pointer-unstable-v1.xml
                          fractional-scale-v1.xml xdg-activation-v1.xml xdg-decoration-unstable-v1.xml)
    string(REGEX REPLACE "\\.xml$" "-client-protocol.h" GLFW_PROTO_HEADER "${GLFW_WAYLAND_PROTO}")
    string(REGEX REPLACE "\\.xml$" "-client-protocol-code.h" GLFW_PROTO_CODE "${GLFW_WAYLAND_PROTO}")
    set(GLFW_PROTO_XML "${GLFW_DIR}/deps/wayland/${GLFW_WAYLAND_PROTO}")
    set(GLFW_PROTO_HEADER "${GLFW_WAYLAND_PROTO_DIR}/${GLFW_PROTO_HEADER}")
    set(GLFW_PROTO_CODE "${GLFW_WAYLAND_PROTO_DIR}/${GLFW_PROTO_CODE}")
    add_custom_command(
        OUTPUT "${GLFW_PROTO_HEADER}"
        COMMAND "${GLFW_WAYLAND_SCANNER}" client-header "${GLFW_PROTO_XML}" "${GLFW_PROTO_HEADER}"
        DEPENDS "${GLFW_PROTO_XML}"
        VERBATIM)
    add_custom_command(
        OUTPUT "${GLFW_PROTO_CODE}"
        COMMAND "${GLFW_WAYLAND_SCANNER}" private-code "${GLFW_PROTO_XML}" "${GLFW_PROTO_CODE}"
        DEPENDS "${GLFW_PROTO_XML}"
        VERBATIM)
    list(APPEND GLFW_WAYLAND_GENERATED "${GLFW_PROTO_HEADER}" "${GLFW_PROTO_CODE}")
endforeach()

include(CheckFunctionExists)
check_function_exists(memfd_create GLFW_HAVE_MEMFD_CREATE)

add_library(glfw_vendor OBJECT
    ${GLFW_DIR}/src/context.c
    ${GLFW_DIR}/src/init.c
    ${GLFW_DIR}/src/input.c
    ${GLFW_DIR}/src/monitor.c
    ${GLFW_DIR}/src/platform.c
    ${GLFW_DIR}/src/vulkan.c
    ${GLFW_DIR}/src/window.c
    ${GLFW_DIR}/src/egl_context.c
    ${GLFW_DIR}/src/osmesa_context.c
    ${GLFW_DIR}/src/null_init.c
    ${GLFW_DIR}/src/null_monitor.c
    ${GLFW_DIR}/src/null_window.c
    ${GLFW_DIR}/src/null_joystick.c
    ${GLFW_DIR}/src/posix_module.c
    ${GLFW_DIR}/src/posix_time.c
    ${GLFW_DIR}/src/posix_thread.c
    ${GLFW_DIR}/src/wl_init.c
    ${GLFW_DIR}/src/wl_monitor.c
    ${GLFW_DIR}/src/wl_window.c
    ${GLFW_DIR}/src/linux_joystick.c
    ${GLFW_DIR}/src/posix_poll.c
    ${GLFW_WAYLAND_GENERATED}
)

target_compile_definitions(glfw_vendor PRIVATE _GLFW_WAYLAND _DEFAULT_SOURCE)
if (GLFW_HAVE_MEMFD_CREATE)
    target_compile_definitions(glfw_vendor PRIVATE HAVE_MEMFD_CREATE)
endif()

target_compile_options(glfw_vendor PRIVATE -Wno-extra -Wno-pedantic)

target_include_directories(glfw_vendor PRIVATE
    "${GLFW_DIR}/src"
    "${GLFW_DIR}/include"
    "${GLFW_WAYLAND_PROTO_DIR}"
    ${GLFW_WAYLAND_INCLUDE_DIRS}
)

find_program(GLSLANG_VALIDATOR NAMES glslangValidator PATHS /usr/bin NOFOLLOW_STRINGS OPTIONAL)
if (NOT GLSLANG_VALIDATOR)
    message(FATAL_ERROR "glslangValidator not found; shaders/solid.vert and shaders/solid.frag cannot be compiled")
endif()
find_program(XUD_SPIRV_VAL NAMES spirv-val PATHS /usr/bin NOFOLLOW_STRINGS OPTIONAL)
if (NOT XUD_SPIRV_VAL)
    message(FATAL_ERROR "spirv-val not found; the repo's rule is that every .spv is validated, not just compiled")
endif()

set(XUD_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen")

add_custom_command(
    OUTPUT "${XUD_GEN_DIR}/solid_vert.spv"
    COMMAND "${GLSLANG_VALIDATOR}" -V -o "${XUD_GEN_DIR}/solid_vert.spv" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/solid.vert"
    COMMAND "${XUD_SPIRV_VAL}" "${XUD_GEN_DIR}/solid_vert.spv"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/solid.vert"
    VERBATIM)
add_custom_command(
    OUTPUT "${XUD_GEN_DIR}/solid_frag.spv"
    COMMAND "${GLSLANG_VALIDATOR}" -V -o "${XUD_GEN_DIR}/solid_frag.spv" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/solid.frag"
    COMMAND "${XUD_SPIRV_VAL}" "${XUD_GEN_DIR}/solid_frag.spv"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/solid.frag"
    VERBATIM)
add_custom_command(
    OUTPUT "${XUD_GEN_DIR}/xud_solid_spv.h"
    COMMAND "${CMAKE_COMMAND}"
        -DSPV_OUT_HEADER=${XUD_GEN_DIR}/xud_solid_spv.h
        -DSPV_GUARD=XUD_SOLID_SPV_H_
        -DSPV_COUNT=2
        -DSPV_ARRAY_0=xudSolidVertSpv
        -DSPV_ARRAY_1=xudSolidFragSpv
        -DSPV_INPUT_0=${XUD_GEN_DIR}/solid_vert.spv
        -DSPV_INPUT_1=${XUD_GEN_DIR}/solid_frag.spv
        -P "${CMAKE_CURRENT_SOURCE_DIR}/scripts/spv_to_header.cmake"
    DEPENDS "${XUD_GEN_DIR}/solid_vert.spv" "${XUD_GEN_DIR}/solid_frag.spv"
    VERBATIM)
set(XUD_GENERATED_HEADERS "${XUD_GEN_DIR}/xud_solid_spv.h")

add_custom_target(xud_generated_headers DEPENDS ${XUD_GENERATED_HEADERS})

add_library(xud STATIC
    src/external/volk.c
    src/xud/xud_vendor_cgltf.c
    src/xud/xud_vendor_stb_truetype.c
    src/xud/xud_vendor_miniaudio.c
    src/xud/xud_core.c
    src/xud/xud_diagnostics.c
    src/xud/xud_loader.c
    src/xud/xud_vulkan.c
    src/xud/xud_instance.c
    src/xud/xud_physical.c
    src/xud/xud_device.c
    src/xud/xud_window.c
    src/xud/xud_input.c
    src/xud/xud_swapchain.c
    src/xud/xud_allocator.c
    src/xud/xud_descriptor.c
    src/xud/xud_batch.c
    src/xud/xud_camera.c
    src/xud/xud_shapes.c
    src/xud/xud_handle.c
    src/xud/xud_material.c
    src/xud/xud_mesh.c
    src/xud/xud_model.c
    src/xud/xud_texture.c
    src/xud/xud_time.c
    src/xud/xud_font.c
    src/xud/xud_shader.c
    src/xud/xud_pipeline.c
    src/xud/xud_renderpass.c
    src/xud/xud_frame.c
    src/xud/xud_renderer.c
)

add_dependencies(xud xud_generated_headers)

target_link_libraries(xud PUBLIC glfw_vendor ${GLFW_WAYLAND_LDFLAGS})

target_link_libraries(xud PUBLIC m)

target_include_directories(xud PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/xud"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/external"
    "${GLFW_DIR}/include"
    "${XUD_GEN_DIR}"
)

add_executable(xud_test
    tests/xud_test_main.c
    tests/xud_test_harness.c
    tests/xud_test_vocabulary.c
    tests/xud_test_loader.c
    tests/xud_test_diagnostics.c
    tests/xud_test_instance.c
    tests/xud_test_physical.c
    tests/xud_test_device.c
    tests/xud_test_gpu_bringup.c
    tests/xud_test_window.c
    tests/xud_test_input.c
    tests/xud_test_swapchain.c
    tests/xud_test_handle.c
    tests/xud_test_shader.c
    tests/xud_test_pipeline.c
    tests/xud_test_renderpass.c
    tests/xud_test_frame.c
    tests/xud_test_renderer.c
    tests/xud_test_allocator.c
    tests/xud_test_descriptor.c
    tests/xud_test_batch.c
    tests/xud_test_shapes.c
    tests/xud_test_camera.c
    tests/xud_test_material.c
    tests/xud_test_mesh.c
    tests/xud_test_model.c
    tests/xud_test_texture.c
    tests/xud_test_time.c
    tests/xud_test_font.c
    tests/xud_test_core.c
    tests/xud_test_vendor.c
)

target_link_libraries(xud_test PRIVATE xud)

add_executable(hello_context
    examples/hello_context.c
)

target_link_libraries(hello_context PRIVATE xud)

add_executable(hello_triangle
    examples/hello_triangle.c
)

target_link_libraries(hello_triangle PRIVATE xud)

add_executable(breakout_3d
    examples/breakout_3d.c
)

target_link_libraries(breakout_3d PRIVATE xud)
