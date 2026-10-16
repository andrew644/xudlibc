/***********************************************************************************************
 *
 *   xud_core.c - XudContext lifecycle
 *
 *   Owns : argument validation, the context shell allocation, the error-code names, the
 *          runtime Vulkan query, and teardown. The loader layer and the instance layer are
 *          reached from here, and only through xud_loader.h / xud_instance.h: nothing in this
 *          file calls a Vulkan entry point directly, and a machine with no driver only ever
 *          gets XUD_UNSUPPORTED, which is what keeps the headless test run green.
 *   Layer: core (bottom of the public API). Sees xud.h, xud_core.h and the layers below it
 *          (loader, instance).
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include "xud.h"
#include "xud_core.h"
#include "xud_loader.h"
#include "xud_instance.h"

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

    /* Loader bring-up: opens, dlsyms the seed entry point, and fills the global-level slots
       of the PFN table. XUD_UNSUPPORTED here means "no Vulkan loader on this machine". */
    result = xud_loader_open(&context->loader);
    if (result != XUD_OK) goto fail;

    /* The instance layer: vkCreateInstance at the VK_API_VERSION_1_0 floor, with only the
       advertised extensions enabled. XUD_UNSUPPORTED here means the loader is present but
       refuses to create an instance - a different reason from "no loader", and worth telling
       apart (see the test that distinguishes the two). */
    result = xud_instance_create(&context->instance, context->loader);
    if (result != XUD_OK) goto fail;

    /* The layers below this one do not exist yet, in order:
         physical device   vkEnumeratePhysicalDevices + caps recorded before any extension is enabled
         device + queues   vkCreateDevice + one queue family (graphics + present)
         swapchain         VkSwapchainKHR from the GLFW surface
         surface           glfwCreateWindowSurface -> VkSurfaceKHR
       None of them is there, so the context cannot draw anything: report that rather than
       hand back something that owns an instance and no way to use it. The instance and the
       loader are still torn down below - a partial bring-up must leak nothing. */
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

    /* Destroy order is the reverse of creation: instance before loader, and the layers that
       come later (swapchain -> surface -> device) slot in before the instance. xud_instance
       _destroy() clears the handle before freeing the struct, so a context that is torn down
       twice cannot double-free. */
    (void)xud_instance_destroy(&victim->instance, victim->loader);
    (void)xud_loader_close(&victim->loader);

    free(victim);
    *context = NULL;

    return XUD_OK;
}
