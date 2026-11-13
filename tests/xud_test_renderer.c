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
 *   This area also owns the background: the byte the renderer holds at open (the render pass's own
 *   default, quantized to the 8-bit grid), the public xud_clear_background() that replaces it, and
 *   the write into the pass's clear value the frame opening makes before it records.
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
    XudRenderPass hdrPass;
    XudFramebuffer hdrFb;
    XudRenderPass prepassPass;
    XudFramebuffer prepassFb;
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
    XudContext shell;
    XudColor red = (XudColor) { 200u, 30u, 10u, 255u };
    XudColor blue = (XudColor) { 10u, 20u, 250u, 128u };

    /* The recreate decision, the half of the resize step that needs no window at all: the two
       answers a rebuild may not start for are the window having no presentable extent (the 0/0 a
       Wayland compositor reports for a minimized window) and the swapchain already being at the
       framebuffer's extent. */
    xud_check(xud_renderer_recreate_needed(0u, 0u, 320u, 240u) == false, "renderer/recreate-nothing-to-build",
        "a 0/0 framebuffer is a window with no presentable extent, and there is no extent to build a"
        " swapchain at, so the ask is a skip rather than a rebuild");
    xud_check(xud_renderer_recreate_needed(0u, 120u, 320u, 240u) == false, "renderer/recreate-zero-width",
        "and so is a width of 0 with a height the window still has - a swapchain's extent needs both");
    xud_check(
        xud_renderer_recreate_needed(120u, 0u, 320u, 240u) == false, "renderer/recreate-zero-height", "and so is the other axis at 0");
    xud_check(xud_renderer_recreate_needed(320u, 240u, 320u, 240u) == false, "renderer/recreate-already-there",
        "a swapchain at the framebuffer's extent is the state the ask wanted - a rebuild would retire"
        " working images for nothing");
    xud_check(xud_renderer_recreate_needed(400u, 300u, 320u, 240u) == true, "renderer/recreate-moved", "an extent that moved is a rebuild");
    xud_check(xud_renderer_recreate_needed(321u, 240u, 320u, 240u) == true, "renderer/recreate-one-pixel",
        "and so is one pixel of it - a swapchain at any other extent is one the compositor calls out"
        " of date on every acquire");

    memset(&batch, 0, sizeof batch);
    memset(&pipeline, 0, sizeof pipeline);
    memset(&descriptor, 0, sizeof descriptor);
    memset(&pass, 0, sizeof pass);
    memset(&hdrPass, 0, sizeof hdrPass);
    memset(&hdrFb, 0, sizeof hdrFb);
    memset(&prepassPass, 0, sizeof prepassPass);
    memset(&prepassFb, 0, sizeof prepassFb);
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
    memset(&shell, 0, sizeof shell);

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
    whole.hdrPass = &hdrPass;
    whole.hdrFramebuffer = &hdrFb;
    whole.prepassPass = &prepassPass;
    whole.hdrPrepassFramebuffer = &prepassFb;
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

    /* --- the background's public half, headless: the storage is the renderer's, so a shell
         context with a zeroed renderer is the one that has no owner to store in, and `whole` is
         the one that does --- */
    xud_check(xud_clear_background(NULL, red) == XUD_INVALID_ARG, "renderer/background-null-context",
        "no context is a bad argument, the same answer the frame pair above gives");
    xud_check(xud_clear_background(&shell, red) == XUD_UNSUPPORTED, "renderer/background-no-renderer",
        "a context with no renderer never completed bring-up, and there is no owner to store the "
        "colour in");
    shell.renderer = &whole;
    xud_check(xud_clear_background(&shell, red) == XUD_OK, "renderer/background-stored", "a live renderer takes the colour");
    xud_check(
        (whole.background.r == red.r) && (whole.background.g == red.g) && (whole.background.b == red.b) && (whole.background.a == red.a),
        "renderer/background-as-given",
        "the value is the renderer's own record, copied AS GIVEN rather than converted: the "
        "byte-to-float half is the draw entry's, at the frame it applies to");
    xud_check(xud_clear_background(&shell, blue) == XUD_OK, "renderer/background-replaced", "and a second call replaces the whole record");
    xud_check((whole.background.r == blue.r) && (whole.background.g == blue.g) && (whole.background.b == blue.b)
            && (whole.background.a == blue.a),
        "renderer/background-second-landed",
        "all four channels moved, so the background set in frame N is the one frame N+1's clear "
        "uses - the raylib order for ClearBackground, which changes the background from the next "
        "BeginDrawing on");
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

    result = xud_init_context(&context, XUD_TEST_RESIZE_OPEN_EXTENT, XUD_TEST_RESIZE_OPEN_EXTENT, "xud_test");
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
    xud_check(context->renderer->frame->inFlight[0u] == 1u, "gpu-resize/first-frame-in-flight",
        "the pipelined loop does not wait its own frame, so the slot the draw used still bounds work - "
        "the resize's frame-close guard is what waits it before anything is torn down");

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
            && (context->renderer->frame->frameFenceCount == XUD_FRAME_IN_FLIGHT_COUNT)
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

    /* The ask-satisfied half of the decision, run against a live window: the flag is set again with
       the swapchain already at the window's framebuffer extent, so the step skips and clears the
       ask without touching a rung - the same answer the 0/0 (minimized) half gives, with the
       difference that here a rebuild that ran would have been visible as a new swapchain. */
    context->renderer->frame->recreateWanted = 1u;
    xud_check(xud_renderer_resize(&context->renderer, context->loader, context->device, &context->physicalDevice->caps, context->allocator,
                  context->physicalDevice, context->window, &context->swapchain)
            == XUD_ALREADY,
        "gpu-resize/ask-satisfied-skip", "an ask the swapchain already satisfies is a skip, not a rebuild");
    xud_check(context->renderer->frame->recreateWanted == 0u, "gpu-resize/satisfied-skip-clears-ask",
        "and the skip cleared the ask, so the next frame does not ask again");
    xud_check(
        (context->swapchain->createInfo.imageExtent.width == fbWidth) && (context->swapchain->createInfo.imageExtent.height == fbHeight),
        "gpu-resize/satisfied-skip-touched-nothing",
        "and the swapchain is still the one the rebuild above made - a rebuild that ran would have "
        "retired it for an identical one");

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

/*-----------------------------------------------------------------------------------------------
 * The background against the driver: the byte the open gave the renderer, the public setter over
 * a completed context, and the write into the pass's clear value the frame opening makes before
 * it records - read back from the pass rather than from the constant, the rule the render-pass
 * area applies to its own depth clear.
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_background_gpu(void) {
    XudContext* context = NULL;
    XudResult result = XUD_OK;
    XudColor red = (XudColor) { 255u, 0u, 0u, 255u };
    XudColor green = (XudColor) { 0u, 255u, 0u, 255u };

    result = xud_init_context(&context, XUD_TEST_RESIZE_OPEN_EXTENT, XUD_TEST_RESIZE_OPEN_EXTENT, "xud_test");
    xud_check(result == XUD_OK, "gpu-background/context", "a context at the small extent, the resize area's own");
    if (context == NULL) {
        return;
    }

    /* The default, at open: the byte quantization of the render pass's own default clear colour,
       the same expression xud_renderer_open() uses, so the check is against the rule rather than
       a copy of it */
    xud_check((context->renderer->background.r == (uint8_t)(XUD_RENDERPASS_CLEAR_COLOR_R * 255.0f + 0.5f))
            && (context->renderer->background.g == (uint8_t)(XUD_RENDERPASS_CLEAR_COLOR_G * 255.0f + 0.5f))
            && (context->renderer->background.b == (uint8_t)(XUD_RENDERPASS_CLEAR_COLOR_B * 255.0f + 0.5f))
            && (context->renderer->background.a == (uint8_t)(XUD_RENDERPASS_CLEAR_COLOR_A * 255.0f + 0.5f)),
        "gpu-background/default-at-open",
        "the first frame a context draws is cleared with the render pass's own default, quantized "
        "to the byte grid the public XudColor lives on - not with whatever a zeroed struct would "
        "be, a transparent black the surface's OPAQUE composite answer cannot take");

    xud_check(xud_clear_background(context, red) == XUD_OK, "gpu-background/set-ok", "the public setter over a completed context");
    xud_check((context->renderer->background.r == red.r) && (context->renderer->background.g == red.g)
            && (context->renderer->background.b == red.b) && (context->renderer->background.a == red.a),
        "gpu-background/stored", "AS GIVEN, in the renderer that owns the frame loop");

    /* A frame: the draw entry writes the stored background into the pass before it records, and the
       pass's row is what the frame loop hands the driver as pClearValues */
    xud_check(xud_begin_drawing(context) == XUD_OK, "gpu-background/begin-1", "one frame under the new background");
    xud_check(xud_end_drawing(context) == XUD_OK, "gpu-background/end-1", "and its close");
    xud_check((context->renderer->renderPass->clearValues[XUD_RENDERPASS_COLOR_ATTACHMENT].color.float32[0] == 1.0f)
            && (context->renderer->renderPass->clearValues[XUD_RENDERPASS_COLOR_ATTACHMENT].color.float32[1] == 0.0f)
            && (context->renderer->renderPass->clearValues[XUD_RENDERPASS_COLOR_ATTACHMENT].color.float32[2] == 0.0f)
            && (context->renderer->renderPass->clearValues[XUD_RENDERPASS_COLOR_ATTACHMENT].color.float32[3] == 1.0f),
        "gpu-background/clear-value-after-frame",
        "the pass's clear value is the colour the caller asked for, read back from the pass: the "
        "frame the driver just cleared used this row, because passBegin's pClearValues IS the "
        "pass's own row rather than a frame-local copy");
    xud_check(context->renderer->renderPass->clearValues[XUD_RENDERPASS_DEPTH_ATTACHMENT].depthStencil.depth == XUD_RENDERPASS_CLEAR_DEPTH,
        "gpu-background/depth-half-untouched",
        "and the depth half of the clear is the pass's own constant, which a background does not "
        "touch");

    /* A second colour follows the first: the write runs at every frame opening, so the background
       a frame's clear uses is the one live at the frame's own opening */
    xud_check(xud_clear_background(context, green) == XUD_OK, "gpu-background/set-2", "a second background in the same context");
    xud_check(xud_begin_drawing(context) == XUD_OK, "gpu-background/begin-2", "and a frame under it");
    xud_check(xud_end_drawing(context) == XUD_OK, "gpu-background/end-2", "and its close");
    xud_check((context->renderer->renderPass->clearValues[XUD_RENDERPASS_COLOR_ATTACHMENT].color.float32[0] == 0.0f)
            && (context->renderer->renderPass->clearValues[XUD_RENDERPASS_COLOR_ATTACHMENT].color.float32[1] == 1.0f)
            && (context->renderer->renderPass->clearValues[XUD_RENDERPASS_COLOR_ATTACHMENT].color.float32[2] == 0.0f)
            && (context->renderer->renderPass->clearValues[XUD_RENDERPASS_COLOR_ATTACHMENT].color.float32[3] == 1.0f),
        "gpu-background/second-colour-follows",
        "the next frame's clear is the next colour: a background set between two frames is the one "
        "the frame after it clears with, the order raylib's ClearBackground has");

    if (context->validate != 0u) {
        xud_check(context->diagnostics->errorMessages == 0u, "gpu-background/validation-clean",
            "and the two frames are validation-clean, not just crash-free");
    }

    result = xud_close_context(&context);
    xud_check(result == XUD_OK, "gpu-background/close-clean", "and the context tears down clean");
    xud_check(context == NULL, "gpu-background/context-null", "and cleared");
}

void xud_test_renderer(void) {
    xud_test_renderer_pure();
    if (xud_gpu_tests_enabled()) {
        xud_test_renderer_resize_gpu();
        xud_test_background_gpu();
    }
}
