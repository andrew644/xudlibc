/***********************************************************************************************
 *
 *   xud_core.h - the XudContext owner type
 *
 *   Owns : the struct definition that completes the opaque `typedef struct XudContext`
 *          declared in xud.h, and nothing else. Internal module header: user code includes
 *          xud.h only, and never sees these fields.
 *   Layer: core (bottom of the public API). Every field below the shell is owned from the step
 *          that fills it on: loader from bring-up, diagnostics from the diagnostics step (NULL in
 *          a release build), window from the window step, instance from the instance layer,
 *          physical device from the physical layer, device from the device layer, swapchain from
 *          the swapchain step - all of them filled by xud_init_context() now, so a context that
 *          comes back XUD_OK owns a live GLFW window, a VkInstance carrying that window's required
 *          extensions, a physical device queried through it, a VkDevice created from that device's
 *          recorded caps, a VkSurfaceKHR for the window and a VkSwapchainKHR for that surface. The
 *          allocator field is the only sentinel left, filled by the steps after this one (see
 *          todo.md). There is deliberately no queue field: XudQueue is a record of a queue, not an
 *          owning type (there is no vkDestroyQueue - grepped in these headers, the name does not
 *          exist), and both role records live inside the XudDevice that owns the handles, so a
 *          context field beside it would be a copy of a record with nothing of its own to tear
 *          down - read them through xud_device_queue_get().
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#ifndef XUD_XUD_CORE_H_
#define XUD_XUD_CORE_H_

#include "xud_device.h" /* Required for: XudDevice, the VkDevice plus its queue row, its enabled features/extensions and its two role queue records - the context owns one from the device layer on */
#include "xud_diagnostics.h" /* Required for: XudDiagnostics, the diagnostics struct the context owns once validation is on */
#include "xud_instance.h" /* Required for: XudInstance, the VkInstance the context owns from the instance layer on */
#include "xud_loader.h" /* Required for: XudLoader, the loader the context owns from bring-up on */
#include "xud_physical.h" /* Required for: XudPhysicalDevice, the chosen handle plus the caps the context owns from the physical layer on */
#include "xud_swapchain.h" /* Required for: XudSwapchain, the VkSwapchainKHR plus the images vkGetSwapchainImagesKHR handed it - the context owns one from the swapchain step on, and the teardown order below makes it die before the device whose queue it used */
#include "xud_window.h" /* Required for: XudWindow, the GLFW window plus the VkSurfaceKHR it was handed - the context will own one, and the teardown order below is what makes the surface die before the window and the window before the instance */
#include <stdint.h>

/* Completed here; xud.h keeps callers to an opaque handle. Every Vulkan field is a
   sentinel (NULL / 0) until the layer named beside it fills it, and is destroyed in the
   reverse order of creation: swapchain -> device -> surface -> window -> physical -> instance ->
   diagnostics -> loader. The device entry sits where it does because a VkDevice is created FROM the
   physical device below it and everything that submits to it (a swapchain, a command buffer)
   belongs to it, so it goes before the surface and the window whose swapchain would have used it -
   and the surface goes before the window because a VkSurfaceKHR is destroyed by vkDestroySurfaceKHR
   through the instance that window's surface was created for, while glfwTerminate - GLFW's own
   teardown, which the window layer runs once no live window is left - must not run while a surface
   still names a window. The diagnostics entry sits where it does because its messenger must die
   before the VkInstance it belongs to (vkDestroyDebugUtilsMessengerEXT takes the owning instance,
   and the extension says that instance must still be alive) - xud_instance_destroy() does that
   teardown, and xud_diagnostics_close() only frees the struct once the messenger is gone. The
   physical entry sits where it does for the same reason one rung down: a VkPhysicalDevice is a
   query result of an instance, so the caps it recorded must be gone before that instance is.
 */
struct XudContext {
    /* Created and validated at repo skeleton step S1. */
    uint32_t width; /* window extent, pixels, as requested                        */
    uint32_t height; /* window extent, pixels, as requested                        */
    uint32_t validate; /* snapshot of XUD_VALIDATE, overridable by VK_INSTANCE_LAYERS */

    /* Owned from loader bring-up on: opened by xud_init_context(), closed by xud_close_context().
     */
    XudLoader* loader; /* Volk's entry-point tables + the loader-level rows, per context  */

    /* Owned from the diagnostics step on, and only when `validate` asked for validation: NULL in
       a release build, which is the "no layer requested, no validation request, no messenger"
       state. Opened before the instance (its layer list and validation-feature struct are what
       the instance create-info carries), freed after the instance is destroyed. */
    XudDiagnostics* diagnostics; /* advertised layers + VkValidationFeaturesEXT + the
                                    VK_EXT_debug_utils messenger */

    /* Owned from the window step on, and BEFORE the instance for a reason that is load-bearing: glF
       WCreateWindowSurface resolves vkCreateWaylandSurfaceKHR through vkGetInstanceProcAddr AGAINST
       TH E INSTANCE IT IS HANDED (src/external/glfw/src/wl_window.c 145), so the instance the
       surface wi ll be created for has to carry GLFW's own required instance extensions, and those
       are only know able once a window exists. xud_init_context() therefore opens the window first
       and hands xud_in stance_create() the window's required-name list, which is what grew that
       extraNames/extraCount p arameter. The field is typed rather than a void* sentinel for the
       same reason a surface cannot b e held: the context owns one XudWindow, and the VkSurfaceKHR
       lives inside it.
     */
    XudWindow* window; /* a GLFW window plus the VkSurfaceKHR it was handed            */

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

    /* Owned from the device layer on: opened by xud_init_context() through xud_device_open(),
       closed by xud_close_context() through xud_device_close() - the wait-idle guard, then
       vkDestroyDevice through a row of the loader's device table, then free. It is no longer the
       last Vulkan rung the context owns through a field of its own - the swapchain below it is
       owned through a field of its own too, and it goes BEFORE this one in teardown order because
       a swapchain is presented through a queue of this device and its teardown runs the device's
       own wait-idle guard. The surface below the device is the one Vulkan rung owned by the window
       above rather than by a context field, which is why no field here names a VkSurfaceKHR. */
    XudDevice* device; /* the VkDevice plus its queue row, its enabled features/extensions and its
                          two role queue records */

    /* Owned from the swapchain step on: opened by xud_init_context() through xud_swapchain_open(),
       closed by xud_close_context() through xud_swapchain_close() FIRST in the teardown order
       below - a swapchain is presented through a queue of the device above, so it must be gone
       before that device is, and its close is the device's own wait-idle guard then
       vkDestroySwapchainKHR. It is the last field created in bring-up (the surface has to exist
       before a swapchain can be made for it) and the first torn down, which is what the order
       comment above states. The images it holds are the swapchain's, not the context's: there is
       no vkDestroyImage call anywhere in this tree, and xud_swapchain_close() frees the array
       rather than destroying it. */
    XudSwapchain* swapchain; /* the VkSwapchainKHR plus the images it was handed, recreated on
                                window resize */

    /* Deliberately absent: the graphics and present queues. XudQueue is a record of a queue, not an
       owning type (grepped in these headers: there is no vkDestroyQueue), and xud_device_open() res
       olved both role records into the XudDevice above, so the context has nothing of its own to
       own or tear down for them - a field here would be a second copy of a record that could
       disagree wi th the device's. xud_device_queue_get() is the accessor that reads them, and it
       answers the coo rdinates alongside the handle so a caller above can check the two queue VUIDs
       itself.
     */

    /* Pending: filled by the bring-up layers below, NULL/0 until then. The allocator is the only
       sentinel left in this struct - the GPU-memory rung that owns every buffer and image below the
       swapchain is not in the tree yet (see todo.md), and unlike the swapchain above there is no
       Vulkan object of that shape to name until it is.
     */
    void* allocator; /* the single GPU-memory allocator (buffers + images)         */
};

#endif /* XUD_XUD_CORE_H_ */
