/***********************************************************************************************
 *
 *   xud_frame.c - the frame loop: one command pool, one command buffer, the acquire/submit/p
 *          resent triple, and the acquire semaphore + per-image fences that bound a frame
 *
 *   Owns : vkCreateCommandPool / vkAllocateCommandBuffers / vkCreateSemaphore / vkCreateFence per
 *          swapchain image, one recorded frame (bind pipeline, set viewport + scissor, begin rende
 *          r pass, draw, end render pass), and the teardown that hands every one of them back.
 *   Layer: above the render pass and the framebuffer array, below the public API. Nothing in this
 *          file calls a Vulkan entry point directly - every one is reached through a row of XudLoa
 *          der's Volk device table, so the archive holds no vk* symbol and the link line has no -lv
 *          ulkan anywhere (AGENTS.md).
 *          The synchronization is the classic kind the repo settles, spelled with core-1.0 names only:
 *          an acquire semaphore waited BY THE SUBMIT at the vertex stage - vkWaitSemaphores is a core-1.2
 *          entry point and this library's instance sits at the core-1.0 floor, so a host-side wait would
 *          be a nil row rather than a refusal with a fallback, and the repo's rule is that a name that is
 *          not there is reported, not worked around. ONE fence per swapchain image, handed to vkQueueSub
 *          mit as its fence argument and waited and reset at the end of the frame that wrote it, and no
 *          VK_KHR_vulkan_memory_model anywhere.
 *          The fence array is per IMAGE rather than per frame, and that is the point of the plan's wording:
 *          what has to be true before an image is written again is that the presentation of THAT image
 *          finished.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud_frame.h"
#include "xud.h"
#include "xud_device.h"
#include "xud_loader.h"
#include "xud_physical.h"
#include "xud_pipeline.h"
#include "xud_renderpass.h"
#include "xud_swapchain.h"
#include "xud_vulkan.h"

#include <assert.h> /* Required for: static_assert, the row and array-size checks */
#include <stdlib.h> /* Required for: calloc, free */

/* The rows this layer calls through must be pointer-sized, the same rule xud_loader.h checks for
   the table pair as a whole. */
static_assert(sizeof(PFN_vkCreateCommandPool) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkDestroyCommandPool) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkAllocateCommandBuffers) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkFreeCommandBuffers) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkResetCommandBuffer) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkBeginCommandBuffer) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkEndCommandBuffer) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkCmdBindPipeline) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkCmdSetViewport) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkCmdSetScissor) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkCmdBeginRenderPass) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkCmdEndRenderPass) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkCmdDraw) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkCreateSemaphore) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkDestroySemaphore) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkCreateFence) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkDestroyFence) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkResetFences) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkWaitForFences) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkAcquireNextImageKHR) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkQueueSubmit) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkQueuePresentKHR) == sizeof(void*), "PFN row must be pointer-sized");

/* The command-buffer row array the allocate call is handed must be exactly the count the allocation
   claims, and the member access inside sizeof is not evaluated. */
static_assert(sizeof(((XudFrame*)0)->commandBufferRows) / sizeof(VkCommandBuffer) == XUD_FRAME_COMMAND_BUFFER_COUNT,
    "the command-buffer row array must be exactly the count the allocation claims");
static_assert(sizeof(((XudFrame*)0)->waitStages) / sizeof(VkPipelineStageFlags) == XUD_FRAME_SUBMIT_COUNT, "one wait-stage row per submit");

/* The create-infos the struct keeps must be the create-infos the driver calls were handed - the sam
   e "the create-info and the record of it cannot disagree" rule xud_swapchain.c states for its own
   pair, one rung below. */
static_assert(sizeof(((XudFrame*)0)->poolInfo) == sizeof(VkCommandPoolCreateInfo),
    "the recorded pool create-info must be the create-info the call was handed");
static_assert(sizeof(((XudFrame*)0)->submitInfo) == sizeof(VkSubmitInfo), "the recorded submit row must be the row the submit was handed");
static_assert(
    sizeof(((XudFrame*)0)->presentInfo) == sizeof(VkPresentInfoKHR), "the recorded present row must be the row the present was handed");

/*-----------------------------------------------------------------------------------------------
 * The pool, the recording rows and the submit/present rows - pure, no driver call
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_frame_fill_pool_info(VkCommandPoolCreateInfo* outCreateInfo, uint32_t queueFamilyIndex) {
    if (outCreateInfo == NULL) {
        return XUD_INVALID_ARG;
    }
    if (queueFamilyIndex == XUD_PHYSICAL_NO_QUEUE_FAMILY) {
        return XUD_INVALID_ARG;
    }

    outCreateInfo->sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    outCreateInfo->pNext = NULL;

    /* VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT is the one flag this rung needs, and the reas
       on is that the command buffer is re-recorded every frame: a pool created without the flag can
       only ever hand out buffers that are recorded once, which would make the second frame's begin a
       validation error rather than a frame. The protected bit is not set - the rule xud_device.c stat
       es for a queue row's flags is the same one here, a bit that is not asked for stays off. */
    outCreateInfo->flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    /* The chosen family, not a re-picked one: the physical layer already chose one family and the dev
       ice layer already created a queue from it, which is the rule xud_swapchain_fill_create_info() s
       tates for its own queue-family row one rung below. */
    outCreateInfo->queueFamilyIndex = queueFamilyIndex;

    return XUD_OK;
}

XudResult xud_frame_fill_allocation(VkCommandBufferAllocateInfo* outAllocation, VkCommandPool commandPool) {
    if (outAllocation == NULL) {
        return XUD_INVALID_ARG;
    }
    if (commandPool == VK_NULL_HANDLE) {
        return XUD_UNSUPPORTED;
    }

    outAllocation->sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    outAllocation->pNext = NULL;
    outAllocation->commandPool = commandPool;
    outAllocation->level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    outAllocation->commandBufferCount = XUD_FRAME_COMMAND_BUFFER_COUNT;

    return XUD_OK;
}

XudResult xud_frame_fill_recording(
    VkCommandBufferBeginInfo* outBegin, VkViewport* outViewport, VkRect2D* outScissor, uint32_t width, uint32_t height) {
    if ((outBegin == NULL) || (outViewport == NULL) || (outScissor == NULL)) {
        return XUD_INVALID_ARG;
    }
    if ((width == 0u) || (height == 0u)) {
        return XUD_INVALID_ARG;
    }

    outBegin->sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    outBegin->pNext = NULL;
    outBegin->flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    outBegin->pInheritanceInfo = NULL; /* no secondary command buffer exists in this tree, so there i
                                         s nothing to inherit */

    /* The viewport and the scissor are the two dynamic states xud_pipeline_fill_state() declares, and
       this is the one place they are set. The extent is the swapchain's / the framebuffer row's, copied
       rather than re-queried (xud_swapchain.c's note about the surface's "undefined" currentExtent). A
       viewport in Vulkan is a rectangle in framebuffer space with the image's own y-down convention, s
       o x/y 0 and the full extent is the "draw into the whole image" answer - and the scissor is the sa
       me rectangle, which is what makes the two agree rather than one clipping the other. */
    outViewport->x = 0.0f;
    outViewport->y = 0.0f;
    outViewport->width = (float)width;
    outViewport->height = (float)height;
    outViewport->minDepth = 0.0f;
    outViewport->maxDepth = 1.0f; /* the depth range this rung does not use - there is no depth attachm
                                    ent and no depth test anywhere in this pipeline - and the spec's defa
                                    ult for a viewport is the full 0..1 range, so writing it is what keep
                                    s the record of the call complete rather than leaving two floats of a
                                    n uninitialised frame-local in it */

    outScissor->offset.x = 0;
    outScissor->offset.y = 0;
    outScissor->extent.width = width;
    outScissor->extent.height = height;

    return XUD_OK;
}

XudResult xud_frame_fill_submit(
    VkSubmitInfo* outSubmit, VkPresentInfoKHR* outPresent, VkCommandBuffer commandBuffer, VkSwapchainKHR swapchain) {
    if ((outSubmit == NULL) || (outPresent == NULL)) {
        return XUD_INVALID_ARG;
    }
    if (commandBuffer == VK_NULL_HANDLE) {
        return XUD_INVALID_ARG;
    }
    if (swapchain == VK_NULL_HANDLE) {
        return XUD_INVALID_ARG;
    }

    outSubmit->sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    outSubmit->pNext = NULL;
    outSubmit->waitSemaphoreCount = 1u;
    outSubmit->pWaitSemaphores = NULL; /* filled by the driver half, which is the side that has a semap
                                         hore handle in hand */
    outSubmit->pWaitDstStageMask = NULL; /* filled by the driver half, which owns the stage row */
    outSubmit->commandBufferCount = XUD_FRAME_COMMAND_BUFFER_COUNT;
    outSubmit->pCommandBuffers = NULL; /* filled by the driver half, which owns the buffer array */
    outSubmit->signalSemaphoreCount = 0u;
    outSubmit->pSignalSemaphores = NULL; /* nothing is signalled by the submit itself: the fence is vkQ
                                           ueueSubmit's own argument, and a signal semaphore here would b
                                           e a second, redundant channel for the same boundary */

    outPresent->sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    outPresent->pNext = NULL;
    outPresent->waitSemaphoreCount = 0u;
    outPresent->pWaitSemaphores = NULL; /* the presentation needs no further signal here: the fence at t
                                          he end of the frame is what bounds it, and a wait semaphore on t
                                          he present would be a second way to say the same thing */
    outPresent->swapchainCount = 1u;
    outPresent->pSwapchains = NULL; /* filled by the driver half, which owns the swapchain handle */
    outPresent->pImageIndices = NULL; /* filled by the driver half, which owns the index */
    outPresent->pResults = NULL; /* the per-swapchain VkResult out-array is refused here rather than as
                                   ked for: the rung's own return code is the report, and a pResults arr
                                   ay would be a second channel for the same answer that a caller above w
                                   ould have to read */

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The bring-up - the driver half
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_frame_open(XudFrame** outFrame, XudLoader* loader, const XudDevice* device, const XudSwapchain* swapchain) {
    XudFrame* frame = NULL;
    XudResult result = XUD_INVALID_ARG;
    VkResult vk = VK_ERROR_UNKNOWN;
    uint32_t index = 0u;
    uint32_t fencesMade = 0u;

    if ((outFrame == NULL) || (*outFrame != NULL)) {
        return XUD_INVALID_ARG;
    }
    if ((loader == NULL) || (device == NULL) || (swapchain == NULL)) {
        return (loader == NULL) ? XUD_INVALID_ARG : XUD_UNSUPPORTED;
    }
    if (!loader->open) {
        return XUD_UNSUPPORTED;
    }
    if ((device->device == VK_NULL_HANDLE) || (swapchain->swapchain == VK_NULL_HANDLE)) {
        return XUD_INVALID_ARG;
    }
    if ((swapchain->imageCount == 0u) || (swapchain->images == NULL)) {
        return XUD_INVALID_ARG;
    }

    /* Every destroy row the teardown below needs, checked BEFORE any create row it would otherwise lea
       k - the rule xud_window.c states for a surface and xud_swapchain.c for a swapchain, one rung bel
       ow. vkResetFences and vkWaitForFences are in this list because the frame loop needs them, and a r
       ungs rows are checked as a set rather than one at a time at each call site, which is what makes a
        half-present table a refusal rather than a crash half-way through a frame. */
    if ((loader->deviceTable.vkDestroyCommandPool == NULL) || (loader->deviceTable.vkCreateCommandPool == NULL)
        || (loader->deviceTable.vkAllocateCommandBuffers == NULL) || (loader->deviceTable.vkFreeCommandBuffers == NULL)
        || (loader->deviceTable.vkDestroySemaphore == NULL) || (loader->deviceTable.vkCreateSemaphore == NULL)
        || (loader->deviceTable.vkDestroyFence == NULL) || (loader->deviceTable.vkCreateFence == NULL)
        || (loader->deviceTable.vkResetFences == NULL) || (loader->deviceTable.vkWaitForFences == NULL)) {
        return XUD_UNSUPPORTED;
    }

    /* The rows must be THIS device's rows - a pool created through another device's rows is a pool this
       device cannot destroy. */
    if (!xud_loader_device_bound(loader, device->device)) {
        return XUD_UNSUPPORTED;
    }

    frame = calloc(1u, sizeof(*frame));
    if (frame == NULL) {
        return XUD_OUT_OF_MEMORY;
    }

    frame->imageFences = calloc(swapchain->imageCount, sizeof(*frame->imageFences));
    if (frame->imageFences == NULL) {
        free(frame);
        return XUD_OUT_OF_MEMORY;
    }
    frame->imageFenceCapacity = swapchain->imageCount;

    /* The per-image layout record, and the reason it is a record rather than a constant: a swapchain
       image has a layout, the render pass this rung begins requires its attachment to be in the
       attachment's initialLayout, and the spec requires the image to have been IN that layout when
       the pass begins (VUID-VkRenderPassBeginInfo-framebuffer-initialLayout-00009). The only way to
       get there is a barrier, and a barrier names the layout the image is in NOW - so the rung has to
       know it. It starts at VK_IMAGE_LAYOUT_UNDEFINED, which is what a freshly created swapchain
       image really is, and every frame leaves its image in the pass's finalLayout, which is the
       source layout of the NEXT frame's barrier. Recording it is what makes that pair testable
       rather than a claim, and the reason it is per image rather than one value is that the images
       are acquired in an order this rung does not control. */
    frame->imageLayouts = calloc(swapchain->imageCount, sizeof(*frame->imageLayouts));
    if (frame->imageLayouts == NULL) {
        free(frame->imageFences);
        free(frame);
        return XUD_OUT_OF_MEMORY;
    }
    frame->imageLayoutCount = swapchain->imageCount;
    for (index = 0u; index < swapchain->imageCount; index++) {
        frame->imageLayouts[index] = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    /* The depth image's layout row, written rather than left at zero: VK_IMAGE_LAYOUT_UNDEFINED happe
       ns to be 0 and calloc would give it, but the field is state the first frame's barrier reads as
       its oldLayout, and writing the constant is what says so to a reader rather than to a numerica
       l coincidence. One field, not an array - see the note in xud_frame.h for why the colour half i
       s an array and this half is not. */
    frame->depthLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    /* The fence and the semaphore create-infos are the empty forms: no initial fence state is asked for
       (VK_FENCE_CREATE_SIGNALED_BIT would be a fence that starts already signalled, which is the opposit
       e of what a fence that has to bound a submission means), and VkSemaphoreCreateInfo has nothing els
       e to say. */
    frame->semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    frame->semaphoreInfo.pNext = NULL;
    frame->semaphoreInfo.flags = 0u;
    frame->fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    frame->fenceInfo.pNext = NULL;
    frame->fenceInfo.flags = 0u;

    /* The wait-stage row is filled at open rather than per frame: it is part of the submit row the stru
       ct keeps, and a stage row that was left at 0 would be a wait bound at no stage at all, which is a
       submission that reads the image before the semaphore was waited. */
    frame->waitStages[0u] = XUD_FRAME_WAIT_STAGE;

    result = xud_frame_fill_pool_info(&frame->poolInfo, device->queueRow.queueFamilyIndex);
    if (result != XUD_OK) {
        free(frame->imageLayouts);
        free(frame->imageFences);
        free(frame);
        return result;
    }

    vk = loader->deviceTable.vkCreateCommandPool(device->device, &frame->poolInfo, NULL, &frame->commandPool);
    if (vk != VK_SUCCESS) {
        free(frame->imageLayouts);
        free(frame->imageFences);
        free(frame);
        return xud_result_from_vk(vk);
    }

    result = xud_frame_fill_allocation(&frame->allocationInfo, frame->commandPool);
    if (result != XUD_OK) {
        (void)loader->deviceTable.vkDestroyCommandPool(device->device, frame->commandPool, NULL);
        frame->commandPool = VK_NULL_HANDLE;
        free(frame->imageLayouts);
        free(frame->imageFences);
        free(frame);
        return result;
    }

    vk = loader->deviceTable.vkAllocateCommandBuffers(device->device, &frame->allocationInfo, frame->commandBufferRows);
    if (vk != VK_SUCCESS) {
        (void)loader->deviceTable.vkDestroyCommandPool(device->device, frame->commandPool, NULL);
        frame->commandPool = VK_NULL_HANDLE;
        free(frame->imageLayouts);
        free(frame->imageFences);
        free(frame);
        return xud_result_from_vk(vk);
    }
    frame->commandBuffer = frame->commandBufferRows[0u];
    if (frame->commandBuffer == VK_NULL_HANDLE) {
        loader->deviceTable.vkFreeCommandBuffers(
            device->device, frame->commandPool, XUD_FRAME_COMMAND_BUFFER_COUNT, frame->commandBufferRows);
        (void)loader->deviceTable.vkDestroyCommandPool(device->device, frame->commandPool, NULL);
        frame->commandPool = VK_NULL_HANDLE;
        frame->commandBuffer = VK_NULL_HANDLE;
        free(frame->imageLayouts);
        free(frame->imageFences);
        free(frame);
        return XUD_INTERNAL_ERROR;
    }

    vk = loader->deviceTable.vkCreateSemaphore(device->device, &frame->semaphoreInfo, NULL, &frame->imageAcquired);
    if (vk != VK_SUCCESS) {
        loader->deviceTable.vkFreeCommandBuffers(
            device->device, frame->commandPool, XUD_FRAME_COMMAND_BUFFER_COUNT, frame->commandBufferRows);
        (void)loader->deviceTable.vkDestroyCommandPool(device->device, frame->commandPool, NULL);
        frame->commandPool = VK_NULL_HANDLE;
        frame->commandBuffer = VK_NULL_HANDLE;
        free(frame->imageLayouts);
        free(frame->imageFences);
        free(frame);
        return xud_result_from_vk(vk);
    }

    for (index = 0u; index < swapchain->imageCount; index++) {
        vk = loader->deviceTable.vkCreateFence(device->device, &frame->fenceInfo, NULL, &frame->imageFences[index]);
        if (vk != VK_SUCCESS) {
            for (fencesMade = index; fencesMade > 0u; fencesMade--) {
                loader->deviceTable.vkDestroyFence(device->device, frame->imageFences[fencesMade - 1u], NULL);
                frame->imageFences[fencesMade - 1u] = VK_NULL_HANDLE;
            }
            (void)loader->deviceTable.vkDestroySemaphore(device->device, frame->imageAcquired, NULL);
            frame->imageAcquired = VK_NULL_HANDLE;
            loader->deviceTable.vkFreeCommandBuffers(
                device->device, frame->commandPool, XUD_FRAME_COMMAND_BUFFER_COUNT, frame->commandBufferRows);
            (void)loader->deviceTable.vkDestroyCommandPool(device->device, frame->commandPool, NULL);
            frame->commandPool = VK_NULL_HANDLE;
            frame->commandBuffer = VK_NULL_HANDLE;
            free(frame->imageLayouts);
            free(frame->imageFences);
            free(frame);
            return xud_result_from_vk(vk);
        }
        fencesMade = (index + 1u);
    }
    frame->imageFenceCount = fencesMade;

    frame->presentedSwapchain = swapchain->swapchain; /* borrowed, updated by every draw */

    *outFrame = frame;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * One frame, end to end - the driver half
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_frame_draw(XudFrame* frame, XudLoader* loader, const XudDevice* device, const XudSwapchain* swapchain,
    const XudPipeline* pipeline, const XudRenderPass* pass, const XudFramebufferArray* framebuffers, uint32_t framebufferCount,
    uint32_t* outImageIndex) {
    XudQueue graphicsQueue;
    XudQueue presentQueue;
    XudFramebuffer* row = NULL;
    XudResult result = XUD_INVALID_ARG;
    VkResult vk = VK_ERROR_UNKNOWN;
    VkCommandBufferBeginInfo begin;
    VkViewport viewport;
    VkRect2D scissor;
    VkRenderPassBeginInfo passBegin;
    VkImageMemoryBarrier imageBarriers[XUD_RENDERPASS_ATTACHMENT_COUNT];
    VkSubmitInfo submit;
    VkPresentInfoKHR present;
    uint32_t imageIndex = 0u;
    VkFence frameFence = VK_NULL_HANDLE;

    if ((frame == NULL) || (loader == NULL) || (device == NULL) || (swapchain == NULL) || (pipeline == NULL) || (pass == NULL)
        || (framebuffers == NULL) || (outImageIndex == NULL)) {
        return XUD_INVALID_ARG;
    }
    if (!loader->open) {
        return XUD_UNSUPPORTED;
    }
    if ((device->device == VK_NULL_HANDLE) || (swapchain->swapchain == VK_NULL_HANDLE) || (pipeline->pipeline == VK_NULL_HANDLE)
        || (pass->renderPass == VK_NULL_HANDLE) || (frame->commandPool == VK_NULL_HANDLE) || (frame->commandBuffer == VK_NULL_HANDLE)
        || (frame->imageAcquired == VK_NULL_HANDLE)) {
        return XUD_INVALID_ARG;
    }
    if ((swapchain->imageCount == 0u) || (framebufferCount == 0u)) {
        return XUD_INVALID_ARG;
    }
    if (pass->depthImage == VK_NULL_HANDLE) {
        /* The pass's second attachment is a depth image this frame has to give a layout, so a pass wi
           th none is a pass this frame cannot run - the same rule xud_framebuffer_open() applies to a
           framebuffer row one rung up. */
        return XUD_INVALID_ARG;
    }

    /* The fence array and the framebuffer array have to match the swapchain in hand. A recreate replace
       d the swapchain, and therefore every image, every framebuffer row and every fence index - so a dr
       aw that carried on through a recreated swapchain would be a presentation against a dead handle. Th
       is is the rule the plan states as "the framebuffer array is recreated with the swapchain", read her
       e as a refusal rather than as a rebuild this rung cannot do without a resize to act on. */
    if ((frame->imageFenceCount != swapchain->imageCount) || (frame->presentedSwapchain != swapchain->swapchain)
        || (framebuffers->count != swapchain->imageCount) || (framebufferCount != swapchain->imageCount)
        || (frame->imageLayoutCount != swapchain->imageCount)) {
        return XUD_UNSUPPORTED;
    }

    /* Every row this frame needs, checked before any of them is called through - the check-before-you-us
       e rule read at the whole frame at once, so a frame that cannot run is refused before it signals an
       acquire it could never wait for. */
    if ((loader->deviceTable.vkAcquireNextImageKHR == NULL) || (loader->deviceTable.vkResetCommandBuffer == NULL)
        || (loader->deviceTable.vkBeginCommandBuffer == NULL) || (loader->deviceTable.vkCmdBindPipeline == NULL)
        || (loader->deviceTable.vkCmdSetViewport == NULL) || (loader->deviceTable.vkCmdSetScissor == NULL)
        || (loader->deviceTable.vkCmdBeginRenderPass == NULL) || (loader->deviceTable.vkCmdDraw == NULL)
        || (loader->deviceTable.vkCmdEndRenderPass == NULL) || (loader->deviceTable.vkCmdPipelineBarrier == NULL)
        || (loader->deviceTable.vkEndCommandBuffer == NULL) || (loader->deviceTable.vkQueueSubmit == NULL)
        || (loader->deviceTable.vkQueuePresentKHR == NULL) || (loader->deviceTable.vkWaitForFences == NULL)
        || (loader->deviceTable.vkResetFences == NULL)) {
        return XUD_UNSUPPORTED;
    }
    if (!xud_loader_device_bound(loader, device->device)) {
        return XUD_UNSUPPORTED;
    }

    result = xud_device_queue_get(&graphicsQueue, device, XUD_QUEUE_ROLE_GRAPHICS);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_device_queue_get(&presentQueue, device, XUD_QUEUE_ROLE_PRESENT);
    if (result != XUD_OK) {
        return result;
    }
    if ((graphicsQueue.queue == VK_NULL_HANDLE) || (presentQueue.queue == VK_NULL_HANDLE)) {
        return XUD_INVALID_ARG;
    }

    result = xud_frame_fill_submit(&submit, &present, frame->commandBuffer, swapchain->swapchain);
    if (result != XUD_OK) {
        return result;
    }

    /* --- 1. acquire. The acquire semaphore is the signal that an image is available; the fence argumen
       t here is VK_NULL_HANDLE because the fence this rung uses bounds the DRAW, not the acquire, and pa
       ssing one here would be a second, different boundary on the same image. --- */
    vk = loader->deviceTable.vkAcquireNextImageKHR(
        device->device, swapchain->swapchain, XUD_FRAME_NO_TIMEOUT, frame->imageAcquired, VK_NULL_HANDLE, &imageIndex);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK) {
        /* An out-of-date acquire answer maps here rather than to a recreate: the recreate path is the res
           ize step's, and this rung's job is to report that the image it was handed is no longer the one t
           he compositor wants. */
        return result;
    }
    if (imageIndex >= swapchain->imageCount) {
        return XUD_INTERNAL_ERROR;
    }

    /* --- 2. the acquire semaphore is NOT waited here on the host. vkWaitSemaphores is a core-1.2 entry
       point and this library's instance sits at the core-1.0 floor (xud_instance.h's XUD_INSTANCE_API_
       FLOOR), so the row is nil rather than a refusal-with-a-fallback, and the repo's rule is that a na
       me that is not there is reported, not worked around. The core-1.0 form of the same boundary is a
       row of the SUBMIT: pWaitSemaphores names the acquire semaphore and pWaitDstStageMask names the ve
       rtex stage, which is where the draw's work begins - so the recorded work cannot touch the image u
       ntil the acquire signal fires, and the gate is on the queue rather than on the host. Step 5 below
       writes exactly that pair. --- */

    /* --- 3. the framebuffer for THIS image, through the array's own accessor. A stale index into an arra
       y a recreate replaced is a caught error, not an out-of-range read - the same rule xud_swapchain_ima
       ges_get() states one rung below. --- */
    result = xud_framebuffer_array_get(framebuffers, imageIndex, &row);
    if (result != XUD_OK) {
        return result;
    }
    if ((row == NULL) || (row->framebuffer == VK_NULL_HANDLE) || (row->view == VK_NULL_HANDLE)) {
        return XUD_INVALID_ARG;
    }

    result = xud_frame_fill_recording(&begin, &viewport, &scissor, row->createInfo.width, row->createInfo.height);
    if (result != XUD_OK) {
        return result;
    }

    /* --- 4. reset then begin the command buffer. The reset is required before a re-record even when the p
       ool has the RESET_COMMAND_BUFFER_BIT set - the flag says the buffer MAY be re-recorded, the call sa
       ys it has been retired, and skipping the call would be re-recording a buffer the driver still holds
       as recorded. --- */
    vk = loader->deviceTable.vkResetCommandBuffer(frame->commandBuffer, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK) {
        return result;
    }

    frame->beginInfo = begin;
    vk = loader->deviceTable.vkBeginCommandBuffer(frame->commandBuffer, &begin);
    if (vk != VK_SUCCESS) {
        return xud_result_from_vk(vk);
    }

    /* The two dynamic states, set inside the pass rather than baked into the pipeline - which is what the
       plan's "the framebuffer array is recreated with the swapchain" buys against a fixed-extent pipeline.
       The Cmd* calls below are void: they cannot answer a VkResult, so the repo's zero-state-before-the-c
       all rule is what applies here, and the only point a recording can fail at is vkEndCommandBuffer. */
    loader->deviceTable.vkCmdSetViewport(frame->commandBuffer, 0u, 1u, &viewport);
    loader->deviceTable.vkCmdSetScissor(frame->commandBuffer, 0u, 1u, &scissor);

    /* The pipeline is bound inside the command buffer, once, per frame - the repo's "explicit pipelines, c
       reated once at init" rule read at the use side: the pipeline object is not rebuilt here, only named
       . */
    loader->deviceTable.vkCmdBindPipeline(frame->commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);

    passBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    passBegin.pNext = NULL;
    passBegin.framebuffer = row->framebuffer;
    passBegin.renderPass = pass->renderPass;
    passBegin.renderArea.offset.x = 0;
    passBegin.renderArea.offset.y = 0;
    passBegin.renderArea.extent.width = row->createInfo.width;
    passBegin.renderArea.extent.height = row->createInfo.height;
    passBegin.pClearValues = pass->clearValues; /* the pass's OWN clear values, in attachment order - the sp
                                                  ec's rule that pClearValues is indexed by attachment index,
                                                  and the reason the array lives in the pass rather than in a
                                                  frame-local temporary */
    passBegin.clearValueCount = XUD_RENDERPASS_ATTACHMENT_COUNT; /* the count beside the array, because the
                                                    spec's rule is about the COUNT: clearValueCount must be
                                                    greater than the largest attachment index that is cleared
                                                    (VUID-VkRenderPassBeginInfo-clearValueCount-00902), and an
                                                    array handed without its count is an array the driver
                                                    cannot size - the validation layer reports a zero count
                                                    here as a hard error, probed today */

    /* The layout transition, and it has to happen BEFORE the pass begins: the image this frame writes is
       in the layout the last frame left it in (VK_IMAGE_LAYOUT_UNDEFINED for an image that has never been
       drawn into, the pass's finalLayout for one that has), and the pass requires its attachment to be in
       the attachment's initialLayout from the first sample. A barrier is the only way across, and it names
       the source layout the image is actually in - which is why the frame loop records one layout per
       image rather than assuming one. Both stage masks are XUD_FRAME_BARRIER_STAGES, ALL_GRAPHICS: the
       access flags this barrier names are a colour-attachment read AND a write, and the spec's table of
       supported access types puts the write at the fragment-test stages, which a shader-stage-only mask
       does not cover (the validation layer says so, probed today). The barrier is outside the render
       pass, before vkCmdBeginRenderPass, so no subset-of-the-bound-pipeline rule reaches it. */
    /* The layout transitions, and they have to happen BEFORE the pass begins: the image this frame wri
       tes is in the layout the last frame left it in (VK_IMAGE_LAYOUT_UNDEFINED for an image that has ne
       ver been drawn into, the pass's finalLayout for one that has), and the pass requires its attachme
       nt to be in the attachment's initialLayout from the first sample. A barrier is the only way acros
       s, and it names the source layout the image is actually in - which is why the frame loop records
       one layout per colour image and one for the depth image rather than assuming one. ONE barrier ca
       rrying TWO image-barrier entries rather than two barriers, because the entries are independent la
       yout transitions of two images and one call is one dependency in the command buffer rather than t
       wo: the array is indexed in the pass's attachment order, the same order the clear-value array abo
       ve is, and the count beside it is the array's own constant so the two cannot disagree. Both stage
       masks are XUD_FRAME_BARRIER_STAGES, ALL_GRAPHICS: the access flags this barrier names are a colou
       r-attachment read AND a write AND the depth-stencil pair, and the spec's table of supported access
       types puts the attachment writes at the fragment-test stages, which a shader-stage-only mask does
       not cover (probed today: TOP_OF_PIPE alone and VERTEX|FRAGMENT alone are both refused for the colo
       ur-write bit, and the depth-write bit has the same rule against VK_PIPELINE_STAGE_DEPTH_ATTACHMENT
       _OUTPUT_BIT, which ALL_GRAPHICS covers). The barrier is outside the render pass, before vkCmdBeg
       inRenderPass, so no subset-of-the-bound-pipeline rule reaches it.
     */
    imageBarriers[XUD_RENDERPASS_COLOR_ATTACHMENT].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    imageBarriers[XUD_RENDERPASS_COLOR_ATTACHMENT].pNext = NULL;
    imageBarriers[XUD_RENDERPASS_COLOR_ATTACHMENT].srcAccessMask
        = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    imageBarriers[XUD_RENDERPASS_COLOR_ATTACHMENT].dstAccessMask
        = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    imageBarriers[XUD_RENDERPASS_COLOR_ATTACHMENT].srcQueueFamilyIndex = 0u; /* the same family on both halves: this barrier is a LAYOUT
                                              transition, not an ownership transfer, and the pair being
                                              equal is what says so - the spec's rule for an EXCLUSIVE
                                              image is that a non-equal pair must name EXTERNAL or a
                                              real family, and an unset pair is garbage the validation
                                              layer reads as an invalid family index */
    imageBarriers[XUD_RENDERPASS_COLOR_ATTACHMENT].dstQueueFamilyIndex = 0u;
    imageBarriers[XUD_RENDERPASS_COLOR_ATTACHMENT].oldLayout = frame->imageLayouts[imageIndex];
    imageBarriers[XUD_RENDERPASS_COLOR_ATTACHMENT].newLayout = pass->attachments[XUD_RENDERPASS_COLOR_ATTACHMENT].initialLayout;
    imageBarriers[XUD_RENDERPASS_COLOR_ATTACHMENT].image = row->image; /* the swapchain's own image, borrowed into the framebuffer row - the
                                        view of it and the framebuffer of it were built for this same image */
    imageBarriers[XUD_RENDERPASS_COLOR_ATTACHMENT].subresourceRange = row->viewInfo.subresourceRange; /* the range the view was created for,
                                        which is the range the framebuffer attaches: one colour aspect, one
                                        mip, one layer */

    /* The SECOND entry: the pass's depth image. The access masks are the DEPTH-STENCIL pair rather th
       an the colour pair, because an access mask that names the colour bits against a depth image is a
       barrier claiming an access the image cannot take (VUID-VkImageMemoryBarrier-srcAccessMask-03096 /
       VUID-VkImageMemoryBarrier-dstAccessMask-03097 are the pair, and the validation layer reads the as
       pect mask of the range against the bits). The subresource range is the one the depth VIEW was cre
       ated for - VK_IMAGE_ASPECT_DEPTH_BIT alone - read off the pass's own depthViewInfo rather than re
       assembled here, so a barrier and the view it guards cannot drift apart. oldLayout is the frame's
       own record rather than a guess, exactly as the colour half's is; for the first frame it is UNDEFIN
       ED and for every frame after it it is the pass's depth finalLayout, which equals the pass's depth
       initialLayout, so the transition is a no-op from the second frame on - which is what a depth atta
       chment being never presented buys. */
    imageBarriers[XUD_RENDERPASS_DEPTH_ATTACHMENT] = imageBarriers[XUD_RENDERPASS_COLOR_ATTACHMENT];
    imageBarriers[XUD_RENDERPASS_DEPTH_ATTACHMENT].srcAccessMask
        = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    imageBarriers[XUD_RENDERPASS_DEPTH_ATTACHMENT].dstAccessMask
        = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    imageBarriers[XUD_RENDERPASS_DEPTH_ATTACHMENT].oldLayout = frame->depthLayout;
    imageBarriers[XUD_RENDERPASS_DEPTH_ATTACHMENT].newLayout = pass->attachments[XUD_RENDERPASS_DEPTH_ATTACHMENT].initialLayout;
    imageBarriers[XUD_RENDERPASS_DEPTH_ATTACHMENT].image = pass->depthImage; /* the pass's OWN image, not a row's:
                                        one depth image for every framebuffer row, which is the reason th
                                        e image and its one view belong to the pass rather than to a row
                                        of the array */
    imageBarriers[XUD_RENDERPASS_DEPTH_ATTACHMENT].subresourceRange = pass->depthViewInfo.subresourceRange;

    loader->deviceTable.vkCmdPipelineBarrier(frame->commandBuffer, XUD_FRAME_BARRIER_STAGES, XUD_FRAME_BARRIER_STAGES, 0u, 0u, NULL, 0u,
        NULL, XUD_RENDERPASS_ATTACHMENT_COUNT, imageBarriers);

    /* The layout this frame LEAVES the image in is recorded below, once the submission has run - see the
       note there. Nothing is recorded here, because a barrier that was recorded into a command buffer is
       not a transition that happened yet: the image is still in frame->imageLayouts[imageIndex] until
       vkQueueSubmit has run the commands, and a barrier that names the wrong oldLayout is a validation
       error on the NEXT frame rather than on this one. */
    loader->deviceTable.vkCmdBeginRenderPass(frame->commandBuffer, &passBegin, VK_SUBPASS_CONTENTS_INLINE);

    /* The draw. vertexCount is the whole argument set of the first-triangle step: there is no vertex buffe
       r and no index buffer, so the count IS the geometry and the shader derives the corner from gl_Vertex
       Index (see shaders/solid.vert). instanceCount 0 is the "draw once" form. */
    loader->deviceTable.vkCmdDraw(frame->commandBuffer, XUD_FRAME_DRAW_VERTEX_COUNT, XUD_FRAME_DRAW_INSTANCE_COUNT, 0u, 0u);

    loader->deviceTable.vkCmdEndRenderPass(frame->commandBuffer);

    vk = loader->deviceTable.vkEndCommandBuffer(frame->commandBuffer);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK) {
        return result;
    }

    /* --- 5. submit on the graphics queue, with the acquire semaphore waited at the vertex stage and the i
       mage's OWN fence as the fence argument. The fence is per image rather than per frame, which is the pl
       an's wording read literally: what has to be true before an image is written again is that the present
       ation of THAT image finished. --- */
    frameFence = frame->imageFences[imageIndex];
    if (frameFence == VK_NULL_HANDLE) {
        return XUD_INVALID_ARG;
    }
    submit.pWaitSemaphores = &frame->imageAcquired;
    submit.pWaitDstStageMask = frame->waitStages;
    submit.pCommandBuffers = frame->commandBufferRows;
    vk = loader->deviceTable.vkQueueSubmit(graphicsQueue.queue, XUD_FRAME_SUBMIT_COUNT, &submit, frameFence);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK) {
        frame->lastFrameFence = VK_NULL_HANDLE;
        return result;
    }
    frame->lastFrameFence = frameFence;

    /* The layout record moves HERE, after a submit that answered success: the barrier above is what
       transitions the image, the barrier runs only inside the submitted commands, so the layout the
       image is in from here on is the pass's finalLayout and nothing before here may claim it. A
       refused submit leaves the record at the layout the image really is still in, which is what keeps
       the next frame's barrier's oldLayout a fact rather than a guess. */
    frame->imageLayouts[imageIndex] = pass->attachments[XUD_RENDERPASS_COLOR_ATTACHMENT].finalLayout;
    frame->depthLayout = pass->attachments[XUD_RENDERPASS_DEPTH_ATTACHMENT].finalLayout;

    /* --- 6. present on the present queue. Both role queues are the same handle on this machine (the devic
       e layer's note), and the rule is still written as two lookups rather than one, because the two roles
       are a library decision and not a fact about this driver. --- */
    present.pSwapchains = &swapchain->swapchain;
    present.pImageIndices = &imageIndex;
    vk = loader->deviceTable.vkQueuePresentKHR(presentQueue.queue, &present);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK) {
        return result;
    }

    /* --- 7. wait the fence that bounded this frame, then reset it. The wait is what makes "this image may b
       e written again" a question the fence answers rather than an assumption, and the reset is what keeps
       the invariant the NEXT submit requires: vkQueueSubmit's fence must be in the not-signalled state, and
       vkWaitForFences does not put it there. --- */
    vk = loader->deviceTable.vkWaitForFences(device->device, 1u, &frameFence, XUD_FRAME_WAIT_ALL, XUD_FRAME_NO_TIMEOUT);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK) {
        return result;
    }
    vk = loader->deviceTable.vkResetFences(device->device, 1u, &frameFence);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK) {
        return result;
    }

    frame->lastFrameFence = VK_NULL_HANDLE; /* waited and reset, so nothing is in flight any more */
    frame->presentedSwapchain = swapchain->swapchain;
    frame->lastImageIndex = imageIndex;
    frame->framesDrawn++;

    *outImageIndex = imageIndex;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The teardown - the driver half
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_frame_close(XudFrame** ioFrame, const XudDevice* device, XudLoader* loader) {
    XudFrame* victim = NULL;
    VkResult vk = VK_ERROR_UNKNOWN;
    uint32_t index = 0u;

    if ((ioFrame == NULL) || (*ioFrame == NULL)) {
        return XUD_INVALID_ARG;
    }
    victim = *ioFrame;

    if ((loader == NULL) || (device == NULL)) {
        return (loader == NULL) ? XUD_INVALID_ARG : XUD_UNSUPPORTED;
    }
    if (!loader->open) {
        return XUD_UNSUPPORTED;
    }
    if (device->device == VK_NULL_HANDLE) {
        return XUD_INVALID_ARG;
    }
    if (!xud_loader_device_bound(loader, device->device)) {
        return XUD_UNSUPPORTED;
    }

    /* A fence still in flight cannot be destroyed - the spec's own rule for vkDestroyFence, and the reaso
       n the wait comes first: a presentation may still be reading the very image this fence bound, and dest
       roying the fence under it is destroying the record of a presentation that has not finished. A guard t
       hat cannot run is a teardown this rung must not start. */
    if (victim->lastFrameFence != VK_NULL_HANDLE) {
        if (loader->deviceTable.vkWaitForFences == NULL) {
            return XUD_UNSUPPORTED;
        }
        vk = loader->deviceTable.vkWaitForFences(device->device, 1u, &victim->lastFrameFence, XUD_FRAME_WAIT_ALL, XUD_FRAME_NO_TIMEOUT);
        if (vk != VK_SUCCESS) {
            return xud_result_from_vk(vk);
        }
        victim->lastFrameFence = VK_NULL_HANDLE;
    }

    if (victim->commandBuffer != VK_NULL_HANDLE) {
        if (loader->deviceTable.vkFreeCommandBuffers == NULL) {
            return XUD_UNSUPPORTED;
        }
        loader->deviceTable.vkFreeCommandBuffers(
            device->device, victim->commandPool, XUD_FRAME_COMMAND_BUFFER_COUNT, victim->commandBufferRows);
        victim->commandBuffer = VK_NULL_HANDLE;
    }
    if (victim->imageAcquired != VK_NULL_HANDLE) {
        if (loader->deviceTable.vkDestroySemaphore == NULL) {
            return XUD_UNSUPPORTED;
        }
        loader->deviceTable.vkDestroySemaphore(device->device, victim->imageAcquired, NULL);
        victim->imageAcquired = VK_NULL_HANDLE;
    }
    for (index = victim->imageFenceCount; index > 0u; index--) {
        if (victim->imageFences[index - 1u] == VK_NULL_HANDLE) {
            continue;
        }
        if (loader->deviceTable.vkDestroyFence == NULL) {
            return XUD_UNSUPPORTED;
        }
        loader->deviceTable.vkDestroyFence(device->device, victim->imageFences[index - 1u], NULL);
        victim->imageFences[index - 1u] = VK_NULL_HANDLE;
    }

    /* The pool LAST: the command buffer belongs to the pool, and the spec requires the pool to outlive ever
       y buffer allocated from it - the same ownership direction xud_swapchain_close() states for a swapchai
       n and the device it was created from, one rung below. */
    if (victim->commandPool != VK_NULL_HANDLE) {
        if (loader->deviceTable.vkDestroyCommandPool == NULL) {
            return XUD_UNSUPPORTED;
        }
        loader->deviceTable.vkDestroyCommandPool(device->device, victim->commandPool, NULL);
        victim->commandPool = VK_NULL_HANDLE;
    }

    free(victim->imageFences);
    victim->imageFences = NULL;
    if (victim->imageLayouts != NULL) {
        free(victim->imageLayouts);
        victim->imageLayouts = NULL;
    }
    victim->imageLayoutCount = 0u;
    victim->imageFenceCount = 0u;
    victim->imageFenceCapacity = 0u;
    victim->lastFrameFence = VK_NULL_HANDLE;
    victim->presentedSwapchain = VK_NULL_HANDLE;
    free(victim);
    *ioFrame = NULL;

    return XUD_OK;
}
