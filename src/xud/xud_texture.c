/***********************************************************************************************
 *   xud_texture.c - the texture rung: the record behind a TEXTURE-kind slot of the handle layer,
 *                   the VkImage + VkImageView + VkSampler one texture owns, the mip chain the CPU
 *                   generates, the staging upload that fills the image, and the public create/close
 *                   pair xud.h declares above it
 *
 *   Owns : the pure halves (the argument check, the mip-count and mip-extent math, the staging
 *          arena's byte layout, the caps gate, the four create-infos this rung stamps, the guard
 *          that reads the staging buffer's own requirement back against the chain's total, and the
 *          write that fills the staging arena) and the driver halves (the bring-up that runs them
 *          in order, the two teardown halves the release, the bring-up and the table's teardown rung
 *          share, the
 *          accessor the rung above reaches instead of the slot array, and the teardown rung above
 *          the handle table's own close).
 *   Layer: above the handle table and the allocator, below xud.h - the third rung that fills a
 *          slot's record field (after the material's and the mesh's) and the second one whose
 *          record owns driver objects. It borrows the handle table (the slot and the record field
 *          through the handle layer's own two accessors), the allocator (both blocks), the loader's
 *          device table, the device and the frame rung's two pure create-infos. It borrows nothing
 *          of the renderer's frame loop, nothing of the batch and nothing of the descriptor: a
 *          texture is not a frame object, it is an object a caller creates once and holds for as
 *          long as anything samples it.
 *
 *   Why the image's memory requirement is asked for rather than stated, and why the image's layout
 *   is never asked for at all: these headers' VkImageCreateInfo has no size member (grepped in
 *   /usr/include/vulkan/vulkan_core.h and in a second generated copy at 1.4.354 - the struct ends
 *   at initialLayout and the string "imageSize" appears in neither file), so the block's size is
 *   vkGetImageMemoryRequirements2's answer rather than this rung's arithmetic. The image's per-level
 *   layout, though, is never queried: the tiling is OPTIMAL (see the constant's note in the header,
 *   which records what this machine's driver answers for a four-level LINEAR image - a layout whose
 *   level footprints overlap one another, which no write plan can follow), and an optimal-tiled
 *   image's interior is implementation-defined by definition. The bytes therefore live in a staging
 *   buffer this rung lays out with its own arithmetic - the arithmetic the headless tests pin - and
 *   vkCmdCopyBufferToImage moves them into an interior the driver owns. That is the repo's staging
 *   rule read literally ("prefer the copy path over mapped writes"), and the mesh rung's mapped-write
 *   exception is an exception for BUFFERS, which have no interior to own; an image does.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud_texture.h" /* Required for: this file's own contract - the record, the constants and every rung declared above */
#include "xud.h" /* Required for: XudResult, XudHandle, XudTextureData, XUD_HANDLE_KIND_TEXTURE - the public surface above */
#include "xud_allocator.h" /* Required for: xud_allocator_acquire_host_visible, xud_allocator_release, xud_allocator_select_memory_type - the block this rung owns */
#include "xud_batch.h" /* Required for: xud_batch_host_visible_bits - the host-visible mask, read rather than restated */
#include "xud_core.h" /* Required for: struct XudContext, whose handleTable/loader/device/allocator the public pair reaches */
#include "xud_device.h" /* Required for: XudDevice, xud_device_wait_idle - the wait-idle guard a destroy owes */
#include "xud_handle.h" /* Required for: XudHandleTable, xud_handle_validate/alloc/free, xud_handle_record_get/set - the slot this record lives in */
#include "xud_loader.h" /* Required for: XudLoader, xud_loader_device_bound - the device-table rows and the bound-handle check */
#include "xud_physical.h" /* Required for: XudDeviceCaps - the image limits every gate below is read off */
#include "xud_vulkan.h" /* Required for: xud_result_from_vk - the one VkResult funnel, and why this rung uses the general one and never the swapchain's */

#include <assert.h> /* Required for: static_assert, the pure halves' own invariants below */
#include <stdlib.h> /* Required for: calloc, free - the record's allocation and release */
#include <string.h> /* Required for: memcpy - the mip-0 copy, row by row at the record's row pitch */

/*-----------------------------------------------------------------------------------------------
 * The chain's arithmetic - pure
 *-----------------------------------------------------------------------------------------------*/

/* The chain's length for one extent: 1 + floor(log2(max(width, height))), which is the spec's bound
 *  (VUID-VkImageCreateInfo-imageMaxLevel) read as the maximum it allows rather than as a quality lev
 *  el. The loop is the log2 rather than a library call because there is no portable integer log2 in
 *  C11 and a float one would be a rounding question rather than an integer answer.
 */
XudResult xud_texture_mip_levels(uint32_t width, uint32_t height, uint32_t* outMipLevels) {
    uint32_t largest = 0u;
    uint32_t levels = 1u;

    if ((outMipLevels == NULL) || (width == 0u) || (height == 0u)) {
        return XUD_INVALID_ARG;
    }

    largest = (width > height) ? width : height;
    while (largest > 1u) {
        largest >>= 1u;
        levels++;
    }

    *outMipLevels = levels;
    return XUD_OK;
}

/* One level's extent: the spec's mip-extent rule rather than a halving - an axis is extent >> level,
 *  or 1 once that shift has reached zero. The clamp is what makes a 3x5 image's level 1 a 1x2 rather
 *  than a 0x1, and an axis of zero would be an axis the driver's own layout does not have, which is w
 *  hy the guard below exists at all. The shift is guarded rather than trusted because a shift at or a
 *  bove the width of the type is not defined in C.
 */
XudResult xud_texture_mip_extent(
    uint32_t width, uint32_t height, uint32_t level, uint32_t mipLevels, uint32_t* outWidth, uint32_t* outHeight) {
    uint32_t w = 0u;
    uint32_t h = 0u;

    if ((outWidth == NULL) || (outHeight == NULL) || (width == 0u) || (height == 0u)) {
        return XUD_INVALID_ARG;
    }
    if (level >= mipLevels) {
        return XUD_INVALID_ARG;
    }

    w = (level >= 32u) ? 0u : (width >> level);
    h = (level >= 32u) ? 0u : (height >> level);

    *outWidth = (w != 0u) ? w : 1u;
    *outHeight = (h != 0u) ? h : 1u;
    return XUD_OK;
}

/* The argument check every entry point above runs before anything is spent. The products are taken i
 *  n 64-bit rather than in the extent's own type for the reason the mesh's check gives: an extent at t
 *  he bound multiplied by itself is a product that wraps in the extent's type, and a wrapped product i
 *  s a texture that passes the bound and then lays out an staging of the wrong size.
 */
XudResult xud_texture_check(const XudTextureData* data, uint32_t* outMipLevels) {
    uint32_t levels = 0u;
    uint64_t texels = 0u;

    if (data == NULL) {
        return XUD_INVALID_ARG;
    }
    if ((data->width == 0u) || (data->height == 0u)) {
        return XUD_INVALID_ARG;
    }
    if ((data->width > XUD_TEXTURE_MAX_DIMENSION) || (data->height > XUD_TEXTURE_MAX_DIMENSION)) {
        return XUD_INVALID_ARG;
    }
    if (data->rgba == NULL) {
        return XUD_INVALID_ARG;
    }
    if (data->filter >= XUD_TEXTURE_FILTER_COUNT) {
        return XUD_INVALID_ARG;
    }
    if (data->addressMode >= XUD_TEXTURE_ADDRESS_COUNT) {
        return XUD_INVALID_ARG;
    }

    texels = (uint64_t)data->width * (uint64_t)data->height;
    if (texels > (uint64_t)XUD_TEXTURE_MAX_TEXELS) {
        return XUD_INVALID_ARG;
    }

    if (xud_texture_mip_levels(data->width, data->height, &levels) != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    if (levels > XUD_TEXTURE_MIP_MAX_LEVELS) {
        return XUD_INVALID_ARG;
    }

    if (outMipLevels != NULL) {
        *outMipLevels = levels;
    }
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The staging arena's byte layout - pure
 *
 *   The layout is a cumulative sum rather than a padded one, and that is the spec's own formula for
 *   the one kind of image this rung creates: for a LINEAR-tiled, uncompressed, single-layer, single-s
 *   ample image, a level's rowPitch is its width times the texel block size, its size is that pitch ti
 *   mes its height, and the next level starts at the previous level's end rounded up to the texel block
 *   size. Every size above is a multiple of four, so the rounding is a no-op and the sum is exact - whi
 *   ch is what makes there be no alignment parameter here at all, unlike the mesh's layout: the mesh's n
 *   umbers are a driver fact (the granularity at which separate resources are placed in memory), and thes
 *   e numbers are a format fact (the texel block size of one format this rung names as a constant). The d
 *   river's own answer is still read back below, and a driver that disagrees is refused rather than writh
 *   en over.
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_texture_layout(const XudTextureData* data, XudTextureRecord* outRecord) {
    uint32_t levels = 0u;
    uint32_t level = 0u;
    uint32_t width = 0u;
    uint32_t height = 0u;
    VkDeviceSize running = 0u;

    if ((data == NULL) || (outRecord == NULL)) {
        return XUD_INVALID_ARG;
    }
    if (xud_texture_check(data, &levels) != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    outRecord->width = data->width;
    outRecord->height = data->height;
    outRecord->mipLevels = levels;

    for (level = 0u; level < levels; level++) {
        if (xud_texture_mip_extent(data->width, data->height, level, levels, &width, &height) != XUD_OK) {
            return XUD_INVALID_ARG;
        }

        outRecord->mipRowPitch[level] = (VkDeviceSize)width * (VkDeviceSize)XUD_TEXTURE_BYTES_PER_TEXEL;
        outRecord->mipSize[level] = outRecord->mipRowPitch[level] * (VkDeviceSize)height;
        outRecord->mipOffset[level] = running;
        running += outRecord->mipSize[level];
    }

    outRecord->stagingBytes = running;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The caps gate - pure
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_texture_caps_support(const XudDeviceCaps* caps, const XudTextureData* data) {
    if ((caps == NULL) || (data == NULL)) {
        return XUD_INVALID_ARG;
    }

    /* maxImageDimension2D is the one recorded limit that bounds an image's extent, and both axes are
       gated against it rather than the larger of the two: the limit bounds every axis of a 2D image, s
       o a device that can hold a 4096x1 image cannot hold a 1x4096 one by a different name.
     */
    if ((data->width > caps->properties.properties.limits.maxImageDimension2D)
        || (data->height > caps->properties.properties.limits.maxImageDimension2D)) {
        return XUD_UNSUPPORTED;
    }

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The three create-infos this rung stamps - pure
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_texture_fill_image_info(const XudTextureData* data, const XudTextureRecord* record, VkImageCreateInfo* outInfo) {
    if ((data == NULL) || (record == NULL) || (outInfo == NULL)) {
        return XUD_INVALID_ARG;
    }
    if ((record->width != data->width) || (record->height != data->height) || (record->mipLevels == 0u)) {
        return XUD_INVALID_ARG;
    }

    outInfo->sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    outInfo->pNext = NULL;
    outInfo->flags = 0u;

    outInfo->imageType = VK_IMAGE_TYPE_2D;
    outInfo->format = XUD_TEXTURE_FORMAT;
    outInfo->extent.width = data->width;
    outInfo->extent.height = data->height;
    outInfo->extent.depth = 1u;
    outInfo->mipLevels = record->mipLevels;
    outInfo->arrayLayers = 1u;
    outInfo->samples = VK_SAMPLE_COUNT_1_BIT;
    outInfo->tiling = XUD_TEXTURE_TILING;
    outInfo->usage = XUD_TEXTURE_USAGE;
    outInfo->sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    outInfo->queueFamilyIndexCount = 0u;
    outInfo->pQueueFamilyIndices = NULL;
    outInfo->initialLayout = XUD_TEXTURE_INITIAL_LAYOUT;

    return XUD_OK;
}

XudResult xud_texture_fill_view_info(const XudTextureRecord* record, VkImageViewCreateInfo* outInfo) {
    if ((record == NULL) || (outInfo == NULL)) {
        return XUD_INVALID_ARG;
    }
    if (record->image == VK_NULL_HANDLE) {
        return XUD_INVALID_ARG;
    }

    outInfo->sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    outInfo->pNext = NULL;
    outInfo->flags = 0u;
    outInfo->image = record->image;
    outInfo->viewType = VK_IMAGE_VIEW_TYPE_2D;
    outInfo->format = XUD_TEXTURE_FORMAT;

    /* The swizzle left as IDENTITY rather than spelled out per channel: an identity component is th
       e header's own way of saying "use the channel as it is", and a view that renamed channels wo
       uld be a texture whose red and blue are swapped at every sample rather than a view that faile
       d.
    */
    outInfo->components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    outInfo->components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    outInfo->components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    outInfo->components.a = VK_COMPONENT_SWIZZLE_IDENTITY;

    /* The range the view exposes, and it is the whole chain rather than mip 0: a view that named on
       ly the first level would be a view the sampler could not select a lower LOD through, which is
       what makes the chain this rung generates do anything at all. layerCount 1 is the image's one
       array layer, the same 1 the image's create-info stamps.
    */
    outInfo->subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    outInfo->subresourceRange.baseMipLevel = 0u;
    outInfo->subresourceRange.levelCount = record->mipLevels;
    outInfo->subresourceRange.baseArrayLayer = 0u;
    outInfo->subresourceRange.layerCount = 1u;

    return XUD_OK;
}

XudResult xud_texture_fill_sampler_info(const XudTextureData* data, VkSamplerCreateInfo* outInfo) {
    if (data == NULL) {
        return XUD_INVALID_ARG;
    }
    if ((data->filter >= XUD_TEXTURE_FILTER_COUNT) || (data->addressMode >= XUD_TEXTURE_ADDRESS_COUNT)) {
        return XUD_INVALID_ARG;
    }

    outInfo->sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    outInfo->pNext = NULL;
    outInfo->flags = 0u;
    outInfo->magFilter = data->filter;
    outInfo->minFilter = data->filter;
    outInfo->mipmapMode = XUD_TEXTURE_MIPMAP_MODE;
    outInfo->addressModeU = data->addressMode;
    outInfo->addressModeV = data->addressMode;
    outInfo->addressModeW = XUD_TEXTURE_ADDRESS_MODE_MIP;
    outInfo->mipLodBias = XUD_TEXTURE_LOD_BIAS;
    outInfo->anisotropyEnable = VK_FALSE;
    outInfo->maxAnisotropy = XUD_TEXTURE_ANISOTROPY_MAX;
    outInfo->compareEnable = VK_FALSE;
    outInfo->compareOp = XUD_TEXTURE_COMPARE_OP;
    outInfo->minLod = XUD_TEXTURE_LOD_MIN;
    outInfo->maxLod = XUD_TEXTURE_LOD_MAX;
    outInfo->borderColor = XUD_TEXTURE_BORDER_COLOR;
    outInfo->unnormalizedCoordinates = VK_FALSE;

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The staging buffer's create-info and the guard that reads its requirement back - pure
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_texture_fill_staging_info(VkBufferCreateInfo* outInfo, VkDeviceSize totalBytes) {
    if (outInfo == NULL) {
        return XUD_INVALID_ARG;
    }
    if (totalBytes == 0u) {
        /* An empty staging buffer is a buffer this rung must not create - the same zero-size rule
           xud_allocator_acquire() states for a block, and the rule's reason is the same one: a
           zero-size create is a buffer whose bind would be a bind of nothing.
        */
        return XUD_UNSUPPORTED;
    }

    outInfo->sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    outInfo->pNext = NULL;
    outInfo->flags = 0u;
    outInfo->size = totalBytes;
    outInfo->usage = XUD_TEXTURE_STAGING_USAGE;
    outInfo->sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    outInfo->queueFamilyIndexCount = 0u;
    outInfo->pQueueFamilyIndices = NULL;

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The guard that reads the driver's own requirement back - pure
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_texture_verify_staging(const XudTextureRecord* record, const VkMemoryRequirements* requirements, VkDeviceSize totalBytes) {
    if ((record == NULL) || (requirements == NULL)) {
        return XUD_INVALID_ARG;
    }

    /* Two numbers, not three: the staging buffer is OURS - its size is this rung's arithmetic and
       the driver's requirement is a fact about a buffer that exists - so what the guard checks is
       that the driver's own size and alignment can hold the chain this rung laid out. The image's
       layout is never queried and never compared: an optimal-tiled image's interior is implementa-
       tion-defined by definition (see the tiling constant's note), which is exactly why the upload
       reaches it through a transfer rather than through a mapping.
    */
    if ((requirements->size < totalBytes) || (requirements->alignment == 0u) || (totalBytes == 0u)) {
        return XUD_UNSUPPORTED;
    }

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The write that fills the chain - pure
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_texture_write_mips(const XudTextureRecord* record, const XudTextureData* data, void* staging, VkDeviceSize stagingSize) {
    uint32_t level = 0u;
    uint32_t y = 0u;
    uint32_t x = 0u;
    uint32_t channel = 0u;
    uint32_t srcW = 0u;
    uint32_t srcH = 0u;
    uint32_t dstW = 0u;
    uint32_t dstH = 0u;
    uint8_t* dst = NULL;
    const uint8_t* src = NULL;
    VkDeviceSize srcPitch = 0u;

    if ((record == NULL) || (data == NULL) || (staging == NULL)) {
        return XUD_INVALID_ARG;
    }
    if ((record->width != data->width) || (record->height != data->height)) {
        return XUD_INVALID_ARG;
    }
    if (record->stagingBytes == 0u) {
        /* A record whose staging was never laid out: the offsets below would be a run of zeros and the
           write would land every level on top of the first. The mesh write's rule and reason, restated
           .
        */
        return XUD_UNSUPPORTED;
    }
    if (stagingSize < record->stagingBytes) {
        return XUD_INVALID_ARG;
    }

    /* Mip 0 is the caller's array, copied row by row at the record's row pitch rather than as one bloc
       k: the pitch is what the guard above checked the driver reported, and a pitch wider than a row i
       s a destination whose rows have gaps in it, which a block copy would fill with the next row's by
       tes rather than leave alone.
     */
    dst = (uint8_t*)((uint8_t*)staging + record->mipOffset[0]);
    src = data->rgba;
    for (y = 0u; y < record->height; y++) {
        memcpy(dst + ((VkDeviceSize)y * record->mipRowPitch[0]),
            src + ((VkDeviceSize)y * (VkDeviceSize)record->width * (VkDeviceSize)XUD_TEXTURE_BYTES_PER_TEXEL),
            (size_t)((VkDeviceSize)record->width * (VkDeviceSize)XUD_TEXTURE_BYTES_PER_TEXEL));
    }

    /* Every level above 0 is generated from the level below it AS IT SITS IN THE STAGING, not from the c
       aller's array: the chain is one set of bytes, and a chain of temporaries could disagree with the
       bytes the driver will read. The filter is a 2x2 box with the odd edge clamped, so a level whose a
       xis is not a whole multiple of two repeats its last row and column rather than reading past the l
       evel below, and each channel is averaged in integer arithmetic, so the result is deterministic an
       d no float rounding is involved - the repo's determinism rule read at the one place this library g
       enerates bytes.
     */
    for (level = 1u; level < record->mipLevels; level++) {
        if (xud_texture_mip_extent(record->width, record->height, level - 1u, record->mipLevels, &srcW, &srcH) != XUD_OK) {
            return XUD_INVALID_ARG;
        }
        if (xud_texture_mip_extent(record->width, record->height, level, record->mipLevels, &dstW, &dstH) != XUD_OK) {
            return XUD_INVALID_ARG;
        }

        /* The level below, read where it sits in the STAGING rather than from the caller's array: the c
           hain is one set of bytes, and generating from temporaries could disagree with the bytes the
           driver will read. The pitch is that level's own rather than mip 0's - a level's row is its o
           wn width times the texel, so indexing a lower level at mip 0's pitch would read the wrong ro
           ws of it.
        */
        src = (const uint8_t*)((uint8_t*)staging + record->mipOffset[level - 1u]);
        dst = (uint8_t*)((uint8_t*)staging + record->mipOffset[level]);
        srcPitch = record->mipRowPitch[level - 1u];

        for (y = 0u; y < dstH; y++) {
            uint32_t y0 = y * 2u;
            uint32_t y1 = (y * 2u) + 1u;
            if (y0 >= srcH) {
                y0 = srcH - 1u;
            }
            if (y1 >= srcH) {
                y1 = srcH - 1u;
            }

            for (x = 0u; x < dstW; x++) {
                uint32_t x0 = x * 2u;
                uint32_t x1 = (x * 2u) + 1u;
                if (x0 >= srcW) {
                    x0 = srcW - 1u;
                }
                if (x1 >= srcW) {
                    x1 = srcW - 1u;
                }

                for (channel = 0u; channel < XUD_TEXTURE_BYTES_PER_TEXEL; channel++) {
                    uint32_t sum = 0u;
                    sum += src[((VkDeviceSize)y0 * srcPitch) + ((VkDeviceSize)x0 * (VkDeviceSize)XUD_TEXTURE_BYTES_PER_TEXEL) + channel];
                    sum += src[((VkDeviceSize)y0 * srcPitch) + ((VkDeviceSize)x1 * (VkDeviceSize)XUD_TEXTURE_BYTES_PER_TEXEL) + channel];
                    sum += src[((VkDeviceSize)y1 * srcPitch) + ((VkDeviceSize)x0 * (VkDeviceSize)XUD_TEXTURE_BYTES_PER_TEXEL) + channel];
                    sum += src[((VkDeviceSize)y1 * srcPitch) + ((VkDeviceSize)x1 * (VkDeviceSize)XUD_TEXTURE_BYTES_PER_TEXEL) + channel];
                    dst[((VkDeviceSize)y * record->mipRowPitch[level]) + ((VkDeviceSize)x * (VkDeviceSize)XUD_TEXTURE_BYTES_PER_TEXEL)
                        + channel] = (uint8_t)(sum / 4u);
                }
            }
        }
    }

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The accessor - pure
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_texture_get(const XudHandleTable* table, XudHandle handle, const XudTextureRecord** outRecord) {
    void* record = NULL;

    if ((table == NULL) || (outRecord == NULL)) {
        return XUD_INVALID_ARG;
    }
    if (table->slots[XUD_HANDLE_KIND_TEXTURE] == NULL) {
        return XUD_INVALID_ARG;
    }
    if (xud_handle_validate(table, handle, XUD_HANDLE_KIND_TEXTURE) != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    if (xud_handle_record_get(table, XUD_HANDLE_KIND_TEXTURE, handle.index, &record) != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    if (record == NULL) {
        return XUD_INVALID_ARG;
    }

    *outRecord = (const XudTextureRecord*)record;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The two halves of the one teardown this rung owns, shared by the release and the table's
 * teardown rung below, and by the bring-up's fail ladder
 *-----------------------------------------------------------------------------------------------*/

/* The driver-object half: the three handles the texture owns and the block the image lives in.
   Reverse creation order (sampler, view, image), because these headers' destroy rows take no
   order promise but the ownership direction does - the image's block goes free only after the
   image that names it is gone, the allocator's own ownership direction read for an image.
*/
static void xud_texture_retire_objects(XudTextureRecord* record, const XudDevice* device, XudLoader* loader, XudAllocator* alloc) {
    if ((record == NULL) || (device == NULL) || (loader == NULL) || (alloc == NULL)) {
        return;
    }

    if ((loader->deviceTable.vkDestroySampler != NULL) && (record->sampler != VK_NULL_HANDLE)) {
        (void)loader->deviceTable.vkDestroySampler(device->device, record->sampler, NULL);
    }
    if ((loader->deviceTable.vkDestroyImageView != NULL) && (record->view != VK_NULL_HANDLE)) {
        (void)loader->deviceTable.vkDestroyImageView(device->device, record->view, NULL);
    }
    if ((loader->deviceTable.vkDestroyImage != NULL) && (record->image != VK_NULL_HANDLE)) {
        (void)loader->deviceTable.vkDestroyImage(device->device, record->image, NULL);
    }
    if (record->imageRange.memory != VK_NULL_HANDLE) {
        (void)xud_allocator_release(alloc, &record->imageRange);
    }

    record->image = VK_NULL_HANDLE;
    record->view = VK_NULL_HANDLE;
    record->sampler = VK_NULL_HANDLE;
    record->imageRange.memory = VK_NULL_HANDLE;
    record->imageRange.size = 0u;
    record->imageRange.offset = 0u;
}

/* The staging half: the mapping, the buffer and the block. The mapping is unmapped FIRST, before
   the buffer is destroyed and before the block is released - a mapping is a promise about a
   VkDeviceMemory, and destroying an object whose bytes live in a block while that block is still
   mapped is the other order; the mesh's arena and the descriptor's arena unmap in the same slot
   of their teardown for the same reason. On the release path below the staging half is already
   empty (the bring-up retires it as soon as the upload has run), so these rows are no-ops there;
   on the bring-up's fail ladder they are the whole teardown of this half, which is why the same
   helper serves both.
*/
static void xud_texture_retire_staging(XudTextureRecord* record, const XudDevice* device, XudLoader* loader, XudAllocator* alloc) {
    if ((record == NULL) || (device == NULL) || (loader == NULL) || (alloc == NULL)) {
        return;
    }

    if ((record->stagingMappedLive != 0u) && (loader->deviceTable.vkUnmapMemory != NULL)
        && (record->stagingRange.memory != VK_NULL_HANDLE)) {
        (void)loader->deviceTable.vkUnmapMemory(device->device, record->stagingRange.memory);
        record->stagingMappedLive = 0u;
    }
    if ((loader->deviceTable.vkDestroyBuffer != NULL) && (record->stagingBuffer != VK_NULL_HANDLE)) {
        (void)loader->deviceTable.vkDestroyBuffer(device->device, record->stagingBuffer, NULL);
    }
    if (record->stagingRange.memory != VK_NULL_HANDLE) {
        (void)xud_allocator_release(alloc, &record->stagingRange);
    }

    record->stagingBuffer = VK_NULL_HANDLE;
    record->stagingRange.memory = VK_NULL_HANDLE;
    record->stagingRange.size = 0u;
    record->stagingRange.offset = 0u;
    record->stagingMapped = NULL;
}

/*-----------------------------------------------------------------------------------------------
 * The bring-up - driver
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_texture_open(XudHandle* outHandle, XudHandleTable* table, XudLoader* loader, const XudDevice* device,
    const XudDeviceCaps* caps, XudAllocator* alloc, const XudTextureData* data) {
    XudTextureRecord* record = NULL;
    XudResult result = XUD_INVALID_ARG;
    VkResult vk = VK_ERROR_UNKNOWN;
    VkMemoryRequirements stagingNeeds;
    VkMemoryRequirements stagingReq;
    VkMemoryRequirements imageReq;
    uint32_t levels = 0u;
    uint32_t hostVisibleMask = 0u;
    uint32_t memoryTypeIndex = 0u;
    uint32_t level = 0u;
    uint32_t levelWidth = 0u;
    uint32_t levelHeight = 0u;
    void* mapped = NULL;
    VkCommandBuffer uploadBuffer = VK_NULL_HANDLE;
    VkCommandPool uploadPool = VK_NULL_HANDLE;
    VkFence uploadFence = VK_NULL_HANDLE;

    if ((outHandle == NULL) || (table == NULL) || (loader == NULL) || (device == NULL) || (caps == NULL) || (alloc == NULL)
        || (data == NULL)) {
        return XUD_INVALID_ARG;
    }
    if (!xud_loader_is_open(loader)) {
        return XUD_UNSUPPORTED;
    }
    if (device->device == VK_NULL_HANDLE) {
        return XUD_INVALID_ARG;
    }
    if (!xud_loader_device_bound(loader, device->device)) {
        return XUD_UNSUPPORTED;
    }

    /* Every row this rung calls is checked before anything is spent, for the rule every rung in this
       tree states: an object this layer cannot destroy is an object it must not create. The fail
       ladder below destroys through these very rows, so a refusal that reached it with a missing row
       would leak what the steps above made.
    */
    if ((loader->deviceTable.vkCreateImage == NULL) || (loader->deviceTable.vkDestroyImage == NULL)
        || (loader->deviceTable.vkCreateImageView == NULL) || (loader->deviceTable.vkDestroyImageView == NULL)
        || (loader->deviceTable.vkCreateSampler == NULL) || (loader->deviceTable.vkDestroySampler == NULL)
        || (loader->deviceTable.vkCreateBuffer == NULL) || (loader->deviceTable.vkDestroyBuffer == NULL)
        || (loader->deviceTable.vkGetBufferMemoryRequirements == NULL) || (loader->deviceTable.vkGetImageMemoryRequirements2 == NULL)
        || (loader->deviceTable.vkBindImageMemory2 == NULL) || (loader->deviceTable.vkBindBufferMemory == NULL)
        || (loader->deviceTable.vkMapMemory == NULL) || (loader->deviceTable.vkUnmapMemory == NULL)
        || (loader->deviceTable.vkCreateCommandPool == NULL) || (loader->deviceTable.vkDestroyCommandPool == NULL)
        || (loader->deviceTable.vkAllocateCommandBuffers == NULL) || (loader->deviceTable.vkFreeCommandBuffers == NULL)
        || (loader->deviceTable.vkBeginCommandBuffer == NULL) || (loader->deviceTable.vkEndCommandBuffer == NULL)
        || (loader->deviceTable.vkCmdPipelineBarrier == NULL) || (loader->deviceTable.vkCmdCopyBufferToImage == NULL)
        || (loader->deviceTable.vkQueueSubmit == NULL) || (loader->deviceTable.vkCreateFence == NULL)
        || (loader->deviceTable.vkDestroyFence == NULL) || (loader->deviceTable.vkWaitForFences == NULL)) {
        return XUD_UNSUPPORTED;
    }

    /* The argument check and the caps gate run before anything is spent, so a refusal leaves no slot
       and no record behind: XUD_INVALID_ARG is the caller's description and XUD_UNSUPPORTED is this
       device, and neither is a half-texture.
    */
    if (xud_texture_check(data, &levels) != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    result = xud_texture_caps_support(caps, data);
    if (result != XUD_OK) {
        return result;
    }

    /* The slot first, then the record: the order is the material rung's, and the reason is the same
       one stated there - a slot allocated with no record is a slot whose record field is still the
       sentinel. Here that transient state is retired by the fail ladder below, which frees the slot
       on every path that does not reach the write.
    */
    result = xud_handle_alloc(outHandle, table, XUD_HANDLE_KIND_TEXTURE);
    if (result != XUD_OK) {
        return result;
    }

    record = calloc(1u, sizeof(*record));
    if (record == NULL) {
        (void)xud_handle_free(table, *outHandle);
        return XUD_OUT_OF_MEMORY;
    }

    /* The zero state the driver needs before it answers, and before the fail ladder reads it: every
       handle is VK_NULL_HANDLE until its own create writes one (so a create that failed has left
       nothing for the ladder to destroy) and every range is empty until its own acquire writes it
       (so the ladder's release has nothing to release). calloc gives both; the loop below is what
       makes the handles explicit rather than what leaves them implied.
    */
    record->image = VK_NULL_HANDLE;
    record->view = VK_NULL_HANDLE;
    record->sampler = VK_NULL_HANDLE;
    record->stagingBuffer = VK_NULL_HANDLE;
    record->imageRange.memory = VK_NULL_HANDLE;
    record->imageRange.size = 0u;
    record->imageRange.offset = 0u;
    record->stagingRange.memory = VK_NULL_HANDLE;
    record->stagingRange.size = 0u;
    record->stagingRange.offset = 0u;
    record->stagingMapped = NULL;
    record->stagingMappedLive = 0u;
    record->imageLayout = XUD_TEXTURE_INITIAL_LAYOUT;

    /* The layout, this library's own arithmetic rather than the driver's: it gives the staging
       arena's total and the per-level offsets, sizes and pitches the write below uses and the copy
       regions below are stamped from. The image's own interior is never laid out here and never
       asked of the driver: an optimal-tiled image's interior is implementation-defined, and the
       upload reaches it through a transfer, which is the one thing that does not need to know it.
    */
    if (xud_texture_layout(data, record) != XUD_OK) {
        result = XUD_UNSUPPORTED;
        goto fail;
    }

    /* The two halves of the chain's arithmetic - the check's mip count and the layout's -
       have to agree before anything is spent: the copy loop below runs the record's count and
       the check above refused on the caller's extent, so a disagreement is a bug in this file
       rather than a driver fact, and it is refused here rather than drawn with.
    */
    if (levels != record->mipLevels) {
        result = XUD_UNSUPPORTED;
        goto fail;
    }

    /* The staging buffer first, and it is created before its memory is asked for for a call-signature
       reason: vkGetBufferMemoryRequirements takes the buffer, so the requirement is a fact about a
       buffer that exists.
    */
    if (xud_texture_fill_staging_info(&record->stagingInfo, record->stagingBytes) != XUD_OK) {
        result = XUD_UNSUPPORTED;
        goto fail;
    }
    vk = loader->deviceTable.vkCreateBuffer(device->device, &record->stagingInfo, NULL, &record->stagingBuffer);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK) {
        record->stagingBuffer = VK_NULL_HANDLE;
        goto fail;
    }

    /* vkGetBufferMemoryRequirements returns VOID, so the zero-state-before-the-call rule this tree
       follows for every void entry point: the row is cleared first, so a driver that wrote nothing
       leaves a row that says nothing rather than a row of whatever the stack held.
    */
    memset(&stagingReq, 0, sizeof(stagingReq));
    loader->deviceTable.vkGetBufferMemoryRequirements(device->device, record->stagingBuffer, &stagingReq);
    if (stagingReq.size == 0u) {
        result = XUD_VULKAN_ERROR;
        goto fail;
    }
    if (xud_texture_verify_staging(record, &stagingReq, record->stagingBytes) != XUD_OK) {
        /* The driver's own buffer cannot hold the chain this rung laid out: the write below would be
           a write past the end of what the copies then read from.
        */
        result = XUD_UNSUPPORTED;
        goto fail;
    }

    /* The mask of the caps' host-visible memory types, read off the batch's own pure half rather
       than restated here: "host-visible" is a requirement of the STAGING block rather than a
       preference, and a caps that answers no host-visible type answers 0 bits, which is what makes
       the staging allocation UNSUPPORTED rather than a mapping CPU code cannot read.
    */
    result = xud_batch_host_visible_bits(caps, &hostVisibleMask);
    if (result != XUD_OK) {
        goto fail;
    }
    if (hostVisibleMask == 0u) {
        result = XUD_UNSUPPORTED;
        goto fail;
    }

    /* The staging block, and it is the host-visible acquire rather than the suballocating one: a
       mapping is per-VkDeviceMemory rather than per-range, so the one carve a rung maps owns the
       block it is mapped in. This is the repo's "allocate once, suballocate always" rule with its
       one exception, and it is the same exception the batch's staging and the descriptor's staging spend.
       The image's block below does NOT spend it - the image is never mapped.
    */
    memset(&stagingNeeds, 0, sizeof(stagingNeeds));
    stagingNeeds.size = stagingReq.size;
    stagingNeeds.alignment = stagingReq.alignment;
    stagingNeeds.memoryTypeBits = stagingReq.memoryTypeBits & hostVisibleMask;
    if (stagingNeeds.memoryTypeBits == 0u) {
        /* The staging buffer's own mask and the host-visible rows do not intersect: this texture
           cannot be staged on memory the CPU can write on this driver.
        */
        result = XUD_UNSUPPORTED;
        goto fail;
    }
    result = xud_allocator_acquire_host_visible(&record->stagingRange, alloc, loader, device, caps, &stagingNeeds);
    if (result != XUD_OK) {
        goto fail;
    }

    /* The check-before-you-use rule read at the mapping rather than at the map: the type the acquire
       actually allocated at has to be host-visible, re-read from the allocator's own type rule rather
       than trusted from the mask that was asked for, because a mask is only a preference and the row
       it picked is the one vkMapMemory is called on.
    */
    result = xud_allocator_select_memory_type(caps, &stagingNeeds, &memoryTypeIndex);
    if ((result != XUD_OK) || (memoryTypeIndex >= caps->memoryProperties.memoryProperties.memoryTypeCount)
        || ((caps->memoryProperties.memoryProperties.memoryTypes[memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            == 0u)) {
        result = XUD_UNSUPPORTED;
        goto fail;
    }

    /* The staging buffer's memory, bound before the map: a buffer used by a command is a buffer whose
       bytes have to live in a VkDeviceMemory, and VUID-vkCmdCopyBufferToImage-srcBuffer-00176 - "if
       srcBuffer is non-sparse then it must be bound completely and contiguously to a single VkDeviceMemory
       object" - is the rule the validation layer answers at the COPY rather than at the bind, which is
       why a rung that skipped this step would fail at a line it never wrote. The mesh's five buffers
       bind in the same slot of its upload for the same reason, at the block's own owner offset rather
       than at zero.
    */
    vk = loader->deviceTable.vkBindBufferMemory(
        device->device, record->stagingBuffer, record->stagingRange.memory, record->stagingRange.offset);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK) {
        goto fail;
    }

    /* The map, once, for the whole staging block, for the length of the upload only: unlike the
       mesh's staging (mapped for the object's whole life) this mapping exists to be written and then
       read by a transfer, so it is unmapped and the buffer destroyed before this rung returns.
       flags 0 is the read/write pair the repo's rule asks for: the staging is written by the CPU and
       read by the copies, and no cache-control flag is claimed for a type this layer has not asked
       to be cached.
    */
    vk = loader->deviceTable.vkMapMemory(
        device->device, record->stagingRange.memory, record->stagingRange.offset, record->stagingRange.size, 0u, &mapped);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK) {
        goto fail;
    }
    record->stagingMapped = mapped;
    record->stagingMappedLive = 1u;

    /* The chain, written into the mapping at this rung's own compact offsets - the write the pure
       half above exists for, and the headless tests pin. The mapping's size is the block's size and
       therefore cannot be smaller than the layout's total, but the write states the rule anyway.
    */
    if (xud_texture_write_mips(record, data, record->stagingMapped, record->stagingRange.size) != XUD_OK) {
        result = XUD_UNSUPPORTED;
        goto fail;
    }

    /* The image, created after the staging bytes are in hand: the create-info's extent and mip count
       are this rung's arithmetic and the interior is the driver's, so the create is the one place
       the two meet.
    */
    if (xud_texture_fill_image_info(data, record, &record->imageInfo) != XUD_OK) {
        result = XUD_UNSUPPORTED;
        goto fail;
    }
    vk = loader->deviceTable.vkCreateImage(device->device, &record->imageInfo, NULL, &record->image);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK) {
        record->image = VK_NULL_HANDLE;
        goto fail;
    }

    /* The image's own requirement: these headers' VkImageCreateInfo has no size member at all, so the
       block's size is the driver's answer rather than this rung's arithmetic (the reason is stated
       at the file top). The image's memory is NOT host-visible: the image is never mapped, and the
       staging block above is the only mapping this rung takes, which is why the acquire below is the
       suballocating one rather than the host-visible one - the image's bytes are read by a transfer
       and by a shader, never by the CPU.
    */
    {
        VkImageMemoryRequirementsInfo2 requirementsInfo = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .pNext = NULL };
        VkMemoryRequirements2 requirements = { .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = NULL };
        requirementsInfo.image = record->image;
        loader->deviceTable.vkGetImageMemoryRequirements2(device->device, &requirementsInfo, &requirements);
        imageReq = requirements.memoryRequirements;
    }
    if (imageReq.size == 0u) {
        result = XUD_VULKAN_ERROR;
        goto fail;
    }

    /* The image's block. The acquire is the suballocating one: nothing maps this block, so the repo's
       "allocate once, suballocate always" rule applies to it without its exception, and the image is
       bound at the block's own owner offset rather than at an assumed zero.
    */
    result = xud_allocator_acquire(&record->imageRange, alloc, loader, device, caps, &imageReq);
    if (result != XUD_OK) {
        goto fail;
    }
    record->imageBindOffset = record->imageRange.offset;
    {
        VkBindImageMemoryInfo bindInfo = { .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO, .pNext = NULL };
        bindInfo.image = record->image;
        bindInfo.memory = record->imageRange.memory;
        bindInfo.memoryOffset = record->imageBindOffset;
        vk = loader->deviceTable.vkBindImageMemory2(device->device, 1u, &bindInfo);
        result = xud_result_from_vk(vk);
        if (result != XUD_OK) {
            goto fail;
        }
    }

    /* The view's create-info, stamped BEFORE the upload rather than at the view's own create below:
       the upload's transition barrier names the image's subresource range, and the view names the same
       range of the same image, so the two are stamped from one record field rather than written twice.
       Stamping it here is what makes the barrier and the view unable to drift apart.
    */
    if (xud_texture_fill_view_info(record, &record->viewInfo) != XUD_OK) {
        result = XUD_UNSUPPORTED;
        goto fail;
    }

    /* The upload, which is the one part of this rung that is not a create: a command buffer, a pool
       to allocate it from and a fence to wait on it, all three created here and all three destroyed
       before this rung returns, because a texture's upload is a one-time event rather than a frame
       object. The pool is this rung's own rather than the frame rung's for the same reason: the frame
       loop's pool belongs to a frame that is mid-draw, and a texture is created outside the frame
       loop, so the two pools have no shared lifetime. The create-info rows are the frame rung's own
       pure halves, read rather than restated.
    */
    {
        VkCommandPoolCreateInfo poolInfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .pNext = NULL };
        if (xud_frame_fill_pool_info(&poolInfo, device->queueRow.queueFamilyIndex) != XUD_OK) {
            result = XUD_UNSUPPORTED;
            goto fail;
        }
        vk = loader->deviceTable.vkCreateCommandPool(device->device, &poolInfo, NULL, &uploadPool);
        result = xud_result_from_vk(vk);
        if (result != XUD_OK) {
            uploadPool = VK_NULL_HANDLE;
            goto fail;
        }
    }
    {
        VkCommandBufferAllocateInfo allocationInfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .pNext = NULL };
        if (xud_frame_fill_allocation(&allocationInfo, uploadPool) != XUD_OK) {
            result = XUD_UNSUPPORTED;
            goto fail;
        }
        allocationInfo.commandBufferCount = 1u;
        vk = loader->deviceTable.vkAllocateCommandBuffers(device->device, &allocationInfo, &uploadBuffer);
        result = xud_result_from_vk(vk);
        if (result != XUD_OK) {
            uploadBuffer = VK_NULL_HANDLE;
            goto fail;
        }
    }
    {
        VkCommandBufferBeginInfo beginInfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .pNext = NULL };
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        beginInfo.pInheritanceInfo = NULL;
        vk = loader->deviceTable.vkBeginCommandBuffer(uploadBuffer, &beginInfo);
        result = xud_result_from_vk(vk);
        if (result != XUD_OK) {
            goto fail;
        }
    }

    /* ONE barrier, carrying two things: the HOST_WRITE to TRANSFER_READ memory barrier over the staging
       writes (the same pair the frame's own batch upload runs over its arenas) and the image's transition
       out of the create-time layout into the upload layout. The two share a call because a barrier is a
       scope, not a buffer, and the copies below read the staging bytes AND write the image - one call
       whose srcStageMask names HOST and TRANSFER and whose dstStageMask names TRANSFER orders both.
       The image starts in XUD_TEXTURE_INITIAL_LAYOUT, which is UNDEFINED: the transition is what makes
       the copies' dstImageLayout a layout the image is actually in, and VUID-vkCmdPipelineBarrier-image-0407
       ("oldLayout must be the current layout of the image") is the rule the validation layer answers when
       it is not, which is the error this step fixes. srcAccessMask is 0 on the image half because UNDEFINED
       promises nothing about what was there before, so no prior access is being ordered.
    */
    {
        const VkMemoryBarrier hostToTransfer = (VkMemoryBarrier) { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = NULL,
            .srcAccessMask = XUD_TEXTURE_STAGING_SRC_ACCESS,
            .dstAccessMask = XUD_TEXTURE_STAGING_DST_ACCESS };
        VkImageMemoryBarrier toUpload = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .pNext = NULL };
        toUpload.srcAccessMask = 0u;
        toUpload.dstAccessMask = XUD_TEXTURE_UPLOAD_DST_ACCESS;
        toUpload.oldLayout = XUD_TEXTURE_INITIAL_LAYOUT;
        toUpload.newLayout = XUD_TEXTURE_UPLOAD_LAYOUT;
        toUpload.srcQueueFamilyIndex = 0u;
        toUpload.dstQueueFamilyIndex = 0u;
        toUpload.image = record->image;
        toUpload.subresourceRange = record->viewInfo.subresourceRange;
        loader->deviceTable.vkCmdPipelineBarrier(uploadBuffer, XUD_TEXTURE_STAGING_SRC_STAGE | XUD_TEXTURE_TRANSITION_SRC_STAGE,
            XUD_TEXTURE_STAGING_DST_STAGE, 0u, 1u, &hostToTransfer, 0u, NULL, 1u, &toUpload);
    }

    /* One copy per mip level, each from the level's own compact run in the staging staging into the
       image's own level: the region's row length and image height are the level's extent, which is
       what makes the staging arena's tight row pitch and the region's row length the same number -
       the layout's arithmetic and the copy's contract meeting at one number rather than two. The
       destination layout is the upload layout for every level, so the image is in one layout across
       the whole run of copies.
    */
    for (level = 0u; level < record->mipLevels; level++) {
        VkBufferImageCopy region = { .bufferOffset = record->mipOffset[level],
            .bufferRowLength = 0u,
            .bufferImageHeight = 0u,
            .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = level, .baseArrayLayer = 0u, .layerCount = 1u },
            .imageOffset = { .x = 0, .y = 0, .z = 0 },
            .imageExtent = { .width = 0u, .height = 0u, .depth = 1u } };
        if (xud_texture_mip_extent(record->width, record->height, level, record->mipLevels, &levelWidth, &levelHeight) != XUD_OK) {
            (void)loader->deviceTable.vkEndCommandBuffer(uploadBuffer);
            result = XUD_UNSUPPORTED;
            goto fail;
        }
        region.bufferRowLength = levelWidth;
        region.bufferImageHeight = levelHeight;
        region.imageExtent.width = levelWidth;
        region.imageExtent.height = levelHeight;
        loader->deviceTable.vkCmdCopyBufferToImage(
            uploadBuffer, record->stagingBuffer, record->image, XUD_TEXTURE_UPLOAD_LAYOUT, 1u, &region);
    }

    /* The transition that leaves the image in the layout a sampler needs: the copies wrote it in the
       upload layout, and a combined image sampler descriptor requires a layout a shader may read.
       TRANSFER_WRITE to SHADER_READ at the two shader stages is the pair the spec's supported-access-
       type table puts those two access types at, and the subresource range is the view's own range
       below, so a transition and the view that names the same image cannot drift apart.
    */
    {
        VkImageMemoryBarrier transition = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .pNext = NULL };
        transition.srcAccessMask = XUD_TEXTURE_TRANSITION_SRC_ACCESS;
        transition.dstAccessMask = XUD_TEXTURE_TRANSITION_DST_ACCESS;
        transition.oldLayout = XUD_TEXTURE_UPLOAD_LAYOUT;
        transition.newLayout = XUD_TEXTURE_SAMPLED_LAYOUT;
        transition.srcQueueFamilyIndex = 0u;
        transition.dstQueueFamilyIndex = 0u;
        transition.image = record->image;
        transition.subresourceRange = record->viewInfo.subresourceRange;
        loader->deviceTable.vkCmdPipelineBarrier(
            uploadBuffer, XUD_TEXTURE_TRANSITION_SRC_STAGE, XUD_TEXTURE_TRANSITION_DST_STAGE, 0u, 0u, NULL, 0u, NULL, 1u, &transition);
    }

    vk = loader->deviceTable.vkEndCommandBuffer(uploadBuffer);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK) {
        goto fail;
    }

    {
        VkFenceCreateInfo fenceInfo = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = NULL };
        fenceInfo.flags = 0u;
        vk = loader->deviceTable.vkCreateFence(device->device, &fenceInfo, NULL, &uploadFence);
        result = xud_result_from_vk(vk);
        if (result != XUD_OK) {
            uploadFence = VK_NULL_HANDLE;
            goto fail;
        }
    }

    /* The submit, on the graphics queue, with the upload buffer as its only command buffer and the
       fence as its own: no wait semaphore, because this upload is not a frame and has no acquire to
       be gated behind.
    */
    {
        VkSubmitInfo submitInfo = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = NULL };
        submitInfo.waitSemaphoreCount = 0u;
        submitInfo.pWaitSemaphores = NULL;
        submitInfo.pWaitDstStageMask = NULL;
        submitInfo.commandBufferCount = 1u;
        submitInfo.pCommandBuffers = &uploadBuffer;
        submitInfo.signalSemaphoreCount = 0u;
        submitInfo.pSignalSemaphores = NULL;
        vk = loader->deviceTable.vkQueueSubmit(device->graphicsQueue.queue, 1u, &submitInfo, uploadFence);
        result = xud_result_from_vk(vk);
        if (result != XUD_OK) {
            goto fail;
        }
    }

    /* The wait, which is what makes the staging unmap below a legal one: the copies have to have read
       the staging bytes before the mapping that made them host-visible is taken away, and an unmapped-
       while-still-read staging buffer is a use after the mapping that made it. The timeout is the frame
       rung's own no-timeout value, read rather than restated: a hang here is a driver fault this rung
       reports rather than a refusal it can take back, and a timeout of 0 would be a wait that returns
       without waiting, which is the opposite of the boundary this rung exists to enforce - these headers
       declare vkWaitForFences' last argument as a uint64_t timeout, so passing a VK_TRUE there is a wait
       that waited one nanosecond, which is what left the fence "currently in use" at the destroy below.
    */
    vk = loader->deviceTable.vkWaitForFences(device->device, 1u, &uploadFence, XUD_TEXTURE_WAIT_ALL, XUD_FRAME_NO_TIMEOUT);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK) {
        goto fail;
    }

    /* The three transient objects, retired in the order the ownership states: the command buffer goes
       back to its pool before the pool is destroyed, and the fence is destroyed once nothing is waiting
       on it - which the wait above just established.
    */
    if (loader->deviceTable.vkFreeCommandBuffers != NULL) {
        (void)loader->deviceTable.vkFreeCommandBuffers(device->device, uploadPool, 1u, &uploadBuffer);
    }
    uploadBuffer = VK_NULL_HANDLE;
    if (loader->deviceTable.vkDestroyCommandPool != NULL) {
        (void)loader->deviceTable.vkDestroyCommandPool(device->device, uploadPool, NULL);
    }
    uploadPool = VK_NULL_HANDLE;
    if (loader->deviceTable.vkDestroyFence != NULL) {
        (void)loader->deviceTable.vkDestroyFence(device->device, uploadFence, NULL);
    }
    uploadFence = VK_NULL_HANDLE;

    /* The staging half, retired now that the upload has run: the mapping is unmapped, the buffer
       destroyed and the block released, in that order, by the same helper the teardown below uses -
       which is why a record that reached this point has its staging fields empty rather than holding
       a copy of every texture's bytes on the host for the texture's whole life.
    */
    xud_texture_retire_staging(record, device, loader, alloc);

    /* The view and the sampler, created after the upload: both are stamped from the image, and the
       image is now in its sampled layout, which is the layout the view's range and the descriptor
       above will name. The view's create-info was stamped ABOVE the upload, beside the transition
       barrier that names the same range, so the create below hands the record's own row to the driver
       rather than stamping a second one that could disagree with the barrier's.
    */
    vk = loader->deviceTable.vkCreateImageView(device->device, &record->viewInfo, NULL, &record->view);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK) {
        record->view = VK_NULL_HANDLE;
        goto fail;
    }

    if (xud_texture_fill_sampler_info(data, &record->samplerInfo) != XUD_OK) {
        result = XUD_UNSUPPORTED;
        goto fail;
    }
    vk = loader->deviceTable.vkCreateSampler(device->device, &record->samplerInfo, NULL, &record->sampler);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK) {
        record->sampler = VK_NULL_HANDLE;
        goto fail;
    }

    record->imageLayout = XUD_TEXTURE_SAMPLED_LAYOUT;

    if (xud_handle_record_set(table, XUD_HANDLE_KIND_TEXTURE, outHandle->index, record) != XUD_OK) {
        xud_texture_retire_objects(record, device, loader, alloc);
        xud_texture_retire_staging(record, device, loader, alloc);
        goto fail;
    }

    return XUD_OK;

fail:;
    /* The transient upload objects first, in the reverse of the order they were made: they are not
       part of the record, so the retire below cannot see them, and the command buffer goes back to
       the pool before the pool is destroyed.
    */
    if (uploadBuffer != VK_NULL_HANDLE) {
        (void)loader->deviceTable.vkFreeCommandBuffers(device->device, uploadPool, 1u, &uploadBuffer);
    }
    if (uploadPool != VK_NULL_HANDLE) {
        (void)loader->deviceTable.vkDestroyCommandPool(device->device, uploadPool, NULL);
    }
    if (uploadFence != VK_NULL_HANDLE) {
        (void)loader->deviceTable.vkDestroyFence(device->device, uploadFence, NULL);
    }
    xud_texture_retire_objects(record, device, loader, alloc);
    xud_texture_retire_staging(record, device, loader, alloc);
    free(record);
    (void)xud_handle_free(table, *outHandle);
    return result;
}

/*-----------------------------------------------------------------------------------------------
 * The release - driver
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_texture_close(XudHandleTable* table, XudHandle handle, XudLoader* loader, const XudDevice* device, XudAllocator* alloc) {
    void* record = NULL;
    XudResult first = XUD_OK;
    XudResult guard = XUD_OK;

    if ((table == NULL) || (loader == NULL) || (device == NULL) || (alloc == NULL)) {
        return XUD_INVALID_ARG;
    }
    if (!xud_loader_is_open(loader)) {
        return XUD_UNSUPPORTED;
    }
    if (device->device == VK_NULL_HANDLE) {
        return XUD_INVALID_ARG;
    }
    if (!xud_loader_device_bound(loader, device->device)) {
        return XUD_UNSUPPORTED;
    }
    if (table->slots[XUD_HANDLE_KIND_TEXTURE] == NULL) {
        /* No TEXTURE array was ever allocated, so no texture of this table's was ever created and the
           handle in hand cannot name one - the same answer xud_texture_close_all() gives for the same
           reason, and the reason xud_handle_validate() below would give anyway.
        */
        return XUD_INVALID_ARG;
    }

    if (xud_handle_validate(table, handle, XUD_HANDLE_KIND_TEXTURE) != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    if (xud_handle_record_get(table, XUD_HANDLE_KIND_TEXTURE, handle.index, &record) != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    if (record == NULL) {
        return XUD_INVALID_ARG;
    }

    /* The wait-idle guard, and it is not decoration: a texture's image is read by a submission the mom
       ent anything samples it, and VUID-vkDestroyImage-image-00009 ("all uses of the image object must
       have completed execution") is the same rule the mesh's buffer destroy and the swapchain's recrea
       te state for their own objects. A guard that cannot run is a teardown this rung must not start,
       because freeing the record would leak every handle the driver still holds; a LOST device is stil
       l torn down and still reported, the rule xud_device_close() follows, because skipping the free wo
       uld leak the block the driver still holds.
    */
    guard = xud_device_wait_idle(device, loader);
    if (guard == XUD_UNSUPPORTED) {
        return XUD_UNSUPPORTED;
    }
    if (guard != XUD_OK) {
        first = guard;
    }

    /* Cleared BEFORE the free, and before xud_handle_free() marks the slot free: the material rung's
       rule, and the reason is the same one - xud_handle_free() leaves the record field as it was, so a
       free slot can otherwise still own a record, and then two teardown paths (this one and the table'
       s) would both destroy it.
    */
    if (xud_handle_record_set(table, XUD_HANDLE_KIND_TEXTURE, handle.index, NULL) != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    xud_texture_retire_objects((XudTextureRecord*)record, device, loader, alloc);
    xud_texture_retire_staging((XudTextureRecord*)record, device, loader, alloc);
    free(record);

    return xud_handle_free(table, handle) != XUD_OK ? XUD_INVALID_ARG : first;
}

/*-----------------------------------------------------------------------------------------------
 * The teardown rung above the handle table's own close - driver
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_texture_close_all(
    XudHandleTable* table, XudLoader* loader, const XudDevice* device, XudAllocator* alloc, uint32_t* outClosed) {
    void* record = NULL;
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
    if (device->device == VK_NULL_HANDLE) {
        return XUD_INVALID_ARG;
    }
    if (!xud_loader_device_bound(loader, device->device)) {
        return XUD_UNSUPPORTED;
    }

    if (table->slots[XUD_HANDLE_KIND_TEXTURE] == NULL) {
        /* No TEXTURE array was ever allocated: nothing of this kind was ever created, which is a count
           of zero and not an error - the same answer the mesh's and the material's close_all give for
           the same reason.
        */
        return XUD_OK;
    }

    /* The guard once for the whole walk rather than once per texture: a submission may still be readin
       g any one of the images below, and one boundary ahead of the walk is the same boundary the walk
       would otherwise ask for n times. The allocator's own wait-idle guard is xud_allocator_close()'s,
       not this one's - this rung releases ranges inside blocks the allocator still owns, and the alloca
       tor's close is what frees the blocks after its own guard.
    */
    guard = xud_device_wait_idle(device, loader);
    if (guard == XUD_UNSUPPORTED) {
        return XUD_UNSUPPORTED;
    }
    if (guard != XUD_OK) {
        first = guard;
    }

    /* Every row of the array is walked, live or free: xud_handle_free() leaves the record field as it
       was, so a slot a caller released still owns its record until this rung clears it, and a record lef
       t behind is a leak the table's own close cannot see (it frees the array, not what the array points
       at). The walk is bounded by the array's own high mark rather than by its capacity, so the rows pas
       t the last slot ever handed out - which have never held a record - are not read.
    */
    for (index = 0u; index < table->slotCount[XUD_HANDLE_KIND_TEXTURE]; index++) {
        if (xud_handle_record_get(table, XUD_HANDLE_KIND_TEXTURE, index, &record) != XUD_OK) {
            return XUD_INVALID_ARG;
        }
        if (record == NULL) {
            continue;
        }
        if (xud_handle_record_set(table, XUD_HANDLE_KIND_TEXTURE, index, NULL) != XUD_OK) {
            return XUD_INVALID_ARG;
        }
        xud_texture_retire_objects((XudTextureRecord*)record, device, loader, alloc);
        xud_texture_retire_staging((XudTextureRecord*)record, device, loader, alloc);
        free(record);
        (*outClosed)++;
    }

    return first;
}

/*-----------------------------------------------------------------------------------------------
 * The public pair above the context - the object's lifetime
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_create_texture(XudHandle* outHandle, XudContext* context, const XudTextureData* texture) {
    if ((outHandle == NULL) || (context == NULL) || (texture == NULL)) {
        return XUD_INVALID_ARG;
    }
    if ((context->handleTable == NULL) || (context->loader == NULL) || (context->device == NULL) || (context->allocator == NULL)) {
        /* A context with no handle table, no loader, no device or no allocator is a context that nev
           er completed bring-up: xud_init_context() hands back NULL for that case, so this is a cal
           ler holding on rather than a state the library leaves behind - the same answer every other
           public entry point gives for the same reason.
        */
        return XUD_UNSUPPORTED;
    }

    return xud_texture_open(
        outHandle, context->handleTable, context->loader, context->device, &context->physicalDevice->caps, context->allocator, texture);
}

XudResult xud_close_texture(XudContext* context, XudHandle handle) {
    if (context == NULL) {
        return XUD_INVALID_ARG;
    }
    if ((context->handleTable == NULL) || (context->loader == NULL) || (context->device == NULL) || (context->allocator == NULL)) {
        return XUD_UNSUPPORTED;
    }

    return xud_texture_close(context->handleTable, handle, context->loader, context->device, context->allocator);
}
