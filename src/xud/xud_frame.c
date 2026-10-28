/***********************************************************************************************
 *
 *   xud_frame.c - the frame loop: one command pool, one command buffer, the acquire/submit/p
 *          resent triple, and the acquire semaphore + per-image fences that bound a frame
 *
 *   Owns : vkCreateCommandPool / vkAllocateCommandBuffers / vkCreateSemaphore / vkCreateFence per
 *          swapchain image, one recorded frame (bind the variant's pipeline row per recorded draw
 *          row, set viewport + scissor, write set 0's
 *          block into the descriptor's mapped arena and upload it with ONE vkCmdCopyBuffer call, upl
 *          oad the batch with five vkCmdCopyBuffer calls behind the ONE host-write barrier that covers
 *          both arenas, bind the batch's four vertex streams with one vkCmdBindVertexBuffers call and
 *          its index buffer with one vkCmdBindIndexBuffer call, bind set 0 with one vkCmdBindDescript
 *          orSets call against the pipeline's own layout, begin render pass, push the per-draw row wi
 *          th vkCmdPushConstants beside each of the batch's recorded draw rows and replay those rows a
 *          s vkCmdDrawIndexed calls, end render pass), the batch reset at the end of a
 *          completed frame, and the teardown that hands every one of them back.
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
 *          The acquire and the present are the only two calls here whose VkResult answers about the swap
 *          chain rather than about the call: VK_SUBOPTIMAL_KHR and VK_ERROR_OUT_OF_DATE_KHR are a frame
 *          that runs and leaves recreateWanted set for the rung above, not a frame that is refused, and
 *          xud_swapchain_result_from_vk() is the second funnel that keeps that pair out of the general
 *          mapping every other call in this file funnels through.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud_frame.h"
#include "xud.h"
#include "xud_batch.h"
#include "xud_descriptor.h"
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
static_assert(sizeof(PFN_vkCmdBindIndexBuffer) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkCmdBindDescriptorSets) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkCmdBindVertexBuffers) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkCmdDrawIndexed) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkCmdPushConstants) == sizeof(void*), "PFN row must be pointer-sized");
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
 * The push row of one draw - the pure half
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_frame_fill_push_row(XudPushRow* outRow, const XudBatchDraw* drawRow) {
    uint32_t axis = 0u;

    if ((outRow == NULL) || (drawRow == NULL)) {
        return XUD_INVALID_ARG;
    }

    /* The model matrix is the IDENTITY, and that is the same honest answer the descriptor rung gives
       its block: a matrix that is not identity is a transform the camera/transform step has to CHOO
       SE, and that step is above this one and has not landed. What is NOT decoration here is the chan
       nel itself - the row is pushed, the vertex stage reads it, and the step above hands the object'
       s own matrix in place of this one without this rung's shape changing. Identity is the one matri
       x that is a fact rather than a choice, exactly as identity is the one viewProj the block holds t
       oday.
    */
    for (axis = 0u; axis < 16u; axis++) {
        outRow->modelMatrix[axis] = ((axis % 5u) == 0u) ? 1.0f : 0.0f;
    }

    /* The batch's sentinel is mapped to slot 0 here rather than passed through, and the reason is th
       e argument's type: the push row's material index is an INDEX the shader would use, and XUD_BAT
       CH_NO_MATERIAL is 0xFFFFFFFF, which is not an index into anything. "No material slot yet" is a
       batch-side fact about a subsystem that does not exist; the per-draw channel has to carry a numb
       er a read could use, and slot 0 is the only slot there is.
    */
    outRow->materialIndex = (drawRow->materialIndex == XUD_BATCH_NO_MATERIAL) ? 0u : drawRow->materialIndex;

    /* Set 1 is declared and never allocated (the texture step's), so there is no sampler array to na
       me an entry of. 0 is the only entry that could ever be read.
    */
    outRow->textureIndex = 0u;

    /* The variant the draw was flushed under, in the per-draw channel rather than a channel of this r
       ung's: the pipeline variant is chosen by BINDING a different pipeline, and this is the same fa
       ct seen from the shader's side, which is what lets a stage branch on the variant without a thi
       rd pipeline. The vertex stage reads only the model matrix today, so this member is recorded an
       d unread - the row is the channel, and a stage need not read every member of a block its layou
       t declares.
    */
    outRow->flags = drawRow->variant;
    outRow->reserved = 0u;

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

    /* No acquire has answered yet, so no recreate is wanted yet - written rather than left to calloc's
       zero, because this is state a rung above reads rather than a numeric coincidence, the same re
       ason xud_frame_open() writes the depth layout row for. */
    frame->recreateWanted = 0u;
    frame->presentedSwapchain = swapchain->swapchain; /* borrowed, updated by every draw */

    *outFrame = frame;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * One frame, end to end - the driver half
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_frame_draw(XudFrame* frame, XudLoader* loader, const XudDevice* device, const XudSwapchain* swapchain,
    const XudPipeline* pipeline, const XudRenderPass* pass, const XudFramebufferArray* framebuffers, uint32_t framebufferCount,
    XudBatch* batch, XudDescriptor* descriptor, uint32_t* outImageIndex) {
    XudBatchDraw drawRow;
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
    uint32_t acquireRecreateWanted = 0u;
    uint32_t presentRecreateWanted = 0u;
    VkFence frameFence = VK_NULL_HANDLE;
    uint32_t vertexCount = 0u;
    uint32_t indexCount = 0u;
    uint32_t vertexCapacity = 0u;
    uint32_t drawCount = 0u;
    uint32_t hasContent = 0u;
    VkDeviceSize stagingBytes = 0u;
    uint32_t draw = 0u;
    XudPushRow pushRow;

    if ((frame == NULL) || (loader == NULL) || (device == NULL) || (swapchain == NULL) || (pipeline == NULL) || (pass == NULL)
        || (framebuffers == NULL) || (outImageIndex == NULL) || (batch == NULL) || (descriptor == NULL)) {
        return XUD_INVALID_ARG;
    }
    if (!loader->open) {
        return XUD_UNSUPPORTED;
    }
    if ((device->device == VK_NULL_HANDLE) || (swapchain->swapchain == VK_NULL_HANDLE)
        || (pipeline->pipeline[XUD_BATCH_VARIANT_OPAQUE] == VK_NULL_HANDLE)
        || (pipeline->pipeline[XUD_BATCH_VARIANT_TRANSPARENT] == VK_NULL_HANDLE) || (pipeline->layout == VK_NULL_HANDLE)
        || (pass->renderPass == VK_NULL_HANDLE) || (frame->commandPool == VK_NULL_HANDLE) || (frame->commandBuffer == VK_NULL_HANDLE)
        || (frame->imageAcquired == VK_NULL_HANDLE)) {
        return XUD_INVALID_ARG;
    }
    if ((descriptor->sets[XUD_DESCRIPTOR_SET_PER_FRAME] == VK_NULL_HANDLE) || (descriptor->uniformBuffer == VK_NULL_HANDLE)
        || (descriptor->uniformStagingBuffer == VK_NULL_HANDLE) || (descriptor->uniformStagingMappedLive == 0u)) {
        /* Set 0 and the block it names are the state this frame binds and uploads, so a descriptor mi
           ssing any of the four is a descriptor this frame cannot run against - the same rule the line
           above applies to the pass and the pipeline one rung up. */
        return XUD_INVALID_ARG;
    }
    if (descriptor->pushRange.size == 0u) {
        /* The push row is pushed against the layout's OWN recorded range, so a descriptor that recorded
           no range is a descriptor whose layout this frame cannot push against - the same shape of ref
           usal as the four rows above, one rung over. The size rather than the stage flags is the fiel
           d checked: a zero-size range is a range that names no bytes at all, and the stage flags of a
           zero-size range would be decoration. */
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
        || (loader->deviceTable.vkCmdBeginRenderPass == NULL) || (loader->deviceTable.vkCmdBindIndexBuffer == NULL)
        || (loader->deviceTable.vkCmdBindDescriptorSets == NULL) || (loader->deviceTable.vkCmdBindVertexBuffers == NULL)
        || (loader->deviceTable.vkCmdDrawIndexed == NULL) || (loader->deviceTable.vkCmdEndRenderPass == NULL)
        || (loader->deviceTable.vkCmdPipelineBarrier == NULL) || (loader->deviceTable.vkCmdPushConstants == NULL)
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
    /* The SECOND funnel, and the reason it is second: this is one of the two calls in the tree whose
       VkResult answers about the swapchain rather than about the call. VK_SUBOPTIMAL_KHR and VK_ERR
       OR_OUT_OF_DATE_KHR both mean "the image you asked for is yours" - the first adds "and this s
       wapchain is not the best shape for the surface", the second adds "and this swapchain no longe
       r matches it, so make a new one before the next acquire" - so neither is a refused frame and
       both leave a recreate wanted. Mapping them through the general funnel is what made a resize a
       frame that never drew: the image was usable, the compositor was only asking for a better swap
       chain, and a refusal here is a whole renderer going quiet. */
    result = xud_swapchain_result_from_vk(vk, &acquireRecreateWanted);
    if (result != XUD_OK) {
        return result;
    }
    if (acquireRecreateWanted != 0u) {
        /* Recorded, not acted on: the swapchain is the context's, a rung below, and recreating it r
           etires every image, every framebuffer row and every fence index this rung holds, which is
           a decision the resize step above owns. Sticky rather than rewritten per frame - the recrea
           te stays wanted until someone makes it. */
        frame->recreateWanted = 1u;
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

    /* --- 3b. the batch is flushed HERE, before the command buffer is begun. Closing the pending run
       into a draw row and reading the row list are both PURE host work - they need no command buffer -
       so doing them here is what keeps every failure path below the begin call one that ends the buffer
       it began rather than returning out of a begun one. The counts come from the batch's own accessor
       rather than from its fields, which is the repo's accessor rule read one rung below: the frame loop
       asks, it does not reach.
    */
    result = xud_batch_stats(batch, &vertexCount, &indexCount, &vertexCapacity, &drawCount, &stagingBytes);
    if (result != XUD_OK) {
        return result;
    }
    hasContent = ((vertexCount > 0u) && (indexCount > 0u)) ? 1u : 0u;
    if (hasContent != 0u) {
        /* The pending batch is closed into a draw row here, which is what makes "the frame loop is the
           flush" a fact rather than a promise: the rows read below are the whole list the frame replays,
           and the variant/material the row names is the one the appends were made under. Nothing in this
           tree flushes mid-frame yet - xud_batch_should_flush is the decision and no caller above has
           acted on it - so the pending run is one row today, and the day it is more than one is the day
           the multi-draw shape below already handles.
        */
        if (xud_batch_record_draw(batch, batch->currentVariant, batch->currentMaterial) != XUD_OK) {
            return XUD_OUT_OF_MEMORY;
        }
        result = xud_batch_stats(batch, &vertexCount, &indexCount, &vertexCapacity, &drawCount, &stagingBytes);
        if (result != XUD_OK) {
            return result;
        }
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

    /* The pipeline is NOT bound here any more: the variant a recorded draw row names is what picks the
       row of the pipeline list to bind, and the bind is command-buffer state, so it moves into the draw
       loop below, one bind per row. Binding once here would be binding one variant for every row of a
       list that can name either, which is a transparent draw replayed through the opaque pipeline - the
       blend row the batch asked for silently not the blend row the draw gets. */

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
    /* --- 5a. set 0's block is uploaded EVERY frame, whether or not the batch has content in it: the
       pipeline's fragment stage reads this block, so a frame that never uploaded it is a frame whose
       shader reads a buffer that was never written. The write is the descriptor rung's (a memcpy into
       an arena mapped once for the descriptor's life) and the copy is the descriptor rung's too; what
       belongs to THIS rung is the barrier either side of the copy and the bind that makes the set vis
       ible to the draws.
    */
    if (xud_descriptor_write_block(descriptor) != XUD_OK) {
        (void)loader->deviceTable.vkEndCommandBuffer(frame->commandBuffer);
        return XUD_INTERNAL_ERROR;
    }

    /* The barrier that makes the HOST WRITES visible to the copies below - both arenas' writes, since
       a barrier is a scope and not a buffer: the batch's appends and the block's memcpy are both host
       writes into device memory and this ONE barrier covers both, which is why the batch's own copy
       block below no longer records a second one. The batch's arena and the block's arena are both map
       ped ONCE for the life of their owner, so these are not a map/unmap pair per frame and the repo's
       rule that a mapped write needs a matching unmap + invalidate does not reach them - what makes th
       em visible is this barrier. HOST is the one stage the spec's supported-access-type table puts
       VK_ACCESS_HOST_WRITE_BIT at, and it is a legal srcStageMask and never a dstStageMask, which is
       why this is its own call rather than a third entry folded into the layout-transition barrier ab
       ove.
    */
    {
        const VkMemoryBarrier hostToTransfer = (VkMemoryBarrier) { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = NULL,
            .srcAccessMask = XUD_FRAME_HOST_WRITE_ACCESS,
            .dstAccessMask = XUD_FRAME_TRANSFER_READ_ACCESS };
        loader->deviceTable.vkCmdPipelineBarrier(frame->commandBuffer, XUD_FRAME_BARRIER_HOST_STAGE, XUD_FRAME_BARRIER_TRANSFER_STAGE, 0u,
            1u, &hostToTransfer, 0u, NULL, 0u, NULL);
    }

    /* --- 5b. the batch's content is uploaded, bound and drawn only when the batch has content in it:
       a frame whose batch is empty clears and presents with no draw at all, which is a legal frame
       rather than an error, and the honest answer to "nothing has appended yet".
    */
    if (hasContent != 0u) {
        /* The five vkCmdCopyBuffer calls, from the batch's own copy rows, over the live byte counts and
           not the capacity: the arena's bytes are rewritten in place every frame, never appended to, so
           the copy is of what is live and the rest of the buffer is stale bytes no draw reads. The call
           is legal only OUTSIDE a render pass instance, which is where it sits - before the begin below.
           A failure here ends the buffer before returning, which is what keeps the rung's claim that a
           begun command buffer is always ended before it returns checkable rather than a promise.
        */
        if (xud_batch_upload(batch, loader, device, frame->commandBuffer) != XUD_OK) {
            (void)loader->deviceTable.vkEndCommandBuffer(frame->commandBuffer);
            return XUD_INTERNAL_ERROR;
        }

        /* The barrier that makes the copies' results visible to the reads that follow them: the streams
           are read as vertex attributes and the uniform block as a uniform, and the validation layer's
           supported-access-type rule is what refuses a dstStageMask that covers neither.
        */
        {
            const VkMemoryBarrier transferToReads = (VkMemoryBarrier) { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .pNext = NULL,
                .srcAccessMask = XUD_FRAME_TRANSFER_WRITE_ACCESS,
                .dstAccessMask = XUD_FRAME_VERTEX_READ_ACCESS };
            loader->deviceTable.vkCmdPipelineBarrier(frame->commandBuffer, XUD_FRAME_BARRIER_TRANSFER_STAGE,
                XUD_FRAME_BARRIER_VERTEX_READ_STAGES, 0u, 1u, &transferToReads, 0u, NULL, 0u, NULL);
        }

        /* ONE call over the whole array of the batch's four streams, at the binding numbers the batch's
           vertex-input rows named - four separate calls would be four calls each leaving three of the
           pipeline's four bindings unbound, which is a draw reading a binding nothing wrote. The offsets
           are INTO THE BUFFER, not the buffer's address in the allocator's block: each stream is its own
           VkBuffer bound at its range's offset, so the vertex data starts at buffer offset 0. The call is
           void and cannot fail, and the arrays are this frame's own, read during the call.
        */
        {
            VkBuffer streamRows[XUD_BATCH_STREAM_COUNT];
            VkDeviceSize streamOffsets[XUD_BATCH_STREAM_COUNT];
            uint32_t stream = 0u;
            for (stream = 0u; stream < XUD_BATCH_STREAM_COUNT; stream++) {
                streamRows[stream] = batch->stream[stream].buffer;
                streamOffsets[stream] = 0u;
            }
            loader->deviceTable.vkCmdBindVertexBuffers(
                frame->commandBuffer, XUD_FRAME_FIRST_BINDING, XUD_BATCH_STREAM_COUNT, streamRows, streamOffsets);
        }

        /* The index buffer is bound HERE, not passed to the draw: vkCmdDrawIndexed names no index buffer
           at all, the bind is the state and the draw reads it. The offset is 0 because the batch's indices
           start at the beginning of its own buffer, and the type is UINT32 because XUD_BATCH_INDEX_SIZE is
           four bytes an entry - a bind that named another type would be a draw reading part of an index.
           The bind is recorded OUTSIDE the render pass, beside the vertex bind above it, which is where
           state-setting belongs.
        */
        loader->deviceTable.vkCmdBindIndexBuffer(frame->commandBuffer, batch->indexBuffer, 0u, XUD_FRAME_INDEX_TYPE);
    }

    /* The block's upload: ONE vkCmdCopyBuffer carrying ONE region, from the block's arena into set 0's
       buffer. One call rather than five because this upload has one source and one destination, which
       is the opposite of the batch's five-call shape, where the five destinations are five buffers. It
       sits after the has-content block rather than inside it because the block ALWAYS has content - it
       is a fixed 120 B and the shader reads it every frame - and before the barrier below because a
       transfer's result is only visible to a read past that barrier.
    */
    if (xud_descriptor_upload(descriptor, loader, device, frame->commandBuffer) != XUD_OK) {
        (void)loader->deviceTable.vkEndCommandBuffer(frame->commandBuffer);
        return XUD_INTERNAL_ERROR;
    }

    /* The barrier that makes the block's upload visible to the stages that read it. It is a SEPARATE c
       all from the vertex-attribute barrier inside the block above because the spec's supported-acces
       s-type rule binds an access bit to the stages that can perform it: VK_ACCESS_UNIFORM_READ_BIT be
       longs at the two shader stages and VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT at the input and vertex s
       tages, and one barrier naming both access bits with one stage mask naming all three is refused.
       The stage mask here is the pair the layout declares for the block.
    */
    {
        const VkMemoryBarrier transferToUniform = (VkMemoryBarrier) { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = NULL,
            .srcAccessMask = XUD_FRAME_TRANSFER_WRITE_ACCESS,
            .dstAccessMask = XUD_FRAME_UNIFORM_READ_ACCESS };
        loader->deviceTable.vkCmdPipelineBarrier(frame->commandBuffer, XUD_FRAME_BARRIER_TRANSFER_STAGE, XUD_FRAME_UNIFORM_READ_STAGES, 0u,
            1u, &transferToUniform, 0u, NULL, 0u, NULL);
    }

    /* Set 0 is bound HERE, once per frame, against the pipeline's OWN layout - the layout the pipeline
       was created with is the layout the set was allocated against, so naming pipeline->layout rather
       than a layout of this rung's is what makes the two agree rather than two records that happen to
       match. One set, firstSet 0: set 1 is the texture step's to allocate and bind, and the pool above
       is sized for it so that step needs no second pool. The bind is recorded OUTSIDE the render pass,
       beside the other two binds, which is where state-setting belongs; the draw inside the pass reads
       this state. The call is void and cannot fail, and the row is this frame's own, read during the c
       all.
    */
    {
        const VkDescriptorSet setRows[XUD_FRAME_SET_COUNT] = { descriptor->sets[XUD_DESCRIPTOR_SET_PER_FRAME] };
        loader->deviceTable.vkCmdBindDescriptorSets(frame->commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->layout,
            XUD_FRAME_FIRST_SET, XUD_FRAME_SET_COUNT, setRows, 0u, NULL);
    }

    loader->deviceTable.vkCmdBeginRenderPass(frame->commandBuffer, &passBegin, VK_SUBPASS_CONTENTS_INLINE);

    /* The draws: one vkCmdDrawIndexed per recorded row of the batch, which is the plan's multi-draw
       shape - the batch's rows are the flush list and the frame loop replays them. The row's firstIndex/
       indexCount are what the batch recorded when it flushed, and the index buffer itself was bound above,
       so the geometry is the batch's and no longer a hard-coded three-vertex triangle. A batch with
       nothing in it has no rows at all, so this loop records nothing and the frame is a clear-and-present
       frame. The row accessor is read inside the pass and a row that cannot be read ENDS the list rather
       than returning out of a begun pass: the count above came from the same batch, so the branch is not
       one this loop reaches, and the draws made so far are the whole list either way, which is what keeps
       the command buffer well formed. instanceCount 1 is the "draw once" form - the PFN's order is
       (indexCount, instanceCount, firstIndex, vertexOffset, firstInstance), so the instance count is the
       SECOND argument, and a 0 there is a legal draw of zero instances, which is a frame that records a
       draw and renders nothing. vertexOffset 0 is the "no shift" form of the int32_t argument.
    */
    for (draw = 0u; draw < drawCount; draw++) {
        if (xud_batch_draw_get(batch, draw, &drawRow) != XUD_OK) {
            break;
        }
        /* The variant is the row of the pipeline list this draw binds, and a variant at or past the
           count ends the list rather than reading a row off the end of the array - the same rule the
           batch's own invariant check applies to the variant a draw row names, read here at the use
           side because this loop is what indexes the array. */
        if (drawRow.variant >= XUD_BATCH_VARIANT_COUNT) {
            break;
        }
        /* The pipeline is bound per draw row, not once per frame: the variant is what picks the row,
           and a bind is command-buffer state, so the last bind before a draw is the one that draw uses.
           This is the Vulkan form of rlgl re-selecting its shader/blend state per batch (rlSetBlendMode
           flushes the batch on a mode change, rlgl.h 2121, then sets the state) - the batch's flush
           decision already closed a run before the variant changed, so a bind here cannot change what
           a draw already recorded. */
        loader->deviceTable.vkCmdBindPipeline(frame->commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline[drawRow.variant]);
        /* The push row is filled and pushed HERE, beside the draw it belongs to, rather than once per
           frame: a push-constant range is command-buffer STATE that the last push before a draw bound,
           which is exactly what makes it the per-draw channel and set 0 the per-frame one. The row is a
           local of this iteration and that is legal where every other recorded row in this tree is a mem
           ber: vkCmdPushConstants takes const void* and the driver COPIES the bytes into the command
           buffer during the call, so nothing above reads them after it - the rule about members instead
           of temporaries is about rows the driver reads by pointer later, and this is not one of those.
           The stage flags are the layout's own range flags, which is the subset rule read as an equalit
           y: the pipeline layout declared this range for exactly these two stages.
        */
        if (xud_frame_fill_push_row(&pushRow, &drawRow) != XUD_OK) {
            break;
        }
        loader->deviceTable.vkCmdPushConstants(frame->commandBuffer, pipeline->layout, descriptor->pushRange.stageFlags,
            descriptor->pushRange.offset, descriptor->pushRange.size, &pushRow);
        loader->deviceTable.vkCmdDrawIndexed(frame->commandBuffer, drawRow.indexCount, 1u, drawRow.firstIndex, 0, 0u);
    }

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
    /* The present's answer goes through the same second funnel the acquire's did, for the same reas
       on: vkQueuePresentKHR is documented to answer the swapchain's two codes, and for it both mean
       the presentation still happened - the image reached the surface - with a recreate wanted. Ret
       urning an error here instead would be a frame that presented and then reported it had not. */
    vk = loader->deviceTable.vkQueuePresentKHR(presentQueue.queue, &present);
    result = xud_swapchain_result_from_vk(vk, &presentRecreateWanted);
    if (result != XUD_OK) {
        return result;
    }
    if (presentRecreateWanted != 0u) {
        frame->recreateWanted = 1u;
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

    /* The batch is reset HERE, at the end of a frame that completed: the frame loop is the flush, so the
       next frame starts from an empty batch and the arena's bytes are rewritten from offset 0 rather than
       appended to. The capacities and the buffers stay - only the live counts restart - which is the same
       property xud_batch_reset's own note states. A frame that failed above leaves the batch alone, because
       a batch whose content was never uploaded is a batch a caller above still has to see.
    */
    if (xud_batch_reset(batch) != XUD_OK) {
        return XUD_INTERNAL_ERROR;
    }

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
    victim->recreateWanted = 0u;
    free(victim);
    *ioFrame = NULL;

    return XUD_OK;
}
