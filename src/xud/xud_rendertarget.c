/***********************************************************************************************
 * xud_rendertarget.c - the off-screen render target rung. See xud_rendertarget.h for the shape;
 * this file is the assembly order and its reverse, under the rule every object rung states: no
 * create before the destroy row that can retire it is checked, nothing left alive when a create
 * below it refuses.
 ***********************************************************************************************/

#include "xud_rendertarget.h"
#include "xud_texture.h" /* Required for: xud_texture_fill_sampler_info - the one sampler shape this rung reuses,
                            and the filter/address constants a target samples with */
#include "xud.h"
#include "xud_core.h" /* Required for: XudContext - the public entry points below answer through it, the way the texture rung's public pair does */
#include "xud_renderer.h" /* Required for: the scope calls the begin/end pair drives, and the active-scope reads the close pair guards on */
#include <stdlib.h>
#include <string.h>

XudResult xud_rendertarget_fill_color_info(VkImageCreateInfo* outCreateInfo, uint32_t width, uint32_t height, VkFormat format) {
    if (outCreateInfo == NULL) {
        return XUD_INVALID_ARG;
    }
    if ((width == 0u) || (height == 0u)) {
        return XUD_INVALID_ARG;
    }

    outCreateInfo->sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    outCreateInfo->pNext = NULL;
    outCreateInfo->flags = 0u;
    outCreateInfo->imageType = VK_IMAGE_TYPE_2D;
    outCreateInfo->format = format;
    outCreateInfo->extent.width = width;
    outCreateInfo->extent.height = height;
    outCreateInfo->extent.depth = 1u;
    outCreateInfo->mipLevels = 1u;
    outCreateInfo->arrayLayers = 1u;
    outCreateInfo->samples = VK_SAMPLE_COUNT_1_BIT;
    outCreateInfo->tiling = VK_IMAGE_TILING_OPTIMAL;
    outCreateInfo->usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    outCreateInfo->sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    outCreateInfo->queueFamilyIndexCount = 0u;
    outCreateInfo->pQueueFamilyIndices = NULL;
    /* Born UNDEFINED, the way the pass rung's depth image and the swapchain's images are: the
       layouts a create accepts are UNDEFINED and PREINITIALIZED, and the first scope's opening
       barrier is the legal discard out of UNDEFINED rather than a claim about bytes that were
       never written. The tracker learns the same word, so map and driver agree from the first
       frame, and the pass's own initialLayout is where the image first lands. */

    outCreateInfo->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    return XUD_OK;
}

/* Everything the record owns, in the one reverse order: the slot before the pair it names, the
   batch before the pass it draws under, the framebuffer before the views it attaches, the view
   and sampler before the image and memory the tracker and the allocator still remember. Every
   step tolerates its own absence, so the same function retires a half-built record. */
static void xud_rendertarget_retire(
    XudRenderTargetRecord* record, XudLoader* loader, const XudDevice* device, XudAllocator* alloc, XudDescriptor* descriptor) {
    if (record == NULL) {
        return;
    }
    if ((record->samplerSlotLive != 0u) && (descriptor != NULL)) {
        (void)xud_descriptor_release_texture_slot(descriptor, loader, device, record->samplerSlot);
        record->samplerSlotLive = 0u;
    }
    if (record->batch != NULL) {
        (void)xud_batch_close(&record->batch, device, loader, alloc);
    }
    if (record->framebuffer != NULL) {
        (void)xud_framebuffer_close(&record->framebuffer, device, loader);
    }
    if (record->pass != NULL) {
        (void)xud_renderpass_close(&record->pass, device, loader, alloc);
    }
    if ((record->sampler != VK_NULL_HANDLE) && (loader != NULL) && (device != NULL) && (loader->deviceTable.vkDestroySampler != NULL)) {
        loader->deviceTable.vkDestroySampler(device->device, record->sampler, NULL);
        record->sampler = VK_NULL_HANDLE;
    }
    if ((record->colorImage != VK_NULL_HANDLE) && (record->layout != NULL)) {
        (void)xud_layout_forget(record->layout, record->colorImage);
    }
    if ((record->colorView != VK_NULL_HANDLE) && (loader != NULL) && (device != NULL) && (loader->deviceTable.vkDestroyImageView != NULL)) {
        loader->deviceTable.vkDestroyImageView(device->device, record->colorView, NULL);
        record->colorView = VK_NULL_HANDLE;
    }
    if ((record->colorImage != VK_NULL_HANDLE) && (loader != NULL) && (device != NULL) && (loader->deviceTable.vkDestroyImage != NULL)) {
        loader->deviceTable.vkDestroyImage(device->device, record->colorImage, NULL);
        record->colorImage = VK_NULL_HANDLE;
    }
    if ((record->colorRange.memory != VK_NULL_HANDLE) && (alloc != NULL)) {
        (void)xud_allocator_release(alloc, &record->colorRange);
    }
}

XudResult xud_rendertarget_open(XudHandle* outHandle, XudHandleTable* table, XudLoader* loader, const XudDevice* device,
    const XudDeviceCaps* caps, XudAllocator* alloc, XudDescriptor* descriptor, XudLayoutTracker* layout, uint32_t width, uint32_t height,
    VkFormat colorFormat) {
    XudRenderTargetRecord* record = NULL;
    XudTextureData samplerData;
    VkMemoryRequirements needs;
    XudResult result = XUD_INVALID_ARG;
    XudHandle handle = XUD_HANDLE_NONE;
    uint32_t index = 0u;

    if ((outHandle == NULL) || (table == NULL) || (loader == NULL) || (descriptor == NULL) || (layout == NULL)) {
        return XUD_INVALID_ARG;
    }
    if ((device == NULL) || (caps == NULL) || (alloc == NULL)) {
        return XUD_UNSUPPORTED;
    }
    if (!loader->open) {
        return XUD_UNSUPPORTED;
    }
    if (device->device == VK_NULL_HANDLE) {
        return XUD_INVALID_ARG;
    }
    if ((width == 0u) || (height == 0u) || (colorFormat == VK_FORMAT_UNDEFINED)) {
        return XUD_INVALID_ARG;
    }
    if ((loader->deviceTable.vkCreateImage == NULL) || (loader->deviceTable.vkDestroyImage == NULL)
        || (loader->deviceTable.vkCreateImageView == NULL) || (loader->deviceTable.vkDestroyImageView == NULL)
        || (loader->deviceTable.vkCreateSampler == NULL) || (loader->deviceTable.vkDestroySampler == NULL)
        || (loader->deviceTable.vkGetImageMemoryRequirements == NULL) || (loader->deviceTable.vkBindImageMemory == NULL)) {
        return XUD_UNSUPPORTED;
    }
    if (!xud_loader_device_bound(loader, device->device)) {
        return XUD_UNSUPPORTED;
    }

    record = calloc(1u, sizeof(*record));
    if (record == NULL) {
        return XUD_OUT_OF_MEMORY;
    }
    record->layout = layout;
    record->width = width;
    record->height = height;

    /* 1. the colour image, then its memory, then the tracker's first word for it - the create,
       carve, bind, register order every image in this tree has used since the render pass rung
       made the first one. The initialLayout the tracker learns is the layout the pass opens in:
       the first scope's opening barrier is then the honest no-op it should be. */
    result = xud_rendertarget_fill_color_info(&record->colorImageInfo, width, height, colorFormat);
    if (result != XUD_OK) {
        free(record);
        return result;
    }
    if (loader->deviceTable.vkCreateImage(device->device, &record->colorImageInfo, NULL, &record->colorImage) != VK_SUCCESS) {
        free(record);
        return XUD_VULKAN_ERROR;
    }
    memset(&needs, 0, sizeof needs);
    loader->deviceTable.vkGetImageMemoryRequirements(device->device, record->colorImage, &needs);
    if (needs.size == 0u) {
        xud_rendertarget_retire(record, loader, device, alloc, NULL);
        free(record);
        return XUD_VULKAN_ERROR;
    }
    result = xud_allocator_acquire(&record->colorRange, alloc, loader, device, caps, &needs);
    if (result != XUD_OK) {
        xud_rendertarget_retire(record, loader, device, alloc, NULL);
        free(record);
        return result;
    }
    result = xud_result_from_vk(
        loader->deviceTable.vkBindImageMemory(device->device, record->colorImage, record->colorRange.memory, record->colorRange.offset));
    if (result != XUD_OK) {
        xud_rendertarget_retire(record, loader, device, alloc, NULL);
        free(record);
        return result;
    }
    result = xud_layout_track(layout, record->colorImage, record->colorImageInfo.initialLayout);
    if (result != XUD_OK) {
        xud_rendertarget_retire(record, loader, device, alloc, NULL);
        free(record);
        return result;
    }

    /* 2. the view the framebuffer attaches and the sampler-array entry names - one view, two
       readers, the same handle, so the entry and the attachment cannot name different things. */
    result = xud_framebuffer_fill_view_info(&record->colorViewInfo, record->colorImage, colorFormat);
    if (result != XUD_OK) {
        xud_rendertarget_retire(record, loader, device, alloc, NULL);
        free(record);
        return result;
    }
    if (loader->deviceTable.vkCreateImageView(device->device, &record->colorViewInfo, NULL, &record->colorView) != VK_SUCCESS) {
        xud_rendertarget_retire(record, loader, device, alloc, NULL);
        free(record);
        return XUD_VULKAN_ERROR;
    }

    /* 3. the sampler: the texture rung's own shape - linear, clamped - because a render target is
       sampled exactly the way an uploaded texture is and nothing about it asks for another. */
    memset(&samplerData, 0, sizeof samplerData);
    samplerData.filter = XUD_TEXTURE_FILTER_LINEAR;
    samplerData.addressMode = XUD_TEXTURE_ADDRESS_CLAMP_TO_EDGE;
    result = xud_texture_fill_sampler_info(&samplerData, &record->samplerInfo);
    if (result != XUD_OK) {
        xud_rendertarget_retire(record, loader, device, alloc, NULL);
        free(record);
        return result;
    }
    if (loader->deviceTable.vkCreateSampler(device->device, &record->samplerInfo, NULL, &record->sampler) != VK_SUCCESS) {
        xud_rendertarget_retire(record, loader, device, alloc, NULL);
        free(record);
        return XUD_VULKAN_ERROR;
    }

    /* 4. the pass rung's off-screen variant: the depth image of THIS extent, registered with the
       tracker by the pass itself, and the colour attachment's finalLayout SHADER_READ_ONLY. */
    result = xud_renderpass_open_offscreen(&record->pass, loader, device, caps, alloc, colorFormat, width, height, layout);
    if (result != XUD_OK) {
        xud_rendertarget_retire(record, loader, device, alloc, NULL);
        free(record);
        return result;
    }

    /* 5. the one framebuffer row - colour view above, depth view the pass's. */
    result = xud_framebuffer_open(&record->framebuffer, loader, device, record->pass, record->colorImage, colorFormat, width, height);
    if (result != XUD_OK) {
        xud_rendertarget_retire(record, loader, device, alloc, NULL);
        free(record);
        return result;
    }

    /* 6. the batch, and only now the handle slot: a table entry that names a record nothing has
       failed against from here on. */
    result = xud_batch_open(&record->batch, loader, device, caps, alloc);
    if (result != XUD_OK) {
        xud_rendertarget_retire(record, loader, device, alloc, NULL);
        free(record);
        return result;
    }
    result = xud_handle_alloc(&handle, table, XUD_HANDLE_KIND_RENDERTEXTURE);
    if (result != XUD_OK) {
        xud_rendertarget_retire(record, loader, device, alloc, NULL);
        free(record);
        return result;
    }

    /* 7. the sampler-array slot, LAST: the set the bind writes names this target's pair from the
       instant it is written, and every object that pair names was created above. */
    result = xud_descriptor_bind_texture_slot(
        descriptor, loader, device, record->colorView, record->sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &record->samplerSlot);
    if (result != XUD_OK) {
        (void)xud_handle_free(table, handle);
        xud_rendertarget_retire(record, loader, device, alloc, NULL);
        free(record);
        return result;
    }
    record->samplerSlotLive = 1u;

    index = handle.index;
    result = xud_handle_record_set(table, XUD_HANDLE_KIND_RENDERTEXTURE, index, record);
    if (result != XUD_OK) {
        (void)xud_rendertarget_close(handle, table, loader, device, alloc, descriptor);
        return result;
    }
    *outHandle = handle;
    return XUD_OK;
}

XudResult xud_rendertarget_close(
    XudHandle handle, XudHandleTable* table, XudLoader* loader, const XudDevice* device, XudAllocator* alloc, XudDescriptor* descriptor) {
    XudRenderTargetRecord* record = NULL;
    XudResult result = XUD_INVALID_ARG;

    if ((table == NULL) || (loader == NULL) || (device == NULL) || (alloc == NULL)) {
        return XUD_INVALID_ARG;
    }
    result = xud_handle_validate(table, handle, XUD_HANDLE_KIND_RENDERTEXTURE);
    if (result != XUD_OK) {
        return result;
    }
    if (!loader->open) {
        return XUD_UNSUPPORTED;
    }
    if ((loader->deviceTable.vkDestroyImage == NULL) || (loader->deviceTable.vkDestroyImageView == NULL)
        || (loader->deviceTable.vkDestroySampler == NULL)) {
        return XUD_UNSUPPORTED;
    }
    if (!xud_loader_device_bound(loader, device->device)) {
        return XUD_UNSUPPORTED;
    }
    result = xud_handle_record_get(table, XUD_HANDLE_KIND_RENDERTEXTURE, handle.index, (void**)&record);
    if (result != XUD_OK) {
        return result;
    }
    if (record == NULL) {
        return XUD_INVALID_ARG;
    }

    xud_rendertarget_retire(record, loader, device, alloc, descriptor);
    free(record);
    result = xud_handle_record_set(table, XUD_HANDLE_KIND_RENDERTEXTURE, handle.index, NULL);
    if (result != XUD_OK) {
        return result;
    }
    return xud_handle_free(table, handle);
}

XudResult xud_rendertarget_close_all(XudHandleTable* table, XudLoader* loader, const XudDevice* device, XudAllocator* alloc,
    XudDescriptor* descriptor, uint32_t* outClosed) {
    XudRenderTargetRecord* record = NULL;
    XudResult first = XUD_OK;
    XudResult guard = XUD_OK;
    uint32_t index = 0u;

    if ((table == NULL) || (loader == NULL) || (device == NULL) || (alloc == NULL) || (outClosed == NULL)) {
        return XUD_INVALID_ARG;
    }
    *outClosed = 0u;
    if (!xud_loader_is_open(loader)) {
        return XUD_UNSUPPORTED;
    }
    if ((loader->deviceTable.vkDestroyImage == NULL) || (loader->deviceTable.vkDestroyImageView == NULL)
        || (loader->deviceTable.vkDestroySampler == NULL)) {
        return XUD_UNSUPPORTED;
    }

    /* One boundary for the whole walk: a submission in flight may still be reading, writing, or
       copying any target's image, and the wait ahead of the first destroy is the same boundary the
       walk would otherwise ask for once per row. */
    guard = xud_device_wait_idle(device, loader);
    if (guard == XUD_UNSUPPORTED) {
        return XUD_UNSUPPORTED;
    }
    if (guard != XUD_OK) {
        first = guard;
    }

    for (index = 0u; index < table->slotCount[XUD_HANDLE_KIND_RENDERTEXTURE]; index++) {
        if (xud_handle_record_get(table, XUD_HANDLE_KIND_RENDERTEXTURE, index, (void**)&record) != XUD_OK) {
            return XUD_INVALID_ARG;
        }
        if (record == NULL) {
            continue;
        }
        if (xud_handle_record_set(table, XUD_HANDLE_KIND_RENDERTEXTURE, index, NULL) != XUD_OK) {
            return XUD_INVALID_ARG;
        }
        xud_rendertarget_retire(record, loader, device, alloc, descriptor);
        free(record);
        (*outClosed)++;
    }

    return first;
}

XudResult xud_rendertarget_get(const XudHandleTable* table, XudHandle handle, const XudRenderTargetRecord** outRecord) {
    XudRenderTargetRecord* record = NULL;
    XudResult result = XUD_INVALID_ARG;

    if ((table == NULL) || (outRecord == NULL)) {
        return XUD_INVALID_ARG;
    }
    result = xud_handle_validate(table, handle, XUD_HANDLE_KIND_RENDERTEXTURE);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_handle_record_get(table, XUD_HANDLE_KIND_RENDERTEXTURE, handle.index, (void**)&record);
    if ((result != XUD_OK) || (record == NULL)) {
        return (result != XUD_OK) ? result : XUD_INVALID_ARG;
    }
    *outRecord = record;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The public pair and the scope calls - the context's view of the rung, the same shape the
 * texture rung's public half uses: validate the context, reach the rungs below through it
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_create_render_texture(XudHandle* outTarget, XudContext* context, uint32_t width, uint32_t height) {
    if ((outTarget == NULL) || (context == NULL)) {
        return XUD_INVALID_ARG;
    }
    if ((context->handleTable == NULL) || (context->loader == NULL) || (context->device == NULL) || (context->allocator == NULL)
        || (context->descriptor == NULL) || (context->swapchain == NULL) || (context->layout == NULL)) {
        /* A context missing any rung this create assembles through never completed bring-up - the
           same answer every other public entry point gives for the same reason. */
        return XUD_UNSUPPORTED;
    }

    /* The colour format is the swapchain's: a target that ends up drawn to the screen is drawn
       through the same blend state the screen pass runs, and a format the screen does not use
       would be a second format the pipelines had to be built for. */
    return xud_rendertarget_open(outTarget, context->handleTable, context->loader, context->device, &context->physicalDevice->caps,
        context->allocator, context->descriptor, context->layout, width, height, context->swapchain->createInfo.imageFormat);
}

XudResult xud_close_render_texture(XudContext* context, XudHandle target) {
    const XudRenderTargetRecord* record = NULL;

    if (context == NULL) {
        return XUD_INVALID_ARG;
    }
    if ((context->handleTable == NULL) || (context->loader == NULL) || (context->device == NULL) || (context->allocator == NULL)
        || (context->descriptor == NULL)) {
        return XUD_UNSUPPORTED;
    }

    if ((context->renderer != NULL) && (xud_rendertarget_get(context->handleTable, target, &record) == XUD_OK)) {
        if ((record != NULL) && (context->renderer->inTarget != 0u) && (context->renderer->activeFramebuffer == record->framebuffer)) {
            /* The renderer's draw paths name this target's batch right now; closing the record
               under them would leave the next draw appending to freed memory. The scope closes
               first - xud_end_texture_mode() is that call. */
            return XUD_UNSUPPORTED;
        }
    }

    return xud_rendertarget_close(target, context->handleTable, context->loader, context->device, context->allocator, context->descriptor);
}

XudResult xud_begin_texture_mode(XudContext* context, XudHandle target) {
    const XudRenderTargetRecord* record = NULL;
    XudResult result = XUD_INVALID_ARG;

    if (context == NULL) {
        return XUD_INVALID_ARG;
    }
    if ((context->renderer == NULL) || (context->handleTable == NULL)) {
        return XUD_UNSUPPORTED;
    }

    result = xud_rendertarget_get(context->handleTable, target, &record);
    if (result != XUD_OK) {
        return result;
    }
    if ((context->renderer->inTarget != 0u) && (context->renderer->activeFramebuffer != record->framebuffer)) {
        /* One scope at a time: the draw paths hold one batch pointer, so a second target opened
           over the first would silently steal it rather than nest. */
        return XUD_ALREADY;
    }

    return xud_renderer_begin_target(context->renderer, (XudRenderPass*)record->pass, record->framebuffer, record->batch);
}

XudResult xud_end_texture_mode(XudContext* context) {
    if (context == NULL) {
        return XUD_INVALID_ARG;
    }
    if (context->renderer == NULL) {
        return XUD_UNSUPPORTED;
    }
    return xud_renderer_end_target(context->renderer);
}
