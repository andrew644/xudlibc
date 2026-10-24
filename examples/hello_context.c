/***********************************************************************************************
 *
 *   hello_context.c - the smallest program that touches the xudlibc public API
 *
 *   Owns : nothing but a call to xud_vulkan_supported() and to xud_init_context() plus its
 *          teardown. One concern: what the library can do today. The bring-up is complete to the
 *          surface and the swapchain now (S8's ownership step, with S9's swapchain rung on top of
 * it), so a valid request answers XUD_OK on a machine that ha s both a Vulkan loader and a window
 * system: volkInitialize() the loader, open the diagnosti cs layer when XUD_VALIDATE asked for it,
 * open a GLFW window, create the VkInstance carrying th at window's required instance extensions,
 * install the debug messenger, open the physical layer thr ough THAT instance (enumerate, rank,
 * record caps), ask GLFW whether that physical device can present t o this window's system, create
 * the VkDevice from the recorded caps and the chosen queue family, resolve th e two role queues,
 * create the VkSurfaceKHR for the window, and create the VkSwapchainKHR for that surface.
 * xud_close_context() tears all of it back, in the reverse order, starting with the swapchain.
 * Everything below the swapchain (the renderer, the GPU-memory allocator) is st ill open - see
 * todo.md - so nothing is drawn here, and every layer below xud.h stays internal: xud.h declares n
 * othing about a window, a surface, a physical device, its caps, a VkDevice or a queue. Layer:
 * examples. Publi c API only (xud.h).
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include <stdio.h>

#include "xud.h"

int main(void) {
    const XudResult supported = xud_vulkan_supported();
    printf("xud_vulkan_supported() -> %s\n", xud_result_name(supported));

    XudContext* context = NULL;
    const XudResult result = xud_init_context(&context, 640, 480);

    printf("xud_init_context(640, 480) -> %s (xudlibc %s)\n", xud_result_name(result), XUD_VERSION);

    if (result != XUD_OK) {
        /* XUD_UNSUPPORTED is the honest state on a machine with no Vulkan loader or no window sys
           tem; anything else is a real failure. On a machine that has both, the call above answered
           XU D_OK and the context below is the one that owns everything.
         */
        return result == XUD_UNSUPPORTED ? 0 : 1;
    }

    const XudResult closed = xud_close_context(&context);
    printf("xud_close_context -> %s\n", xud_result_name(closed));

    return closed == XUD_OK ? 0 : 1;
}
