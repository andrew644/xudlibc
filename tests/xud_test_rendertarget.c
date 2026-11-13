/***********************************************************************************************
 *
 *   xud_test_rendertarget.c - the off-screen render target rung and its public API above it
 *
 *   Owns : the colour image's create-info, the guards of the rung's four entry points, the record
 *          a RENDERTEXTURE-kind slot carries, the scope's begin/end semantics on the renderer, and
 *          the public quartet xud_create_render_texture()/xud_close_render_texture()/
 *          xud_begin_texture_mode()/xud_end_texture_mode() - and nothing else: the pass's own shape
 *          is the renderpass area's, the scope recording inside the frame is the frame area's, and
 *          the sampler-array's free list is the descriptor area's.
 *
 *   Layer: tests, above the texture area and below the core area. The pure half runs with no
 *   driver present. The GPU half runs the quartet against a real context, then closes the gap
 *   no other rung can show: it draws a solid box into a 64x64 target, samples the target onto the
 *   screen with xud_draw_texture(), presents, and copies the presented swapchain image back - the
 *   box's pixels inside the sampled box and the background's outside it are the proof that a pass
 *   that is not the screen pass rendered, that its image became sampleable through its finalLayout,
 *   and that the sampler-array entry resolved to it in the same frame.
 *
 *   Deterministic: fixed extents, fixed colors, fixed loop counts, no time, no randomness, no
 *   filesystem.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include <math.h>   /* Required for: powf, the sRGB transfer function the readback's expected bytes are stated through */
#include <string.h> /* Required for: memset, the zeroed infos the readback stamps */

#include "xud.h" /* Required for: the public quartet, XudHandle, XUD_HANDLE_KIND_RENDERTEXTURE */
#include "xud_allocator.h" /* Required for: XudMemoryRange, xud_allocator_acquire_host_visible - the readback's block */
#include "xud_batch.h"     /* Required for: xud_batch_stats - the target batch's own vertex count */
#include "xud_core.h"      /* Required for: struct XudContext, whose fields the GPU half observes */
#include "xud_descriptor.h" /* Required for: the samplerSlotMask, whose bits the bind and release move */
#include "xud_handle.h"     /* Required for: xud_handle_table_open/alloc/free/close - the pure half's table */
#include "xud_layout.h" /* Required for: xud_layout_get - the tracker's word for the target's colour image */
#include "xud_renderer.h"     /* Required for: the renderer fields the scope moves - batch, inTarget, scopeCount */
#include "xud_rendertarget.h" /* Required for: the rung under test */
#include "xud_test_harness.h"

static void xud_test_rendertarget_pure(void) {
    VkImageCreateInfo info;
    XudHandleTable* table = NULL;
    XudHandle handle = XUD_HANDLE_NONE;
    const XudRenderTargetRecord* record = NULL;

    memset(&info, 0, sizeof info);

    xud_check(xud_rendertarget_fill_color_info(NULL, 4u, 4u, VK_FORMAT_B8G8R8A8_UNORM) == XUD_INVALID_ARG, "rt/fill-null",
        "the filler refuses to stamp nowhere");
    xud_check(xud_rendertarget_fill_color_info(&info, 0u, 4u, VK_FORMAT_B8G8R8A8_UNORM) == XUD_INVALID_ARG, "rt/fill-zero-width",
        "a zero-width extent is a zero-area attachment, refused");
    xud_check(xud_rendertarget_fill_color_info(&info, 4u, 0u, VK_FORMAT_B8G8R8A8_UNORM) == XUD_INVALID_ARG, "rt/fill-zero-height",
        "and the same for the other axis");
    xud_check(xud_rendertarget_fill_color_info(&info, 64u, 32u, VK_FORMAT_B8G8R8A8_SRGB) == XUD_OK, "rt/fill-ok", "the shape stamps");
    xud_check((info.imageType == VK_IMAGE_TYPE_2D) && (info.mipLevels == 1u) && (info.arrayLayers == 1u)
            && (info.samples == VK_SAMPLE_COUNT_1_BIT) && (info.tiling == VK_IMAGE_TILING_OPTIMAL),
        "rt/fill-shape", "one subresource of a 2D optimal image, the way every attachment image in this tree is made");
    xud_check(
        (info.extent.width == 64u) && (info.extent.height == 32u) && (info.extent.depth == 1u) && (info.format == VK_FORMAT_B8G8R8A8_SRGB),
        "rt/fill-extent", "and the caller's extent and format are the ones stamped");
    xud_check(info.usage == (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT), "rt/fill-usage",
        "the two usages that define the rung: the pass writes the image and the sampler array reads it");
    xud_check(info.initialLayout == VK_IMAGE_LAYOUT_UNDEFINED, "rt/fill-initial-layout",
        "the image is born UNDEFINED - the only layout a create may name for bytes never written, the same birth the depth image has");

    /* The guards, against a live table rather than a NULL one: the kind check and the free-slot
       check are only observable once the slot machinery exists, and the table is heap-only. */
    xud_check(xud_handle_table_open(&table) == XUD_OK, "rt/table-open", "a bare handle table opens for the guard checks");
    if (table == NULL) {
        return;
    }
    xud_check(xud_handle_alloc(&handle, table, XUD_HANDLE_KIND_RENDERTEXTURE) == XUD_OK, "rt/handle-alloc",
        "a RENDERTEXTURE slot allocates - the kind is inside the table's row count");
    xud_check(xud_rendertarget_open(NULL, table, NULL, NULL, NULL, NULL, NULL, NULL, 4u, 4u, VK_FORMAT_B8G8R8A8_UNORM) == XUD_INVALID_ARG,
        "rt/open-null-out", "the open refuses to write a handle nowhere");
    xud_check(xud_rendertarget_open(&handle, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 4u, 4u, VK_FORMAT_B8G8R8A8_UNORM) == XUD_INVALID_ARG,
        "rt/open-null-table", "and with no table to put it in");
    xud_check(xud_rendertarget_close(handle, table, NULL, NULL, NULL, NULL) == XUD_INVALID_ARG, "rt/close-null-loader",
        "the close refuses a loader it would retire through");
    xud_check(xud_rendertarget_close(XUD_HANDLE_NONE, table, NULL, NULL, NULL, NULL) == XUD_INVALID_ARG, "rt/close-none-handle",
        "and the handle that names nothing");
    xud_check(xud_rendertarget_get(table, handle, &record) == XUD_INVALID_ARG, "rt/get-empty-record",
        "a live slot with no record hung off it is not a live render target");
    xud_check(xud_rendertarget_get(table, XUD_HANDLE_NONE, &record) == XUD_INVALID_ARG, "rt/get-none-handle",
        "and the null handle is refused before the table is read");
    xud_check(xud_rendertarget_close_all(NULL, NULL, NULL, NULL, NULL, NULL) == XUD_INVALID_ARG, "rt/close-all-null",
        "the whole-kind walk refuses its NULL arguments");
    (void)xud_handle_free(table, handle);
    (void)xud_handle_table_close(&table);
}

/* The GPU half's extent: the window's, and the target's. 128 leaves the readback's probe points
   - inside and outside the 64x64 sampled box - far from every edge the compositor might clip. */
#define XUD_TEST_RT_WINDOW 128u
#define XUD_TEST_RT_TARGET 64u

static void xud_test_rendertarget_gpu(void) {
    XudContext* context = NULL;
    XudResult result = XUD_OK;
    XudHandle target = XUD_HANDLE_NONE;
    XudHandle second = XUD_HANDLE_NONE;
    const XudRenderTargetRecord* record = NULL;
    const XudRenderTargetRecord* secondRecord = NULL;
    XudBatchStats stats;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t live = 0u;
    uint32_t frame = 0u;
    uint32_t savedSlot = 0u; /* the slot and the image the record named, saved before its close:
                                the post-close checks below read these, not the freed record */
    VkImage savedImage = VK_NULL_HANDLE;
    uint32_t targetVertices = 0u;
    uint32_t targetIndices = 0u;
    uint32_t targetCapacity = 0u;
    uint32_t targetDraws = 0u;
    VkDeviceSize targetStaging = 0u;

    result = xud_init_context(&context, XUD_TEST_RT_WINDOW, XUD_TEST_RT_WINDOW, "xud_test");
    xud_check(result == XUD_OK, "rt-gpu/bringup", "the context completes, so the target rung has a device under it");
    if (context == NULL) {
        return;
    }

    result = xud_create_render_texture(&target, context, XUD_TEST_RT_TARGET, XUD_TEST_RT_TARGET);
    xud_check(result == XUD_OK, "rt-gpu/create", "a 64x64 target is created against the live context");
    xud_check(xud_handle_table_live_count(context->handleTable, XUD_HANDLE_KIND_RENDERTEXTURE, &live) == XUD_OK, "rt-gpu/live-query",
        "the kind's live count answers");
    xud_check(live == 1u, "rt-gpu/live-one", "and one record is live");
    xud_check(xud_rendertarget_get(context->handleTable, target, &record) == XUD_OK, "rt-gpu/get", "the record reads back");
    if (record == NULL) {
        (void)xud_close_context(&context);
        return;
    }
    xud_check((record->width == XUD_TEST_RT_TARGET) && (record->height == XUD_TEST_RT_TARGET), "rt-gpu/extent",
        "the record carries the caller's extent");
    savedSlot = record->samplerSlot;
    savedImage = record->colorImage;
    xud_check((record->colorImage != VK_NULL_HANDLE) && (record->colorView != VK_NULL_HANDLE) && (record->sampler != VK_NULL_HANDLE)
            && (record->pass != NULL) && (record->framebuffer != NULL) && (record->batch != NULL),
        "rt-gpu/objects", "every object the record owns is live");
    xud_check((record->samplerSlotLive == 1u) && (record->samplerSlot != 0u), "rt-gpu/slot-bound",
        "the record's sampler-array entry is bound, and slot 0 - the reserved white default - is not it");
    xud_check((context->descriptor->samplerSlotMask & (1u << record->samplerSlot)) != 0u, "rt-gpu/slot-mask",
        "and the descriptor's free list holds the slot as taken");
    xud_check(xud_layout_get(&layout, context->layout, record->colorImage) == XUD_OK, "rt-gpu/tracked",
        "the layout tracker knows the colour image");
    xud_check(layout == VK_IMAGE_LAYOUT_UNDEFINED, "rt-gpu/tracked-initial",
        "and it carries the UNDEFINED the create stamped - the first frame's opening barrier is the legal discard out of it");

    /* The scope's semantics, with no frame in flight: the renderer's pointers move and come back. */
    xud_check(xud_begin_texture_mode(context, target) == XUD_OK, "rt-gpu/begin", "the scope opens");
    xud_check((context->renderer->inTarget == 1u) && (context->renderer->batch == record->batch)
            && (context->renderer->activeFramebuffer == record->framebuffer),
        "rt-gpu/begin-moved", "and the draw paths are on the target's batch");
    xud_check(xud_begin_texture_mode(context, target) == XUD_OK, "rt-gpu/begin-same", "re-opening the same target is the same scope");
    xud_check(context->renderer->scopeCount == 1u, "rt-gpu/begin-dedup", "and it did not add a second row");
    xud_check(xud_create_render_texture(&second, context, 32u, 32u) == XUD_OK, "rt-gpu/create-second", "a second target is created");
    xud_check(xud_rendertarget_get(context->handleTable, second, &secondRecord) == XUD_OK, "rt-gpu/get-second", "and reads back");
    xud_check(xud_begin_texture_mode(context, second) == XUD_ALREADY, "rt-gpu/begin-nested-refused",
        "a second target opened over the first is refused, not a silent steal of the batch pointer");
    xud_check(xud_close_render_texture(context, target) == XUD_UNSUPPORTED, "rt-gpu/close-active-refused",
        "and the open target cannot be closed out from under the renderer");
    xud_check(xud_close_render_texture(context, second) == XUD_OK, "rt-gpu/close-inactive",
        "but the inactive one closes - its record was no one's active scope");
    xud_check(xud_end_texture_mode(context) == XUD_OK, "rt-gpu/end", "the scope closes");
    xud_check((context->renderer->inTarget == 0u) && (context->renderer->batch == context->renderer->screenBatch), "rt-gpu/end-back",
        "and the draw paths are back on the screen batch");
    xud_check(xud_end_texture_mode(context) == XUD_ALREADY, "rt-gpu/end-again", "and there is only one scope to close");

    /* The frames: every one draws a solid box into the target and samples the target onto the
       screen. One frame per swapchain image at least, so whichever image the readback's acquire
       returns holds the same picture. */
    for (frame = 0u; frame < (context->swapchain->imageCount + 1u); frame++) {
        xud_check(xud_begin_drawing(context) == XUD_OK, "rt-gpu/frame-begin", "the frame boundary opens");
        xud_check(context->renderer->scopeCount == 0u, "rt-gpu/frame-scopes-reset", "and the boundary emptied the scope list");
        xud_check(xud_clear_background(context, (XudColor) { .r = 255u, .g = 0u, .b = 0u, .a = 255u }) == XUD_OK, "rt-gpu/clear-red",
            "the frame's background - every pass's clear - is red");
        xud_check(xud_begin_texture_mode(context, target) == XUD_OK, "rt-gpu/frame-begin-scope", "the target's scope opens again");
        xud_check(xud_draw_rectangle(context, 0, 0, (int32_t)XUD_TEST_RT_TARGET, (int32_t)XUD_TEST_RT_TARGET,
                      (XudColor) { .r = 0u, .g = 0u, .b = 255u, .a = 255u })
                == XUD_OK,
            "rt-gpu/frame-box", "the solid box is appended to the target's batch");
        xud_check((xud_batch_stats(record->batch, &targetVertices, &targetIndices, &targetCapacity, &targetDraws, &targetStaging) == XUD_OK)
                && (targetVertices == 4u) && (targetDraws == 1u),
            "rt-gpu/target-stats-box", "the box landed in the TARGET's batch - four vertices, one draw row - not the screen's");
        xud_check(xud_end_texture_mode(context) == XUD_OK, "rt-gpu/frame-end-scope", "the scope closes");
        xud_check(xud_draw_texture(context, target, 0, 0, (XudColor) { .r = 255u, .g = 255u, .b = 255u, .a = 255u }) == XUD_OK,
            "rt-gpu/frame-sample", "and the screen batch samples the target through its own handle");
        xud_check((xud_get_batch_stats(&stats, context) == XUD_OK) && (stats.vertexCount == 4u), "rt-gpu/main-stats-quad",
            "the screen batch holds the sampled box's four vertices while the frame is still open");
        xud_check(xud_end_drawing(context) == XUD_OK, "rt-gpu/frame-end", "the frame records, draws and presents whole");
    }

    xud_check(context->renderer->scopeCount == 1u, "rt-gpu/scope-row-lived", "the frame kept its one scope row");
    xud_check((xud_batch_stats(record->batch, &targetVertices, &targetIndices, &targetCapacity, &targetDraws, &targetStaging) == XUD_OK)
            && (targetVertices == 0u) && (targetDraws == 0u),
        "rt-gpu/target-reset", "and the frame ended by resetting the target's batch, the way it resets the screen's");
    xud_check(xud_layout_get(&layout, context->layout, record->colorImage) == XUD_OK, "rt-gpu/tracked-after",
        "the tracker still knows the image");
    xud_check(layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, "rt-gpu/tracked-sampled",
        "and the image is in the layout the pass's finalLayout moved it to - the barrier that makes it sampleable ran");
    xud_check((xud_get_batch_stats(&stats, context) == XUD_OK) && (stats.vertexCount == 0u), "rt-gpu/main-reset",
        "and a completed frame ended by resetting the SCREEN's batch too - both batches replay from empty next frame");

    xud_check(xud_close_render_texture(context, target) == XUD_OK, "rt-gpu/close", "the target closes after its last frame");
    xud_check((context->descriptor->samplerSlotMask & (1u << savedSlot)) == 0u, "rt-gpu/slot-released",
        "and its sampler-array slot - saved before the close - is back on the free list");
    xud_check(xud_layout_get(&layout, context->layout, savedImage) != XUD_OK, "rt-gpu/tracked-forgotten",
        "and the tracker forgot the image with it");

    result = xud_close_context(&context);
    xud_check((result == XUD_OK) || (result == XUD_ALREADY), "rt-gpu/close-context", "and the context closes with the rung inside it");
    xud_check(context == NULL, "rt-gpu/context-cleared", "the pointer is cleared");
}

/* The readback: the same presented-image-copy the texture area's readback runs (acquire, barrier
   to TRANSFER_SRC, copy to a host-visible block, fence), but the expected pixels come from the
   render target's path rather than a directly-sampled sheet: inside the 64x64 box the target held,
   the blue of the box drawn INSIDE the scope; outside it, the red of the screen's own clear. If
   the off-screen pass never rendered, the barrier never moved the image to sampleable, or the
   scope's draws went to the wrong batch, one of the two probes is the wrong colour. */
#define XUD_TEST_RT_TOLERANCE 2u

static uint32_t xud_test_rt_srgb_byte(uint8_t value) {
    const float c = (float)value / 255.0f;
    const float e = (c <= 0.0031308f) ? (12.92f * c) : (1.055f * powf(c, 1.0f / 2.4f) - 0.055f);

    return (uint32_t)(e * 255.0f + 0.5f);
}

static void xud_test_rendertarget_readback(void) {
    XudContext* context = NULL;
    XudResult result = XUD_OK;
    XudHandle target = XUD_HANDLE_NONE;
    VkResult vk = VK_SUCCESS;
    VkBuffer readback = VK_NULL_HANDLE;
    XudMemoryRange readbackRange;
    VkMemoryRequirements readbackReq;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    uint8_t* pixels = NULL;
    uint32_t acquired = 0u;
    uint32_t frame = 0u;
    uint32_t blueByte = xud_test_rt_srgb_byte(255u);
    uint32_t zeroByte = xud_test_rt_srgb_byte(0u);

    result = xud_init_context(&context, XUD_TEST_RT_WINDOW, XUD_TEST_RT_WINDOW, "xud_test");
    xud_check(result == XUD_OK, "rt-readback/bringup", "the readback context completes");
    if (context == NULL) {
        return;
    }
    result = xud_create_render_texture(&target, context, XUD_TEST_RT_TARGET, XUD_TEST_RT_TARGET);
    xud_check(result == XUD_OK, "rt-readback/create", "the 64x64 target is created");

    /* One frame per swapchain image plus one: every presentable image ends up holding the picture,
       so whichever one the acquire below returns carries the same bytes. */
    for (frame = 0u; frame < (context->swapchain->imageCount + 1u); frame++) {
        (void)xud_begin_drawing(context);
        (void)xud_clear_background(context, (XudColor) { .r = 255u, .g = 0u, .b = 0u, .a = 255u });
        (void)xud_begin_texture_mode(context, target);
        (void)xud_draw_rectangle(
            context, 0, 0, (int32_t)XUD_TEST_RT_TARGET, (int32_t)XUD_TEST_RT_TARGET, (XudColor) { .r = 0u, .g = 0u, .b = 255u, .a = 255u });
        (void)xud_end_texture_mode(context);
        (void)xud_draw_texture(context, target, 0, 0, (XudColor) { .r = 255u, .g = 255u, .b = 255u, .a = 255u });
        if (xud_end_drawing(context) != XUD_OK) {
            break;
        }
    }

    vk = context->loader->deviceTable.vkAcquireNextImageKHR(
        context->device->device, context->swapchain->swapchain, UINT64_MAX, VK_NULL_HANDLE, VK_NULL_HANDLE, &acquired);
    xud_check(vk == VK_SUCCESS, "rt-readback/acquired", "a presented image is acquired for the copy");
    if (vk != VK_SUCCESS) {
        (void)xud_close_context(&context);
        return;
    }
    {
        const uint32_t width = context->swapchain->createInfo.imageExtent.width;
        const uint32_t height = context->swapchain->createInfo.imageExtent.height;
        const VkFormat format = context->swapchain->createInfo.imageFormat;
        const uint32_t blueIndex = (format == VK_FORMAT_B8G8R8A8_SRGB) || (format == VK_FORMAT_B8G8R8A8_UNORM) ? 0u : 2u;
        const uint32_t redIndex = (blueIndex == 0u) ? 2u : 0u;
        VkImage image = context->swapchain->images[acquired];
        VkBufferCreateInfo bufferInfo;
        VkCommandPoolCreateInfo poolInfo;
        VkCommandBufferAllocateInfo allocationInfo;
        VkCommandBufferBeginInfo beginInfo;
        VkImageMemoryBarrier imageBarrier;
        VkImageSubresourceLayers subresource;
        VkBufferImageCopy region;
        VkSubmitInfo submitInfo;
        VkFenceCreateInfo fenceInfo;

        xud_check((width >= (XUD_TEST_RT_TARGET * 2u)) && (height >= (XUD_TEST_RT_TARGET * 2u)), "rt-readback/extent",
            "the presented extent leaves both probe points - inside and outside the sampled box - in view");
        xud_check((format == VK_FORMAT_R8G8B8A8_SRGB) || (format == VK_FORMAT_B8G8R8A8_SRGB) || (format == VK_FORMAT_R8G8B8A8_UNORM)
                || (format == VK_FORMAT_B8G8R8A8_UNORM),
            "rt-readback/format", "and the format is one of the four 8888s the byte arithmetic below reads");

        memset(&bufferInfo, 0, sizeof bufferInfo);
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = (VkDeviceSize)width * (VkDeviceSize)height * 4u;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vk = context->loader->deviceTable.vkCreateBuffer(context->device->device, &bufferInfo, NULL, &readback);
        xud_check(vk == VK_SUCCESS, "rt-readback/buffer", "the destination buffer is created");
        context->loader->deviceTable.vkGetBufferMemoryRequirements(context->device->device, readback, &readbackReq);
        {
            const VkMemoryType* memTypes = context->physicalDevice->caps.memoryProperties.memoryProperties.memoryTypes;
            const uint32_t memTypeCount = context->physicalDevice->caps.memoryProperties.memoryProperties.memoryTypeCount;
            uint32_t hostVisibleMask = 0u;
            uint32_t type = 0u;

            for (type = 0u; type < memTypeCount; type++) {
                if ((memTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0u) {
                    hostVisibleMask |= (1u << type);
                }
            }
            readbackReq.memoryTypeBits &= hostVisibleMask;
        }
        result = xud_allocator_acquire_host_visible(
            &readbackRange, context->allocator, context->loader, context->device, &context->physicalDevice->caps, &readbackReq);
        xud_check(result == XUD_OK, "rt-readback/block", "a host-visible block is carved for it");
        vk = context->loader->deviceTable.vkBindBufferMemory(context->device->device, readback, readbackRange.memory, readbackRange.offset);
        xud_check(vk == VK_SUCCESS, "rt-readback/bound", "and the buffer binds into it");
        vk = context->loader->deviceTable.vkMapMemory(
            context->device->device, readbackRange.memory, readbackRange.offset, readbackRange.size, 0u, (void**)&pixels);
        xud_check((vk == VK_SUCCESS) && (pixels != NULL), "rt-readback/mapped", "and the mapping is live");

        memset(&poolInfo, 0, sizeof poolInfo);
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = context->device->graphicsQueue.queueFamilyIndex;
        vk = context->loader->deviceTable.vkCreateCommandPool(context->device->device, &poolInfo, NULL, &pool);
        memset(&allocationInfo, 0, sizeof allocationInfo);
        allocationInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocationInfo.commandPool = pool;
        allocationInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocationInfo.commandBufferCount = 1u;
        vk = context->loader->deviceTable.vkAllocateCommandBuffers(context->device->device, &allocationInfo, &command);
        memset(&beginInfo, 0, sizeof beginInfo);
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vk = context->loader->deviceTable.vkBeginCommandBuffer(command, &beginInfo);

        memset(&imageBarrier, 0, sizeof imageBarrier);
        imageBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        imageBarrier.srcAccessMask = 0u;
        imageBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        imageBarrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        imageBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        imageBarrier.image = image;
        imageBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imageBarrier.subresourceRange.levelCount = 1u;
        imageBarrier.subresourceRange.layerCount = 1u;
        context->loader->deviceTable.vkCmdPipelineBarrier(
            command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, NULL, 0u, NULL, 1u, &imageBarrier);
        memset(&subresource, 0, sizeof subresource);
        subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        subresource.layerCount = 1u;
        memset(&region, 0, sizeof region);
        region.bufferRowLength = 0u;
        region.bufferImageHeight = height;
        region.imageSubresource = subresource;
        region.imageExtent.width = width;
        region.imageExtent.height = height;
        region.imageExtent.depth = 1u;
        context->loader->deviceTable.vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1u, &region);
        imageBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        imageBarrier.dstAccessMask = 0u;
        imageBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        imageBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        context->loader->deviceTable.vkCmdPipelineBarrier(
            command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0u, 0u, NULL, 0u, NULL, 1u, &imageBarrier);
        vk = context->loader->deviceTable.vkEndCommandBuffer(command);
        xud_check(vk == VK_SUCCESS, "rt-readback/cmd", "the copy command is ended with its barriers");
        memset(&fenceInfo, 0, sizeof fenceInfo);
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        vk = context->loader->deviceTable.vkCreateFence(context->device->device, &fenceInfo, NULL, &fence);
        memset(&submitInfo, 0, sizeof submitInfo);
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1u;
        submitInfo.pCommandBuffers = &command;
        vk = context->loader->deviceTable.vkQueueSubmit(context->device->graphicsQueue.queue, 1u, &submitInfo, fence);
        vk = context->loader->deviceTable.vkWaitForFences(context->device->device, 1u, &fence, VK_TRUE, UINT64_MAX);
        xud_check(vk == VK_SUCCESS, "rt-readback/copied", "and the fence signals, so the buffer holds the presented image");

        if ((vk == VK_SUCCESS) && (pixels != NULL)) {
            /* The probes: pixel (16,16) sits inside the sampled box's 64x64 corner - the target's
               blue must be there. Pixel (target+32, target+32) sits outside it on the screen's own
               red. A swapped probe pair would mean the sample drew the background instead of the
               target's content; a red pair means the target never rendered. */
            const uint32_t insideAt = ((16u * width) + 16u) * 4u;
            const uint32_t outsideAt = (((XUD_TEST_RT_TARGET + 32u) * width) + (XUD_TEST_RT_TARGET + 32u)) * 4u;

            xud_check((pixels[insideAt + blueIndex] >= (blueByte - XUD_TEST_RT_TOLERANCE))
                    && (pixels[insideAt + blueIndex] <= (blueByte + XUD_TEST_RT_TOLERANCE))
                    && (pixels[insideAt + redIndex] <= (zeroByte + XUD_TEST_RT_TOLERANCE)),
                "rt-readback/inside-blue", "inside the sampled box the pixel is the blue drawn INSIDE the target's scope");
            xud_check((pixels[outsideAt + redIndex] >= (blueByte - XUD_TEST_RT_TOLERANCE))
                    && (pixels[outsideAt + redIndex] <= (blueByte + XUD_TEST_RT_TOLERANCE))
                    && (pixels[outsideAt + blueIndex] <= (zeroByte + XUD_TEST_RT_TOLERANCE)),
                "rt-readback/outside-red", "outside it the pixel is the red of the screen pass's own clear");
        }

        if (fence != VK_NULL_HANDLE) {
            context->loader->deviceTable.vkDestroyFence(context->device->device, fence, NULL);
        }
        if (pool != VK_NULL_HANDLE) {
            context->loader->deviceTable.vkFreeCommandBuffers(context->device->device, pool, 1u, &command);
            context->loader->deviceTable.vkDestroyCommandPool(context->device->device, pool, NULL);
        }
        if (readback != VK_NULL_HANDLE) {
            context->loader->deviceTable.vkDestroyBuffer(context->device->device, readback, NULL);
        }
        (void)xud_allocator_release(context->allocator, &readbackRange);
    }

    (void)xud_close_context(&context);
}

void xud_test_rendertarget(void) {
    xud_test_rendertarget_pure();
    if (xud_gpu_tests_enabled() != 0) {
        xud_test_rendertarget_gpu();
        xud_test_rendertarget_readback();
    }
}
