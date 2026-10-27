/***********************************************************************************************
 *
 *   xud_renderer.c - the renderer rung: the owner of one batch, one pipeline, one render pass,
 *                    one framebuffer per swapchain image and one frame loop
 *
 *   Owns : the five rungs below the public API that make a frame possible - the batch rung's four
 *          SoA attribute streams and its one index buffer (plus the one mapped staging arena the
 *          CPU-side bytes are written into), the pipeline rung's one graphics pipeline (with its
 *          layout and its two shader modules inside it), the render-pass rung's one VkRenderPass,
 *          that rung's array of one VkFramebuffer per swapchain image, and the frame loop's comm
 *          and pool, command buffer, acquire semaphore and per-image fences - and the one teardown
 *          order that hands all five back.
 *   Layer: above the frame loop, below the public API. This is the rung of the plan's layer
 *          ladder ("xudrl renderer (passes, pipelines, batches, buffers)") and the rung xud_init
 *          _context() wires, which is what makes the context's renderer field live rather than a
 *          sentinel. Nothing in this file calls a Vulkan entry point at all: every one of them is
 *          called by the rung that owns it, through a row of XudLoader's Volk tables, so the arch
 *          ive holds no vk* symbol and the link line has no -lvulkan anywhere (AGENTS.md).
 *
 *   The order is load-bearing in both directions, and it is the plan's dependency direction read
 *          both ways. Open: render pass -> batch -> pipeline -> framebuffer array -> frame. The pass
 *          is first because the core-1.4 binding makes it an argument of the pipeline's create (VU
 *          ID-VkGraphicsPipelineCreateInfo-dynamicRendering-06576: with dynamicRendering not enab
 *          led - and it is not, see the render-pass decision in AGENTS.md - a graphics pipeline w
 *          ith pre-rasterization or fragment state MUST name the pass it belongs to, and the valid
 *          ation layer on this machine reports VK_NULL_HANDLE there as a hard error, probed today).
 *          The batch is second because what it needs is the two records BELOW this rung - the caps
 *          and the allocator - and neither the pass nor the pipeline is an argument of its create;
 *          it sits before the pipeline because the pipeline's vertex-input state is the thing that
 *          names the batch's buffers, so the buffers have to exist before the pipeline is built ag
 *          ainst them and must outlive it. A framebuffer is created FOR a pass too (the spec requi
 *          res the framebuffer's attachment list to match the pass's descriptions exactly) and a f
 *          rame records a draw naming a pipeline, a pass and a framebuffer row, so each of the rest
 *          needs the ones above it. Close: the exact reverse - frame first, because a frame is the
 *          only thing in this set that can still have a presentation in flight against the swapcha
 *          in's images and its own close is the fence guard that retires it; then the framebuffer
 *          array, whose views name those very images; then the pipeline, then the batch and then t
 *          he pass, all of which name that pass's handle, so it dies last.
 *
 *   What this rung does NOT own, and why: no swapchain (the context owns it, one rung below), no
 *          VkImage of a swapchain image (the swapchain owns its images - xud_swapchain.c's note
 *          about vkDestroyImage), no GPU-memory allocator of its own (the allocator rung IS in the
 *          tree now (S12) and is owned by the context rather than by this rung - which is why the
 *          batch's buffers are handed to the allocator rather to a block of this rung's), and no d
 *          escriptor rung of its own either (the context owns that one too, and this rung holds a
 *          POINTER to it because the pipeline below is built against that record's two set layouts
 *          and the frame loop binds that record's set 0 every frame - the same outlive rule the bat
 *          ch's note above gives). The object kinds a public XudHandle names are the step above thi
 *          s one that has not landed. THE BATCH RUNG, though, IS in the tree now and this rung owns
 *          it - and what is now true of it, stated rather than hidden: the pipeline this rung build
 *          s is built against the batch (xud_pipeline_open() takes it and xud_pipeline_fill_vertex_
 *          input() points the vertex-input row at the batch's four binding and four attribute rows)
 *          , and the frame loop this rung draws through takes it too, so the batch's six buffers ar
 *          e created, uploaded through xud_batch_upload(), bound by one vkCmdBindVertexBuffers cal
 *          l over the four streams and one vkCmdBindIndexBuffer call over the index buffer, and dra
 *          wn by vkCmdDrawIndexed per recorded draw row rather than by a hard-coded vkCmdDraw. Wha
 *          t is still open in this rung is the push constant, the transparent variant and the handl
 *          e layer's record - the descriptor bind and the per-frame uniform copy landed with the de
 *          scriptor staging step, and the frame loop above does both.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include "xud_renderer.h"
#include "xud.h"
#include "xud_allocator.h"
#include "xud_batch.h"
#include "xud_descriptor.h"
#include "xud_device.h"
#include "xud_frame.h"
#include "xud_loader.h"
#include "xud_physical.h"
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

static void xud_renderer_retire(XudRenderer* renderer, const XudDevice* device, XudLoader* loader, XudAllocator* alloc) {
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
    rung = xud_batch_close(&renderer->batch, device, loader, alloc);
    (void)rung;
    rung = xud_renderpass_close(&renderer->renderPass, device, loader, alloc);
    (void)rung;
}

/*-----------------------------------------------------------------------------------------------
 * The bring-up - the five rungs, in the one order that is correct
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_renderer_open(XudRenderer** outRenderer, XudLoader* loader, const XudDevice* device, const XudDeviceCaps* caps,
    XudDescriptor* descriptor, XudAllocator* alloc, const XudSwapchain* swapchain) {
    XudRenderer* renderer = NULL;
    XudResult result = XUD_INVALID_ARG;

    if ((outRenderer == NULL) || (*outRenderer != NULL)) {
        return XUD_INVALID_ARG;
    }
    if ((loader == NULL) || (device == NULL) || (descriptor == NULL) || (swapchain == NULL)) {
        return (loader == NULL) ? XUD_INVALID_ARG : XUD_UNSUPPORTED;
    }
    if ((caps == NULL) || (alloc == NULL)) {
        /* The render pass this rung opens now needs both: the caps are the memory rows the depth
           image's allocation is chosen from, and the allocator is the one owner of that memory. Th
           e renderer borrows both rather than owning either - the context owns them, one rung below
           this one, and closes them after this rung's teardown has released the range - which is th
           e same shape the descriptor record below is already borrowed in. */
        return XUD_UNSUPPORTED;
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
    result = xud_renderpass_open(&renderer->renderPass, loader, device, caps, alloc, swapchain);
    if (result != XUD_OK) {
        free(renderer);
        return result;
    }

    /* 2. the batch rung, below the pipeline in the ladder and above the allocator: its five
       buffers are carved out of the allocator's memory, which is already in hand from the pass
       above, and its vertex-input state is gated on the caps' vertex-input limits. It is handed
       neither the pass nor the pipeline because neither is an argument of its create - what it
       needs is the two records below this rung and nothing else. Opened BEFORE the pipeline for
       the direction the file-top note states: the pipeline's vertex-input state names these buffers,
       so the buffers have to exist before the pipeline is built against them and must outlive it -
       and xud_pipeline_open() below takes the batch for exactly that reason.
     */
    result = xud_batch_open(&renderer->batch, loader, device, caps, alloc);
    if (result != XUD_OK) {
        free(renderer);
        return result;
    }

    /* 3. the pipeline, which names the pass above it: the core-1.4 binding makes the render pass
       an argument of vkCreateGraphicsPipelines (VUID-VkGraphicsPipelineCreateInfo-dynamicRenderi
       ng-06576, and the validation layer on this machine reports VK_NULL_HANDLE there as a hard
       error, probed today), so the pass has to exist before the pipeline does. The descriptor re
       cord is an argument too, and not a field of this rung: the pipeline's create-info names the
       two set layouts and the push-constant range the rung below this one created, so a pipeline
       built without them is a pipeline that can never be bound to the set its per-frame data was
       written into. The renderer borrows the record rather than owning it - the context owns it, a
       bove the allocator and below this rung, and closes it after this rung's teardown has retired
       the pipeline that names those handles.
     */
    result = xud_pipeline_open(&renderer->pipeline, loader, device, descriptor, renderer->batch, renderer->renderPass->renderPass);
    if (result != XUD_OK) {
        xud_renderer_retire(renderer, device, loader, alloc);
        free(renderer);
        return result;
    }

    /* 4. the framebuffer array - one row per swapchain image, in image-index order, each row bu
       ilt from the image xud_swapchain_images_get() hands back. That accessor is the repo's rule
       for a rung above the swapchain: a caller above holds an INDEX rather than a VkImage, beca
       use a recreate replaces the whole array. */
    result = xud_framebuffer_array_open(&renderer->framebuffers, loader, device, renderer->renderPass, swapchain);
    if (result != XUD_OK) {
        xud_renderer_retire(renderer, device, loader, alloc);
        free(renderer);
        return result;
    }

    /* 5. the frame loop, last: it owns the fences that bound a presentation of THESE images and
       the command buffer that records a draw naming the pipeline, the pass and a row of the arra
       y above, so it needs all three. */
    result = xud_frame_open(&renderer->frame, loader, device, swapchain);
    if (result != XUD_OK) {
        xud_renderer_retire(renderer, device, loader, alloc);
        free(renderer);
        return result;
    }

    renderer->framebufferCount = swapchain->imageCount;
    /* The descriptor is the ONE record of the rungs above this one that this rung holds a pointer
       to rather than owns: the pipeline built above was created against THIS record's two set lay
       outs, and the frame loop binds THIS record's set 0 every frame, so a renderer that forgot it
       would be a renderer whose draw had no set to bind. The owner of the record is the context be
       low, which closes it after this rung's teardown has released the range - the same outlive ru
       le the batch's note in the header gives.
    */
    renderer->descriptor = descriptor;

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
    if ((renderer->pipeline == NULL) || (renderer->renderPass == NULL) || (renderer->framebuffers == NULL) || (renderer->frame == NULL)
        || (renderer->descriptor == NULL)) {
        return XUD_INVALID_ARG;
    }

    /* The count handed down is the array's own count, and the recorded one is compared against
       it rather than trusted: a renderer whose array was replaced by something else's would be
       a draw against rows that are not this swapchain's, which xud_frame_draw() refuses below. */
    if (renderer->framebufferCount != renderer->framebuffers->count) {
        return XUD_UNSUPPORTED;
    }

    return xud_frame_draw(renderer->frame, loader, device, swapchain, renderer->pipeline, renderer->renderPass, renderer->framebuffers,
        renderer->framebufferCount, renderer->batch, renderer->descriptor, outImageIndex);
}

/*-----------------------------------------------------------------------------------------------
 * The teardown
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_renderer_close(XudRenderer** ioRenderer, const XudDevice* device, XudLoader* loader, XudAllocator* alloc) {
    XudRenderer* victim = NULL;

    if ((ioRenderer == NULL) || (*ioRenderer == NULL)) {
        return XUD_INVALID_ARG;
    }
    victim = *ioRenderer;

    if ((loader == NULL) || (device == NULL) || (alloc == NULL)) {
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
    xud_renderer_retire(victim, device, loader, alloc);

    /* Every close above already freed and cleared the field it owned, so each free below is the
       no-op for a rung that retired and the last chance for one that refused; free(NULL) is a
       no-op in both cases, which is what makes one line per field the honest form here. */
    free(victim->framebuffers);
    victim->framebuffers = NULL;
    free(victim->pipeline);
    victim->pipeline = NULL;
    free(victim->batch);
    victim->batch = NULL;
    free(victim->renderPass);
    victim->renderPass = NULL;
    free(victim->frame);
    victim->frame = NULL;
    victim->framebufferCount = 0u;

    free(victim);
    *ioRenderer = NULL;

    return XUD_OK;
}
