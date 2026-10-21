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

# xud_loader.c dlopens the Vulkan ICD loader: dlopen/dlsym/dlclose come from libc, so
# there is still no -lvulkan and no -ldl on any link line (verified with ldd + nm -u).
# xud_vulkan.c, xud_instance.c, xud_diagnostics.c and xud_physical.c are the layers above it: share
# d VkResult/name vocabulary, vkCreateInstance/vkDestroyInstance, the diagnostics layer's vkEnumerat
# eInstanceLayerProperties + vkCreate/vkDestroyDebugUtilsMessengerEXT, and the physical layer's vkEnu
# meratePhysicalDevices + the three *2 caps getters + vkEnumerateDeviceExtensionProperties - every one
# of them reached only through the loader's function-pointer table (the two messenger entry points res
# olved on demand through a live VkInstance, never cached in a slot). Still no Vulkan symbol at link t
# ime: the archive holds no vk* undefined symbol, and dlclose/dlopen/dlsym come from libc.
add_library(xud STATIC
    src/xud/xud_core.c
    src/xud/xud_diagnostics.c
    src/xud/xud_loader.c
    src/xud/xud_vulkan.c
    src/xud/xud_instance.c
    src/xud/xud_physical.c
)

target_include_directories(xud PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/xud"
)

#-------------------------------------------------------------------------------------
# Tests: headless by default, must stay green with no Vulkan driver present
#-------------------------------------------------------------------------------------

# The test links the archive, nothing else: every layer under the public API is in it, so the test
# TU is one main() plus the checks, and there is no second executable to duplicate the harness for.
# The physical layer joined the archive with the rest when its driver half landed (S5.4) - its header
# contract is complete now, both halves are defined, so no undefined declaration is left in the TU an
# d nothing has to be compiled into the test binary specially.
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
