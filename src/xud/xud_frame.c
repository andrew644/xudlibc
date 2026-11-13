/***********************************************************************************************
 *
 *   xud_frame.c - the frame loop: one command pool, one command buffer per in-flight slot, the
 *          acquire/submit/present triple, and the per-slot acquire semaphore, render-finished
 *          pair + fence that bounds a frame
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
 *          entry point, live at this library's 1.3 floor and still not called: the submit-side gate is
 *          this layer's own choice, not a fallback for a missing name, and the repo's rule that a name
 *          not there is reported, not worked around. A SECOND semaphore is the render-finished one: the
 *          frame's submit signals it and a no-command submit on the present queue waits it, which is the
 *          submit-to-present boundary for a device whose two role queues are two distinct handles - two
 *          queues have no implicit ordering between their submissions, and a bare vkQueuePresentKHR waits
 *          on nothing. ONE fence per in-flight slot, handed to vkQueueSubmit as its fence argument
 *          and waited and reset at the START of that slot's next use, and no
 *          VK_KHR_vulkan_memory_model anywhere.
 *          The fence array is per FRAME-SLOT rather than per image: what the fence bounds is the
 *          slot's recorded work, and the image a frame wrote is recycled by the swapchain's own
 *          acquire, which is the boundary that says a presentation of that image finished.
 *
 *          The loop is PIPELINED: XUD_FRAME_IN_FLIGHT_COUNT frames may be on the go at once, each
 *          slot carrying its own command buffer, acquire semaphore, render-finished semaphore and
 *          fence. xud_frame_draw waits the slot's fence (if that slot still bounds work) BEFORE it
 *          re-records the slot and returns without waiting its own frame, so the CPU records frame
 *          N+1 while the GPU still draws frame N; the wait only ever holds the CPU back to at most
 *          one frame behind the GPU. The per-slot semaphore reuse is legal because each slot's
 *          wait submit is enqueued before the same slot is next signalled - the host orders the
 *          alternating signal/wait sequence, and the fence bounds the graphics half of it.
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

XudResult xud_frame_fill_submit(VkSubmitInfo* outSubmit, VkPresentInfoKHR* outPresent, VkCommandBuffer commandBuffer,
    VkSwapchainKHR swapchain, VkSemaphore renderFinished) {
    if ((outSubmit == NULL) || (outPresent == NULL)) {
        return XUD_INVALID_ARG;
    }
    if (commandBuffer == VK_NULL_HANDLE) {
        return XUD_INVALID_ARG;
    }
    if (swapchain == VK_NULL_HANDLE) {
        return XUD_INVALID_ARG;
    }
    if (renderFinished == VK_NULL_HANDLE) {
        return XUD_INVALID_ARG;
    }

    outSubmit->sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    outSubmit->pNext = NULL;
    outSubmit->waitSemaphoreCount = 1u;
    outSubmit->pWaitSemaphores = NULL; /* filled by the driver half, which is the side that has a semap
                                         hore handle in hand */
    outSubmit->pWaitDstStageMask = NULL; /* filled by the driver half, which owns the stage row */
    outSubmit->commandBufferCount = 1u; /* ONE command buffer per submit - the in-flight slot's own */
    outSubmit->pCommandBuffers = NULL; /* filled by the driver half, which owns the buffer array */
    outSubmit->signalSemaphoreCount = 1u;
    outSubmit->pSignalSemaphores = NULL; /* filled by the driver half, which owns the semaphore storage: the pure half
                                           would store the address of a parameter that dies with the call */

    outPresent->sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    outPresent->pNext = NULL;
    outPresent->waitSemaphoreCount = 0u;
    outPresent->pWaitSemaphores = NULL; /* the presentation itself waits on nothing: on the present queue it
                                          is ordered behind the no-command wait submit, and the render-finished
                                          semaphore lives there rather than on this row */
    outPresent->swapchainCount = 1u;
    outPresent->pSwapchains = NULL; /* filled by the driver half, which owns the swapchain handle */
    outPresent->pImageIndices = NULL; /* filled by the driver half, which owns the index */
    outPresent->pResults = NULL; /* the per-swapchain VkResult out-array is refused here rather than as
                                   ked for: the rung's own return code is the report, and a pResults arr
                                   ay would be a second channel for the same answer that a caller above w
                                   ould have to read */

    return XUD_OK;
}

XudResult xud_frame_fill_present_wait(VkSubmitInfo* outSubmit, VkSemaphore renderFinished) {
    if ((outSubmit == NULL) || (renderFinished == VK_NULL_HANDLE)) {
        return XUD_INVALID_ARG;
    }

    outSubmit->sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    outSubmit->pNext = NULL;
    outSubmit->waitSemaphoreCount = 1u;
    outSubmit->pWaitSemaphores = NULL; /* filled by the driver half, which owns the semaphore storage: the pure half
                                         would store the address of a parameter that dies with the call */
    outSubmit->pWaitDstStageMask = NULL; /* filled by the driver half; there is no command of this submit
                                            to wait a stage for, and the row exists because the count demands it */
    outSubmit->commandBufferCount = 0u;
    outSubmit->pCommandBuffers = NULL; /* the no-command submit: it exists to order the queue, not to run */
    outSubmit->signalSemaphoreCount = 0u;
    outSubmit->pSignalSemaphores = NULL;

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

    /* The model matrix is the draw's own, copied out of the row the batch recorded it in: a plain
       append recorded the identity into that member (xud_batch_record_draw's own stamp), a posed
       mesh, a posed model or a screen-space 2D row recorded the transform it was asked for, and
       this rung's job is to push the row's channel through rather than to choose a matrix of its
       own. The identity the row carries for a plain draw is the same fact the stage below sees
       either way: the vertex is placed by frame-viewProj * the draw's own model, and a draw whose
       model is the identity is a draw the viewProj alone places.
    */
    for (axis = 0u; axis < XUD_BATCH_MODEL_MATRIX_COUNT; axis++) {
        outRow->modelMatrix[axis] = drawRow->modelMatrix[axis];
    }

    /* The batch's sentinel is mapped to slot 0 here rather than passed through, and the reason is th
       e argument's type: the push row's material index is an INDEX the shader would use, and XUD_BAT
       CH_NO_MATERIAL is 0xFFFFFFFF, which is not an index into anything. "No material slot yet" is a
       batch-side fact about a draw made with no material - which is what xud_end_material() and xud
       _begin_drawing() move the state to when no material is live - and the per-draw channel has to c
       arry a number a read could use, so slot 0 is what a no-material draw names. A real material's
       slot index passes through unchanged, because the batch's material channel IS the handle layer'
       s MATERIAL slot index: the public pair validated the handle against that table before it mov
       ed the index in.
    */
    outRow->materialIndex = (drawRow->materialIndex == XUD_BATCH_NO_MATERIAL) ? 0u : drawRow->materialIndex;

    /* The same sentinel mapping one channel over: a draw with no map names slot 0 of set 1's
       sampler array - the white default the descriptor rung wrote at its bring-up - and the push
       row's member is an index a read could use, so the sentinel that indexes nothing is mapped to
       the slot every no-map draw samples through. A real slot passes through unchanged: it is the
       number the frame loop's slot-set rebind names beside this draw.
    */
    outRow->textureIndex = (drawRow->textureIndex == XUD_BATCH_NO_TEXTURE) ? 0u : drawRow->textureIndex;

    /* The variant the draw was flushed under, in the per-draw channel rather than a channel of this r
       ung's: the pipeline variant is chosen by BINDING a different pipeline, and this is the same fa
       ct seen from the shader's side, which is what lets a stage branch on the variant without a thi
       rd pipeline. The vertex stage reads it now: solid.vert skips the per-frame viewProj when the v
       ariant is XUD_BATCH_VARIANT_UI, the screen-space column whose position is clip space already,
       so the channel is what makes the overlay screen-fixed rather than a flag this rung would have
       to special-case beside each push. */
    outRow->flags = drawRow->variant;
    outRow->reserved = 0u;
    /* The tint is the draw's own, copied out of the row the same way the matrix above is: white for
       a plain append, the caller's colour for a posed or a 2D draw, and the fragment stage is the
       one that multiplies it in - the push row carries the channel, the stage spends it. */
    for (axis = 0u; axis < XUD_BATCH_TINT_COUNT; axis++) {
        outRow->tint[axis] = drawRow->tint[axis];
    }

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The bring-up - the driver half
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_frame_open(
    XudFrame** outFrame, XudLoader* loader, const XudDevice* device, const XudSwapchain* swapchain, XudLayoutTracker* layout) {
    XudFrame* frame = NULL;
    XudResult result = XUD_INVALID_ARG;
    VkResult vk = VK_ERROR_UNKNOWN;
    uint32_t index = 0u;
    uint32_t buffersAllocated = 0u;

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
    if (layout == NULL) {
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

    frame->layout = layout;

    /* The fence and the semaphore create-infos are the empty forms: a fence that started already
       signalled is the opposite of a fence that has to bound a submission. */
    frame->semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    frame->semaphoreInfo.pNext = NULL;
    frame->semaphoreInfo.flags = 0u;
    frame->fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    frame->fenceInfo.pNext = NULL;
    frame->fenceInfo.flags = 0u;

    /* The wait-stage row is filled at open rather than per frame: it is part of the submit row the
       struct keeps, and a stage row left at 0 would wait the acquire semaphore at no stage at all. */
    frame->waitStages[0u] = XUD_FRAME_WAIT_STAGE;

    result = xud_frame_fill_pool_info(&frame->poolInfo, device->queueRows[0].queueFamilyIndex);
    if (result != XUD_OK) {
        free(frame);
        return result;
    }

    vk = loader->deviceTable.vkCreateCommandPool(device->device, &frame->poolInfo, NULL, &frame->commandPool);
    if (vk != VK_SUCCESS) {
        free(frame);
        return xud_result_from_vk(vk);
    }

    result = xud_frame_fill_allocation(&frame->allocationInfo, frame->commandPool);
    if (result != XUD_OK) {
        goto fail;
    }

    vk = loader->deviceTable.vkAllocateCommandBuffers(device->device, &frame->allocationInfo, frame->commandBufferRows);
    if (vk != VK_SUCCESS) {
        result = xud_result_from_vk(vk);
        goto fail;
    }
    buffersAllocated = 1u;
    for (index = 0u; index < XUD_FRAME_IN_FLIGHT_COUNT; index++) {
        if (frame->commandBufferRows[index] == VK_NULL_HANDLE) {
            result = XUD_INTERNAL_ERROR;
            goto fail;
        }
    }
    frame->commandBuffer = frame->commandBufferRows[0u];

    /* One acquire + render-finished semaphore pair per in-flight slot; the handle lands in the
       struct only once the create answered success, so the fail path below retires exactly the
       live rows. */
    for (index = 0u; index < XUD_FRAME_IN_FLIGHT_COUNT; index++) {
        VkSemaphore made = VK_NULL_HANDLE;
        vk = loader->deviceTable.vkCreateSemaphore(device->device, &frame->semaphoreInfo, NULL, &made);
        if (vk != VK_SUCCESS) {
            result = xud_result_from_vk(vk);
            goto fail;
        }
        frame->imageAcquired[index] = made;
        made = VK_NULL_HANDLE;
        vk = loader->deviceTable.vkCreateSemaphore(device->device, &frame->semaphoreInfo, NULL, &made);
        if (vk != VK_SUCCESS) {
            result = xud_result_from_vk(vk);
            goto fail;
        }
        frame->renderFinished[index] = made;
    }

    /* One fence per in-flight slot: the fence bounds the slot's recorded work, and the slot's
       next use is what waits and resets it. */
    for (index = 0u; index < XUD_FRAME_IN_FLIGHT_COUNT; index++) {
        VkFence madeFence = VK_NULL_HANDLE;
        vk = loader->deviceTable.vkCreateFence(device->device, &frame->fenceInfo, NULL, &madeFence);
        if (vk != VK_SUCCESS) {
            result = xud_result_from_vk(vk);
            goto fail;
        }
        frame->frameFences[index] = madeFence;
    }
    frame->frameFenceCount = XUD_FRAME_IN_FLIGHT_COUNT;

    /* Nothing is in flight and no acquire has answered yet - written rather than left to calloc's
       zero, because these are rows the draw and the teardown read as state. */
    for (index = 0u; index < XUD_FRAME_IN_FLIGHT_COUNT; index++) {
        frame->inFlight[index] = 0u;
    }
    frame->currentFrame = 0u;
    frame->recreateWanted = 0u;
    frame->presentedSwapchain = swapchain->swapchain; /* borrowed, updated by every draw */

    *outFrame = frame;
    return XUD_OK;

fail:
    /* Everything created so far, retired in the reverse of the create order; a row of the calloc'd
       struct is VK_NULL_HANDLE until its own create wrote a handle, so a non-null row is exactly a
       live object. */
    for (index = XUD_FRAME_IN_FLIGHT_COUNT; index > 0u; index--) {
        if (frame->frameFences[index - 1u] != VK_NULL_HANDLE) {
            (void)loader->deviceTable.vkDestroyFence(device->device, frame->frameFences[index - 1u], NULL);
        }
    }
    for (index = XUD_FRAME_IN_FLIGHT_COUNT; index > 0u; index--) {
        if (frame->renderFinished[index - 1u] != VK_NULL_HANDLE) {
            (void)loader->deviceTable.vkDestroySemaphore(device->device, frame->renderFinished[index - 1u], NULL);
        }
        if (frame->imageAcquired[index - 1u] != VK_NULL_HANDLE) {
            (void)loader->deviceTable.vkDestroySemaphore(device->device, frame->imageAcquired[index - 1u], NULL);
        }
    }
    if (buffersAllocated != 0u) {
        loader->deviceTable.vkFreeCommandBuffers(
            device->device, frame->commandPool, XUD_FRAME_COMMAND_BUFFER_COUNT, frame->commandBufferRows);
    }
    (void)loader->deviceTable.vkDestroyCommandPool(device->device, frame->commandPool, NULL);
    frame->commandPool = VK_NULL_HANDLE;
    free(frame);
    return result;
}

/* Flush one batch - the pending run closed into a draw row - and answer whether it has content
   at all. Pure host work, so the loop runs it for EVERY batch it will replay (each scope's own
   batch first, the screen batch last) BEFORE the command buffer is begun - the rule xud_frame_dra
   w() states for the screen batch, kept as one rung-wide discipline rather than one batch's lucky
   path. The counts come from the batch's own accessor, not its fields - the repo's accessor rule.
*/
static XudResult xud_frame_flush_batch(XudBatch* batch, uint32_t* outHasContent) {
    uint32_t vertexCount = 0u;
    uint32_t indexCount = 0u;
    uint32_t vertexCapacity = 0u;
    uint32_t drawCount = 0u;
    VkDeviceSize stagingBytes = 0u;

    if (xud_batch_stats(batch, &vertexCount, &indexCount, &vertexCapacity, &drawCount, &stagingBytes) != XUD_OK) {
        return XUD_INTERNAL_ERROR;
    }
    *outHasContent = ((vertexCount > 0u) && (indexCount > 0u)) ? 1u : 0u;
    if (*outHasContent != 0u) {
        if (xud_batch_record_draw(batch, batch->currentTopology, batch->currentVariant, batch->currentMaterial, batch->currentTexture)
            != XUD_OK) {
            return XUD_OUT_OF_MEMORY;
        }
    }
    return XUD_OK;
}

/* One pass scope, end to end inside an ALREADY BEGUN command buffer: the two dynamic states for
   this pass's extent, the opening layout transitions read from the tracker (the rule the swapchain
   path stated first - a barrier's oldLayout has no other source in this tree), the batch's upload
   and binds, the pass, the replay of the batch's draw rows, the pass's end. Every comment that
   made the swapchain recording correct makes this recording correct for ANY pass - the rows it
   names (pass, framebuffer row, batch) are arguments now, so the screen pass and an off-screen
   pass run the same code. `sampledAfter` is the off-screen half of the contract: the pass's own
   finalLayout has already moved the colour image to SHADER_READ_ONLY inside the pass, and the
   barrier this flag adds puts the WRITE before the screen pass's sample on the same command
   buffer - an execution and memory dependency between the two passes, which no subpass
   dependency of either pass covers on its own. A failure after the begins ends the command
   buffer before returning, which keeps the rung's claim that a begun buffer is always ended
   checkable rather than a promise. `prepass` is the depth pre-pass (S2.4): the same batch rows,
   the same buffers and the same push rows replayed through the colorless pipeline, with the
   columns that write no depth (TRANSPARENT, UI) and the topology with no pre-pass row (LINES)
   stepped over, and no slot-set bind - the pre-pass pipeline reads set 0 and the push row and
   nothing else.
*/
static XudResult xud_frame_record_pass(XudFrame* frame, XudLoader* loader, const XudDevice* device, const XudPipeline* pipeline,
    const XudRenderPass* pass, const XudFramebuffer* row, XudBatch* batch, XudDescriptor* descriptor, uint32_t hasContent,
    uint32_t sampledAfter, uint32_t resolveSlot, uint32_t prepass) {
    XudBatchDraw drawRow;
    XudBatchGeometry geometryRow;
    XudPushRow pushRow;
    XudResult result;
    VkCommandBufferBeginInfo begin;
    VkViewport viewport;
    VkRect2D scissor;
    VkRenderPassBeginInfo passBegin;
    VkImageMemoryBarrier imageBarriers[XUD_RENDERPASS_ATTACHMENT_COUNT];
    VkImageMemoryBarrier filledBarriers[XUD_RENDERPASS_ATTACHMENT_COUNT];
    uint32_t vertexCount = 0u;
    uint32_t indexCount = 0u;
    uint32_t vertexCapacity = 0u;
    uint32_t drawCount = 0u;
    VkDeviceSize stagingBytes = 0u;
    uint32_t draw = 0u;
    uint32_t boundGeometry = XUD_BATCH_GEOMETRY_NONE;
    uint32_t boundTexture = XUD_BATCH_NO_TEXTURE;

    result = xud_frame_fill_recording(&begin, &viewport, &scissor, row->createInfo.width, row->createInfo.height);
    if (result != XUD_OK) {
        (void)loader->deviceTable.vkEndCommandBuffer(frame->commandBuffer);
        return result;
    }
    loader->deviceTable.vkCmdSetViewport(frame->commandBuffer, 0u, 1u, &viewport);
    loader->deviceTable.vkCmdSetScissor(frame->commandBuffer, 0u, 1u, &scissor);

    passBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    passBegin.pNext = NULL;
    passBegin.framebuffer = row->framebuffer;
    passBegin.renderPass = pass->renderPass;
    passBegin.renderArea.offset.x = 0;
    passBegin.renderArea.offset.y = 0;
    passBegin.renderArea.extent.width = row->createInfo.width;
    passBegin.renderArea.extent.height = row->createInfo.height;
    passBegin.pClearValues = pass->clearValues;
    passBegin.clearValueCount = XUD_RENDERPASS_ATTACHMENT_COUNT;

    imageBarriers[XUD_RENDERPASS_COLOR_ATTACHMENT].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    imageBarriers[XUD_RENDERPASS_COLOR_ATTACHMENT].pNext = NULL;
    imageBarriers[XUD_RENDERPASS_COLOR_ATTACHMENT].srcAccessMask
        = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    imageBarriers[XUD_RENDERPASS_COLOR_ATTACHMENT].dstAccessMask
        = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    imageBarriers[XUD_RENDERPASS_COLOR_ATTACHMENT].srcQueueFamilyIndex = 0u;
    imageBarriers[XUD_RENDERPASS_COLOR_ATTACHMENT].dstQueueFamilyIndex = 0u;
    imageBarriers[XUD_RENDERPASS_COLOR_ATTACHMENT].newLayout = pass->attachments[XUD_RENDERPASS_COLOR_ATTACHMENT].initialLayout;
    imageBarriers[XUD_RENDERPASS_COLOR_ATTACHMENT].image = row->image;
    imageBarriers[XUD_RENDERPASS_COLOR_ATTACHMENT].subresourceRange = row->viewInfo.subresourceRange;
    imageBarriers[XUD_RENDERPASS_DEPTH_ATTACHMENT] = imageBarriers[XUD_RENDERPASS_COLOR_ATTACHMENT];
    imageBarriers[XUD_RENDERPASS_DEPTH_ATTACHMENT].srcAccessMask
        = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    imageBarriers[XUD_RENDERPASS_DEPTH_ATTACHMENT].dstAccessMask
        = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    imageBarriers[XUD_RENDERPASS_DEPTH_ATTACHMENT].newLayout = pass->attachments[XUD_RENDERPASS_DEPTH_ATTACHMENT].initialLayout;
    imageBarriers[XUD_RENDERPASS_DEPTH_ATTACHMENT].image = pass->depthImage;
    imageBarriers[XUD_RENDERPASS_DEPTH_ATTACHMENT].subresourceRange = pass->depthViewInfo.subresourceRange;

    if ((xud_layout_barrier_fill(&filledBarriers[XUD_RENDERPASS_COLOR_ATTACHMENT], frame->layout,
             imageBarriers[XUD_RENDERPASS_COLOR_ATTACHMENT].image, &imageBarriers[XUD_RENDERPASS_COLOR_ATTACHMENT])
            != XUD_OK)
        || (xud_layout_barrier_fill(&filledBarriers[XUD_RENDERPASS_DEPTH_ATTACHMENT], frame->layout,
                imageBarriers[XUD_RENDERPASS_DEPTH_ATTACHMENT].image, &imageBarriers[XUD_RENDERPASS_DEPTH_ATTACHMENT])
            != XUD_OK)) {
        (void)loader->deviceTable.vkEndCommandBuffer(frame->commandBuffer);
        return XUD_INTERNAL_ERROR;
    }
    loader->deviceTable.vkCmdPipelineBarrier(frame->commandBuffer, XUD_FRAME_BARRIER_STAGES, XUD_FRAME_BARRIER_STAGES, 0u, 0u, NULL, 0u,
        NULL, XUD_RENDERPASS_ATTACHMENT_COUNT, filledBarriers);

    if (xud_batch_stats(batch, &vertexCount, &indexCount, &vertexCapacity, &drawCount, &stagingBytes) != XUD_OK) {
        (void)loader->deviceTable.vkEndCommandBuffer(frame->commandBuffer);
        return XUD_INTERNAL_ERROR;
    }
    if (hasContent != 0u) {
        if (xud_batch_upload(batch, loader, device, frame->commandBuffer) != XUD_OK) {
            (void)loader->deviceTable.vkEndCommandBuffer(frame->commandBuffer);
            return XUD_INTERNAL_ERROR;
        }
        {
            const VkMemoryBarrier transferToReads = (VkMemoryBarrier) { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .pNext = NULL,
                .srcAccessMask = XUD_FRAME_TRANSFER_WRITE_ACCESS,
                .dstAccessMask = XUD_FRAME_VERTEX_READ_ACCESS };
            loader->deviceTable.vkCmdPipelineBarrier(frame->commandBuffer, XUD_FRAME_BARRIER_TRANSFER_STAGE,
                XUD_FRAME_BARRIER_VERTEX_READ_STAGES, 0u, 1u, &transferToReads, 0u, NULL, 0u, NULL);
        }
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
        loader->deviceTable.vkCmdBindIndexBuffer(frame->commandBuffer, batch->indexBuffer, 0u, XUD_FRAME_INDEX_TYPE);
        boundGeometry = XUD_BATCH_GEOMETRY_OWN;
    }

    loader->deviceTable.vkCmdBeginRenderPass(frame->commandBuffer, &passBegin, VK_SUBPASS_CONTENTS_INLINE);
    /* Set 0 is bound HERE - inside the pass, after vkCmdBeginRenderPass - once per recorded pass,
       against the pipeline's OWN layout: the layout the pipeline was created with is the layout the
       set was allocated against, so naming pipeline->layout makes the two agree rather than two re-
       cords that happen to match. The bind sits after BeginRenderPass and not beside the frame's
       other state-setting because that is where the draws' read of it was measured to work: with
       the bind recorded before BeginRenderPass the pass's clears landed but every draw that reads
       the block drew nothing, and the same bytes read back intact from the buffer - probed on this
       machine 2026-11-13. One bind per pass, not one per frame, because each recorded pass is its
       own state scope: an off-screen scope and the screen pass both read the block and neither may
       inherit a bind recorded outside it. The slot sets (set 1) stay a per-draw bind below: the set
       a draw samples depends on that draw's texture, and binding the right one per draw is what
       keeps every descriptor written before its set is bound.
    */
    {
        const VkDescriptorSet setRows[XUD_FRAME_SET_COUNT] = { descriptor->sets[XUD_DESCRIPTOR_SET_PER_FRAME] };
        loader->deviceTable.vkCmdBindDescriptorSets(frame->commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->layout,
            XUD_FRAME_FIRST_SET, XUD_FRAME_SET_COUNT, setRows, 0u, NULL);
    }
    if (resolveSlot != XUD_FRAME_NO_RESOLVE) {
        /* The resolve pass's own recording: the scratch image's slot set takes set 1 exactly the
           way a textured batch draw's slot set does - one set per slot, this pass's slot chosen at
           the renderer's bring-up - and the resolve pipeline draws the three vertices the command
           buffer itself invents. No vertex buffers are bound and none may be: the pipeline has zero
           vertex bindings, and the batch's streams bound above belong to the scratch pass that came
           BEFORE this one, not to a pass that records no batch rows at all. */
        const VkDescriptorSet slotSet = descriptor->sets[XUD_DESCRIPTOR_SLOT_SET_INDEX(resolveSlot)];
        loader->deviceTable.vkCmdBindDescriptorSets(
            frame->commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->layout, XUD_DESCRIPTOR_SET_MATERIAL, 1u, &slotSet, 0u, NULL);
        loader->deviceTable.vkCmdBindPipeline(frame->commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->resolvePipeline);
        loader->deviceTable.vkCmdDraw(frame->commandBuffer, 3u, 1u, 0u, 0u);
    }
    /* The second half of the condition is the batch rows' gate: a resolve pass records no draws,
       the triangle above is its whole content, and the stats row read above still counts the scene
       batch's rows - they belong to the pass that ran BEFORE this one. */
    for (draw = 0u; (draw < drawCount) && (resolveSlot == XUD_FRAME_NO_RESOLVE); draw++) {
        if (xud_batch_draw_get(batch, draw, &drawRow) != XUD_OK) {
            break;
        }
        if ((drawRow.topology >= XUD_BATCH_TOPOLOGY_COUNT) || (drawRow.variant >= XUD_BATCH_VARIANT_COUNT)) {
            break;
        }
        if ((prepass != 0u)
            && ((drawRow.topology != XUD_BATCH_TOPOLOGY_TRIANGLES) || (drawRow.variant == XUD_BATCH_VARIANT_TRANSPARENT)
                || (drawRow.variant == XUD_BATCH_VARIANT_UI))) {
            /* The pre-pass is TRIANGLES-only and depth-only: the transparent and screen-space
               columns write no depth the sort would keep (and the UI's clip-space positions
               would poison it), and line geometry has no colorless pipeline to replay
               through - a line list read as a triangle list is the one thing worse than
               skipping it. Lines still depth-TEST in the shaded pass, against this sort. */
            continue;
        }
        if (drawRow.geometryIndex != boundGeometry) {
            if (xud_batch_geometry_get(batch, drawRow.geometryIndex, &geometryRow) != XUD_OK) {
                break;
            }
            loader->deviceTable.vkCmdBindVertexBuffers(
                frame->commandBuffer, XUD_FRAME_FIRST_BINDING, XUD_BATCH_STREAM_COUNT, geometryRow.buffer, geometryRow.offset);
            loader->deviceTable.vkCmdBindIndexBuffer(frame->commandBuffer, geometryRow.buffer[XUD_BATCH_STREAM_COUNT],
                geometryRow.offset[XUD_BATCH_STREAM_COUNT], XUD_FRAME_INDEX_TYPE);
            boundGeometry = drawRow.geometryIndex;
        }
        loader->deviceTable.vkCmdBindPipeline(frame->commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            (prepass != 0u) ? pipeline->prepassPipeline : pipeline->pipeline[XUD_PIPELINE_ROW(drawRow.topology, drawRow.variant)]);
        if (xud_frame_fill_push_row(&pushRow, &drawRow) != XUD_OK) {
            break;
        }
        if (pushRow.textureIndex >= XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE) {
            break;
        }
        if ((prepass == 0u) && (pushRow.textureIndex != boundTexture)) {
            const VkDescriptorSet slotSet = descriptor->sets[XUD_DESCRIPTOR_SLOT_SET_INDEX(pushRow.textureIndex)];
            loader->deviceTable.vkCmdBindDescriptorSets(frame->commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->layout,
                XUD_DESCRIPTOR_SET_MATERIAL, 1u, &slotSet, 0u, NULL);
            boundTexture = pushRow.textureIndex;
        }
        loader->deviceTable.vkCmdPushConstants(frame->commandBuffer, pipeline->layout, descriptor->pushRange.stageFlags,
            descriptor->pushRange.offset, descriptor->pushRange.size, &pushRow);
        loader->deviceTable.vkCmdDrawIndexed(frame->commandBuffer, drawRow.indexCount, 1u, drawRow.firstIndex, 0, 0u);
    }
    loader->deviceTable.vkCmdEndRenderPass(frame->commandBuffer);

    if (sampledAfter != 0u) {
        const VkImageMemoryBarrier toSample = (VkImageMemoryBarrier) { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = NULL,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = pass->attachments[XUD_RENDERPASS_COLOR_ATTACHMENT].finalLayout,
            .newLayout = pass->attachments[XUD_RENDERPASS_COLOR_ATTACHMENT].finalLayout,
            .image = row->image,
            .subresourceRange = row->viewInfo.subresourceRange,
            .srcQueueFamilyIndex = 0u,
            .dstQueueFamilyIndex = 0u };
        loader->deviceTable.vkCmdPipelineBarrier(frame->commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0u, 0u, NULL, 0u, NULL, 1u, &toSample);
    }
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * One frame, end to end - the driver half
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_frame_draw(XudFrame* frame, XudLoader* loader, const XudDevice* device, const XudSwapchain* swapchain,
    const XudPipeline* pipeline, const XudRenderPass* pass, const XudFramebufferArray* framebuffers, uint32_t framebufferCount,
    const XudRenderPass* hdrPass, const XudFramebuffer* hdrFramebuffer, const XudRenderPass* prepassPass,
    const XudFramebuffer* prepassFramebuffer, uint32_t hdrSlot, const XudFramePassScope* scopes, uint32_t scopeCount, XudBatch* batch,
    XudDescriptor* descriptor, uint32_t* outImageIndex) {
    XudQueue graphicsQueue;
    XudQueue presentQueue;
    XudFramebuffer* row = NULL;
    XudResult result = XUD_INVALID_ARG;
    VkResult vk = VK_ERROR_UNKNOWN;
    VkCommandBufferBeginInfo begin;
    VkViewport viewport;
    VkRect2D scissor;
    VkSubmitInfo submit;
    VkPresentInfoKHR present;
    VkSubmitInfo presentWaitSubmit;
    uint32_t imageIndex = 0u;
    uint32_t acquireRecreateWanted = 0u;
    uint32_t presentRecreateWanted = 0u;
    VkFence frameFence = VK_NULL_HANDLE;
    uint32_t hasContent = 0u;
    uint32_t pipelineRow = 0u;
    uint32_t scope = 0u;
    uint32_t scopeContent[XUD_FRAME_SCOPE_MAX];
    uint32_t slot = 0u;

    if (((scopes == NULL) && (scopeCount > 0u)) || (scopeCount > XUD_FRAME_SCOPE_MAX)) {
        return XUD_INVALID_ARG;
    }
    for (scope = 0u; scope < scopeCount; scope++) {
        if ((scopes[scope].pass == NULL) || (scopes[scope].pass->renderPass == VK_NULL_HANDLE)
            || (scopes[scope].pass->depthImage == VK_NULL_HANDLE) || (scopes[scope].framebuffer == NULL)
            || (scopes[scope].framebuffer->framebuffer == VK_NULL_HANDLE) || (scopes[scope].framebuffer->view == VK_NULL_HANDLE)
            || (scopes[scope].batch == NULL)) {
            return XUD_INVALID_ARG;
        }
    }

    if ((frame == NULL) || (loader == NULL) || (device == NULL) || (swapchain == NULL) || (pipeline == NULL) || (pass == NULL)
        || (framebuffers == NULL) || (outImageIndex == NULL) || (batch == NULL) || (descriptor == NULL)) {
        return XUD_INVALID_ARG;
    }
    if (!loader->open) {
        return XUD_UNSUPPORTED;
    }
    if ((device->device == VK_NULL_HANDLE) || (swapchain->swapchain == VK_NULL_HANDLE) || (pipeline->layout == VK_NULL_HANDLE)
        || (pass->renderPass == VK_NULL_HANDLE) || (frame->commandPool == VK_NULL_HANDLE) || (frame->commandBuffer == VK_NULL_HANDLE)
        || (hdrPass == NULL) || (hdrPass->renderPass == VK_NULL_HANDLE) || (hdrFramebuffer == NULL)
        || (hdrFramebuffer->framebuffer == VK_NULL_HANDLE) || (pipeline->resolvePipeline == VK_NULL_HANDLE) || (prepassPass == NULL)
        || (prepassPass->renderPass == VK_NULL_HANDLE) || (prepassFramebuffer == NULL)
        || (prepassFramebuffer->framebuffer == VK_NULL_HANDLE) || (pipeline->prepassPipeline == VK_NULL_HANDLE)
        || (hdrSlot >= XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE)) {
        /* The scratch pass, its framebuffer and its slot are as load-bearing as the screen pass's
           own three: the frame draws the scene into them BEFORE the screen pass exists to show it,
           and the resolve triangle names the slot by number. The pre-pass pair and the pre-pass
           pipeline are the fourth load-bearing row: the scratch pass LOADs a depth buffer someone
           has to have cleared and sorted, and that someone is this frame's first recording. */
        return XUD_INVALID_ARG;
    }
    /* Every row of the pipeline list has to be live, not just the two the variant axis names: a draw
       row can name either axis, so a list with a row of VK_NULL_HANDLE in it is a list this frame
       could bind a null pipeline from. The loop is the whole list rather than two named rows, which
       is what makes growing either axis a guard that needs no edit.
    */
    for (pipelineRow = 0u; pipelineRow < XUD_PIPELINE_ROW_COUNT; pipelineRow++) {
        if (pipeline->pipeline[pipelineRow] == VK_NULL_HANDLE) {
            return XUD_INVALID_ARG;
        }
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
    if ((frame->frameFenceCount != XUD_FRAME_IN_FLIGHT_COUNT) || (frame->presentedSwapchain != swapchain->swapchain)
        || (framebuffers->count != swapchain->imageCount) || (framebufferCount != swapchain->imageCount)) {
        return XUD_UNSUPPORTED;
    }
    if (frame->layout == NULL) {
        return XUD_UNSUPPORTED; /* the two opening barriers read their oldLayouts from the context's
                                   map; a frame assembled without one cannot answer them */
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

    /* --- 0. the in-flight slot. The CPU waits THIS slot's fence only when the slot still bounds
       work (the frame two before this one, in a two-slot ring) and never its own frame, which is
       what keeps the CPU at most one frame ahead of the GPU rather than lock-step with it. --- */
    slot = frame->currentFrame;
    if ((slot >= XUD_FRAME_IN_FLIGHT_COUNT) || (frame->commandBufferRows[slot] == VK_NULL_HANDLE)
        || (frame->imageAcquired[slot] == VK_NULL_HANDLE) || (frame->renderFinished[slot] == VK_NULL_HANDLE)
        || (frame->frameFences[slot] == VK_NULL_HANDLE)) {
        return XUD_INTERNAL_ERROR;
    }
    if (frame->inFlight[slot] != 0u) {
        vk = loader->deviceTable.vkWaitForFences(device->device, 1u, &frame->frameFences[slot], XUD_FRAME_WAIT_ALL, XUD_FRAME_NO_TIMEOUT);
        result = xud_result_from_vk(vk);
        if (result != XUD_OK) {
            return result;
        }
        vk = loader->deviceTable.vkResetFences(device->device, 1u, &frame->frameFences[slot]);
        result = xud_result_from_vk(vk);
        if (result != XUD_OK) {
            return result;
        }
        frame->inFlight[slot] = 0u;
    }
    frame->commandBuffer = frame->commandBufferRows[slot];

    result = xud_frame_fill_submit(&submit, &present, frame->commandBuffer, swapchain->swapchain, frame->renderFinished[slot]);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_frame_fill_present_wait(&presentWaitSubmit, frame->renderFinished[slot]);
    if (result != XUD_OK) {
        return result;
    }
    presentWaitSubmit.pWaitDstStageMask = frame->waitStages;
    submit.pSignalSemaphores = &frame->renderFinished[slot]; /* the storage the signal row names lives in the frame, not in a
                                                                parameter the fill died with */
    presentWaitSubmit.pWaitSemaphores = &frame->renderFinished[slot]; /* and the wait row names the same storage: the two halves
                                                                         of the boundary cannot name different semaphores */

    /* --- 1. acquire. The acquire semaphore is the signal that an image is available; the fence argumen
       t here is VK_NULL_HANDLE because the fence this rung uses bounds the DRAW, not the acquire, and pa
       ssing one here would be a second, different boundary on the same image. --- */
    vk = loader->deviceTable.vkAcquireNextImageKHR(
        device->device, swapchain->swapchain, XUD_FRAME_NO_TIMEOUT, frame->imageAcquired[slot], VK_NULL_HANDLE, &imageIndex);
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
       point, so at this library's 1.3 floor (xud_instance.h's XUD_INSTANCE_API_FLOOR) the row is live
       rather than nil - at the 1.0 floor it was the other way round, and the repo's rule that a name
       that is not there is reported, not worked around, is why the host-side wait was never an option
       there. It is still not called here: the submit-side gate is this layer's own choice, and the
       core-1.0 form of the same boundary is a row of the SUBMIT - pWaitSemaphores names the acquire
       semaphore and pWaitDstStageMask names the vertex stage, which is where the draw's work begins,
       so the recorded work cannot touch the image until the acquire signal fires and the gate is on
       the queue rather than on the host. Step 5 below writes exactly that pair. --- */

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

    /* --- 3b. EVERY batch this frame replays is flushed HERE, before the command buffer is begun:
       each off-screen scope's own batch first and the screen batch last. Closing the pending run
       into a draw row and reading the row list are both PURE host work - they need no command buf
       fer - so doing them here is what keeps every failure path below the begin call one that end
       s the buffer it began rather than returning out of a begun one. What the flush records is t
       he whole list that pass replays; xud_frame_flush_batch() carries the reasoning.
    */
    for (scope = 0u; scope < scopeCount; scope++) {
        result = xud_frame_flush_batch(scopes[scope].batch, &scopeContent[scope]);
        if (result != XUD_OK) {
            return result;
        }
    }
    result = xud_frame_flush_batch(batch, &hasContent);
    if (result != XUD_OK) {
        return result;
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

    /* The two dynamic states belong to the pass whose extent they cover, so they move into
       xud_frame_record_pass() - one pair per pass, the screen pass's included. */

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

    /* --- 5c. the off-screen scopes are recorded FIRST: their passes run before the screen pass,
       so an image one of them wrote is in SHADER_READ_ONLY - the pass's own finalLayout, followed
       by the record pass's sampled-sync barrier - before any fragment stage of the screen pass r
       eads it. Every scope ran its own opening barriers from the tracker, uploaded its own batch
       and replayed its own draw rows; the screen pass that follows needs nothing else of theirs.
    */
    for (scope = 0u; scope < scopeCount; scope++) {
        result = xud_frame_record_pass(frame, loader, device, pipeline, scopes[scope].pass, scopes[scope].framebuffer, scopes[scope].batch,
            descriptor, scopeContent[scope], 1u, XUD_FRAME_NO_RESOLVE, 0u);
        if (result != XUD_OK) {
            return result;
        }
    }

    /* --- 5d. the depth pre-pass: the same scene batch, colorless, into the scratch pass's own
       depth buffer, before the shaded pass runs. The upload rides HERE (hasContent on this call
       only): the batch's buffers are copied, readied for vertex-index reads and bulk-bound once,
       and the scratch recording that follows binds its geometry rows through the per-row path -
       xud_batch_geometry_get() serves the batch's own arena from the live stream rows, so the
       shaded pass re-binds rather than re-uploads. The pass clears the depth (its own loadOp)
       and the scratch pass one recording later LOADs it - the pairing xud_renderpass_open_prepass
       struck when the two passes were built. No sampled-sync barrier: this pass writes no colour.
    */
    result = xud_frame_record_pass(
        frame, loader, device, pipeline, prepassPass, prepassFramebuffer, batch, descriptor, hasContent, 0u, XUD_FRAME_NO_RESOLVE, 1u);
    if (result != XUD_OK) {
        return result;
    }

    /* --- 5e. the scratch pass: the scene, at the swapchain's extent into the float image, through
       the same recording helper with no swapchain image to wait on - the scratch image's and its
       depth's opening barriers come from the tracker exactly as a target scope's do. The depth it
       LOADs is the pre-pass's sort; the GEQUAL/colored pipelines reject fragments behind it before
       any fragment shader runs. The sampled-sync barrier fires after the pass (sampledAfter on):
       the resolve pass one recording later samples this very image, and the barrier is the same
       write-before-read guarantee an off-screen target gets before the screen pass samples it. The
       pass ends in SAMPLED - its own finalLayout - and the tracker keeps that honest for next
       frame.
    */
    result = xud_frame_record_pass(
        frame, loader, device, pipeline, hdrPass, hdrFramebuffer, batch, descriptor, 0u, 1u, XUD_FRAME_NO_RESOLVE, 0u);
    if (result != XUD_OK) {
        return result;
    }

    /* --- 5f. the screen pass, the same recording through the same helper, with the resolve
       triangle as its only content: it names the scratch image's slot, so set 1 is bound to the
       scratch pair rather than to a batch draw's, and no batch row is replayed here - the scene is
       already drawn, into the scratch image, two passes ago. */
    result = xud_frame_record_pass(frame, loader, device, pipeline, pass, row, batch, descriptor, 0u, 0u, hdrSlot, 0u);
    if (result != XUD_OK) {
        return result;
    }

    vk = loader->deviceTable.vkEndCommandBuffer(frame->commandBuffer);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK) {
        return result;
    }

    /* --- 5. submit on the graphics queue, with the slot's acquire semaphore waited at the vertex
       stage and the SLOT's own fence as the fence argument. The fence bounds the slot's recorded
       work; the image a frame wrote is recycled by the swapchain's own acquire, so no host-side
       wait on this frame is needed to protect the image. --- */
    frameFence = frame->frameFences[slot];
    submit.pWaitSemaphores = &frame->imageAcquired[slot];
    submit.pWaitDstStageMask = frame->waitStages;
    submit.pCommandBuffers = &frame->commandBufferRows[slot];
    vk = loader->deviceTable.vkQueueSubmit(graphicsQueue.queue, XUD_FRAME_SUBMIT_COUNT, &submit, frameFence);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK) {
        return result;
    }
    frame->inFlight[slot] = 1u;

    /* The layout record moves HERE, after a submit that answered success: the barrier above is wh
       at transitions the image into the pass's initialLayout, the pass's own initial/final transi
       tions happen inside the subpass where no barrier sees them, and the submission is what runs
       all of it - so the map moves once, to the pass's finalLayout, and nothing before here may
       claim it. A refused submit leaves the map at the layout the images really are still in, wh
       ich is what keeps the next frame's barrier's oldLayout a fact rather than a guess. */
    (void)xud_layout_set(frame->layout, row->image, pass->attachments[XUD_RENDERPASS_COLOR_ATTACHMENT].finalLayout);
    (void)xud_layout_set(frame->layout, pass->depthImage, pass->attachments[XUD_RENDERPASS_DEPTH_ATTACHMENT].finalLayout);
    for (scope = 0u; scope < scopeCount; scope++) {
        (void)xud_layout_set(
            frame->layout, scopes[scope].framebuffer->image, scopes[scope].pass->attachments[XUD_RENDERPASS_COLOR_ATTACHMENT].finalLayout);
        (void)xud_layout_set(
            frame->layout, scopes[scope].pass->depthImage, scopes[scope].pass->attachments[XUD_RENDERPASS_DEPTH_ATTACHMENT].finalLayout);
    }

    /* --- 5b. the present queue's no-command wait submit, BEFORE the present itself. This is the render
       -finished boundary spelled as a queue-side submit rather than as the present's own wait pair, and
       the reason the shape is a submit is the queue it runs on: on a device whose family exposes more
       than one queue, the two role handles differ, two VkQueue objects have no implicit ordering between
       their submissions, and a bare vkQueuePresentKHR waits on nothing - so the present could run ahead
       of the render it presents. A no-command submit that waits the semaphore the frame's submit just
       signalled puts the boundary on the present queue where the present itself is, and the queue's own
       FIFO then orders the present behind it. On a family with one queue the submit is a no-op round
       trip rather than a save, which is the price of the rule being written for both shapes. --- */
    vk = loader->deviceTable.vkQueueSubmit(presentQueue.queue, XUD_FRAME_SUBMIT_COUNT, &presentWaitSubmit, VK_NULL_HANDLE);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK) {
        return result;
    }

    /* --- 6. present on the present queue, ordered behind the wait submit above by the queue's own FIFO.
       The two role queues are two lookups rather than one because the two roles are a library decision
       and not a fact about this driver: the handles may agree on one machine and differ on another, and
       the wait submit above is what keeps the second shape correct. --- */
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

    /* --- 7. NO fence wait here. The fence this submit carries is waited and reset at the START of
       the next use of THIS slot - step 0 above - which is what lets the CPU record the next frame
       while this one still runs. inFlight[slot] is the record that it bounds work. --- */
    frame->currentFrame = (slot + 1u) % XUD_FRAME_IN_FLIGHT_COUNT;
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
    for (scope = 0u; scope < scopeCount; scope++) {
        if (xud_batch_reset(scopes[scope].batch) != XUD_OK) {
            return XUD_INTERNAL_ERROR;
        }
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
    for (index = 0u; index < XUD_FRAME_IN_FLIGHT_COUNT; index++) {
        if ((victim->inFlight[index] == 0u) || (victim->frameFences[index] == VK_NULL_HANDLE)) {
            continue;
        }
        if (loader->deviceTable.vkWaitForFences == NULL) {
            return XUD_UNSUPPORTED;
        }
        vk = loader->deviceTable.vkWaitForFences(device->device, 1u, &victim->frameFences[index], XUD_FRAME_WAIT_ALL, XUD_FRAME_NO_TIMEOUT);
        if (vk != VK_SUCCESS) {
            return xud_result_from_vk(vk);
        }
        victim->inFlight[index] = 0u;
    }

    /* A semaphore still named by a queue's submission cannot be destroyed either, and the last frame's
       wait submit is exactly such a naming: the fence above proves the GRAPHICS queue is past its signal
       of render-finished, but the PRESENT queue's no-command wait of that same semaphore is a queue-side
       op with no fence of its own, and may still be sitting in the queue when close lands. A wait-idle
       on each role queue is the boundary that makes the destroy below name nothing; on a family with one
       queue the two lookups agree and the second idle is a no-op. */
    if (loader->deviceTable.vkQueueWaitIdle == NULL) {
        return XUD_UNSUPPORTED;
    }
    (void)loader->deviceTable.vkQueueWaitIdle(device->graphicsQueue.queue);
    (void)loader->deviceTable.vkQueueWaitIdle(device->presentQueue.queue);

    if (victim->commandBuffer != VK_NULL_HANDLE) {
        if (loader->deviceTable.vkFreeCommandBuffers == NULL) {
            return XUD_UNSUPPORTED;
        }
        loader->deviceTable.vkFreeCommandBuffers(
            device->device, victim->commandPool, XUD_FRAME_COMMAND_BUFFER_COUNT, victim->commandBufferRows);
        victim->commandBuffer = VK_NULL_HANDLE;
    }
    for (index = XUD_FRAME_IN_FLIGHT_COUNT; index > 0u; index--) {
        if (victim->renderFinished[index - 1u] == VK_NULL_HANDLE) {
            continue;
        }
        if (loader->deviceTable.vkDestroySemaphore == NULL) {
            return XUD_UNSUPPORTED;
        }
        loader->deviceTable.vkDestroySemaphore(device->device, victim->renderFinished[index - 1u], NULL);
        victim->renderFinished[index - 1u] = VK_NULL_HANDLE;
    }
    for (index = XUD_FRAME_IN_FLIGHT_COUNT; index > 0u; index--) {
        if (victim->imageAcquired[index - 1u] == VK_NULL_HANDLE) {
            continue;
        }
        if (loader->deviceTable.vkDestroySemaphore == NULL) {
            return XUD_UNSUPPORTED;
        }
        loader->deviceTable.vkDestroySemaphore(device->device, victim->imageAcquired[index - 1u], NULL);
        victim->imageAcquired[index - 1u] = VK_NULL_HANDLE;
    }
    for (index = victim->frameFenceCount; index > 0u; index--) {
        if (victim->frameFences[index - 1u] == VK_NULL_HANDLE) {
            continue;
        }
        if (loader->deviceTable.vkDestroyFence == NULL) {
            return XUD_UNSUPPORTED;
        }
        loader->deviceTable.vkDestroyFence(device->device, victim->frameFences[index - 1u], NULL);
        victim->frameFences[index - 1u] = VK_NULL_HANDLE;
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

    for (index = 0u; index < XUD_FRAME_IN_FLIGHT_COUNT; index++) {
        victim->inFlight[index] = 0u;
    }
    victim->currentFrame = 0u;
    victim->frameFenceCount = 0u;
    victim->presentedSwapchain = VK_NULL_HANDLE;
    victim->recreateWanted = 0u;
    free(victim);
    *ioFrame = NULL;

    return XUD_OK;
}
