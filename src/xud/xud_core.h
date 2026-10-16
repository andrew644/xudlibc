/***********************************************************************************************
 *
 *   xud_core.h - the XudContext owner type
 *
 *   Owns : the struct definition that completes the opaque `typedef struct XudContext`
 *          declared in xud.h, and nothing else. Internal module header: user code includes
 *          xud.h only, and never sees these fields.
 *   Layer: core (bottom of the public API). The loader field is owned from step S2 on;
 *          the instance, device, queue, surface, swapchain and allocator fields are still
 *          sentinels, filled by bring-up steps S3..S8 (see todo.md).
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#ifndef XUD_XUD_CORE_H_
#define XUD_XUD_CORE_H_

#include <stdint.h>
#include "xud_loader.h"   /* Required for: XudLoader, the loader the context owns from step S2 on */

/* Completed here; xud.h keeps callers to an opaque handle. Every Vulkan field is a
   sentinel (NULL / 0) until the step named beside it runs, and is destroyed in the
   reverse order of creation: swapchain -> surface -> device -> instance -> loader. */
struct XudContext {
    /* Created and validated at step S1. */
    uint32_t width;            /* window extent, pixels, as requested                        */
    uint32_t height;           /* window extent, pixels, as requested                        */
    uint32_t validate;         /* snapshot of XUD_VALIDATE, overridable by VK_INSTANCE_LAYERS */

    /* Owned from step S2 on: opened by xud_init_context(), closed by xud_close_context(). */
    XudLoader *loader;         /* S2: dlopen handle for libvulkan.so.1 + the PFN table       */

    /* Pending: filled by the bring-up steps below, NULL/0 until then. */
    void *instance;            /* S3: VkInstance                                             */
    void *physicalDevice;      /* S5: VkPhysicalDevice (a handle: never destroyed)           */
    void *device;              /* S6: VkDevice                                               */
    void *queueGraphics;       /* S6: the one graphics queue of the chosen family            */
    void *queuePresent;        /* S6: the one present queue of the chosen family             */
    void *surface;             /* S7: VkSurfaceKHR handed over by glfwCreateWindowSurface    */
    void *swapchain;           /* next step: VkSwapchainKHR, recreated on window resize      */
    void *allocator;           /* later: the single GPU-memory allocator (buffers + images)  */
};

#endif /* XUD_XUD_CORE_H_ */
