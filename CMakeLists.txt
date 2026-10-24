# xudlibc - a C11, Vulkan-only library shaped after raylib's public API.
#
# There is deliberately NO -lvulkan on any link line: the Vulkan loader is dlopen()ed at
# runtime (bring-up step S2), so the library must build and link on a machine that has no
# driver installed.

cmake_minimum_required(VERSION 3.25)
project(xudlibc LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS OFF)          # -std=c11, never -std=gnu11

# Debug/dev build by default: XUD_VALIDATE=1 turns validation layers on. Flip with
# -DXUD_DEBUG=OFF for a release build (-O2, validation off).
option(XUD_DEBUG "Debug build: validation layers on, -g" ON)
option(XUD_SANITIZERS "Build every target with ASan + UBSan (tests/ only in practice)" OFF)

# NOTE: the flag -Ddefault=lib is NOT used. With gcc 16.2.1 and clang 22.1.8 it is a
# preprocessor define of a macro named `default`, which rewrites every `default:` label
# in a switch and breaks the build. Verified on this machine before omitting it.

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

#-------------------------------------------------------------------------------------
# Vendored GLFW 3.5.1 (src/external/glfw, see src/external/PROVENANCE.md): Wayland backend
#-------------------------------------------------------------------------------------

# The window/input rung is the vendored GLFW tree, built here rather than linked from the
# system package (decided 2026-10-23, see AGENTS.md *Settled decisions*): that keeps a -lglfw
# off every link line and makes the library build on a machine without the Arch glfw package.
# The backend is Wayland (GLFW_BUILD_WAYLAND -> -D_GLFW_WAYLAND, upstream's own switch, glfw/src
# CMakeLists.txt 54-55); GLFW_BUILD_X11 stays off, so x11_*.c and xkb_unicode.c are not in the
# build and there is still no X11 path of our own.
#
# The source set below is upstream's glfw/src/CMakeLists.txt read for GLFW_BUILD_WAYLAND=ON,
# GLFW_BUILD_X11/COCOA/WIN32=OFF on Linux: its always-compiled base set (context.c init.c input.c
# monitor.c platform.c vulkan.c window.c + the null_* backend halves, which the GLFW_PLATFORM init
# hint can select at runtime), the Linux/POSIX time-thread-module-poll bits (upstream 12-16 and
# the X11-or-Wayland posix_poll half), the Wayland half (wl_init.c wl_monitor.c wl_window.c + wl
# _platform.h, upstream 44-48) and linux_joystick.c (upstream 50, gated on (X11 or Wayland) and
# __linux__). The win32_*/cocoa_*/macos_*/nsgl_* halves and glx_context.c are excluded; egl_cont
# ext.c and osmesa_context.c are NOT: they are in upstream's always-compiled base set and wl_win
# dow.c, wl_init.c and null_init.c/null_window.c reference _glfwInitEGL/_glfwCreateContextEGL/_glfw
# InitOSMesa/_glfwTerminateEGL by name, so leaving them out is a link error, not a smaller binary.
# Neither pulls a GL or EGL library: GLFW dlopen()s those at runtime (verified with ldd below),
# and glfwCreateWindowSurface refuses a window whose client API is not GLFW_NO_API, so the Vulkan
# path never runs through them.
#
# The Wayland protocol headers/sources are generated at build time from the 9 XMLs GLFW ships in
# deps/wayland, by wayland-scanner (upstream's own find_program + FATAL_ERROR at 75-77 and its
# generate_wayland_protocol macro at 81-107, same 9 files). They are generated into build/glfw-
# wayland/ so nothing generated lands next to the vendored sources, which are never edited in place.
#
# The vendored TUs are NOT compiled in the library's warning mode. GLFW 3.5.1 does not compile
# under -Wextra or -Wpedantic (verified: -Wpedantic is a hard error on the dlsym object-pointer
# <-> function-pointer casts in posix_module.c 50, the same ISO C rule AGENTS.md records for our
# own loader; -Wextra is a hard error on the unused parameters every null_* half and every Wayland
# protocol callback carries). Upstream asks for -Wall only (glfw/src/CMakeLists.txt 130) and C99;
# here the tree is compiled as C11 (the project's standard, verified clean with gcc 16.2.1 and
# clang 22.1.8) with -Wall -Werror kept and -Wextra/-Wpedantic switched off per target, so the
# vendored bucket still has to be warning-clean under -Wall. -D_DEFAULT_SOURCE is upstream's own
# Linux define (glfw/src/CMakeLists.txt 141): -std=c11 sets __STRICT_ANSI__, which is what would
# otherwise hide the glibc POSIX extensions wl_init.c/wl_window.c need.

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

# Upstream's HAVE_MEMFD_CREATE probe (glfw/src/CMakeLists.txt 66-70, check_function_exists):
# wl_window.c 105-106 uses memfd_create for the shared-memory window hint when it is there.
include(CheckFunctionExists)
check_function_exists(memfd_create GLFW_HAVE_MEMFD_CREATE)

add_library(glfw_vendor OBJECT
    # base set, always compiled by upstream
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
    # Linux/POSIX time, thread, module (upstream 12-16, the non-APPLE non-WIN32 branch)
    ${GLFW_DIR}/src/posix_module.c
    ${GLFW_DIR}/src/posix_time.c
    ${GLFW_DIR}/src/posix_thread.c
    # Wayland backend + the halves (X11 or Wayland) share on Linux
    ${GLFW_DIR}/src/wl_init.c
    ${GLFW_DIR}/src/wl_monitor.c
    ${GLFW_DIR}/src/wl_window.c
    ${GLFW_DIR}/src/linux_joystick.c
    ${GLFW_DIR}/src/posix_poll.c
    # generated by wayland-scanner from deps/wayland/*.xml, above
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

#-------------------------------------------------------------------------------------
# Library: one static archive, one public header, one TU per module
#-------------------------------------------------------------------------------------

# The loader rung is now vendored Volk (src/external/volk.c, see src/external/PROVENANCE.md):
# volkInitialize() dlopen()s libvulkan.so.1 (RTLD_NOW|RTLD_LOCAL, volk.c 114 with a libvulkan.so
# fallback at 116) and volkFinalize() dlclose()s it - dlopen/dlsym/dlclose come from libc, so there
# is still no -lvulkan and no -ldl on any link line (verified with ldd + nm -u).
# xud_loader.c is the per-context pair of Volk entry-point tables around it; xud_vulkan.c, xud_inst
# ance.c, xud_diagnostics.c, xud_physical.c and xud_device.c are the layers above it: shared VkResu
# lt/name vocabulary, vkCreateInstance/vkDestroyInstance, the diagnostics layer's vkEnumerateInsta
# nceLayerProperties + vkCreate/vkDestroyDebugUtilsMessengerEXT, the physical layer's vkEnumerateP
# hysicalDevices + the three *2 caps getters + vkEnumerateDeviceExtensionProperties, and the device
# layer's vkCreateDevice/vkGetDeviceQueue/vkDeviceWaitIdle/vkDestroyDevice - every one of them rea
# ched through a Volk table row (struct VolkInstanceTable filled in bulk by volkLoadInstanceTable,
# struct VolkDeviceTable by volkLoadDeviceTable), never through a Vulkan call the linker could see.
# Under VK_NO_PROTOTYPES volk.h declares every entry point as `extern PFN_vkXxx vkXxx;` - a data p
# ointer, not a prototype - so "no Vulkan symbol at link time" is now a link-time fact: leaving vo
# lk.c out of the link leaves those pointers undefined. Verified on this machine: the archive and
# both executables hold no vk* undefined symbol and ldd lists no libvulkan.
# src/external/volk.c is the vendored loader rung (upstream code, never edited in place - re-vend
# or from upstream instead, see src/external/PROVENANCE.md). It is compiled in the same mode as th
# e library's own TUs, which is what makes VK_NO_PROTOTYPES hold everywhere: volk.h defines the m
# acro itself when <vulkan/vulkan.h> has not been seen yet, and errors when it has, so in every T
# U volk.h comes before the Vulkan header (see xud_loader.h's include-order note) and src/external
# has to be on the include path for that to resolve.
add_library(xud STATIC
    src/external/volk.c
    src/xud/xud_core.c
    src/xud/xud_diagnostics.c
    src/xud/xud_loader.c
    src/xud/xud_vulkan.c
    src/xud/xud_instance.c
    src/xud/xud_physical.c
    src/xud/xud_device.c
    src/xud/xud_window.c
)

# glfw_vendor is an OBJECT library, so its objects go into this archive rather than into a
# second archive: `ar -t build/libxud.a` lists the vendored volk.c.o and the GLFW Wayland objects
# among the library's own module objects, and there is still exactly one library to link. The
# Wayland libraries the wl_* halves call are named on this link, PUBLIC, beside it: an object
# library's own link dependencies are not propagated to the targets that use it (verified both
# ways here: glfw_vendor PRIVATE and glfw_vendor PUBLIC both left every executable's link line a
# t libxud.a only, with ldd listing libc and nothing else, so a GLFW call would have failed to l
# ink) and a static archive records no dependencies of its own. PUBLIC on xud is what makes Way
# land a property of the library: ldd now lists libwayland-client.so.0, libwayland-cursor.so.0,
# libwayland-egl.so.1, libxkbcommon.so.0 and libffi.so.8 (wayland-client's own dependency) for bo
# th executables, while nm -u still lists no vk* or glfw* symbol and ldd still lists no libvulkan,
# no libglfw and no libGL/libEGL - GLFW dlopen()s the Vulkan loader and the GL/EGL backends at ru
# ntime, so there is still no -lglfw and no -ldl.
# The link flags come from pkg_check_modules' GLFW_WAYLAND_LDFLAGS, not its GLFW_WAYLAND_LIBS: t
# he LIBS form is only filled for the _DEBUG/_RELEASE variants (verified here, GLFW_WAYLAND_LIBS
# expanded to nothing and left the link line at libxud.a), and the _STATIC_LDFLAGS form would ad
# d -lrt, which is not a library on this machine.
target_link_libraries(xud PUBLIC glfw_vendor ${GLFW_WAYLAND_LDFLAGS})

target_include_directories(xud PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/xud"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/external"
    "${GLFW_DIR}/include"
)

# src/external is Volk's home (xud_loader.h includes "volk.h", which must come before <vulkan/vul
# kan.h> - see the include-order trap in AGENTS.md). ${GLFW_DIR}/include is here because xud_windo
# w.h - the one header of the window rung - includes <GLFW/glfw3.h>, and it is PUBLIC: the test an
# d example executables link xud, so they inherit the include path and compile xud_window.h withou
# t naming GLFW's include dir themselves. Nothing but xud_window.h includes glfw3.h in this tree, an
# d glfw3.h's Vulkan declarations (glfwVulkanSupported, glfwGetRequiredInstanceExtensions, glfwGetP
# hysicalDevicePresentationSupport, glfwCreateWindowSurface) sit inside #if defined(VK_VERSION_1_0)
# -, so <vulkan/vulkan.h> has to be seen first; that is a property of the header's own include bloc
# k, not of the compile line.

#-------------------------------------------------------------------------------------
# Tests: headless by default, must stay green with no Vulkan driver present
#-------------------------------------------------------------------------------------

# The test links the archive, nothing else: every layer under the public API is in it, so the test
# TU is one main() plus the checks, and there is no second executable to duplicate the harness for.
# The physical layer joined the archive with the rest when its driver half landed (S5.4) and the device lay
# er joined with it at S6 - both headers' contracts are complete, every declaration in them is defined, so no u
# ndefined declaration is left in the TU and nothing has to be compiled into the test binary specially.
add_executable(xud_test
    tests/xud_test.c
)

target_link_libraries(xud_test PRIVATE xud)

#-------------------------------------------------------------------------------------
# Examples: small, one concern each
#-------------------------------------------------------------------------------------

add_executable(hello_context
    examples/hello_context.c
)

target_link_libraries(hello_context PRIVATE xud)
