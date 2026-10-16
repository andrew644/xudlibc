/***********************************************************************************************
 *
 *   hello_context.c - the smallest program that touches the xudlibc public API
 *
 *   Owns : nothing but a call to xud_init_context() and its teardown. One concern: what
 *          the library can do today. At bring-up step S1 the Vulkan loader is not
 *          resolved yet, so the honest answer is XUD_UNSUPPORTED and this prints it.
 *   Layer: examples. Public API only (xud.h).
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include <stdio.h>

#include "xud.h"

int main(void)
{
    XudContext *context = NULL;
    const XudResult result = xud_init_context(&context, 640, 480);

    if (result != XUD_OK) {
        printf("xud_init_context(640, 480) -> %s (xudlibc %s)\n",
               xud_result_name(result), XUD_VERSION);
        /* XUD_UNSUPPORTED is the expected state until bring-up step S2 resolves the
           Vulkan loader; anything else is a real failure. */
        return result == XUD_UNSUPPORTED ? 0 : 1;
    }

    const XudResult closed = xud_close_context(&context);
    printf("xud_close_context -> %s\n", xud_result_name(closed));

    return closed == XUD_OK ? 0 : 1;
}
