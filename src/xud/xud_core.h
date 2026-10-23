/***********************************************************************************************
 *
 *   xud_core.h - the XudContext owner type
 *
 *   Owns : the struct definition that completes the opaque `typedef struct XudContext`
 *          declared in xud.h, and nothing else. Internal module header: user code includes
 *          xud.h only, and never sees these fields.
 *   Layer: core (bottom of the public API). The loader field is owned from bring-up on, the
 *          diagnostics field from the diagnostics step on (NULL in a release build), the instan
 *          ce field from the instance layer on, and the physical field from the physical layer
 *          on; the device, queue, surface, swapchain and allocator fields are still sentinels,
 *          filled by the bring-up steps after this one (see todo.md) - the device layer exists in
 *          the tree now (xud_device.h/.c, landed and tested at S6) but nothing in the context cal
 *          ls it until S8's ownership step, so the device field stays a sentinel here.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#ifndef XUD_XUD_CORE_H_
#define XUD_XUD_CORE_H_

#include "xud_diagnostics.h" /* Required for: XudDiagnostics, the diagnostics struct the context owns once validation is on */
#include "xud_instance.h" /* Required for: XudInstance, the VkInstance the context owns from the instance layer on */
#include "xud_loader.h" /* Required for: XudLoader, the loader the context owns from bring-up on */
#include "xud_physical.h" /* Required for: XudPhysicalDevice, the chosen handle plus the caps the context owns from the physical layer on */
#include <stdint.h>

/* Completed here; xud.h keeps callers to an opaque handle. Every Vulkan field is a
   sentinel (NULL / 0) until the layer named beside it fills it, and is destroyed in the
   reverse order of creation: swapchain -> surface -> device -> physical -> instance -> diagnos
   tics -> loader. The diagnostics entry sits where it does because its messenger must die befo
   re the VkInstance it belongs to (vkDestroyDebugUtilsMessengerEXT takes the owning instance, an
   d the extension says that instance must still be alive) - xud_instance_destroy() does that te
   ardown, and xud_diagnostics_close() only frees the struct once the messenger is gone. The ph
   ysical entry sits where it does for the same reason one rung down: a VkPhysicalDevice is a qu
   ery result of an instance, so the caps it recorded must be gone before that instance is.
 */
struct XudContext {
    /* Created and validated at repo skeleton step S1. */
    uint32_t width; /* window extent, pixels, as requested                        */
    uint32_t height; /* window extent, pixels, as requested                        */
    uint32_t validate; /* snapshot of XUD_VALIDATE, overridable by VK_INSTANCE_LAYERS */

    /* Owned from loader bring-up on: opened by xud_init_context(), closed by xud_close_context().
     */
    XudLoader* loader; /* dlopen handle for libvulkan.so.1 + the PFN table           */

    /* Owned from the diagnostics step on, and only when `validate` asked for validation: NULL in
       a release build, which is the "no layer requested, no validation request, no messenger"
       state. Opened before the instance (its layer list and validation-feature struct are what
       the instance create-info carries), freed after the instance is destroyed. */
    XudDiagnostics* diagnostics; /* advertised layers + VkValidationFeaturesEXT + the
                                    VK_EXT_debug_utils messenger */

    XudInstance* instance; /* the VkInstance plus the create-info that produced it       */

    /* Owned from the physical layer on: opened by xud_init_context() through xud_physical_open(),
       closed by xud_close_context() through xud_physical_close(). The close frees memory and call
       s no driver function at all - a VkPhysicalDevice is a query result of the instance above,
       and Vulkan has no vkDestroyPhysicalDevice (grepped in these headers, the name does not exi
       st). The caps inside are what VkDeviceCreateInfo's ppEnabled* arrays are intersected with a
       t S6, and what xud_vulkan_supported() reads.
     */
    XudPhysicalDevice*
        physicalDevice; /* the chosen VkPhysicalDevice plus the caps recorded from it */

    /* Pending: filled by the bring-up layers below, NULL/0 until then. */
    void* device; /* VkDevice                                                     */
    void* queueGraphics; /* the one graphics queue of the chosen family                */
    void* queuePresent; /* the one present queue of the chosen family                 */
    void* surface; /* VkSurfaceKHR handed over by glfwCreateWindowSurface        */
    void* swapchain; /* VkSwapchainKHR, recreated on window resize                 */
    void* allocator; /* the single GPU-memory allocator (buffers + images)         */
};

#endif /* XUD_XUD_CORE_H_ */
