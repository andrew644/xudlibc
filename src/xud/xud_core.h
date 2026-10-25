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
 *          the swapchain step, the allocator from the allocator step, renderer from the first-triangle
 *          step, handle table from the handle step - all of them filled by xud_init_context() now, so
 *          a context that comes back XUD_OK owns a live GLFW window, a VkInstance carrying that windo
 *          w's required extensions, a physical device queried through it, a VkDevice created from that
 *          device's recorded caps, a VkSwapchainKHR for that surface, one block allocator over the memo
 *          ry rows the physical layer recorded, the renderer rung that draws one triangle into its image
 *          s (one pipeline, one render pass, one framebuffer per image, one frame loop) and the one table
 *          the public handle currency indexes. No field below the shell is a sentinel any more - the all
 *          ocator rung that was the last one is in the tree (see todo.md and todo_completed.md). There is
 *          deliberately no queue field: XudQueue is a record of a queue, not an owning type (there is no
 *          vkDestroyQueue - grepped in these headers, the name does not exist), and both role records live
 *          inside the XudDevice that owns the handles, so a context field beside it would be a copy of a
 *          record with nothing of its own to tear down - read them through xud_device_queue_get().
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#ifndef XUD_XUD_CORE_H_
#define XUD_XUD_CORE_H_

#include "xud_allocator.h" /* Required for: XudAllocator, the GPU-memory rung that owns every VkDeviceMemory this tree creates - the context owns one from the allocator step on, and the teardown order below makes it die before the device it was allocated from */
#include "xud_descriptor.h" /* Required for: XudDescriptor, the descriptor-layout rung the context owns between the allocator and the renderer - its bring-up carves the per-frame uniform buffer out of the allocator, and the teardown order makes this rung die after the renderer and before the allocator */
#include "xud_device.h" /* Required for: XudDevice, the VkDevice plus its queue row, its enabled features/extensions and its two role queue records - the context owns one from the device layer on */
#include "xud_diagnostics.h" /* Required for: XudDiagnostics, the diagnostics struct the context owns once validation is on */
#include "xud_handle.h" /* Required for: XudHandleTable, the per-kind slot arrays behind the public XudHandle currency - the context owns one from the handle step on, and it is the FIRST rung torn down because a handle records an object of every rung below it */
#include "xud_instance.h" /* Required for: XudInstance, the VkInstance the context owns from the instance layer on */
#include "xud_loader.h" /* Required for: XudLoader, the loader the context owns from bring-up on */
#include "xud_physical.h" /* Required for: XudPhysicalDevice, the chosen handle plus the caps the context owns from the physical layer on */
#include "xud_renderer.h" /* Required for: XudRenderer, the renderer rung - the pipeline, the render pass, the framebuffer per swapchain image and the frame loop the context owns from the first-triangle step on, and the rung that goes SECOND in the teardown order */
#include "xud_swapchain.h" /* Required for: XudSwapchain, the VkSwapchainKHR plus the images vkGetSwapchainImagesKHR handed it - the context owns one from the swapchain step on, and the teardown order below makes it die before the device whose queue it used */
#include "xud_window.h" /* Required for: XudWindow, the GLFW window plus the VkSurfaceKHR it was handed - the context will own one, and the teardown order below is what makes the surface die before the window and the window before the instance */
#include <stdint.h>

/* Completed here; xud.h keeps callers to an opaque handle. Every Vulkan field is a
   sentinel (NULL / 0) until the layer named beside it fills it, and is destroyed in the
   reverse order of creation: handle table -> renderer -> allocator -> swapchain -> device -> surface
   -> window -> physical -> instance -> diagnostics -> loader. The handle table goes FIRST because it
   is the top of the tree: a handle records an object of every rung below it, so a table still holding
   live slots is a table naming objects the rungs below are about to retire - and it is the last rung
   opened, since no object of any kind can be created before the context that will own it exists. The
   renderer entry sits where it does because the frame loop inside it records a draw that binds a pipe
   line and presents an image of the swapchain below it, and its own close is the fence guard that ret
   ires a presentation still in flight - so the renderer must be gone before the swapchain whose image
   s it presented is. The allocator entry sits where it does because a VkDeviceMemory is created FROM
   the VkDevice below it and the renderer above it is what will name the buffers and images carved out
   of it, so it dies after the renderer and before the swapchain and the device; its own close guards
   with xud_device_wait_idle() first, for the same reason xud_swapchain_close() gives. The device entry
   sits where it does because a VkDevice is created FROM the physical device below it and everything
   that submits to it (a swapchain, a command buffer) belongs to it, so it goes before the surface
   and the window whose swapchain would have used it -
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
    XudPhysicalDevice* physicalDevice; /* the chosen VkPhysicalDevice plus the caps recorded from it */

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
       vkDestroySwapchainKHR. It is the last Vulkan rung created before the renderer above it (the
       surface has to exist before a swapchain can be made for it, and the renderer needs this one's
       chosen format and image count), and it goes before the device in teardown, which is what the
       order comment above states. The images it holds are the swapchain's, not the context's: there is
       no vkDestroyImage call anywhere in this tree, and xud_swapchain_close() frees the array
       rather than destroying it. */
    XudSwapchain* swapchain; /* the VkSwapchainKHR plus the images it was handed, recreated on
                                window resize */

    /* Owned from the renderer step on: opened by xud_init_context() through xud_renderer_open() and
       closed by xud_close_context() through xud_renderer_close() SECOND in the teardown order below,
       right after the handle table and before the swapchain. It is the owner of the four rungs that
       make a frame possible - the pipeline (with its layout and its two shader modules inside it),
       the one explicit VkRenderPass of the repo's settled render-pass decision, one VkFramebuffer
       (and one VkImageView) per swapchain image, and the frame loop's command pool, command buffer,
       acquire semaphore and per-image fences - and it goes where it does because a frame records a
       draw that binds a pipeline and presents an image of the swapchain below it, so the fences insi
       de it are the last thing that can still be in flight against those very images. Its close runs
       that fence guard before it destroys anything. The order inside it is the plan's dependency
       direction read both ways: pipeline -> render pass -> framebuffer array -> frame on the way in,
       the exact reverse on the way out (see xud_renderer.h).
     */
    XudRenderer* renderer; /* the pipeline, the render pass, the framebuffer per swapchain image
                              and the frame loop that draws one triangle into them */

    /* Deliberately absent: the graphics and present queues. XudQueue is a record of a queue, not an
       owning type (grepped in these headers: there is no vkDestroyQueue), and xud_device_open() res
       olved both role records into the XudDevice above, so the context has nothing of its own to
       own or tear down for them - a field here would be a second copy of a record that could
       disagree wi th the device's. xud_device_queue_get() is the accessor that reads them, and it
       answers the coo rdinates alongside the handle so a caller above can check the two queue VUIDs
       itself.
     */

    /* Owned from the handle step on: opened by xud_init_context() through xud_handle_table_open()
       and closed by xud_close_context() through xud_handle_table_close() FIRST in the teardown order
       above. It is the table the public XudHandle of xud.h indexes, and it is per context for the
       reason the handle layer's own note gives: two contexts have two tables, and a handle from one
       is refused by the other rather than indexing the wrong array. It is the top of the tree today
       - every slot record is still the sentinel the handle layer opens it with, because the object
       steps that would fill it (a texture, a font, a mesh, a material, a model) are not in the tree
       even though the renderer rung below it is - so closing it frees records and calls nothing, and
       it still goes first because a handle is a
       request for an object of every rung below it.
     */
    XudHandleTable* handleTable; /* the per-kind slot arrays behind the public handle currency */

    /* The allocator, the GPU-memory rung below the renderer and above the device: one block alloca
       tor over the memory rows the physical layer already recorded, and the ONE owner of every
       VkDeviceMemory this tree creates - nothing above it may call vkAllocateMemory for itself, wh
       ich is what makes "allocate once, suballocate always" a rule rather than a wish. It is fille
       d by xud_init_context() after the swapchain and torn down by xud_close_context() BEFORE the
       swapchain and before the device, because a VkDeviceMemory is created from a VkDevice and a
       submission or a presentation may still be reading the very blocks being freed (xud_allocator_
       close() guards with xud_device_wait_idle() for exactly that reason). What is NOT in the tree
       yet is what the allocator would hand out: there is still no buffer, no vertex stream, no desc
       riptor set and no batch, so a context that comes back XUD_OK owns an allocator with no block
       in it, and the field below is typed rather than a sentinel because this rung IS in the tree
       - the last sentinel this struct had.
     */
    XudAllocator* allocator; /* the single GPU-memory allocator (buffers + images)         */

    /* Owned from the descriptor-layout step on: opened by xud_init_context() through xud_descrip
       tor_open() AFTER the allocator and closed by xud_close_context() through xud_descriptor_clos
       e() BEFORE it and after the renderer. The position is the ownership ladder this rung sits on:
       a VkBuffer is created from memory the allocator owns and is named by the pipeline layout the
       renderer builds, so the order in is allocator -> descriptor -> renderer and the order out is
       its exact reverse. What is live here today is the two set layouts, the pool, set 0, the per-
       frame uniform buffer bound into the allocator's first block and the write that binds it; set
       1's layout exists and its SET does not, because the texture step is what allocates it.
     */
    XudDescriptor* descriptor; /* the layouts, the pool, set 0 and the uniform buffer inside it  */
};

#endif /* XUD_XUD_CORE_H_ */
