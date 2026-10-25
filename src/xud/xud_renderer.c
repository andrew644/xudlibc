/***********************************************************************************************
 *
 *   xud_renderer.c - the renderer rung: the owner of one pipeline, one render pass, one
 *                    framebuffer per swapchain image and one frame loop
 *
 *   Owns : the four rungs below the public API that make a frame possible - the pipeline rung's
 *          one graphics pipeline (with its layout and its two shader modules inside it), the
 *          render-pass rung's one VkRenderPass, that rung's array of one VkFramebuffer per
 *          swapchain image, and the frame loop's command pool, command buffer, acquire
 *          semaphore and per-image fences - and the one teardown order that hands all four back.
 *   Layer: above the frame loop, below the public API. This is the rung of the plan's layer
 *          ladder ("xudrl renderer (passes, pipelines, batches, buffers)") and the rung xud_init
 *          _context() wires, which is what makes the context's renderer field live rather than a
 *          sentinel. Nothing in this file calls a Vulkan entry point at all: every one of them is
 *          called by the rung that owns it, through a row of XudLoader's Volk tables, so the arch
 *          ive holds no vk* symbol and the link line has no -lvulkan anywhere (AGENTS.md).
 *
 *   The order is load-bearing in both directions, and it is the plan's dependency direction read
 *          both ways. Open: render pass -> pipeline -> framebuffer array -> frame. The pass is first
 *          because the core-1.4 binding makes it an argument of the pipeline's create (VUID-VkGr
 *          aphicsPipelineCreateInfo-dynamicRendering-06576: with dynamicRendering not enabled -
 *          and it is not, see the render-pass decision in AGENTS.md - a graphics pipeline with
 *          pre-rasterization or fragment state MUST name the pass it belongs to, and the validat
 *          ion layer on this machine reports VK_NULL_HANDLE there as a hard error, probed today).
 *          A framebuffer is created FOR a pass too (the spec requires the framebuffer's attachme
 *          nt list to match the pass's descriptions exactly) and a frame records a draw naming a
 *          pipeline, a pass and a framebuffer row, so each of the four needs the ones above it.
 *          Close: the exact reverse - frame first, because a frame is the only thing in this set
 *          that can still have a presentation in flight against the swapchain's images and its
 *          own close is the fence guard that retires it; then the framebuffer array, whose views
 *          name those very images; then the pipeline and then the pass, both of which name that
 *          pass's handle, so it dies last.
 *
 *   What this rung does NOT own, and why: no swapchain (the context owns it, one rung below), no
 *          VkImage of a swapchain image (the swapchain owns its images - xud_swapchain.c's note
 *          about vkDestroyImage), no vertex buffer and no GPU-memory allocator of its own (the
 *          allocator rung IS in the tree now (S12) and is owned by the context rather than by
 *          this rung - which is why the pipeline declares zero vertex
 *          bindings and the draw's whole geometry is a vertex count). The batch, the descriptor
 *          layout and the object kinds a public XudHandle names are steps above this one that
 *          have not landed; this rung is the smallest one that can draw a triangle.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include "xud_renderer.h"
#include "xud.h"
#include "xud_device.h"
#include "xud_frame.h"
#include "xud_loader.h"
#include "xud_pipeline.h"
#include "xud_renderpass.h"
#include "xud_swapchain.h"

#include <assert.h> /* Required for: static_assert, the count that cannot disagree */
#include <stdlib.h> /* Required for: calloc, free */

/* The count xud_frame_draw() is handed is the array's own count, and the field beside it is w
   hat keeps that true: the renderer records the count it opened the array for, and the array re
   cords the count it allocated. */
static_assert(
    sizeof(((XudRenderer*)0)->framebufferCount) == sizeof(uint32_t), "the recorded count is the count type the array's own field uses");

/*-----------------------------------------------------------------------------------------------
 * The one retirement order, shared by the teardown and by every failure path of the bring-up
 *-----------------------------------------------------------------------------------------------*/

static void xud_renderer_retire(XudRenderer* renderer, const XudDevice* device, XudLoader* loader) {
    XudResult rung = XUD_OK;

    if ((renderer == NULL) || (loader == NULL) || (device == NULL)) {
        return;
    }

    /* Reverse creation order, and every rung is attempted after one refuses: a refused rung is
       reported to whoever can read it, and skipping the rungs below it would leak them - the rul
       e xud_close_context() states for its own rungs, one rung up. Each close answers XUD_ALREA
       DY for a field that was never filled, which is what makes this one order safe to call on a
       renderer that failed part-way up its bring-up. */
    rung = xud_frame_close(&renderer->frame, device, loader);
    (void)rung;
    rung = xud_framebuffer_array_close(&renderer->framebuffers, device, loader);
    (void)rung;
    rung = xud_pipeline_close(&renderer->pipeline, device, loader);
    (void)rung;
    rung = xud_renderpass_close(&renderer->renderPass, device, loader);
    (void)rung;
}

/*-----------------------------------------------------------------------------------------------
 * The bring-up - the four rungs, in the one order that is correct
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_renderer_open(XudRenderer** outRenderer, XudLoader* loader, const XudDevice* device, const XudSwapchain* swapchain) {
    XudRenderer* renderer = NULL;
    XudResult result = XUD_INVALID_ARG;

    if ((outRenderer == NULL) || (*outRenderer != NULL)) {
        return XUD_INVALID_ARG;
    }
    if ((loader == NULL) || (device == NULL) || (swapchain == NULL)) {
        return (loader == NULL) ? XUD_INVALID_ARG : XUD_UNSUPPORTED;
    }
    if (!loader->open) {
        return XUD_UNSUPPORTED;
    }
    if (device->device == VK_NULL_HANDLE) {
        return XUD_INVALID_ARG;
    }
    if ((swapchain->swapchain == VK_NULL_HANDLE) || (swapchain->imageCount == 0u) || (swapchain->images == NULL)) {
        return XUD_INVALID_ARG;
    }

    /* The chosen format is the swapchain's own record, read rather than re-queried: the surface
       this swapchain was created for is owned by the window above and this rung cannot name it,
       and xud_swapchain.c's note about the surface's "undefined" currentExtent is the same reaso
       n the extent below is copied rather than asked for again. No chosen format means no attach
       ment description, which is a refusal rather than a pass built on VK_FORMAT_UNDEFINED. */
    if (swapchain->createInfo.imageFormat == VK_FORMAT_UNDEFINED) {
        return XUD_UNSUPPORTED;
    }

    renderer = calloc(1u, sizeof(*renderer));
    if (renderer == NULL) {
        return XUD_OUT_OF_MEMORY;
    }

    /* 1. the render pass. It is first because the core-1.4 binding makes the pass an argument of
       the pipeline's create (see the note at the call below) and the framebuffer's create-info na
       mes it too, so both of those need it in hand. Every teardown rung of every one of the four
       is checked by the rung itself before its create - a pass this layer cannot destroy is a pas
       s it must not create, the rule xud_window.c applies to a surface and xud_swapchain.c to a
       swapchain, one rung below. */
    result = xud_renderpass_open(&renderer->renderPass, loader, device, swapchain->createInfo.imageFormat);
    if (result != XUD_OK) {
        free(renderer);
        return result;
    }

    /* 2. the pipeline, which names the pass above it: the core-1.4 binding makes the render pass
       an argument of vkCreateGraphicsPipelines (VUID-VkGraphicsPipelineCreateInfo-dynamicRenderi
       ng-06576, and the validation layer on this machine reports VK_NULL_HANDLE there as a hard
       error, probed today), so the pass has to exist before the pipeline does. */
    result = xud_pipeline_open(&renderer->pipeline, loader, device, renderer->renderPass->renderPass);
    if (result != XUD_OK) {
        xud_renderer_retire(renderer, device, loader);
        free(renderer);
        return result;
    }

    /* 3. the framebuffer array - one row per swapchain image, in image-index order, each row bu
       ilt from the image xud_swapchain_images_get() hands back. That accessor is the repo's rule
       for a rung above the swapchain: a caller above holds an INDEX rather than a VkImage, beca
       use a recreate replaces the whole array. */
    result = xud_framebuffer_array_open(&renderer->framebuffers, loader, device, renderer->renderPass, swapchain);
    if (result != XUD_OK) {
        xud_renderer_retire(renderer, device, loader);
        free(renderer);
        return result;
    }

    /* 4. the frame loop, last: it owns the fences that bound a presentation of THESE images and
       the command buffer that records a draw naming the pipeline, the pass and a row of the arra
       y above, so it needs all three. */
    result = xud_frame_open(&renderer->frame, loader, device, swapchain);
    if (result != XUD_OK) {
        xud_renderer_retire(renderer, device, loader);
        free(renderer);
        return result;
    }

    renderer->framebufferCount = swapchain->imageCount;

    *outRenderer = renderer;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * One frame, through the rungs this rung owns - the caller above names none of them
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_renderer_draw(
    XudRenderer* renderer, XudLoader* loader, const XudDevice* device, const XudSwapchain* swapchain, uint32_t* outImageIndex) {
    if ((renderer == NULL) || (outImageIndex == NULL)) {
        return XUD_INVALID_ARG;
    }
    if ((renderer->pipeline == NULL) || (renderer->renderPass == NULL) || (renderer->framebuffers == NULL) || (renderer->frame == NULL)) {
        return XUD_INVALID_ARG;
    }

    /* The count handed down is the array's own count, and the recorded one is compared against
       it rather than trusted: a renderer whose array was replaced by something else's would be
       a draw against rows that are not this swapchain's, which xud_frame_draw() refuses below. */
    if (renderer->framebufferCount != renderer->framebuffers->count) {
        return XUD_UNSUPPORTED;
    }

    return xud_frame_draw(renderer->frame, loader, device, swapchain, renderer->pipeline, renderer->renderPass, renderer->framebuffers,
        renderer->framebufferCount, outImageIndex);
}

/*-----------------------------------------------------------------------------------------------
 * The teardown
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_renderer_close(XudRenderer** ioRenderer, const XudDevice* device, XudLoader* loader) {
    XudRenderer* victim = NULL;

    if ((ioRenderer == NULL) || (*ioRenderer == NULL)) {
        return XUD_INVALID_ARG;
    }
    victim = *ioRenderer;

    if ((loader == NULL) || (device == NULL)) {
        return (loader == NULL) ? XUD_INVALID_ARG : XUD_UNSUPPORTED;
    }
    if (!loader->open) {
        return XUD_UNSUPPORTED;
    }
    if (device->device == VK_NULL_HANDLE) {
        return XUD_INVALID_ARG;
    }

    /* The four rungs retire in the reverse of the open order, and each one's own close is the
       guard plus the destroy through a row of the loader's device table - the frame's is the fen
       ce guard, the pipeline's is nothing but a destroy order. A rung whose rows are nil refuses
       and leaves the VULKAN HANDLES of that rung behind - the C struct below is freed either way,
       so the handles are the leak that the refusal reports, and the rungs below still retire bec
       ause skipping them would leak them too. The rows are checked at open by the same rung, so
       this is the one state a broken loader can reach, and it is reported rather than hidden. */
    xud_renderer_retire(victim, device, loader);

    /* Every close above already freed and cleared the field it owned, so each free below is the
       no-op for a rung that retired and the last chance for one that refused; free(NULL) is a
       no-op in both cases, which is what makes one line per field the honest form here. */
    free(victim->framebuffers);
    victim->framebuffers = NULL;
    free(victim->pipeline);
    victim->pipeline = NULL;
    free(victim->renderPass);
    victim->renderPass = NULL;
    free(victim->frame);
    victim->frame = NULL;
    victim->framebufferCount = 0u;

    free(victim);
    *ioRenderer = NULL;

    return XUD_OK;
}
