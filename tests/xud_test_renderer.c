/***********************************************************************************************
 *
 *   xud_test_renderer.c - the renderer rung's resize: the one rung that acts on the recreate ask
 *   the frame loop leaves behind
 *
 *   Owns : xud_renderer_resize() - the ask (XudFrame.recreateWanted, which only the two swapchain-
 *   boundary codes set), the window's FRAMEBUFFER extent as the answer to that ask (xud_window_fra
 *   mebuffer_size(), never the window's requested extent), and the rebuild that follows: frame clos
 *   ed, framebuffer rows closed, the context's swapchain recreated at that extent, the pass's depth
 *   image recreated at it, the rows reopened against the new images and the new depth view, and the
 *   frame loop reopened with one fence per new image. The half that is testable with no device pres
 *   ent is the half that decides a rebuild may not start - the ask, the count that no longer matches
 *   the swapchain in hand, and the extent that cannot be read - and the half that needs the driver i
 *   s the rebuild itself, run end to end against a completed context below.
 *   Layer: tests. Sees xud_renderer.h (the rung under test), xud_window.h (the extent the ask is an
 *   swered against), xud_core.h (the owner a completed bring-up runs against) and the rungs the rebu
 *   ild touches.
 *
 *   Deterministic: fixed extents, fixed loop counts, no time, no randomness, no filesystem.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud.h" /* Required for: XudResult, XUD_VERSION, the XUD_GPU_TESTS gate */
#include "xud_batch.h" /* Required for: XudBatch, the rung the rebuild does NOT touch and says so */
#include "xud_core.h" /* Required for: XudContext, the owner the GPU half runs a completed bring-up against */
#include "xud_frame.h" /* Required for: XudFrame, the recreate ask and the fence count the rebuild reopens */
#include "xud_renderer.h" /* Required for: XudRenderer, xud_renderer_resize, xud_renderer_draw */
#include "xud_renderpass.h" /* Required for: XudRenderPass, the depth attachment the rebuild recreates */
#include "xud_swapchain.h" /* Required for: XudSwapchain, the context's swapchain slot the rebuild replaces */
#include "xud_test_harness.h" /* Required for: xud_check, xud_gpu_tests_enabled, the area entry points */
#include "xud_window.h" /* Required for: xud_window_resize, xud_window_framebuffer_size, XudWindow */
#include <string.h> /* Required for: memset, the synthetic records this area builds */

/* The extents the GPU half resizes between: the context opens at the first and the window is resize
   d to the second, both well inside the surface's limits on this machine and both distinct, which is
   what makes the framebuffer extent's moving a fact rather than a coincidence. */
#define XUD_TEST_RESIZE_OPEN_EXTENT 256u
#define XUD_TEST_RESIZE_WIDE_EXTENT 400u
#define XUD_TEST_RESIZE_TALL_EXTENT 300u

/*-----------------------------------------------------------------------------------------------
 * The rebuild's guards, headless: every one of them fires before a driver call, and the two that
 * answer with no driver present at all are the ask itself (no ask, no rebuild) and the count that
 * no longer matches the swapchain in hand. The rebuild itself is the GPU half's below.
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_renderer_pure(void) {
    XudBatch batch;
    XudPipeline pipeline;
    XudDescriptor descriptor;
    XudRenderPass pass;
    XudFramebufferArray rows;
    XudFrame frame;
    XudLoader loader;
    XudDevice device;
    XudDeviceCaps caps;
    XudAllocator alloc;
    XudSwapchain swap;
    XudPhysicalDevice physical;
    XudWindow window;
    XudRenderer bare;
    XudRenderer whole;
    XudRenderer* rendererSlot = NULL;
    XudRenderer* wholeSlot = &whole;
    XudRenderer* bareSlot = &bare;
    XudSwapchain* swapHolder = &swap;
    XudSwapchain* swapSlot = NULL;

    memset(&batch, 0, sizeof batch);
    memset(&pipeline, 0, sizeof pipeline);
    memset(&descriptor, 0, sizeof descriptor);
    memset(&pass, 0, sizeof pass);
    memset(&rows, 0, sizeof rows);
    memset(&frame, 0, sizeof frame);
    memset(&loader, 0, sizeof loader);
    memset(&device, 0, sizeof device);
    memset(&caps, 0, sizeof caps);
    memset(&alloc, 0, sizeof alloc);
    memset(&swap, 0, sizeof swap);
    memset(&physical, 0, sizeof physical);
    memset(&window, 0, sizeof window);
    memset(&bare, 0, sizeof bare);
    memset(&whole, 0, sizeof whole);

    /* A renderer whose five rungs are all present, against a loader whose rows are all nil: the
       rebuild's guards have to fire before any of those nil rows is reached, which is the whole
       point of testing this half with no device present. */
    loader.open = true;
    device.device = (VkDevice)0x1100u;
    swap.swapchain = (VkSwapchainKHR)0x1200u;
    swap.imageCount = 2u;
    swap.createInfo.imageExtent.width = 320u;
    swap.createInfo.imageExtent.height = 240u;
    rows.count = 2u;
    whole.batch = &batch;
    whole.pipeline = &pipeline;
    whole.descriptor = &descriptor;
    whole.renderPass = &pass;
    whole.framebuffers = &rows;
    whole.frame = &frame;
    whole.framebufferCount = 2u;

    xud_check(xud_renderer_resize(NULL, &loader, &device, &caps, &alloc, &physical, &window, &swapHolder) == XUD_INVALID_ARG,
        "renderer/resize-null-renderer", "no renderer at all is a bad argument");
    xud_check(xud_renderer_resize(&rendererSlot, &loader, &device, &caps, &alloc, &physical, &window, &swapHolder) == XUD_INVALID_ARG,
        "renderer/resize-empty-renderer-slot", "and so is a slot that owns no renderer");
    xud_check(xud_renderer_resize(&wholeSlot, &loader, &device, &caps, &alloc, &physical, &window, &swapSlot) == XUD_INVALID_ARG,
        "renderer/resize-null-swapchain-slot",
        "a NULL swapchain SLOT is a bad argument, and the slot is an argument because a recreate rep"
        "laces the context's own swapchain rather than this rung's");
    xud_check(xud_renderer_resize(&wholeSlot, NULL, &device, &caps, &alloc, &physical, &window, &swapHolder) == XUD_INVALID_ARG,
        "renderer/resize-null-loader", "a NULL loader is a bad argument, not an unsupported loader");
    xud_check(xud_renderer_resize(&wholeSlot, &loader, NULL, &caps, &alloc, &physical, &window, &swapHolder) == XUD_INVALID_ARG,
        "renderer/resize-null-device", "a NULL device is a bad argument, as is every other rung this rebuild needs");
    xud_check(xud_renderer_resize(&wholeSlot, &loader, &device, NULL, &alloc, &physical, &window, &swapHolder) == XUD_INVALID_ARG,
        "renderer/resize-null-caps", "a NULL caps record is a bad argument, and it is what the allocator's memory-type rule reads");
    xud_check(xud_renderer_resize(&wholeSlot, &loader, &device, &caps, NULL, &physical, &window, &swapHolder) == XUD_INVALID_ARG,
        "renderer/resize-null-allocator",
        "a NULL allocator is a bad argument, and it is the only owner of the memory the new depth image lives in");
    xud_check(xud_renderer_resize(&wholeSlot, &loader, &device, &caps, &alloc, NULL, &window, &swapSlot) == XUD_INVALID_ARG,
        "renderer/resize-null-physical",
        "a NULL physical device is a bad argument, and it is the surface the recreated swapchain is asked about");
    xud_check(xud_renderer_resize(&wholeSlot, &loader, &device, &caps, &alloc, &physical, NULL, &swapSlot) == XUD_INVALID_ARG,
        "renderer/resize-null-window", "a NULL window is a bad argument, and its framebuffer extent IS the new extent being asked for");
    xud_check(xud_renderer_resize(&wholeSlot, &loader, &device, &caps, &alloc, &physical, &window, &swapSlot) == XUD_INVALID_ARG,
        "renderer/resize-empty-swapchain-slot",
        "and so is a slot that owns no swapchain - the rung below that creates one is the call for that");

    xud_check(xud_renderer_resize(&bareSlot, &loader, &device, &caps, &alloc, &physical, &window, &swapHolder) == XUD_INVALID_ARG,
        "renderer/resize-missing-rungs",
        "a renderer missing any rung this rebuilds was never brought up whole, and xud_renderer_open"
        "() is the call for that - a rebuild would otherwise have nothing to rebuild");

    xud_check(xud_renderer_resize(&wholeSlot, &loader, &device, &caps, &alloc, &physical, &window, &swapHolder) == XUD_ALREADY,
        "renderer/resize-no-ask",
        "nothing was asked for: the flag IS the ask, and the two swapchain-boundary codes are the on"
        "ly things that set it, so a draw that answered VK_SUCCESS leaves this rung to be called and "
        "to answer that the extent has not moved - which is what makes calling it every frame cheap "
        "rather than a rebuild every frame");

    frame.recreateWanted = 1u;
    whole.framebufferCount = 3u;
    xud_check(xud_renderer_resize(&wholeSlot, &loader, &device, &caps, &alloc, &physical, &window, &swapHolder) == XUD_UNSUPPORTED,
        "renderer/resize-count-disagrees",
        "the rows and the fences no longer belong to the swapchain in hand - the same mismatch xud_fr"
        "ame_draw() reports one rung down, read here before a rebuild would make it worse");

    whole.framebufferCount = 2u;
    xud_check(xud_renderer_resize(&wholeSlot, &loader, &device, &caps, &alloc, &physical, &window, &swapHolder) == XUD_INVALID_ARG,
        "renderer/resize-no-framebuffer-extent",
        "the ask was answered by a window that has no framebuffer extent to give, which is a bad arg"
        "ument rather than a rebuild at an extent of 0 - and it is the last guard before the rebuild "
        "starts, so nothing has been torn down by the time it answers");
}

/*-----------------------------------------------------------------------------------------------
 * The rebuild against the driver: a completed context, one frame drawn so a presentation exists,
 * the window resized, and the whole rebuild run in the one order that is correct. This machine's
 * compositor never answers the two out-of-band codes (probed today: VK_SUCCESS for every acquire),
 * so the ask is set the way the frame area tampers its own fields - the funnel is what sets it on a
 * machine whose surface moved, and the rebuild is the half that has to work either way.
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_renderer_resize_gpu(void) {
    XudContext* context = NULL;
    XudResult result = XUD_OK;
    uint32_t imageIndex = 0u;
    uint32_t fbWidth = 0u;
    uint32_t fbHeight = 0u;

    result = xud_init_context(&context, XUD_TEST_RESIZE_OPEN_EXTENT, XUD_TEST_RESIZE_OPEN_EXTENT);
    xud_check(result == XUD_OK, "gpu-resize/context-completes", "the ladder has to complete before anything can be resized");
    if (context == NULL) {
        return;
    }
    if ((context->renderer == NULL) || (context->window == NULL)) {
        (void)xud_close_context(&context);
        return;
    }

    xud_check((context->swapchain->createInfo.imageExtent.width == XUD_TEST_RESIZE_OPEN_EXTENT)
            && (context->swapchain->createInfo.imageExtent.height == XUD_TEST_RESIZE_OPEN_EXTENT),
        "gpu-resize/extent-at-open", "the swapchain was created at the extent the context was opened at");
    xud_check((context->renderer->renderPass->depthImageInfo.extent.width == context->swapchain->createInfo.imageExtent.width)
            && (context->renderer->renderPass->depthImageInfo.extent.height == context->swapchain->createInfo.imageExtent.height),
        "gpu-resize/depth-matches-swapchain",
        "and the depth attachment is at that same extent, which is the attachment-extent rule a "
        "framebuffer is created under - the reason a new swapchain extent forces a new depth buffer");
    xud_check(context->renderer->frame->recreateWanted == 0u, "gpu-resize/no-ask-before-the-first-draw",
        "no acquire has answered yet, so the frame loop opens with no rebuild queued");

    result = xud_renderer_draw(context->renderer, context->loader, context->device, context->swapchain, &imageIndex);
    xud_check(result == XUD_OK, "gpu-resize/first-frame-draws",
        "a frame has to have been drawn, and presented, before there is anything for the rebuild's "
        "fence guard to wait for");
    xud_check(context->renderer->frame->lastFrameFence == VK_NULL_HANDLE, "gpu-resize/nothing-in-flight-after-first",
        "and the frame waited and reset its own fence, so the guard below has nothing left to wait");

    xud_check(xud_renderer_resize(&context->renderer, context->loader, context->device, &context->physicalDevice->caps, context->allocator,
                  context->physicalDevice, context->window, &context->swapchain)
            == XUD_ALREADY,
        "gpu-resize/no-ask-no-rebuild",
        "with no ask the rung refuses to rebuild, and it refuses BEFORE it tears anything down: the "
        "swapchain, the rows and the fences are still the ones the draw above used");
    xud_check(context->swapchain->createInfo.imageExtent.width == XUD_TEST_RESIZE_OPEN_EXTENT, "gpu-resize/refusal-changed-nothing",
        "and the refusal left the swapchain at the extent it was created at");

    /* The ask, set the way the frame area tampers its own fields: on this machine the compositor ans
       wers VK_SUCCESS for every acquire, so the funnel never sets the flag here. What is under test
       is the rebuild, and the flag is the only input to it. */
    context->renderer->frame->recreateWanted = 1u;

    xud_check(xud_window_framebuffer_size(&fbWidth, &fbHeight, context->window) == XUD_OK, "gpu-resize/fb-size-before",
        "the window's framebuffer extent is readable before the resize");
    xud_check(xud_window_resize(context->window, XUD_TEST_RESIZE_WIDE_EXTENT, XUD_TEST_RESIZE_TALL_EXTENT) == XUD_OK,
        "gpu-resize/window-resized", "GLFW took the new extent");
    xud_check((context->window->width == XUD_TEST_RESIZE_WIDE_EXTENT) && (context->window->height == XUD_TEST_RESIZE_TALL_EXTENT),
        "gpu-resize/window-recorded", "the window's requested extent is the extent asked for");
    xud_check(xud_window_framebuffer_size(&fbWidth, &fbHeight, context->window) == XUD_OK, "gpu-resize/fb-size-after",
        "and so is the framebuffer extent after it");
    xud_check((fbWidth != XUD_TEST_RESIZE_OPEN_EXTENT) || (fbHeight != XUD_TEST_RESIZE_OPEN_EXTENT), "gpu-resize/fb-moved",
        "the framebuffer extent moved with the window, which is the extent the rebuild is asked for "
        "- never the window's requested one, which on a scaled session is a different pair and a pa"
        "ir a swapchain at it would be called out of date on every acquire");

    result = xud_renderer_resize(&context->renderer, context->loader, context->device, &context->physicalDevice->caps, context->allocator,
        context->physicalDevice, context->window, &context->swapchain);
    xud_check(result == XUD_OK, "gpu-resize/rebuild-completes",
        "the whole rebuild ran in the one order that is correct: frame closed, rows closed, swapchai"
        "n recreated, depth recreated, rows reopened, frame reopened");

    xud_check(
        (context->swapchain->createInfo.imageExtent.width == fbWidth) && (context->swapchain->createInfo.imageExtent.height == fbHeight),
        "gpu-resize/swapchain-at-framebuffer-extent",
        "the context's swapchain is now at the window's FRAMEBUFFER extent, and the slot the rung wa"
        "s handed is what made the replacement visible above it");
    xud_check(context->renderer->renderPass->depthImageInfo.extent.width == fbWidth, "gpu-resize/depth-at-new-extent",
        "the depth attachment was recreated at the new extent, which is the one attachment a swapch"
        "ain recreate forces");
    xud_check((context->renderer->renderPass->depthImage != VK_NULL_HANDLE) && (context->renderer->renderPass->depthView != VK_NULL_HANDLE),
        "gpu-resize/depth-row-live", "and the new image and its one view are both live");
    xud_check(context->renderer->renderPass->depthRange.size > 0u, "gpu-resize/depth-range-live",
        "and the new row's carve of allocator memory is live, so the old one was released and the n"
        "ew one was carved rather than the old one being left dangling");
    xud_check((context->renderer->framebuffers->count == context->swapchain->imageCount)
            && (context->renderer->frame->imageFenceCount == context->swapchain->imageCount)
            && (context->renderer->framebufferCount == context->swapchain->imageCount),
        "gpu-resize/arrays-match-new-swapchain",
        "the rows, the fences and the count the draw hands down all match the NEW swapchain, which i"
        "s the pair the draw compares against the swapchain in hand");
    xud_check(context->renderer->framebuffers->rows[0u]->createInfo.width == fbWidth, "gpu-resize/row-at-new-extent",
        "a reopened row was built at the new extent rather than at the old one");
    xud_check(context->renderer->framebuffers->rows[0u]->attachmentViews[XUD_RENDERPASS_DEPTH_ATTACHMENT]
            == context->renderer->renderPass->depthView,
        "gpu-resize/rows-name-new-depth",
        "the reopened rows name the NEW depth view rather than the retired one, which is the only r"
        "eason the old view could be destroyed at all: the rows that named it died first");
    xud_check(context->renderer->frame->presentedSwapchain == context->swapchain->swapchain, "gpu-resize/frame-names-new-swapchain",
        "the reopened frame loop names the new swapchain rather than the one it was opened against");
    xud_check(context->renderer->frame->recreateWanted == 0u, "gpu-resize/ask-cleared",
        "the ask has been acted on, so the flag is cleared: leaving it set would queue a second rebu"
        "ild behind a swapchain that is already at the right extent");
    xud_check(context->renderer->frame->framesDrawn == 0u, "gpu-resize/reopened-loop-starts-fresh",
        "the reopened frame loop is a new one and its count starts at 0, so the rebuild drew nothing "
        "of its own - the count the draw above incremented belongs to the frame loop that was clos"
        "ed as part of the rebuild");
    xud_check((context->renderer->batch != NULL) && (context->renderer->pipeline != NULL) && (context->renderer->descriptor != NULL),
        "gpu-resize/untouched-rungs-still-live",
        "the batch, the pipeline and the descriptor were not touched: none of them names an extent, s"
        "o a resize that rebuilt them would be more work than the resize needs");

    result = xud_renderer_draw(context->renderer, context->loader, context->device, context->swapchain, &imageIndex);
    xud_check(result == XUD_OK, "gpu-resize/frame-at-new-extent",
        "a frame draws at the new extent, which is what says the new rows, the new fences and the n"
        "ew depth buffer are a set the frame loop can actually run against rather than a set that h"
        "appened to survive the rebuild");
    xud_check(imageIndex < context->swapchain->imageCount, "gpu-resize/index-in-new-range",
        "and the index the new swapchain's acquire wrote names an image it has");

    xud_check(xud_renderer_resize(&context->renderer, context->loader, context->device, &context->physicalDevice->caps, context->allocator,
                  context->physicalDevice, context->window, &context->swapchain)
            == XUD_ALREADY,
        "gpu-resize/second-call-already",
        "a second call answers that nothing was asked for, so one ask cannot turn into a loop of re"
        "builds");

    if (context->validate != 0u) {
        xud_check(context->diagnostics->errorMessages == 0u, "gpu-resize/validation-clean",
            "the rebuild and the frame after it are validation-clean, not just crash-free: the old "
            "rows died before the old images were retired by the recreate, the old depth view died b"
            "efore the old depth image, and no handle survived its destroy");
    }

    result = xud_close_context(&context);
    xud_check(result == XUD_OK, "gpu-resize/close-clean", "and the resized context tears down clean");
    xud_check(context == NULL, "gpu-resize/context-null", "and cleared");
}

void xud_test_renderer(void) {
    xud_test_renderer_pure();
    if (xud_gpu_tests_enabled()) {
        xud_test_renderer_resize_gpu();
    }
}
