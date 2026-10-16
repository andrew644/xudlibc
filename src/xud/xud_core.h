/***********************************************************************************************
 *
 *   xud_core.h - the XudContext owner type
 *
 *   Owns : the struct definition that completes the opaque `typedef struct XudContext`
 *          declared in xud.h, and nothing else. Internal module header: user code includes
 *          xud.h only, and never sees these fields.
 *   Layer: core (bottom of the public API). The loader field is owned from bring-up on and
 *          the instance field from the instance layer on; the physical device, device, queue,
 *          surface, swapchain and allocator fields are still sentinels, filled by the bring-up
 *          steps after this one (see todo.md).
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#ifndef XUD_XUD_CORE_H_
#define XUD_XUD_CORE_H_

#include <stdint.h>
#include "xud_loader.h"    /* Required for: XudLoader, the loader the context owns from bring-up on */
#include "xud_instance.h"  /* Required for: XudInstance, the VkInstance the context owns from the instance layer on */

/* Completed here; xud.h keeps callers to an opaque handle. Every Vulkan field is a
   sentinel (NULL / 0) until the layer named beside it fills it, and is destroyed in the
   reverse order of creation: swapchain -> surface -> device -> instance -> loader. */
struct XudContext {
    /* Created and validated at repo skeleton step S1. */
    uint32_t width;            /* window extent, pixels, as requested                        */
    uint32_t height;           /* window extent, pixels, as requested                        */
    uint32_t validate;         /* snapshot of XUD_VALIDATE, overridable by VK_INSTANCE_LAYERS */

    /* Owned from loader bring-up on: opened by xud_init_context(), closed by xud_close_context(). */
    XudLoader *loader;         /* dlopen handle for libvulkan.so.1 + the PFN table           */
    XudInstance *instance;     /* the VkInstance plus the create-info that produced it       */

    /* Pending: filled by the bring-up layers below, NULL/0 until then. */
    void *physicalDevice;      /* VkPhysicalDevice (a handle: never destroyed)               */
    void *device;              /* VkDevice                                                     */
    void *queueGraphics;       /* the one graphics queue of the chosen family                */
    void *queuePresent;        /* the one present queue of the chosen family                 */
    void *surface;             /* VkSurfaceKHR handed over by glfwCreateWindowSurface        */
    void *swapchain;           /* VkSwapchainKHR, recreated on window resize                 */
    void *allocator;           /* the single GPU-memory allocator (buffers + images)         */
};

#endif /* XUD_XUD_CORE_H_ */
