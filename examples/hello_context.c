/***********************************************************************************************
 *
 *   hello_context.c - the smallest program that touches the xudlibc public API
 *
 *   Owns : nothing but a call to xud_vulkan_supported() and to xud_init_context() plus its
 *          teardown. One concern: what the library can do today. At bring-up step S2 the
 *          Vulkan loader is resolved - so the runtime query answers - but instance / device
 *          / surface are not, so the honest answer for a context is still XUD_UNSUPPORTED.
 *   Layer: examples. Public API only (xud.h).
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include <stdio.h>

#include "xud.h"

int main(void)
{
    const XudResult supported = xud_vulkan_supported();
    printf("xud_vulkan_supported() -> %s\n", xud_result_name(supported));

    XudContext *context = NULL;
    const XudResult result = xud_init_context(&context, 640, 480);

    printf("xud_init_context(640, 480) -> %s (xudlibc %s)\n",
           xud_result_name(result), XUD_VERSION);

    if (result != XUD_OK) {
        /* XUD_UNSUPPORTED is the expected state until bring-up step S3 creates an instance;
           anything else is a real failure. */
        return result == XUD_UNSUPPORTED ? 0 : 1;
    }

    const XudResult closed = xud_close_context(&context);
    printf("xud_close_context -> %s\n", xud_result_name(closed));

    return closed == XUD_OK ? 0 : 1;
}
