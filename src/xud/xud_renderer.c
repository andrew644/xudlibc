/***********************************************************************************************
 *
 *   xud_renderer.c - the renderer rung: the owner of one batch, the pipeline rung's variant
 *                    list of graphics pipelines, one render pass, one framebuffer per swapchain
 *                    image and one frame loop
 *
 *   Owns : the five rungs below the public API that make a frame possible - the batch rung's four
 *          SoA attribute streams and its one index buffer (plus the one mapped staging arena the
 *          CPU-side bytes are written into), the pipeline rung's variant list of graphics pipelines
 *          (one layout and two shader modules shared under the whole list), the render-pass rung's
 *          one VkRenderPass,
 *          that rung's array of one VkFramebuffer per swapchain image, and the frame loop's comm
 *          and pool, command buffer, acquire semaphore and per-image fences - and the one teardown
 *          order that hands all five back, plus xud_renderer_resize(): the one rung in this tree
 *          that acts on the recreate ask the frame loop leaves behind, run BETWEEN draws because a
 *          recreate replaces the images, the rows and the fence indices at once. The order it runs
 *          is frame closed -> framebuffer array closed -> the context's swapchain recreated at the
 *          window's FRAMEBUFFER extent -> the pass's depth image recreated at it -> rows reopened
 *          -> frame reopened, and the batch, the pipeline and the descriptor are not touched at all
 *          because none of them names an extent. What the draw entry above does every frame, be
 *          fore the frame loop runs, is hand the camera this rung holds to the camera rung, which
 *          writes the frame's view-projection matrix into set 0's block - the block is copied into
 *          the descriptor's staging arena BY the frame loop, so a matrix written after that copy
 *          would be a matrix the NEXT frame uploaded rather than this one.
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
#include "xud_camera.h" /* Required for: xud_camera_apply, the camera rung's write of set 0's matrix */
#include "xud_descriptor.h"
#include "xud_device.h"
#include "xud_frame.h"
#include "xud_light.h" /* Required for: XUD_SHADOW_MAP_SIZE - the occluder map's edge, sized where it is stated */
#include "xud_loader.h"
#include "xud_physical.h"
#include "xud_pipeline.h"
#include "xud_renderpass.h"
#include "xud_rendertarget.h" /* Required for: xud_rendertarget_fill_color_info - the scratch image's create-info is the render target's create-info, and one row says both */
#include "xud_swapchain.h"
#include "xud_texture.h" /* Required for: xud_texture_fill_sampler_info - the scratch image samples with the render target's sampler shape */
#include "xud_window.h"

#include <assert.h> /* Required for: static_assert, the count that cannot disagree */
#include <stdlib.h> /* Required for: calloc, free */
#include <string.h> /* Required for: memset - the sampler and barrier rows the scratch bring-up fills */

/* The count xud_frame_draw() is handed is the array's own count, and the field beside it is w
   hat keeps that true: the renderer records the count it opened the array for, and the array re
   cords the count it allocated. */
static_assert(
    sizeof(((XudRenderer*)0)->framebufferCount) == sizeof(uint32_t), "the recorded count is the count type the array's own field uses");

/*-----------------------------------------------------------------------------------------------
 * The high-dynamic-range scratch image: the format choice, the bring-up, the retirement
 *-----------------------------------------------------------------------------------------------*/

static void xud_renderer_hdr_retire(
    XudRenderer* renderer, XudLoader* loader, const XudDevice* device, XudAllocator* alloc, XudDescriptor* descriptor);

/* The scratch format, asked of THIS physical device rather than assumed: a format is a colour
   attachment only as far as the driver says it is, and the resolve pass samples the very image the
   attachment writes, so both bits have to be answered for a candidate to be usable. The order is
   the order of preference: half floats first (the format the tonemap curve was tuned against, and
   the one every desktop driver since Vulkan 1.0's mandatory-support table answers), full floats
   second, and the swapchain's own format last - the last row is not a failure but a degenerate
   high-dynamic-range range, and the resolve still runs and still tonemaps against it, which is what
   keeps the present path one path on every driver. */
static VkFormat xud_renderer_pick_hdr_format(XudLoader* loader, VkPhysicalDevice physicalDevice, VkFormat fallback) {
    static const VkFormat wanted[2] = { VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT };
    const VkFormatFeatureFlags needed
        = (VkFormatFeatureFlags)(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    uint32_t candidate = 0u;

    if ((loader == NULL) || (physicalDevice == VK_NULL_HANDLE) || (loader->instanceTable.vkGetPhysicalDeviceFormatProperties == NULL)) {
        return fallback;
    }
    for (candidate = 0u; candidate < (sizeof(wanted) / sizeof(wanted[0])); candidate++) {
        VkFormatProperties properties;
        loader->instanceTable.vkGetPhysicalDeviceFormatProperties(physicalDevice, wanted[candidate], &properties);
        if ((properties.optimalTilingFeatures & needed) == needed) {
            return wanted[candidate];
        }
    }
    return fallback;
}

/* The scratch image, its view, its sampler, its pass and its one framebuffer row - the render
   target rung's open order minus the batch and the handle, because the scratch pass belongs to the
   renderer rather than to a caller's handle and has no batch of its own (the batch that draws into
   it is the renderer's screen batch). The slot is bound LAST for the render target's reason: no
   descriptor set may ever name a half-built pair. */
static XudResult xud_renderer_hdr_open(XudRenderer* renderer, XudLoader* loader, const XudDevice* device, const XudDeviceCaps* caps,
    XudAllocator* alloc, XudDescriptor* descriptor, const XudSwapchain* swapchain) {
    XudTextureData samplerData;
    VkMemoryRequirements needs;
    XudResult result = XUD_INVALID_ARG;
    uint32_t width = swapchain->createInfo.imageExtent.width;
    uint32_t height = swapchain->createInfo.imageExtent.height;

    result = xud_rendertarget_fill_color_info(&renderer->hdrColorImageInfo, width, height, renderer->hdrFormat);
    if (result != XUD_OK) {
        return result;
    }
    if (loader->deviceTable.vkCreateImage(device->device, &renderer->hdrColorImageInfo, NULL, &renderer->hdrColorImage) != VK_SUCCESS) {
        return XUD_VULKAN_ERROR;
    }
    memset(&needs, 0, sizeof needs);
    loader->deviceTable.vkGetImageMemoryRequirements(device->device, renderer->hdrColorImage, &needs);
    if (needs.size == 0u) {
        xud_renderer_hdr_retire(renderer, loader, device, alloc, descriptor);
        return XUD_VULKAN_ERROR;
    }
    result = xud_allocator_acquire(&renderer->hdrColorRange, alloc, loader, device, caps, &needs);
    if (result != XUD_OK) {
        xud_renderer_hdr_retire(renderer, loader, device, alloc, descriptor);
        return result;
    }
    result = xud_result_from_vk(loader->deviceTable.vkBindImageMemory(
        device->device, renderer->hdrColorImage, renderer->hdrColorRange.memory, renderer->hdrColorRange.offset));
    if (result != XUD_OK) {
        xud_renderer_hdr_retire(renderer, loader, device, alloc, descriptor);
        return result;
    }
    result = xud_layout_track(renderer->layout, renderer->hdrColorImage, renderer->hdrColorImageInfo.initialLayout);
    if (result != XUD_OK) {
        xud_renderer_hdr_retire(renderer, loader, device, alloc, descriptor);
        return result;
    }

    result = xud_framebuffer_fill_view_info(&renderer->hdrColorViewInfo, renderer->hdrColorImage, renderer->hdrFormat);
    if (result != XUD_OK) {
        xud_renderer_hdr_retire(renderer, loader, device, alloc, descriptor);
        return result;
    }
    if (loader->deviceTable.vkCreateImageView(device->device, &renderer->hdrColorViewInfo, NULL, &renderer->hdrColorView) != VK_SUCCESS) {
        xud_renderer_hdr_retire(renderer, loader, device, alloc, descriptor);
        return XUD_VULKAN_ERROR;
    }

    memset(&samplerData, 0, sizeof samplerData);
    samplerData.filter = XUD_TEXTURE_FILTER_LINEAR;
    samplerData.addressMode = XUD_TEXTURE_ADDRESS_CLAMP_TO_EDGE;
    result = xud_texture_fill_sampler_info(&samplerData, &renderer->hdrSamplerInfo);
    if (result != XUD_OK) {
        xud_renderer_hdr_retire(renderer, loader, device, alloc, descriptor);
        return result;
    }
    if (loader->deviceTable.vkCreateSampler(device->device, &renderer->hdrSamplerInfo, NULL, &renderer->hdrSampler) != VK_SUCCESS) {
        xud_renderer_hdr_retire(renderer, loader, device, alloc, descriptor);
        return XUD_VULKAN_ERROR;
    }

    result = xud_renderpass_open_offscreen(
        &renderer->hdrPass, loader, device, caps, alloc, renderer->hdrFormat, width, height, renderer->layout);
    if (result != XUD_OK) {
        xud_renderer_hdr_retire(renderer, loader, device, alloc, descriptor);
        return result;
    }
    result = xud_framebuffer_open(
        &renderer->hdrFramebuffer, loader, device, renderer->hdrPass, renderer->hdrColorImage, renderer->hdrFormat, width, height);
    if (result != XUD_OK) {
        xud_renderer_hdr_retire(renderer, loader, device, alloc, descriptor);
        return result;
    }
    /* The pre-pass pass pairs with the scratch pass (and flips its depth row to LOAD in the one
       call), and its framebuffer is the second row over the same two views: the subpass shapes
       differ, so render-pass compatibility says two rows. */
    result = xud_renderpass_open_prepass(&renderer->prepassPass, loader, device, renderer->hdrPass);
    if (result != XUD_OK) {
        xud_renderer_hdr_retire(renderer, loader, device, alloc, descriptor);
        return result;
    }
    result = xud_framebuffer_open(&renderer->hdrPrepassFramebuffer, loader, device, renderer->prepassPass, renderer->hdrColorImage,
        renderer->hdrFormat, width, height);
    if (result != XUD_OK) {
        xud_renderer_hdr_retire(renderer, loader, device, alloc, descriptor);
        return result;
    }

    result = xud_descriptor_bind_texture_slot(descriptor, loader, device, renderer->hdrColorView, renderer->hdrSampler,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &renderer->hdrSlot);
    if (result != XUD_OK) {
        xud_renderer_hdr_retire(renderer, loader, device, alloc, descriptor);
        return result;
    }
    renderer->hdrSlotLive = 1u;
    renderer->hdrWidth = width;
    renderer->hdrHeight = height;
    return XUD_OK;
}

/* Everything the scratch rows own, in the render target retire's reverse order minus the batch and
   the handle: the slot before the pair it names, the framebuffer before the pass, the pass (its own
   depth image included) before the sampler, the tracker forget before the view, and the view before
   the image and the allocator's range. Every step tolerates its own absence, so the same function
   retires a half-built scratch pair - and the resize rung runs it before a fresh hdr_open. */
static void xud_renderer_hdr_retire(
    XudRenderer* renderer, XudLoader* loader, const XudDevice* device, XudAllocator* alloc, XudDescriptor* descriptor) {
    if ((renderer == NULL) || (loader == NULL) || (device == NULL)) {
        return;
    }
    if ((renderer->hdrSlotLive != 0u) && (descriptor != NULL)) {
        (void)xud_descriptor_release_texture_slot(descriptor, loader, device, renderer->hdrSlot);
        renderer->hdrSlotLive = 0u;
    }
    if (renderer->hdrPrepassFramebuffer != NULL) {
        (void)xud_framebuffer_close(&renderer->hdrPrepassFramebuffer, device, loader);
    }
    if (renderer->prepassPass != NULL) {
        /* Before hdrPass, whose depth row it borrows - and the borrow's retire touches no depth
           handle (xud_renderpass_retire_depth stops at ownsDepth), so the ordering is about the
           VkRenderPass objects the pipeline named, not about the image. */
        (void)xud_renderpass_close(&renderer->prepassPass, device, loader, alloc);
    }
    if (renderer->hdrFramebuffer != NULL) {
        (void)xud_framebuffer_close(&renderer->hdrFramebuffer, device, loader);
    }
    if (renderer->hdrPass != NULL) {
        (void)xud_renderpass_close(&renderer->hdrPass, device, loader, alloc);
    }
    if ((renderer->hdrSampler != VK_NULL_HANDLE) && (loader->deviceTable.vkDestroySampler != NULL)) {
        loader->deviceTable.vkDestroySampler(device->device, renderer->hdrSampler, NULL);
        renderer->hdrSampler = VK_NULL_HANDLE;
    }
    if ((renderer->hdrColorImage != VK_NULL_HANDLE) && (renderer->layout != NULL)) {
        (void)xud_layout_forget(renderer->layout, renderer->hdrColorImage);
    }
    if ((renderer->hdrColorView != VK_NULL_HANDLE) && (loader->deviceTable.vkDestroyImageView != NULL)) {
        loader->deviceTable.vkDestroyImageView(device->device, renderer->hdrColorView, NULL);
        renderer->hdrColorView = VK_NULL_HANDLE;
    }
    if ((renderer->hdrColorImage != VK_NULL_HANDLE) && (loader->deviceTable.vkDestroyImage != NULL)) {
        loader->deviceTable.vkDestroyImage(device->device, renderer->hdrColorImage, NULL);
        renderer->hdrColorImage = VK_NULL_HANDLE;
    }
    if ((renderer->hdrColorRange.memory != VK_NULL_HANDLE) && (alloc != NULL)) {
        (void)xud_allocator_release(alloc, &renderer->hdrColorRange);
    }
}

/* Reverse of the make, and every step tolerates its own absence: the framebuffer before the
   pass whose view it attaches (the pass owns that view), the pass - its depth image, view,
   range and tracker entry all inside xud_renderpass_close's hands - before the sampler, since
   the descriptor write that names view and sampler is history the moment the pass is gone:
   nothing can bind a set whose renderer is retired, and the descriptor itself goes one rung
   above this one, after this teardown returns. */
static void xud_renderer_shadow_retire(XudRenderer* renderer, XudLoader* loader, const XudDevice* device, XudAllocator* alloc) {
    if ((renderer == NULL) || (loader == NULL) || (device == NULL)) {
        return;
    }
    if (renderer->shadowFramebuffer != NULL) {
        (void)xud_framebuffer_close(&renderer->shadowFramebuffer, device, loader);
    }
    if (renderer->shadowPass != NULL) {
        (void)xud_renderpass_close(&renderer->shadowPass, device, loader, alloc);
    }
    if ((renderer->shadowSampler != VK_NULL_HANDLE) && (loader->deviceTable.vkDestroySampler != NULL)) {
        loader->deviceTable.vkDestroySampler(device->device, renderer->shadowSampler, NULL);
        renderer->shadowSampler = VK_NULL_HANDLE;
    }
}

/* The shadow map's bring-up (S2.5): the comparison sampler, the map's own pass (its depth image
   and view ride inside it, ownsDepth-style, and the tracker gets the image's first word from
   the renderpass rung), the one-attachment framebuffer row, and the ONE descriptor write that
   puts the pair into set 0's binding 1. It sits beside the scratch bring-up rather than inside
   it because it does not follow the swapchain: nothing in here is sized by the extent, nothing
   in here is retired by a resize, and the pair is made once per renderer. The order is the
   dependency's: the sampler before the write that names it, the pass before the row that
   attaches its view, and all of it before the pipeline that is bound to this pass - the call
   that needs shadowPass->renderPass lives at step 1c of the bring-up below. */
static XudResult xud_renderer_shadow_open(
    XudRenderer* renderer, XudLoader* loader, const XudDevice* device, const XudDeviceCaps* caps, XudAllocator* alloc) {
    XudResult result = XUD_OK;

    memset(&renderer->shadowSamplerInfo, 0, sizeof renderer->shadowSamplerInfo);
    renderer->shadowSamplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    renderer->shadowSamplerInfo.magFilter = VK_FILTER_LINEAR;
    renderer->shadowSamplerInfo.minFilter = VK_FILTER_LINEAR;
    renderer->shadowSamplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    renderer->shadowSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    renderer->shadowSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    renderer->shadowSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    renderer->shadowSamplerInfo.compareEnable = VK_TRUE;
    renderer->shadowSamplerInfo.compareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    renderer->shadowSamplerInfo.minLod = 0.0f;
    renderer->shadowSamplerInfo.maxLod = 0.0f;
    if (loader->deviceTable.vkCreateSampler(device->device, &renderer->shadowSamplerInfo, NULL, &renderer->shadowSampler) != VK_SUCCESS) {
        return XUD_VULKAN_ERROR;
    }

    result = xud_renderpass_open_shadow(
        &renderer->shadowPass, loader, device, caps, alloc, XUD_SHADOW_MAP_SIZE, renderer->hdrFormat, renderer->layout);
    if (result != XUD_OK) {
        xud_renderer_shadow_retire(renderer, loader, device, alloc);
        return result;
    }
    result = xud_framebuffer_open_shadow(&renderer->shadowFramebuffer, loader, device, renderer->shadowPass, XUD_SHADOW_MAP_SIZE);
    if (result != XUD_OK) {
        xud_renderer_shadow_retire(renderer, loader, device, alloc);
        return result;
    }

    result
        = xud_descriptor_set_shadow_texture(renderer->descriptor, loader, device, renderer->shadowPass->depthView, renderer->shadowSampler);
    if (result != XUD_OK) {
        xud_renderer_shadow_retire(renderer, loader, device, alloc);
        return result;
    }
    return XUD_OK;
}

/* The chain's images, views and rows, retired - every step tolerating its own absence, the way
   every other retire in this file does, so the same function cleans a half-built chain. The
   pass handle is NOT touched: it survives a resize because the pipeline's five chain rows are
   bound to it, and only the bring-up's own failure path and the renderer's teardown close it.
   The binding-2 write is not released either - it is not a slot, and the reopen's own write
   overwrites it once the new views exist; a draw between the two retires and reopens cannot
   happen, because the resize closes the frame loop before this runs and reopens it after. */
static void xud_renderer_bloom_images_retire(
    XudRenderer* renderer, XudLoader* loader, const XudDevice* device, XudAllocator* alloc, XudDescriptor* descriptor) {
    uint32_t level = 0u;

    if ((renderer == NULL) || (loader == NULL) || (device == NULL)) {
        return;
    }
    for (level = 0u; level < XUD_DESCRIPTOR_BLOOM_LEVELS; level++) {
        if ((renderer->bloomSourceLive[level] != 0u) && (descriptor != NULL)) {
            (void)xud_descriptor_release_texture_slot(descriptor, loader, device, renderer->bloomSourceSlot[level]);
            renderer->bloomSourceLive[level] = 0u;
            renderer->bloomSourceSlot[level] = 0u;
        }
        renderer->bloomRows[level].pass = NULL;
        renderer->bloomRows[level].framebuffer = NULL;
        if (renderer->bloomFramebuffer[level] != NULL) {
            (void)xud_framebuffer_close(&renderer->bloomFramebuffer[level], device, loader);
        }
        if ((renderer->bloomImage[level] != VK_NULL_HANDLE) && (renderer->layout != NULL)) {
            (void)xud_layout_forget(renderer->layout, renderer->bloomImage[level]);
        }
        if ((renderer->bloomView[level] != VK_NULL_HANDLE) && (loader->deviceTable.vkDestroyImageView != NULL)) {
            loader->deviceTable.vkDestroyImageView(device->device, renderer->bloomView[level], NULL);
            renderer->bloomView[level] = VK_NULL_HANDLE;
        }
        if ((renderer->bloomImage[level] != VK_NULL_HANDLE) && (loader->deviceTable.vkDestroyImage != NULL)) {
            loader->deviceTable.vkDestroyImage(device->device, renderer->bloomImage[level], NULL);
            renderer->bloomImage[level] = VK_NULL_HANDLE;
        }
        if ((renderer->bloomRange[level].memory != VK_NULL_HANDLE) && (alloc != NULL)) {
            (void)xud_allocator_release(alloc, &renderer->bloomRange[level]);
        }
    }
    renderer->bloomLive = 0u;
}

/* The chain's rows at the CURRENT scratch extent: level 0 at half the scratch's width and height
   (rounded up, so a 1-pixel scratch still leaves a 1-pixel level), each further level half the
   one above by the same rule. Every image is the scratch's own create-info shape - float format,
   COLOR_ATTACHMENT | SAMPLED - tracked in the context's map the way the scratch image is, so the
   chain's barriers read the tracker exactly as the scratch pass's do. The descriptor writes land
   LAST, when every view exists: the present row's four-view array is one write, and the three
   source slots the blur rows bind are claimed one per view - a chain with a hole in it is
   refused rather than half-written into a set the first frame binds. */
static XudResult xud_renderer_bloom_images_open(XudRenderer* renderer, XudLoader* loader, const XudDevice* device,
    const XudDeviceCaps* caps, XudAllocator* alloc, XudDescriptor* descriptor) {
    XudResult result = XUD_OK;
    uint32_t level = 0u;
    uint32_t width = 0u;
    uint32_t height = 0u;

    width = (renderer->hdrWidth + 1u) / 2u;
    height = (renderer->hdrHeight + 1u) / 2u;
    for (level = 0u; level < XUD_DESCRIPTOR_BLOOM_LEVELS; level++) {
        if (width == 0u) {
            width = 1u;
        }
        if (height == 0u) {
            height = 1u;
        }
        renderer->bloomWidth[level] = width;
        renderer->bloomHeight[level] = height;
        result = xud_rendertarget_fill_color_info(&renderer->bloomImageInfo[level], width, height, renderer->hdrFormat);
        if (result != XUD_OK) {
            xud_renderer_bloom_images_retire(renderer, loader, device, alloc, descriptor);
            return result;
        }
        if (loader->deviceTable.vkCreateImage(device->device, &renderer->bloomImageInfo[level], NULL, &renderer->bloomImage[level])
            != VK_SUCCESS) {
            xud_renderer_bloom_images_retire(renderer, loader, device, alloc, descriptor);
            return XUD_VULKAN_ERROR;
        }
        {
            VkMemoryRequirements needs;
            memset(&needs, 0, sizeof needs);
            loader->deviceTable.vkGetImageMemoryRequirements(device->device, renderer->bloomImage[level], &needs);
            if (needs.size == 0u) {
                xud_renderer_bloom_images_retire(renderer, loader, device, alloc, descriptor);
                return XUD_VULKAN_ERROR;
            }
            result = xud_allocator_acquire(&renderer->bloomRange[level], alloc, loader, device, caps, &needs);
            if (result != XUD_OK) {
                xud_renderer_bloom_images_retire(renderer, loader, device, alloc, descriptor);
                return result;
            }
        }
        result = xud_result_from_vk(loader->deviceTable.vkBindImageMemory(
            device->device, renderer->bloomImage[level], renderer->bloomRange[level].memory, renderer->bloomRange[level].offset));
        if (result != XUD_OK) {
            xud_renderer_bloom_images_retire(renderer, loader, device, alloc, descriptor);
            return result;
        }
        result = xud_layout_track(renderer->layout, renderer->bloomImage[level], renderer->bloomImageInfo[level].initialLayout);
        if (result != XUD_OK) {
            xud_renderer_bloom_images_retire(renderer, loader, device, alloc, descriptor);
            return result;
        }
        result = xud_framebuffer_fill_view_info(&renderer->bloomViewInfo[level], renderer->bloomImage[level], renderer->hdrFormat);
        if (result != XUD_OK) {
            xud_renderer_bloom_images_retire(renderer, loader, device, alloc, descriptor);
            return result;
        }
        if (loader->deviceTable.vkCreateImageView(device->device, &renderer->bloomViewInfo[level], NULL, &renderer->bloomView[level])
            != VK_SUCCESS) {
            xud_renderer_bloom_images_retire(renderer, loader, device, alloc, descriptor);
            return XUD_VULKAN_ERROR;
        }
        result = xud_framebuffer_open_bloom(&renderer->bloomFramebuffer[level], loader, device, renderer->bloomPass,
            renderer->bloomImage[level], renderer->hdrFormat, width, height);
        if (result != XUD_OK) {
            xud_renderer_bloom_images_retire(renderer, loader, device, alloc, descriptor);
            return result;
        }
        width = (width + 1u) / 2u;
        height = (height + 1u) / 2u;
    }

    result = xud_descriptor_set_bloom_textures(descriptor, loader, device, renderer->bloomView, renderer->hdrSampler);
    if (result != XUD_OK) {
        xud_renderer_bloom_images_retire(renderer, loader, device, alloc, descriptor);
        return result;
    }
    for (level = 0u; level + 1u < XUD_DESCRIPTOR_BLOOM_LEVELS; level++) {
        result = xud_descriptor_bind_texture_slot(descriptor, loader, device, renderer->bloomView[level], renderer->hdrSampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &renderer->bloomSourceSlot[level]);
        if (result != XUD_OK) {
            xud_renderer_bloom_images_retire(renderer, loader, device, alloc, descriptor);
            return result;
        }
        renderer->bloomSourceLive[level] = 1u;
    }
    for (level = 0u; level < XUD_DESCRIPTOR_BLOOM_LEVELS; level++) {
        renderer->bloomRows[level].pass = renderer->bloomPass;
        renderer->bloomRows[level].framebuffer = renderer->bloomFramebuffer[level];
        renderer->bloomRows[level].sourceSlot = (level == 0u) ? renderer->hdrSlot : renderer->bloomSourceSlot[level - 1u];
    }
    renderer->bloomLive = 1u;
    return XUD_OK;
}

/* The chain's bring-up: the ONE pass first (it is an argument of the pipeline's create three
   steps below, the way the scratch pass is), its clear filled black - the chain's background is
   black whatever the window's is, and unlike every other pass here it is never re-filled per
   frame - and then the four levels' rows at the current scratch extent. It sits beside the
   shadow map's bring-up rather than inside the scratch pair's because it follows a third rule:
   sized by the extent like the scratch, recreated with it like the scratch, but bound to the
   pipeline like the shadow map, so its pass belongs to the pipeline's argument list and its
   images belong to the resize. */
static XudResult xud_renderer_bloom_open(XudRenderer* renderer, XudLoader* loader, const XudDevice* device, const XudDeviceCaps* caps,
    XudAllocator* alloc, XudDescriptor* descriptor) {
    XudResult result = XUD_OK;

    result = xud_renderpass_open_bloom(&renderer->bloomPass, loader, device, renderer->hdrFormat);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_renderpass_set_clear_color(renderer->bloomPass, 0.0f, 0.0f, 0.0f, 0.0f);
    if (result != XUD_OK) {
        (void)xud_renderpass_close(&renderer->bloomPass, device, loader, alloc);
        return result;
    }
    result = xud_renderer_bloom_images_open(renderer, loader, device, caps, alloc, descriptor);
    if (result != XUD_OK) {
        (void)xud_renderpass_close(&renderer->bloomPass, device, loader, alloc);
        return result;
    }
    return XUD_OK;
}

/* Reverse of the bring-up: the rows before the pass that named nothing of them and the
   descriptor read them through, the pass last - and the sampler it sampled with belongs to the
   scratch pair above this retire, which closes after it. */
static void xud_renderer_bloom_retire(XudRenderer* renderer, XudLoader* loader, const XudDevice* device, XudAllocator* alloc) {
    xud_renderer_bloom_images_retire(renderer, loader, device, alloc, renderer->descriptor);
    if ((renderer != NULL) && (renderer->bloomPass != NULL)) {
        (void)xud_renderpass_close(&renderer->bloomPass, device, loader, alloc);
    }
}

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
    rung = xud_batch_close(&renderer->screenBatch, device, loader, alloc);
    renderer->batch = NULL; /* the ACTIVE pointer may name a target's batch, which that target's
                               record owns - the retirement goes to the owned one, and the alias
                               is cleared so nothing reads it again */
    (void)rung;
    xud_renderer_bloom_retire(renderer, loader, device, alloc); /* the chain goes with its own
                               rows and pass, after the pipeline that named the pass and (through
                               binding 2) its views - and before the scratch retire below, whose
                               sampler the chain's descriptor write named */
    xud_renderer_hdr_retire(renderer, loader, device, alloc, renderer->descriptor); /* the scratch rows
                               go after the pipeline that names the scratch pass and before the
                               screen pass rung below; the descriptor outlives the renderer, so the
                               slot it releases is still a live slot here */
    xud_renderer_shadow_retire(renderer, loader, device, alloc); /* the map goes with its own
                               sampler and pass, after the pipeline that named the pass and (as
                               set 0's binding 1) its view - and before the screen pass rung,
                               which owns nothing of it */
    rung = xud_renderpass_close(&renderer->renderPass, device, loader, alloc);
    (void)rung;
}

/*-----------------------------------------------------------------------------------------------
 * The bring-up - the five rungs, in the one order that is correct
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_renderer_open(XudRenderer** outRenderer, XudLoader* loader, const XudDevice* device, VkPhysicalDevice physicalDevice,
    const XudDeviceCaps* caps, XudDescriptor* descriptor, XudAllocator* alloc, const XudSwapchain* swapchain, XudLayoutTracker* layout) {
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
    if (layout == NULL) {
        return XUD_INVALID_ARG; /* the render pass's depth image and the frame's barriers both speak
                                   through the context's map */
    }
    if (!loader->open) {
        return XUD_UNSUPPORTED;
    }
    if (device->device == VK_NULL_HANDLE) {
        return XUD_INVALID_ARG;
    }
    if (physicalDevice == VK_NULL_HANDLE) {
        return XUD_INVALID_ARG; /* the scratch format is a question about THIS physical device */
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
    renderer->layout = layout;
    renderer->descriptor = descriptor; /* the ONE record of the rungs above this one that this rung
                                          holds a pointer to rather than owns - set first, because
                                          every retirement path below reaches it: the scratch slot
                                          the bring-up binds is a row of THIS record */

    /* The default background, quantized to the byte grid the public XudColor lives on: the
       render pass's own default clear colour, each channel times 255 and rounded to the nearest
       byte, so the first frame a context draws is cleared with the colour the render pass's note
       has always described rather than with whatever the zero byte would be (a transparent black,
       which the surface's OPAQUE composite answer needs a 1 alpha for). The byte's
       value-over-255 and the pass's float differ by less than half a code value per channel - the
       quantization the public 8-bit surface makes of any colour it is handed. */
    renderer->background.r = (uint8_t)(XUD_RENDERPASS_CLEAR_COLOR_R * 255.0f + 0.5f);
    renderer->background.g = (uint8_t)(XUD_RENDERPASS_CLEAR_COLOR_G * 255.0f + 0.5f);
    renderer->background.b = (uint8_t)(XUD_RENDERPASS_CLEAR_COLOR_B * 255.0f + 0.5f);
    renderer->background.a = (uint8_t)(XUD_RENDERPASS_CLEAR_COLOR_A * 255.0f + 0.5f);

    /* 1. the render pass. It is first because the core-1.4 binding makes the pass an argument of
       the pipeline's create (see the note at the call below) and the framebuffer's create-info na
       mes it too, so both of those need it in hand. Every teardown rung of every one of the four
       is checked by the rung itself before its create - a pass this layer cannot destroy is a pas
       s it must not create, the rule xud_window.c applies to a surface and xud_swapchain.c to a
       swapchain, one rung below. */
    result = xud_renderpass_open(&renderer->renderPass, loader, device, caps, alloc, swapchain, layout);
    if (result != XUD_OK) {
        free(renderer);
        return result;
    }

    /* 1b. the high-dynamic-range scratch: the format this physical device answers for colour
       attachment AND sampling, then the image, view, sampler, pass and one framebuffer row at the
       swapchain's extent, then the sampler-array slot - the render target's assembly with no handle
       and no batch, because the scratch belongs to the renderer. It sits HERE rather than beside the
       pipeline because the pipeline's scene rows are BOUND to this pass: pass compatibility is
       format-for-format, so the same eight pipelines cannot serve a float attachment and an 8-bit
       one, and the split is the one this step makes. */
    renderer->hdrFormat = xud_renderer_pick_hdr_format(loader, physicalDevice, swapchain->createInfo.imageFormat);
    result = xud_renderer_hdr_open(renderer, loader, device, caps, alloc, descriptor, swapchain);
    if (result != XUD_OK) {
        (void)xud_renderpass_close(&renderer->renderPass, device, loader, alloc);
        free(renderer);
        return result;
    }

    /* 1c. the shadow map: comparison sampler, map pass, one-view framebuffer row, and set 0's
       binding-1 write - before the pipeline, because the shadow pipeline is BOUND to this pass
       the same way the scene list is bound to the scratch pass, and before the first frame,
       because set 0 may not be bound while one of its bindings is unwritten.
    */
    result = xud_renderer_shadow_open(renderer, loader, device, caps, alloc);
    if (result != XUD_OK) {
        xud_renderer_retire(renderer, device, loader, alloc);
        free(renderer);
        return result;
    }

    /* 1c2. the bloom chain: the bright/blur images, their one pass, their rows, and set 0's
       binding-2 write - before the pipeline, because the chain's five pipelines are BOUND to
       this pass the way the scene list is bound to the scratch pass, and before the first
       frame, because set 0 may not be bound while one of its bindings is unwritten. It is
       sized by the scratch extent, which is why it follows the scratch pair rather than
       leading it.
    */
    result = xud_renderer_bloom_open(renderer, loader, device, caps, alloc, descriptor);
    if (result != XUD_OK) {
        xud_renderer_retire(renderer, device, loader, alloc);
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
    renderer->screenBatch = renderer->batch; /* the batch the draw paths come back to when a target
                                                scope closes; `batch` is the ACTIVE one and may be
                                                a target's for the length of a scope */
    if (result != XUD_OK) {
        xud_renderer_retire(renderer, device, loader, alloc); /* the pass and the scratch rows above
                                                                  this rung are retired too - every
                                                                  step of the retirement tolerates
                                                                  the absence of what was never made */
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
    result = xud_pipeline_open(&renderer->pipeline, loader, device, descriptor, renderer->batch, renderer->hdrPass->renderPass,
        renderer->renderPass->renderPass, renderer->prepassPass->renderPass, renderer->shadowPass->renderPass,
        renderer->bloomPass->renderPass);
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
    result = xud_frame_open(&renderer->frame, loader, device, swapchain, renderer->layout);
    if (result != XUD_OK) {
        xud_renderer_retire(renderer, device, loader, alloc);
        free(renderer);
        return result;
    }

    renderer->framebufferCount = swapchain->imageCount;
    /* (the descriptor pointer the retirement paths read was recorded at the top of this rung,
       beside the layout pointer, for the reason given there.) */

    *outRenderer = renderer;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * One frame, through the rungs this rung owns - the caller above names none of them
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_renderer_draw(
    XudRenderer* renderer, XudLoader* loader, const XudDevice* device, const XudSwapchain* swapchain, uint32_t* outImageIndex) {
    float aspect = 0.0f;
    uint32_t scope = 0u;
    const XudCamera3D* camera = NULL;
    XudResult result = XUD_OK;

    if ((renderer == NULL) || (outImageIndex == NULL)) {
        return XUD_INVALID_ARG;
    }
    if ((renderer->pipeline == NULL) || (renderer->renderPass == NULL) || (renderer->hdrPass == NULL) || (renderer->hdrFramebuffer == NULL)
        || (renderer->prepassPass == NULL) || (renderer->hdrPrepassFramebuffer == NULL) || (renderer->framebuffers == NULL)
        || (renderer->shadowPass == NULL) || (renderer->shadowFramebuffer == NULL) || (renderer->frame == NULL)
        || (renderer->descriptor == NULL)) {
        return XUD_INVALID_ARG;
    }

    /* The camera's matrix, placed in set 0's block BEFORE the frame loop copies the block into the
       descriptor's staging arena: that copy is of the block's bytes, so a matrix written after it
       would be a matrix the NEXT frame uploaded rather than this one. The aspect is the extent thi
       s frame renders at, read off the swapchain's own recorded create info rather than off the wi
       ndow's requested one (AGENTS.md: on a scaled session the two differ, and the one that is a
       fact about this frame is the one the viewport is set from) - and recomputed every frame rather
       than stored, which is what keeps a resize from leaving a matrix built for the old extent in
       the block. No camera is two facts, not one: none live AND nothing latched by a close that has
       not been drawn yet is the identity matrix - the block's matrix is a function of THIS frame's
       camera state, never of whatever the last frame's camera wrote - while a closed-but-latched
       scope is the record itself, because the draws this flush carries were recorded under it.
    */
    if (swapchain->createInfo.imageExtent.height > 0u) {
        aspect = (float)swapchain->createInfo.imageExtent.width / (float)swapchain->createInfo.imageExtent.height;
    }
    camera = NULL;
    if (renderer->cameraWanted != 0u) {
        camera = &renderer->camera;
    } else if ((renderer->cameraLatched != 0u) && (renderer->cameraLatchDraw == renderer->drawCount)) {
        camera = &renderer->camera;
    }
    renderer->cameraLatched = 0u;
    if (xud_camera_apply(renderer->descriptor, camera, aspect) != XUD_OK) {
        /* A camera the renderer holds that is no longer usable - a caller that tampered the record
           after the begin call that validated it, or a zero-height extent, which is the state a
           swapchain that never completed its create is in. The frame is refused rather than drawn
           with the matrix the last frame left in the block.
        */
        return XUD_INVALID_ARG;
    }

    /* The count handed down is the array's own count, and the recorded one is compared against
       it rather than trusted: a renderer whose array was replaced by something else's would be
       a draw against rows that are not this swapchain's, which xud_frame_draw() refuses below. */
    if (renderer->framebufferCount != renderer->framebuffers->count) {
        return XUD_UNSUPPORTED;
    }

    /* The background, written into the pass's clear value BEFORE the frame loop begins the render
       pass: passBegin's pClearValues is the pass's own row (see xud_frame.c's note beside the
       begin), so the value this frame's clear uses is whatever the row holds when the recording
       runs, and a write after it would be a background the NEXT frame's clear used rather than
       this one's. The byte is converted to its value-over-255 here rather than in the rung the
       row lives in, because the renderer owns the byte the caller handed and the render pass owns
       the float4 the driver reads - the same division the camera's give and apply make between
       the record the caller named and the block this rung writes. */
    if (xud_renderpass_set_clear_color(renderer->renderPass, (float)renderer->background.r / 255.0f, (float)renderer->background.g / 255.0f,
            (float)renderer->background.b / 255.0f, (float)renderer->background.a / 255.0f)
        != XUD_OK) {
        /* The pass was checked live above, so this is the layer's own record in a state the rung
           refuses rather than a caller argument - reported, not drawn around. */
        return XUD_INVALID_ARG;
    }

    /* The scratch pass carries the same background: it is the pass whose clear the caller sees,
       the screen pass's own clear being replaced pixel-for-pixel by the resolve triangle. */
    if (xud_renderpass_set_clear_color(renderer->hdrPass, (float)renderer->background.r / 255.0f, (float)renderer->background.g / 255.0f,
            (float)renderer->background.b / 255.0f, (float)renderer->background.a / 255.0f)
        != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    /* The same background clears the off-screen passes: a target whose first frame drew nothing
       shows this frame's background rather than whatever its allocation handed over. */
    for (scope = 0u; scope < renderer->scopeCount; scope++) {
        if (xud_renderpass_set_clear_color(renderer->scopes[scope].pass, (float)renderer->background.r / 255.0f,
                (float)renderer->background.g / 255.0f, (float)renderer->background.b / 255.0f, (float)renderer->background.a / 255.0f)
            != XUD_OK) {
            return XUD_INVALID_ARG;
        }
    }

    result = xud_frame_draw(renderer->frame, loader, device, swapchain, renderer->pipeline, renderer->renderPass, renderer->framebuffers,
        renderer->framebufferCount, renderer->hdrPass, renderer->hdrFramebuffer, renderer->prepassPass, renderer->hdrPrepassFramebuffer,
        renderer->shadowPass, renderer->shadowFramebuffer, (renderer->bloomLive != 0u) ? renderer->bloomRows : NULL,
        (renderer->bloomLive != 0u) ? XUD_DESCRIPTOR_BLOOM_LEVELS : 0u, renderer->hdrSlot, renderer->scopes, renderer->scopeCount,
        renderer->screenBatch, renderer->descriptor, outImageIndex);
    if (result == XUD_OK) {
        renderer->drawCount++;
    }
    return result;
}

/*-----------------------------------------------------------------------------------------------
 * The resize - the one rung that acts on the recreate ask the frame loop left behind
 *-----------------------------------------------------------------------------------------------*/

/// See xud_renderer.h. The order is the open order run down and back up again, and it is the same
/// dependency direction the file-top note states: the frame's fences are what a presentation agai
/// nst the OLD images may still be waiting on, and the framebuffer rows' views are what name the
/// swapchain images the recreate below is about to retire, so both have to die BEFORE it.
/// See xud_renderer.h. The two answers a recreate ask does not need a rebuild for: the window has
/// no presentable extent at all (minimized - there is nothing to build a swapchain at), or the
/// swapchain is already at the extent the window's framebuffer is (the ask has been satisfied,
/// and rebuilding at the extent a swapchain already has would retire working images for nothing).
bool xud_renderer_recreate_needed(uint32_t fbWidth, uint32_t fbHeight, uint32_t swapchainWidth, uint32_t swapchainHeight) {
    if ((fbWidth == 0u) || (fbHeight == 0u)) {
        return false;
    }
    return ((fbWidth != swapchainWidth) || (fbHeight != swapchainHeight));
}

XudResult xud_renderer_resize(XudRenderer** ioRenderer, XudLoader* loader, const XudDevice* device, const XudDeviceCaps* caps,
    XudAllocator* alloc, const XudPhysicalDevice* physicalDevice, const XudWindow* window, XudSwapchain** ioSwapchain) {
    XudRenderer* renderer = NULL;
    XudSwapchain* swapchain = NULL;
    XudResult result = XUD_INVALID_ARG;
    uint32_t width = 0u;
    uint32_t height = 0u;

    if ((ioRenderer == NULL) || (*ioRenderer == NULL)) {
        return XUD_INVALID_ARG;
    }
    renderer = *ioRenderer;

    if ((loader == NULL) || (device == NULL) || (caps == NULL) || (alloc == NULL) || (physicalDevice == NULL) || (window == NULL)
        || (ioSwapchain == NULL) || (*ioSwapchain == NULL)) {
        return XUD_INVALID_ARG;
    }
    swapchain = *ioSwapchain;

    if (!loader->open) {
        return XUD_UNSUPPORTED;
    }
    if ((device->device == VK_NULL_HANDLE) || (swapchain->swapchain == VK_NULL_HANDLE)) {
        return XUD_INVALID_ARG;
    }
    if ((renderer->frame == NULL) || (renderer->framebuffers == NULL) || (renderer->renderPass == NULL) || (renderer->pipeline == NULL)
        || (renderer->hdrPass == NULL) || (renderer->hdrFramebuffer == NULL) || (renderer->prepassPass == NULL)
        || (renderer->hdrPrepassFramebuffer == NULL) || (renderer->batch == NULL)) {
        /* A renderer missing any rung this rebuilds is a renderer that was never brought up whole,
           and xud_renderer_open() is the call for that - a rebuild would otherwise have nothing to
           rebuild. */
        return XUD_INVALID_ARG;
    }
    if (renderer->frame->recreateWanted == 0u) {
        /* Nothing was asked for. The flag IS the ask, and the two swapchain-boundary codes are the
           only things that set it (xud_vulkan.h's funnel), so a draw that answered VK_SUCCESS leaves
           this rung to be called and to answer that the extent has not moved - which is what makes
           calling it every frame cheap rather than a rebuild every frame. */
        return XUD_ALREADY;
    }
    if (renderer->framebufferCount != swapchain->imageCount) {
        /* The rows and the fences no longer belong to the swapchain in hand - the same mismatch xud
           _frame_draw() reports below, read one rung up before a rebuild would make it worse. */
        return XUD_UNSUPPORTED;
    }

    /* The window's FRAMEBUFFER extent, never its requested one: GLFW scales wl.width / wl.height by
       the fractional scale or the buffer scale into wl.fbWidth / wl.fbHeight, and a swapchain whose
       extent is not that one is a swapchain the compositor calls out of date on every acquire, whi
       ch is what sets the flag again forever - an endless recreate loop rather than a resize. */
    result = xud_window_framebuffer_size(&width, &height, window);
    if (result != XUD_OK) {
        return result;
    }
    if (!xud_renderer_recreate_needed(width, height, swapchain->createInfo.imageExtent.width, swapchain->createInfo.imageExtent.height)) {
        /* Either the ask has already been satisfied - the swapchain is at the extent the window's
           framebuffer is, so the state asked for is the state present - or the window has no
           presentable extent at all, the 0/0 a Wayland compositor reports for a minimized window.
           Both are a skip rather than a refusal: there is nothing to build, so the flag is cleared
           (one ask cannot turn into a loop) and no rung is touched - the swapchain keeps the frame
           it already has, and a restore to a size the swapchain does not have re-sets the ask
           through the out-of-date code the next acquire answers with. */
        renderer->frame->recreateWanted = 0u;
        return XUD_ALREADY;
    }

    /* The teardown half, in the reverse of the open order, and a refusal in the middle of it is rep
       orted rather than half-run: xud_frame_close() refuses only when its fence guard cannot run,
       in which case nothing has been torn down and the renderer is still whole. */
    result = xud_frame_close(&renderer->frame, device, loader);
    if ((result != XUD_OK) && (result != XUD_ALREADY)) {
        return result;
    }
    result = xud_framebuffer_array_close(&renderer->framebuffers, device, loader);
    if ((result != XUD_OK) && (result != XUD_ALREADY)) {
        return result;
    }

    /* The swapchain is the context's, which is why this rung is handed the context's SLOT: the
       create carries oldSwapchain, the driver retires the old images as part of it, and the whole
       array of images is replaced - so every row and every fence index above it dies with it, whic
       h is what the two closes above just did. */
    result = xud_swapchain_recreate(ioSwapchain, loader, physicalDevice, window, device, width, height);
    if ((result != XUD_OK) && (result != XUD_ALREADY)) {
        return result;
    }
    swapchain = *ioSwapchain;

    /* The depth image at the new extent, because VUID-VkFramebufferCreateInfo-attachments-02702 re
       quires every attachment of one framebuffer at the framebuffer's own extent. The pass's handle
       and its attachment descriptions stay: only the image, its one view and the allocator's range
       they back change. */
    result = xud_renderpass_recreate_depth(&renderer->renderPass, loader, device, caps, alloc, swapchain);
    if ((result != XUD_OK) && (result != XUD_ALREADY)) {
        return result;
    }

    /* The scratch image at the new extent, and the same rule the screen pass follows keeps its
       handle: the PASS is not recreated - the scene pipelines are bound to it, and a new handle
       would orphan them - while image, view, depth row and framebuffer row all follow the extent.
       The slot releases first (its descriptor set names the OLD view), the pass's depth goes next
       (the rung runs its own device wait-idle guard, and that is what bounds any submission that
       still reads the colour image below), and the pair is re-bound into a slot of its own after
       the new view exists. */
    (void)xud_framebuffer_close(&renderer->hdrFramebuffer, device, loader);
    (void)xud_framebuffer_close(&renderer->hdrPrepassFramebuffer, device, loader); /* its depth
                                       attachment view is the OLD depth view from the row below,
                                       and both rows die before that view does */
    if (renderer->hdrSlotLive != 0u) {
        (void)xud_descriptor_release_texture_slot(renderer->descriptor, loader, device, renderer->hdrSlot);
        renderer->hdrSlotLive = 0u;
    }
    result = xud_renderpass_recreate_depth(&renderer->hdrPass, loader, device, caps, alloc, swapchain);
    if ((result != XUD_OK) && (result != XUD_ALREADY)) {
        return result;
    }
    /* The pre-pass SURVIVES the resize - an attachment description carries a format, not an
       extent, and its VkRenderPass handle is what the pre-pass pipeline is bound to - but its
       BORROWED depth row is stale the moment the recreate lands: re-state the handles from the
       source before anything reads the pre-pass's own depthImage field again (the frame loop's
       opening barrier does exactly that). */
    (void)xud_renderpass_borrow_depth(renderer->prepassPass, renderer->hdrPass);
    if ((renderer->hdrColorImage != VK_NULL_HANDLE) && (renderer->layout != NULL)) {
        (void)xud_layout_forget(renderer->layout, renderer->hdrColorImage);
    }
    if ((renderer->hdrColorView != VK_NULL_HANDLE) && (loader->deviceTable.vkDestroyImageView != NULL)) {
        loader->deviceTable.vkDestroyImageView(device->device, renderer->hdrColorView, NULL);
        renderer->hdrColorView = VK_NULL_HANDLE;
    }
    if ((renderer->hdrColorImage != VK_NULL_HANDLE) && (loader->deviceTable.vkDestroyImage != NULL)) {
        loader->deviceTable.vkDestroyImage(device->device, renderer->hdrColorImage, NULL);
        renderer->hdrColorImage = VK_NULL_HANDLE;
    }
    if ((renderer->hdrColorRange.memory != VK_NULL_HANDLE) && (alloc != NULL)) {
        (void)xud_allocator_release(alloc, &renderer->hdrColorRange);
    }
    renderer->hdrWidth = swapchain->createInfo.imageExtent.width;
    renderer->hdrHeight = swapchain->createInfo.imageExtent.height;
    result = xud_rendertarget_fill_color_info(&renderer->hdrColorImageInfo, renderer->hdrWidth, renderer->hdrHeight, renderer->hdrFormat);
    if (result != XUD_OK) {
        return result;
    }
    if (loader->deviceTable.vkCreateImage(device->device, &renderer->hdrColorImageInfo, NULL, &renderer->hdrColorImage) != VK_SUCCESS) {
        return XUD_VULKAN_ERROR;
    }
    {
        VkMemoryRequirements needs;
        memset(&needs, 0, sizeof needs);
        loader->deviceTable.vkGetImageMemoryRequirements(device->device, renderer->hdrColorImage, &needs);
        result = xud_allocator_acquire(&renderer->hdrColorRange, alloc, loader, device, caps, &needs);
        if (result != XUD_OK) {
            return result;
        }
    }
    result = xud_result_from_vk(loader->deviceTable.vkBindImageMemory(
        device->device, renderer->hdrColorImage, renderer->hdrColorRange.memory, renderer->hdrColorRange.offset));
    if (result != XUD_OK) {
        return result;
    }
    result = xud_layout_track(renderer->layout, renderer->hdrColorImage, renderer->hdrColorImageInfo.initialLayout);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_framebuffer_fill_view_info(&renderer->hdrColorViewInfo, renderer->hdrColorImage, renderer->hdrFormat);
    if (result != XUD_OK) {
        return result;
    }
    if (loader->deviceTable.vkCreateImageView(device->device, &renderer->hdrColorViewInfo, NULL, &renderer->hdrColorView) != VK_SUCCESS) {
        return XUD_VULKAN_ERROR;
    }
    result = xud_framebuffer_open(&renderer->hdrFramebuffer, loader, device, renderer->hdrPass, renderer->hdrColorImage,
        renderer->hdrFormat, renderer->hdrWidth, renderer->hdrHeight);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_framebuffer_open(&renderer->hdrPrepassFramebuffer, loader, device, renderer->prepassPass, renderer->hdrColorImage,
        renderer->hdrFormat, renderer->hdrWidth, renderer->hdrHeight);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_descriptor_bind_texture_slot(renderer->descriptor, loader, device, renderer->hdrColorView, renderer->hdrSampler,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &renderer->hdrSlot);
    if (result != XUD_OK) {
        return result;
    }
    renderer->hdrSlotLive = 1u;

    /* The bloom chain follows the scratch pair down and back up exactly as it did at bring-up:
       the rows (which name the OLD views), the tracker's forgets, the views, images and ranges
       go first, and the levels are rebuilt at the new extent behind a fresh binding-2 write -
       an array naming retired views is a resolve sampling freed handles. The pass handle stays
       put: the five chain pipelines are bound to it, the same rule the scratch pass above
       follows. The resize has closed the frame loop, so no draw can bind set 0 between the
       retire and the re-write. */
    xud_renderer_bloom_images_retire(renderer, loader, device, alloc, renderer->descriptor);
    result = xud_renderer_bloom_images_open(renderer, loader, device, caps, alloc, renderer->descriptor);
    if (result != XUD_OK) {
        return result;
    }

    /* And back up: the rows first (they are created FOR the pass, and their views name the NEW imag
       es), then the frame (its fences are one per image, so its count comes from the new swapchain
       and xud_frame_open() reads it there). The batch, the pipeline and the descriptor are not touc
       hed: none of them names an extent. */
    result = xud_framebuffer_array_open(&renderer->framebuffers, loader, device, renderer->renderPass, swapchain);
    if ((result != XUD_OK) && (result != XUD_ALREADY)) {
        return result;
    }
    result = xud_frame_open(&renderer->frame, loader, device, swapchain, renderer->layout);
    if ((result != XUD_OK) && (result != XUD_ALREADY)) {
        return result;
    }

    /* The count the draw entry above hands down is the array's own count, recorded rather than dedu
       ced - the same invariant xud_renderer_open() writes. */
    renderer->framebufferCount = swapchain->imageCount;

    /* The ask has been acted on, so the flag is cleared: leaving it set would make the NEXT draw's
       acquire answer leave a second rebuild queued behind a swapchain that is already at the right
       extent. */
    renderer->frame->recreateWanted = 0u;

    return XUD_OK;
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
    free(victim->screenBatch); /* the screen batch is retired by pointer-to-the-owned-one, never by
                                  `batch`: a renderer closed with a scope still open would otherwise
                                  free a target's batch and leak the screen's */
    victim->screenBatch = NULL;
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

/*-----------------------------------------------------------------------------------------------
 * The render-target scope entries - the batch swap and the frame's scope list
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_renderer_begin_target(XudRenderer* renderer, XudRenderPass* pass, const XudFramebuffer* framebuffer, XudBatch* batch) {
    uint32_t scope = 0u;

    if ((renderer == NULL) || (pass == NULL) || (framebuffer == NULL) || (batch == NULL)) {
        return XUD_INVALID_ARG;
    }
    for (scope = 0u; scope < renderer->scopeCount; scope++) {
        if (renderer->scopes[scope].framebuffer == framebuffer) {
            renderer->batch = batch;
            renderer->inTarget = 1u;
            renderer->activeFramebuffer = framebuffer;
            return XUD_OK;
        }
    }
    if (renderer->scopeCount >= XUD_FRAME_SCOPE_MAX) {
        return XUD_UNSUPPORTED;
    }
    renderer->scopes[renderer->scopeCount].pass = pass;
    renderer->scopes[renderer->scopeCount].framebuffer = framebuffer;
    renderer->scopes[renderer->scopeCount].batch = batch;
    renderer->scopeCount++;
    renderer->batch = batch;
    renderer->inTarget = 1u;
    renderer->activeFramebuffer = framebuffer;
    return XUD_OK;
}

XudResult xud_renderer_end_target(XudRenderer* renderer) {
    if (renderer == NULL) {
        return XUD_INVALID_ARG;
    }
    if (renderer->inTarget == 0u) {
        return XUD_ALREADY;
    }
    renderer->batch = renderer->screenBatch;
    renderer->inTarget = 0u;
    renderer->activeFramebuffer = NULL;
    return XUD_OK;
}

XudResult xud_renderer_reset_targets(XudRenderer* renderer) {
    if (renderer == NULL) {
        return XUD_INVALID_ARG;
    }
    renderer->scopeCount = 0u;
    renderer->batch = renderer->screenBatch;
    renderer->inTarget = 0u;
    renderer->activeFramebuffer = NULL;
    return XUD_OK;
}

XudResult xud_renderer_pixel_extent(
    const XudRenderer* renderer, uint32_t screenWidth, uint32_t screenHeight, uint32_t* outWidth, uint32_t* outHeight) {
    if ((renderer == NULL) || (outWidth == NULL) || (outHeight == NULL)) {
        return XUD_INVALID_ARG;
    }
    if ((renderer->inTarget != 0u) && (renderer->activeFramebuffer != NULL)) {
        /* The scope's own extent, read off the row's create-info: pixels inside a scope are the
           target's pixels, and the 256x256 target's full box is the same [0,1] box the window's
           full box is on screen. */
        *outWidth = renderer->activeFramebuffer->createInfo.width;
        *outHeight = renderer->activeFramebuffer->createInfo.height;
        return XUD_OK;
    }
    *outWidth = screenWidth;
    *outHeight = screenHeight;
    return XUD_OK;
}
