/***********************************************************************************************
 *
 *   xud_test_frame.c - the frame loop and the renderer that draws through it
 *
 *   Owns : the command-pool and command-buffer rows, the submit and present rows, the core-1.0 acquire gate, the per-image layout record
 * and the barrier that makes the next frame's oldLayout a fact - plus the GPU half that draws real frames through the renderer, with a
 * triangle appended to the batch before each of them so the upload / bind / draw path inside the frame
 * loop is the path actually taken rather than the empty-batch path, and refuses the failure shapes of
 * the frame loop against those very live objects. Layer: tests. Sees xud_frame.h, xud_batch.h, xud_renderer.h and xud_core.h: the
 * renderer is the owner of the rungs and xud_renderer_draw() is the one call a caller above makes, so
 * the loop, the batch it consumes and its owner are one area.
 * area.
 *
 *   Deterministic: fixed loop counts, no time, no randomness, no filesystem.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud.h" /* Required for: XudResult, XUD_VERSION, the XUD_GPU_TESTS gate */
#include "xud_batch.h" /* Required for: XudBatch, the batch the frame loop uploads / binds and draws */
#include "xud_core.h" /* Required for: XudContext, the owner the GPU half runs against */
#include "xud_frame.h" /* Required for: XudFrame, the fill rungs, the allocation and the recording rows */
#include "xud_renderer.h" /* Required for: xud_renderer_draw, the one call the GPU half draws through */
#include "xud_test_harness.h" /* Required for: xud_check, xud_gpu_tests_enabled, the area entry points */
#include <stdio.h>

/*-----------------------------------------------------------------------------------------------
 * The frame loop, headless: the four create-info assemblies the driver calls below are written f
 * rom, and the argument guards. The acquire semaphore's wait stage and the all-ones timeout are
 * both load-bearing constants of this rung, which is why the header declares them rather than lea
 * ving the calls to spell them.
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_frame_pure(void) {
    VkCommandPoolCreateInfo pool = (VkCommandPoolCreateInfo) { .sType = VK_STRUCTURE_TYPE_MAX_ENUM };
    VkCommandBufferAllocateInfo alloc = (VkCommandBufferAllocateInfo) { .sType = VK_STRUCTURE_TYPE_MAX_ENUM };
    VkCommandBufferBeginInfo begin = (VkCommandBufferBeginInfo) { .sType = VK_STRUCTURE_TYPE_MAX_ENUM };
    VkViewport viewport = (VkViewport) { 0 };
    VkRect2D scissor = (VkRect2D) { 0 };
    VkSubmitInfo submit = (VkSubmitInfo) { .sType = VK_STRUCTURE_TYPE_MAX_ENUM };
    VkPresentInfoKHR present = (VkPresentInfoKHR) { .sType = VK_STRUCTURE_TYPE_MAX_ENUM };
    const VkCommandPool poolHandle = (VkCommandPool)0x1000u;
    const VkCommandBuffer bufferHandle = (VkCommandBuffer)0x2000u;
    const VkSwapchainKHR swapchainHandle = (VkSwapchainKHR)0x3000u;

    xud_check(xud_frame_fill_pool_info(NULL, 0u) == XUD_INVALID_ARG, "frame/pool-null-out", "refused");
    xud_check(xud_frame_fill_pool_info(&pool, XUD_PHYSICAL_NO_QUEUE_FAMILY) == XUD_INVALID_ARG, "frame/pool-no-family",
        "a pool for a queue family that was never chosen is a bad argument, because the family is "
        "an argument of the create call");
    xud_check(xud_frame_fill_pool_info(&pool, 0u) == XUD_OK, "frame/pool-ok", "the pool create-info is assembled");
    xud_check(pool.sType == VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, "frame/pool-stamped", "stamped");
    xud_check(pool.pNext == NULL, "frame/pool-pnext-clear", "and its chain stays empty");
    xud_check(pool.flags == VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, "frame/pool-reset-flag",
        "the reset bit, which is what lets the buffer be recorded again next frame without recreat"
        "ing the pool - the frame loop records one buffer per frame, forever");
    xud_check(pool.queueFamilyIndex == 0u, "frame/pool-family",
        "the family the physical layer chose is handed over as the argument it was, which is what k"
        "eeps the pool on the same family the device's queues came from");

    xud_check(xud_frame_fill_allocation(NULL, poolHandle) == XUD_INVALID_ARG, "frame/alloc-null-out", "refused");
    xud_check(xud_frame_fill_allocation(&alloc, VK_NULL_HANDLE) == XUD_UNSUPPORTED, "frame/alloc-no-pool",
        "an allocation for no pool is reported as unsupported rather than assembled, because the po"
        "ol handle is the argument the driver takes");
    xud_check(xud_frame_fill_allocation(&alloc, poolHandle) == XUD_OK, "frame/alloc-ok", "the allocation is assembled");
    xud_check(alloc.sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, "frame/alloc-stamped", "stamped");
    xud_check(alloc.commandPool == poolHandle, "frame/alloc-pool-handle", "the pool handle in hand is recorded as the argument it was");
    xud_check(alloc.level == VK_COMMAND_BUFFER_LEVEL_PRIMARY, "frame/alloc-primary-level",
        "a primary buffer, which is the only kind that can be submitted directly - and this tree h"
        "as no secondary buffer anywhere");
    xud_check(alloc.commandBufferCount == XUD_FRAME_COMMAND_BUFFER_COUNT, "frame/alloc-count",
        "ONE buffer, which is the plan's one batch per frame read at the allocation as well as at t"
        "he submit, so the two cannot disagree");

    xud_check(xud_frame_fill_recording(NULL, &viewport, &scissor, 320u, 240u) == XUD_INVALID_ARG, "frame/record-null-begin", "refused");
    xud_check(xud_frame_fill_recording(&begin, NULL, &scissor, 320u, 240u) == XUD_INVALID_ARG, "frame/record-null-viewport",
        "and so is a viewport that cannot be written");
    xud_check(xud_frame_fill_recording(&begin, &viewport, NULL, 320u, 240u) == XUD_INVALID_ARG, "frame/record-null-scissor",
        "and so is a scissor that cannot be written");
    xud_check(xud_frame_fill_recording(&begin, &viewport, &scissor, 0u, 240u) == XUD_INVALID_ARG, "frame/record-zero-extent",
        "and so is a zero extent");
    xud_check(xud_frame_fill_recording(&begin, &viewport, &scissor, 320u, 240u) == XUD_OK, "frame/record-ok",
        "the recording create-info is assembled");
    xud_check(begin.sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, "frame/record-stamped", "stamped");
    xud_check(begin.flags == VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, "frame/record-one-time",
        "ONE_TIME_SUBMIT, which is what makes the buffer re-recorded every frame rather than reall"
        "ed - the pool's reset bit above is the other half of that pair");
    xud_check(begin.pInheritanceInfo == NULL, "frame/record-no-inheritance",
        "no inheritance info, which is the spec's rule for a ONE_TIME_SUBMIT primary buffer: a non"
        "-NULL one would name a render pass this rung has not begun yet");
    xud_check((viewport.x == 0.0f) && (viewport.y == 0.0f), "frame/viewport-origin", "the viewport starts at the origin");
    xud_check((viewport.width == 320.0f) && (viewport.height == 240.0f), "frame/viewport-extent",
        "the full extent, which is what the pipeline's dynamic VIEWPORT state is set to every frame"
        " - and the swapchain's extent, not the surface's, whose currentExtent is the undefined ans"
        "wer here");
    xud_check((viewport.minDepth == 0.0f) && (viewport.maxDepth == 1.0f), "frame/viewport-depth-range",
        "the full depth range, written even though this rung has no depth attachment, because the f"
        "ield is a value the driver reads");
    xud_check((scissor.offset.x == 0) && (scissor.offset.y == 0), "frame/scissor-origin", "the scissor starts at the origin");
    xud_check((scissor.extent.width == 320u) && (scissor.extent.height == 240u), "frame/scissor-extent",
        "the scissor is the full extent, so nothing outside the image is written - a scissor bigge"
        "r than the image is what the spec forbids, not allows");

    xud_check(xud_frame_fill_submit(NULL, &present, bufferHandle, swapchainHandle) == XUD_INVALID_ARG, "frame/submit-null-out", "refused");
    xud_check(xud_frame_fill_submit(&submit, NULL, bufferHandle, swapchainHandle) == XUD_INVALID_ARG, "frame/present-null-out",
        "and so is the present row that cannot be written");
    xud_check(xud_frame_fill_submit(&submit, &present, VK_NULL_HANDLE, swapchainHandle) == XUD_INVALID_ARG, "frame/submit-no-buffer",
        "a submit with nothing to execute is refused");
    xud_check(xud_frame_fill_submit(&submit, &present, bufferHandle, VK_NULL_HANDLE) == XUD_INVALID_ARG, "frame/present-no-swapchain",
        "and so is a present of no swapchain");
    xud_check(
        xud_frame_fill_submit(&submit, &present, bufferHandle, swapchainHandle) == XUD_OK, "frame/submit-ok", "the two rows are assembled");
    xud_check((submit.sType == VK_STRUCTURE_TYPE_SUBMIT_INFO) && (present.sType == VK_STRUCTURE_TYPE_PRESENT_INFO_KHR),
        "frame/submit-two-stamped", "both rows are stamped, and the two stamps are two different constants, so no caller can confuse them");
    xud_check((submit.pNext == NULL) && (present.pNext == NULL), "frame/submit-chains-clear", "and both chains stay empty");
    xud_check(submit.waitSemaphoreCount == 1u, "frame/submit-wait-count",
        "one wait, which is the acquire semaphore: the image may not be written until it has been "
        "acquired");
    xud_check((submit.pWaitSemaphores == NULL) && (submit.pWaitDstStageMask == NULL), "frame/submit-wait-rows-null",
        "both rows beside that count are left empty by the pure half, which has no semaphore handle "
        "in hand; the driver half points them at the frame's own semaphore and its own stage row, w"
        "hich is the storage that outlives the call");
    xud_check((submit.commandBufferCount == XUD_FRAME_COMMAND_BUFFER_COUNT) && (submit.pCommandBuffers == NULL), "frame/submit-command-row",
        "one buffer, and the row itself is the driver half's, because the buffer is what the drive"
        "r allocated");
    xud_check((submit.signalSemaphoreCount == 0u) && (submit.pSignalSemaphores == NULL), "frame/submit-no-signal",
        "no signal semaphore, because the fence is what says a frame finished, and a second mecha"
        "nism for the same boundary would be two owners of one fact");
    xud_check(present.swapchainCount == 1u, "frame/present-swapchain-count", "one swapchain presented");
    xud_check((present.waitSemaphoreCount == 0u) && (present.pWaitSemaphores == NULL), "frame/present-no-wait",
        "the presentation waits nothing: the fence at the end of the frame is what bounds it, and a "
        "wait semaphore here would be a second channel for the same boundary");
    xud_check((present.pSwapchains == NULL) && (present.pImageIndices == NULL), "frame/present-rows-null",
        "the swapchain row and the image-index row are both the driver half's: the index is what vkA"
        "cquireNextImageKHR WRITES, so the pure half has no index to point at, which is what makes "
        "the row a driver-half detail rather than a constant this half could invent");
    xud_check(present.pResults == NULL, "frame/present-no-results",
        "the per-swapchain VkResult out-array is refused here rather than asked for: the rung's own "
        "return code is the report, and a pResults array would be a second channel for the same ans"
        "wer that a caller above would have to read");

    /* The push row of one draw, in both material shapes: the batch's sentinel and a real slot, bec"
       ause the sentinel mapping is the branch this rung owns and the only one a synthetic row can ex"
       ercise. The two rows are also the two variants, which is the flags member's whole point. */
    {
        XudPushRow pushRow;
        const XudBatchDraw sentinelRow = { 0u, 3u, XUD_BATCH_VARIANT_OPAQUE, XUD_BATCH_NO_MATERIAL };
        const XudBatchDraw realRow = { 12u, 6u, XUD_BATCH_VARIANT_TRANSPARENT, 3u };
        uint32_t axis = 0u;
        uint32_t offDiagonal = 0u;

        xud_check(xud_frame_fill_push_row(NULL, &sentinelRow) == XUD_INVALID_ARG, "frame/push-null-out", "refused");
        xud_check(xud_frame_fill_push_row(&pushRow, NULL) == XUD_INVALID_ARG, "frame/push-null-draw",
            "and so is a draw with no row behind it - the row's variant and material slot are what th"
            "e push carries, so there is nothing to carry without one");

        xud_check(xud_frame_fill_push_row(&pushRow, &sentinelRow) == XUD_OK, "frame/push-sentinel-ok", "the row is assembled");
        xud_check((pushRow.modelMatrix[0] == 1.0f) && (pushRow.modelMatrix[5] == 1.0f) && (pushRow.modelMatrix[10] == 1.0f)
                && (pushRow.modelMatrix[15] == 1.0f),
            "frame/push-identity-diagonal",
            "the model matrix is the identity, which is the same honest answer the block's viewProj g"
            "ives: a non-identity matrix is a transform the step above has to choose");
        for (axis = 0u; axis < 16u; axis++) {
            if ((axis % 5u) != 0u) {
                offDiagonal += (pushRow.modelMatrix[axis] == 0.0f) ? 1u : 0u;
            }
        }
        xud_check(offDiagonal == 12u, "frame/push-identity-off-diagonal",
            "and every off-diagonal element is zero, so the matrix is the identity rather than a matr"
            "ix that happens to have ones on its diagonal");
        xud_check(pushRow.materialIndex == 0u, "frame/push-sentinel-mapped",
            "the batch's 0xFFFFFFFF sentinel is mapped to slot 0, because the push row's member is a"
            "n index a read could use and the sentinel indexes nothing");
        xud_check((pushRow.textureIndex == 0u) && (pushRow.flags == XUD_BATCH_VARIANT_OPAQUE) && (pushRow.reserved == 0u),
            "frame/push-other-members",
            "texture 0 (set 1 is declared and never allocated, so there is no sampler array with an e"
            "ntry in it), flags the variant the draw was flushed under, and the padding byte-zeroed");

        xud_check(xud_frame_fill_push_row(&pushRow, &realRow) == XUD_OK, "frame/push-real-ok", "and so for a real material slot");
        xud_check((pushRow.materialIndex == 3u) && (pushRow.flags == XUD_BATCH_VARIANT_TRANSPARENT), "frame/push-real-passthrough",
            "a slot that is not the sentinel is passed through unchanged, and the variant follows the"
            "row - the per-draw channel carries the draw's own facts, not this rung's");
        xud_check(pushRow.modelMatrix[0] == 1.0f, "frame/push-identity-again",
            "and the model matrix is the identity for every row, because the matrix is not a per-draw"
            "fact until the transform step above makes it one");
    }
}

/* How many frames xud_test_gpu_draw() draws. More than one, because the point of drawing several i
   s to exercise a REUSED command buffer, a reused acquire semaphore and a fence that has been waite
   d and reset rather than a first-use path, and the swapchain here has at least two images, so the
   indices acquire hands back are expected to move. */
#define XUD_TEST_GPU_DRAW_FRAMES 4
/*-----------------------------------------------------------------------------------------------
 * GPU-gated: the first triangle end to end. A completed context carries the renderer rung, and the
 * renderer is the only thing a caller above names - no pipeline, no render pass, no framebuffer ro
 * w, no fence, no command buffer appears in this test's calls at all, which is the point of the ru
 * ng. What a draw can answer is checked rather than assumed: the image index acquire wrote, the co
 * unter that says the loop ran more than once (a reused command buffer and a reused acquire semaph
 * ore are only exercised if it did), the swapchain the presentation named, and the fact that the l
 * ast frame's fence is back in the not-signalled state. The validation-clean rule is read again aft
 * er the frames, because a draw is where a validation complaint about a render pass, a pipeline or
 * a present would land.
 *-----------------------------------------------------------------------------------------------*/
static void xud_test_gpu_draw(void) {
    XudContext* context = NULL;
    XudResult result = XUD_OK;
    uint32_t frame = 0u;
    uint32_t previousIndex = 0u;
    uint32_t probeIndex = 0u;
    bool indexMoved = false;
    /* The triangle appended to the batch before every frame: fixed coordinates, a fixed colour and a
       fixed normal, so the run is deterministic and the validation layer sees the same geometry every
       run. The three positions are distinct and inside the clip volume, so the draw is a draw of a
       triangle rather than a draw of a degenerate one at the origin.
    */
    const float trianglePos[3][3] = { { -0.5f, -0.5f, 0.0f }, { 0.5f, -0.5f, 0.0f }, { 0.0f, 0.5f, 0.0f } };
    const float triangleUv[3][2] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 0.5f, 1.0f } };
    const uint8_t triangleColor[4] = { 255u, 255u, 255u, 255u };
    const float triangleNormal[3] = { 0.0f, 0.0f, 1.0f };

    result = xud_init_context(&context, 256u, 256u);
    xud_check(result == XUD_OK, "gpu-draw/context-completes", "the ladder has to complete before anything can be drawn");
    if (context == NULL)
        return;

    xud_check(context->renderer != NULL, "gpu-draw/renderer-owned",
        "a completed context owns the renderer rung, so the first-triangle path is wired");
    if (context->renderer == NULL) {
        (void)xud_close_context(&context);
        return;
    }

    xud_check((context->renderer->pipeline != NULL) && (context->renderer->renderPass != NULL) && (context->renderer->framebuffers != NULL)
            && (context->renderer->frame != NULL),
        "gpu-draw/four-rungs-live", "the renderer owns all four rungs, none of them a sentinel");
    xud_check((context->renderer->pipeline->pipeline[XUD_BATCH_VARIANT_OPAQUE] != VK_NULL_HANDLE)
            && (context->renderer->pipeline->pipeline[XUD_BATCH_VARIANT_TRANSPARENT] != VK_NULL_HANDLE)
            && (context->renderer->renderPass->renderPass != VK_NULL_HANDLE),
        "gpu-draw/handles-live", "the VkPipeline and the VkRenderPass inside the two rungs are live handles");
    xud_check(context->renderer->framebufferCount == context->swapchain->imageCount, "gpu-draw/count-is-image-count",
        "the renderer recorded one framebuffer per swapchain image, and the count it hands the draw matches");
    xud_check((context->renderer->framebuffers->count == context->swapchain->imageCount)
            && (context->renderer->frame->imageFenceCount == context->swapchain->imageCount),
        "gpu-draw/arrays-match-swapchain",
        "the framebuffer array and the fence array both match the swapchain, which is the pair the "
        "draw compares against the swapchain in hand");
    xud_check((context->renderer->frame->commandPool != VK_NULL_HANDLE) && (context->renderer->frame->commandBuffer != VK_NULL_HANDLE)
            && (context->renderer->frame->imageAcquired != VK_NULL_HANDLE),
        "gpu-draw/frame-loop-live", "the command pool, the command buffer and the acquire semaphore are live");
    xud_check(context->renderer->frame->imageFences[0u] != VK_NULL_HANDLE, "gpu-draw/first-fence-live",
        "and so is at least one of the per-image fences");
    xud_check((context->renderer->framebuffers->rows[0u]->framebuffer != VK_NULL_HANDLE)
            && (context->renderer->framebuffers->rows[0u]->view != VK_NULL_HANDLE),
        "gpu-draw/first-row-live", "the first framebuffer row and the image view of the swapchain image it was built for are live");
    xud_check(context->renderer->framebuffers->rows[0u]->createInfo.width == 256u, "gpu-draw/row-extent",
        "the row was built at the context's extent, copied from the swapchain rather than re-queried");
    xud_check(context->renderer->frame->lastFrameFence == VK_NULL_HANDLE, "gpu-draw/nothing-in-flight-before",
        "no frame has been drawn, so no fence is in flight");

    /* The failure shapes of the frame loop, against THIS context's live objects rather than against
       synthetic ones: a synthetic struct could not tell a nil row from a dead handle, and the whole
       point of the guards is that they fire before the acquire, because an acquire that had already
       run would have signalled a semaphore nobody was going to wait. The two state checks below the
       block are what say so - framesDrawn is the counter the loop below is about to move, and last
       FrameFence is the one field a frame that had started would have filled.
    */
    xud_check(xud_frame_draw(context->renderer->frame, context->loader, context->device, context->swapchain, context->renderer->pipeline,
                  context->renderer->renderPass, context->renderer->framebuffers, context->swapchain->imageCount, context->renderer->batch,
                  context->renderer->descriptor, NULL)
            == XUD_INVALID_ARG,
        "gpu-draw/refuse-no-out-index", "a frame with nowhere to write the acquired index is a bad argument");
    xud_check(xud_frame_draw(context->renderer->frame, context->loader, context->device, context->swapchain, NULL,
                  context->renderer->renderPass, context->renderer->framebuffers, context->swapchain->imageCount, context->renderer->batch,
                  context->renderer->descriptor, &probeIndex)
            == XUD_INVALID_ARG,
        "gpu-draw/refuse-no-pipeline", "a frame with no pipeline to bind is a bad argument, not a draw with no pipeline");
    xud_check(xud_frame_draw(context->renderer->frame, context->loader, context->device, context->swapchain, context->renderer->pipeline,
                  NULL, context->renderer->framebuffers, context->swapchain->imageCount, context->renderer->batch,
                  context->renderer->descriptor, &probeIndex)
            == XUD_INVALID_ARG,
        "gpu-draw/refuse-no-pass", "a frame with no render pass to begin is a bad argument");
    xud_check(xud_frame_draw(context->renderer->frame, context->loader, context->device, context->swapchain, context->renderer->pipeline,
                  context->renderer->renderPass, NULL, context->swapchain->imageCount, context->renderer->batch,
                  context->renderer->descriptor, &probeIndex)
            == XUD_INVALID_ARG,
        "gpu-draw/refuse-no-rows", "a frame with no framebuffer rows has no attachment to draw into");
    xud_check(xud_frame_draw(context->renderer->frame, context->loader, context->device, context->swapchain, context->renderer->pipeline,
                  context->renderer->renderPass, context->renderer->framebuffers, 0u, context->renderer->batch,
                  context->renderer->descriptor, &probeIndex)
            == XUD_INVALID_ARG,
        "gpu-draw/refuse-zero-count", "a frame told it has no rows to attach is a bad argument");
    xud_check(xud_frame_draw(context->renderer->frame, context->loader, context->device, context->swapchain, context->renderer->pipeline,
                  context->renderer->renderPass, context->renderer->framebuffers, context->swapchain->imageCount + 1u,
                  context->renderer->batch, context->renderer->descriptor, &probeIndex)
            == XUD_UNSUPPORTED,
        "gpu-draw/refuse-count-disagrees",
        "a count that is not the swapchain's is a frame this rung "
        "cannot run, and XUD_UNSUPPORTED is the code for a rung that cannot, not for a bad argument");
    xud_check(xud_renderer_draw(NULL, context->loader, context->device, context->swapchain, &probeIndex) == XUD_INVALID_ARG,
        "gpu-draw/refuse-no-renderer", "no renderer means no frame at all");
    xud_check(xud_renderer_draw(context->renderer, context->loader, context->device, context->swapchain, NULL) == XUD_INVALID_ARG,
        "gpu-draw/refuse-renderer-no-out-index", "a renderer with nowhere to write the index is a bad argument");

    xud_check(context->renderer->frame->framesDrawn == 0u, "gpu-draw/refusals-drew-nothing",
        "not one of the refusals above acquired, recorded or submitted - the counter is still zero, "
        "which is the whole point of checking the rows and their owners before the acquire");
    xud_check(context->renderer->frame->lastFrameFence == VK_NULL_HANDLE, "gpu-draw/refusals-in-flight-still-none",
        "and no fence was signalled, so a refused frame leaves nothing in flight to be waited");

    /* The batch the frame loop is about to consume, read before the first frame reaches it: the loop
       names the batch's six buffers in a copy, a vertex bind and an index bind, so a nil batch or a
       nil buffer in it is a frame recording reads of nothing. The renderer opened the batch before it
       opened the pipeline, so the buffers exist by the time a frame can be drawn at all.
    */
    xud_check(
        context->renderer->batch != NULL, "gpu-draw/batch-present", "the renderer owns a batch, which is what the frame loop consumes");
    xud_check(context->renderer->batch->indexBuffer != VK_NULL_HANDLE, "gpu-draw/index-buffer-live",
        "the batch's index buffer is a real VkBuffer, which is what vkCmdBindIndexBuffer names");
    xud_check(xud_batch_check_invariant(context->renderer->batch) == XUD_OK, "gpu-draw/batch-invariant",
        "the batch's own invariant holds on the renderer's batch, so the frame loop is consuming a batch that is well formed");

    /* The descriptor the frame loop is about to update, read before the first frame reaches it: the
       loop writes the block into the arena, copies it into the buffer and binds the set, so a nil
       set or a nil buffer in the pair is a frame that binds nothing. The arena is mapped for the
       descriptor's whole life, so the flag is set long before any frame and stays set through every
       one of them - the check below the loop says the frame loop unmapped nothing.
    */
    xud_check((context->renderer->descriptor->sets[XUD_DESCRIPTOR_SET_PER_FRAME] != VK_NULL_HANDLE)
            && (context->renderer->descriptor->uniformBuffer != VK_NULL_HANDLE),
        "gpu-draw/set-and-block-live", "set 0 and the uniform buffer inside it are both live, which is what the bind and the copy name");
    xud_check((context->renderer->descriptor->uniformStagingBuffer != VK_NULL_HANDLE)
            && (context->renderer->descriptor->uniformStagingMapped != NULL)
            && (context->renderer->descriptor->uniformStagingMappedLive == 1u),
        "gpu-draw/arena-mapped-open",
        "the staging arena the frame loop writes into is a real buffer and is mapped for the descriptor's life");
    xud_check(context->renderer->descriptor->uniformBlock.viewProj[0u] == 1.0f, "gpu-draw/block-identity-before",
        "the block the first frame uploads is the identity the context's camera left in it");
    xud_check(xud_frame_draw(context->renderer->frame, context->loader, context->device, context->swapchain, context->renderer->pipeline,
                  context->renderer->renderPass, context->renderer->framebuffers, context->swapchain->imageCount, NULL,
                  context->renderer->descriptor, &probeIndex)
            == XUD_INVALID_ARG,
        "gpu-draw/refuse-no-batch",
        "a frame with no batch to upload, bind or draw is a bad argument rather than a frame that clears and presents a whole screen of "
        "nothing");
    xud_check(xud_frame_draw(context->renderer->frame, context->loader, context->device, context->swapchain, context->renderer->pipeline,
                  context->renderer->renderPass, context->renderer->framebuffers, context->swapchain->imageCount, context->renderer->batch,
                  NULL, &probeIndex)
            == XUD_INVALID_ARG,
        "gpu-draw/refuse-no-descriptor",
        "a frame with no descriptor has no set 0 to upload and no set to bind, which is a bad argument "
        "rather than a draw of a pipeline whose block was never written");
    xud_check((context->renderer->descriptor->pushRange.size == XUD_DESCRIPTOR_PUSH_SIZE)
            && (context->renderer->descriptor->pushRange.offset == 0u)
            && (context->renderer->descriptor->pushRange.stageFlags == XUD_DESCRIPTOR_PUSH_STAGES),
        "gpu-draw/push-range-recorded",
        "the layout's push range is a recorded row rather than a discarded one, and it is the row the "
        "frame loop pushes against - so the per-draw channel has an extent, an offset and two stages be"
        "hind it rather than a call that names constants of the loop's own");

    for (frame = 0u; frame < XUD_TEST_GPU_DRAW_FRAMES; frame++) {
        uint32_t imageIndex = 0u;
        /* One triangle appended to the batch before the frame is drawn, which is what makes the
           upload / bind / draw path inside the frame loop the path this run actually takes: with an
           empty batch the loop would clear and present and never reach a copy, a bind or a draw at
           all, and a green run of that loop would say nothing about the batch being consumed. The
           batch is reset at the end of every frame, so each frame appends its own three vertices
           into a batch that starts empty again - which is also what says the reset happens.
        */
        /* The variant this frame's run is accumulated under: the LAST frame of the loop runs the
           TRANSPARENT row of the pipeline list and the others the opaque default, which is what makes
           the frame loop's per-row pipeline bind the path this run takes for both rows. The batch's
           currentVariant is the drawing state a public blend-mode call will set (todo.md's D3, 2026-10-26
           - the public spelling is xud_set_blend_mode / xud_end_blend_mode in xud.h, raylib's SetBlendMode/
           EndBlendMode pair, and it lands with the shapes step that wants one), so the field is written
           directly here, the same tamper-and-restore the count-disagrees check below uses.
        */
        context->renderer->batch->currentVariant
            = ((frame + 1u) == XUD_TEST_GPU_DRAW_FRAMES) ? XUD_BATCH_VARIANT_TRANSPARENT : XUD_BATCH_VARIANT_OPAQUE;
        xud_check(xud_batch_append_vertex(context->renderer->batch, trianglePos[0], triangleUv[0], triangleColor, triangleNormal) == XUD_OK,
            "gpu-draw/append-vertex-a", "the batch accepts a vertex within its capacity");
        xud_check(xud_batch_append_vertex(context->renderer->batch, trianglePos[1], triangleUv[1], triangleColor, triangleNormal) == XUD_OK,
            "gpu-draw/append-vertex-b", "and a second");
        xud_check(xud_batch_append_vertex(context->renderer->batch, trianglePos[2], triangleUv[2], triangleColor, triangleNormal) == XUD_OK,
            "gpu-draw/append-vertex-c", "and a third, so the batch has a triangle in it");
        xud_check(xud_batch_append_index(context->renderer->batch, 0u) == XUD_OK, "gpu-draw/append-index-0",
            "the batch accepts an index within its capacity");
        xud_check(xud_batch_append_index(context->renderer->batch, 1u) == XUD_OK, "gpu-draw/append-index-1", "and the next");
        xud_check(xud_batch_append_index(context->renderer->batch, 2u) == XUD_OK, "gpu-draw/append-index-2",
            "and the last, so the index buffer names the three vertices");
        result = xud_renderer_draw(context->renderer, context->loader, context->device, context->swapchain, &imageIndex);
        xud_check(result == XUD_OK, "gpu-draw/frame-draws",
            "acquire, record the batch's copies / binds / draw, submit with the image's own fence, present, wait and reset that fence");
        xud_check(imageIndex < context->swapchain->imageCount, "gpu-draw/index-in-range",
            "the index acquire wrote names an image this swapchain has");
        if ((frame + 1u) == XUD_TEST_GPU_DRAW_FRAMES) {
            xud_check(context->renderer->batch->currentVariant == XUD_BATCH_VARIANT_OPAQUE, "gpu-draw/variant-reset-by-frame",
                "the frame that drew the transparent row reset the batch's variant back to the opaque "
                "default, which is what the batch's own reset rule states and this frame is the only "
                "thing that ran it");
        }
        /* The frame loop reset the batch at its end, so the triangle that was drawn this frame is
           gone from the batch before the next one appends - which is the same property the batch
           area checks of xud_batch_reset on its own, read here on the renderer's batch through the
           frame loop rather than through a direct call.
        */
        xud_check(context->renderer->batch->vertexCount == 0u, "gpu-draw/batch-reset-after-frame",
            "the frame that drew the triangle left the batch with zero live vertices, so the next frame appends into an empty batch");
        xud_check(context->renderer->batch->drawCount == 0u, "gpu-draw/batch-draws-reset",
            "and zero recorded draw rows, so the row the frame replayed is not kept for the next one");
        if (frame > 0u) {
            indexMoved = indexMoved || (imageIndex != previousIndex);
        }
        previousIndex = imageIndex;
    }

    xud_check(indexMoved, "gpu-draw/index-moves",
        "the presented index moved at least once over the loop, which is what says acquire is cycling "
        "through the images rather than handing one constant back");
    xud_check(context->renderer->frame->framesDrawn == XUD_TEST_GPU_DRAW_FRAMES, "gpu-draw/frames-counted",
        "the counter says every frame of the loop completed, so the command buffer and the acquire "
        "semaphore were each reused, which a single draw could not show");
    xud_check(context->renderer->frame->presentedSwapchain == context->swapchain->swapchain, "gpu-draw/presented-own-swapchain",
        "the last present named this context's own swapchain");
    xud_check(context->renderer->frame->lastImageIndex < context->swapchain->imageCount, "gpu-draw/last-index-recorded",
        "the index the last acquire wrote is recorded in the frame loop");
    xud_check(context->renderer->frame->lastFrameFence == VK_NULL_HANDLE, "gpu-draw/nothing-in-flight-after",
        "the last frame waited and reset its fence, so the teardown's fence guard has nothing left to wait");
    xud_check(
        (context->renderer->descriptor->uniformStagingMapped != NULL) && (context->renderer->descriptor->uniformStagingMappedLive == 1u),
        "gpu-draw/arena-still-mapped-after",
        "the frames unmapped nothing, so the arena the loop wrote into is the one mapping the descript"
        "or's own close will unmap - a per-frame map/unmap pair would have left this flag clear");

    /* The renderer's own count-vs-array rule, read on the live renderer: the count it hands the frame
       loop is the one it recorded at open, and a draw asked with a count that is not its array's is
       refused one rung ABOVE xud_frame_draw(). The field is tampered and restored here because this
       is the only way to ask the question at all - a real recreate replaces the swapchain and every
       row with it, which is a resize the context above would have to act on, and the refusal is the
       half that matters without one.
    */
    context->renderer->framebufferCount = context->swapchain->imageCount + 1u;
    xud_check(xud_renderer_draw(context->renderer, context->loader, context->device, context->swapchain, &probeIndex) == XUD_UNSUPPORTED,
        "gpu-draw/renderer-count-disagrees", "a renderer whose recorded count is not its array's refuses the draw");
    xud_check(context->renderer->frame->framesDrawn == XUD_TEST_GPU_DRAW_FRAMES, "gpu-draw/refusal-did-not-draw",
        "and the refused draw added no frame, so the refusal was "
        "made before the frame loop was reached at all");
    context->renderer->framebufferCount = context->swapchain->imageCount;

    /* The two rows of the pipeline list, read on the live renderer: the variant the batch names has a
       pipeline behind it rather than an index into an array of one, and the two rows are two handles,
       which is what makes the variant a pipeline choice rather than a flag on one pipeline. The frame
       loop bound the row the variant names, and the run is validation-clean below - so the blend-
       enabled row is a pipeline a frame actually drew through, not a row of the list nothing reaches.
    */
    xud_check(context->renderer->pipeline->pipeline[XUD_BATCH_VARIANT_TRANSPARENT] != VK_NULL_HANDLE, "gpu-draw/transparent-pipeline-live",
        "the transparent row of the pipeline list is a live handle, so the variant the batch named has "
        "a pipeline behind it rather than an index into an array of one");
    xud_check(context->renderer->pipeline->pipeline[XUD_BATCH_VARIANT_TRANSPARENT]
            != context->renderer->pipeline->pipeline[XUD_BATCH_VARIANT_OPAQUE],
        "gpu-draw/two-pipeline-handles",
        "the two rows of the list are two handles, which is what makes the variant a pipeline choice "
        "rather than a flag on one pipeline");

    if (context->validate != 0u) {
        xud_check(context->diagnostics->errorMessages == 0u, "gpu-draw/validation-clean",
            "the frames drew validation-clean, not just crash-free");
    }

    result = xud_close_context(&context);
    xud_check(result == XUD_OK, "gpu-draw/close-clean",
        "a context that drew frames tears back down with nothing refused, which is the renderer's "
        "close retiring a frame whose presentations have all been waited");
    xud_check(context == NULL, "gpu-draw/close-clears", "and the out-param is cleared");
}

void xud_test_frame(void) {
    xud_test_frame_pure();
    if (xud_gpu_tests_enabled()) {
        xud_test_gpu_draw();
    }
}
