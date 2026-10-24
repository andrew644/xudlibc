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
)

target_include_directories(xud PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/xud"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/external"
)

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
