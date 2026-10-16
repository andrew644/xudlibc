/***********************************************************************************************
 *
 *   xud_core.c - XudContext lifecycle
 *
 *   Owns : argument validation, the context shell allocation, the error-code names, and
 *          teardown. No Vulkan entry point is reached from this file at step S1: the
 *          loader is not dlopen()ed yet (that is step S2), so nothing here can depend on
 *          a driver being present, which is what keeps the headless test run green.
 *   Layer: core (bottom of the public API). Sees only xud.h and xud_core.h.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include "xud.h"
#include "xud_core.h"

#include <stdlib.h>

/* Internal: allocate a zeroed context shell. Not part of the public API - the only way
   user code may obtain a context is xud_init_context(), so xud_close_context() can rely
   on every context it frees being heap-allocated. */
static XudResult xud_context_create(XudContext **outContext, uint32_t width, uint32_t height)
{
    XudContext *context = NULL;

    if (outContext == NULL || *outContext != NULL) return XUD_INVALID_ARG;
    if (width == 0u || height == 0u) return XUD_INVALID_ARG;
    if (width > XUD_MAX_WINDOW_EXTENT || height > XUD_MAX_WINDOW_EXTENT) return XUD_INVALID_ARG;

    context = calloc(1, sizeof(XudContext));
    if (context == NULL) return XUD_OUT_OF_MEMORY;

    context->width = width;
    context->height = height;
    context->validate = XUD_VALIDATE;

    *outContext = context;
    return XUD_OK;
}

const char *xud_result_name(XudResult result)
{
    switch (result) {
    case XUD_OK:               return "XUD_OK";
    case XUD_UNSUPPORTED:      return "XUD_UNSUPPORTED";
    case XUD_INVALID_ARG:      return "XUD_INVALID_ARG";
    case XUD_OUT_OF_MEMORY:    return "XUD_OUT_OF_MEMORY";
    case XUD_OUT_OF_RESOURCES: return "XUD_OUT_OF_RESOURCES";
    case XUD_ALREADY:          return "XUD_ALREADY";
    case XUD_LOST:             return "XUD_LOST";
    case XUD_VULKAN_ERROR:     return "XUD_VULKAN_ERROR";
    case XUD_INTERNAL_ERROR:   return "XUD_INTERNAL_ERROR";
    }
    return "XUD_OUT_OF_RANGE";
}

XudResult xud_init_context(XudContext **outContext, uint32_t width, uint32_t height)
{
    XudContext *context = NULL;
    XudResult result = XUD_INVALID_ARG;

    if (outContext == NULL || *outContext != NULL) return XUD_INVALID_ARG;

    result = xud_context_create(&context, width, height);
    if (result != XUD_OK) return result;

    /* Bring-up steps S2..S7 slot in here, in order:
         S2 dlopen("libvulkan.so.1") + vkGetInstanceProcAddr into the PFN table
         S3 vkCreateInstance (VK_API_VERSION_1_0 floor, advertised extensions only)
         S5 vkEnumeratePhysicalDevices + caps recorded before any extension is enabled
         S6 vkCreateDevice + one queue family (graphics + present)
         S7 glfwCreateWindowSurface -> VkSurfaceKHR
       None of them exist yet, so there is nothing to own: report that rather than hand
       back a context that cannot draw anything. */
    result = XUD_UNSUPPORTED;
    goto fail;

fail:
    xud_close_context(&context);
    *outContext = NULL;
    return result;
}

XudResult xud_close_context(XudContext **context)
{
    XudContext *victim = NULL;

    if (context == NULL) return XUD_INVALID_ARG;
    if (*context == NULL) return XUD_ALREADY;

    victim = *context;

    /* Destroy order is the reverse of creation. Steps S3..S8 add the Vulkan teardown
       calls here (device -> surface -> instance -> loader) before the shell is freed. */

    free(victim);
    *context = NULL;

    return XUD_OK;
}
