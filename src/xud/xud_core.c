/***********************************************************************************************
 *
 *   xud_core.c - XudContext lifecycle
 *
 *   Owns : argument validation, the context shell allocation, the error-code names, the
 *          runtime Vulkan query, and teardown. The loader layer is reached from here (step
 *          S2), and only through xud_loader.h: nothing in this file calls a Vulkan entry
 *          point directly, and a machine with no driver only ever gets XUD_UNSUPPORTED,
 *          which is what keeps the headless test run green.
 *   Layer: core (bottom of the public API). Sees xud.h, xud_core.h and the loader layer.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include "xud.h"
#include "xud_core.h"
#include "xud_loader.h"

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

XudResult xud_vulkan_supported(void)
{
    XudLoader *loader = NULL;
    XudResult result = XUD_INVALID_ARG;
    uint32_t resolved = 0u;

    result = xud_loader_open(&loader);
    if (result != XUD_OK)
    {
        if (loader != NULL) (void)xud_loader_close(&loader);
        return result;
    }

    resolved = loader->resolvedCount;
    (void)xud_loader_close(&loader);

    return (resolved >= XUD_LOADER_GLOBAL_SLOT_COUNT) ? XUD_OK : XUD_UNSUPPORTED;
}

XudResult xud_init_context(XudContext **outContext, uint32_t width, uint32_t height)
{
    XudContext *context = NULL;
    XudResult result = XUD_INVALID_ARG;

    if (outContext == NULL || *outContext != NULL) return XUD_INVALID_ARG;

    result = xud_context_create(&context, width, height);
    if (result != XUD_OK) return result;

    /* S2: the loader opens, dlsyms the seed entry point, and fills the global-level slots
       of the PFN table. XUD_UNSUPPORTED here means "no Vulkan loader on this machine" -
       still XUD_UNSUPPORTED overall, but a different reason, and worth telling apart. */
    result = xud_loader_open(&context->loader);
    if (result != XUD_OK) goto fail;

    /* Bring-up steps S3..S7 slot in here, in order:
         S3 vkCreateInstance (VK_API_VERSION_1_0 floor, advertised extensions only)
         S5 vkEnumeratePhysicalDevices + caps recorded before any extension is enabled
         S6 vkCreateDevice + one queue family (graphics + present)
         S7 glfwCreateWindowSurface -> VkSurfaceKHR
       None of them exist yet, so the loader is all there is to own: report that rather
       than hand back a context that cannot draw anything. */
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
       calls here (swapchain -> surface -> device -> instance) before the loader is closed;
       at step S2 the loader is the only sub-object that exists. */
    (void)xud_loader_close(&victim->loader);

    free(victim);
    *context = NULL;

    return XUD_OK;
}
