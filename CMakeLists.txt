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

add_library(xud STATIC
    src/xud/xud_core.c
)

target_include_directories(xud PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/xud"
)

#-------------------------------------------------------------------------------------
# Tests: headless by default, must stay green with no Vulkan driver present
#-------------------------------------------------------------------------------------

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
