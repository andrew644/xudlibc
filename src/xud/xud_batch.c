/***********************************************************************************************
 *
 *   xud_batch.c - the batch rung: the four SoA attribute streams, the one index buffer, the
 *          mapped staging arena the CPU-side bytes are written into, and the flush decision
 *
 *   Owns : the rung below the frame loop that the settled "dynamic per-attribute stream
 *          buffers + one index buffer (SoA)" decision lands in. The pure half (the arena's
 *          layout, the capacity doubling, the two vertex-input rows, the five copy regions,
 *          the invariant, the flush decision, the draw rows, the appends) calls no driver
 *          function at all and is what the headless test run exercises; the driver half is
 *          six buffers created and bound out of the allocator, one map of the arena for the
 *          whole life of the batch, five vkCmdCopyBuffer calls, and the teardown that unmaps before
 *          it destroys.
 *   Layer: above the descriptor and the allocator, below the frame loop. Nothing in this file
 *          calls a Vulkan entry point directly - every one is reached through a row of
 *          XudLoader's Volk device table, so the archive holds no vk* symbol and the link line
 *          has no -lvulkan anywhere (AGENTS.md).
 *
 *   The one rule this file states rather than inherits: the arena is mapped ONCE, at open, and
 *          unmapped once, at close. The repo's rule is "no vkMapMemory per frame without a
 *          matching vkUnmapMemory + VkBufferInvalidateRangeInfo (or a vkCmdPipelineBarrier
 *          with the right scope)" - mapping once for the whole life of the batch is the form
 *          that satisfies it without a per-frame map/unmap pair to keep honest, and the
 *          per-frame part of the arena is its content being rewritten, not its mapping being
 *          redone. The two barriers that make a host write visible to a transfer and a
 *          transfer visible to a vertex shader are the frame loop's, one rung up, because the
 *          frame loop is what owns the command buffer they are recorded into.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include "xud_batch.h"
#include "xud.h"
#include "xud_allocator.h"
#include "xud_device.h"
#include "xud_loader.h"
#include "xud_physical.h"
#include "xud_vulkan.h"

#include <assert.h> /* Required for: static_assert, the counts that cannot disagree */
#include <stdlib.h> /* Required for: calloc, free, realloc */
#include <string.h> /* Required for: memcpy, the one host write into the mapped arena */

/* The rows this layer calls through must be pointer-sized, the same rule xud_loader.h checks
   for the table pair as a whole. */
static_assert(sizeof(PFN_vkCreateBuffer) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkDestroyBuffer) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkGetBufferMemoryRequirements) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkBindBufferMemory) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkMapMemory) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkUnmapMemory) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkCmdCopyBuffer) == sizeof(void*), "PFN row must be pointer-sized");

/* The stream array, the binding array, the attribute array and the copy-region array all have
   to be the count the layer's own constants claim, or the loops below and the counts handed to
   the driver would disagree. The member access inside sizeof is not evaluated. */
static_assert(
    sizeof(((XudBatch*)0)->stream) / sizeof(XudBatchStream) == XUD_BATCH_STREAM_COUNT, "the stream array must be exactly the four streams");
static_assert(sizeof(((XudBatch*)0)->streamOffset) / sizeof(VkDeviceSize) == XUD_BATCH_STREAM_COUNT,
    "the arena's offset array must be exactly the four streams");

/*-----------------------------------------------------------------------------------------------
 * The one retirement order, shared by the teardown and by every failure path of the bring-up
 *-----------------------------------------------------------------------------------------------*/

/* Reverse creation order, and every rung attempted after one refuses: a refused rung is reported
   to whoever can read it and the objects below still retire, because skipping them would leak
   them - the rule xud_close_context() states, one rung up. Each destroy/release is a no-op for
   an object that was never created, which is what makes this one order callable on a batch that
   failed part-way up its bring-up. The arena unmaps FIRST because a mapped range whose memory is
   about to be freed is a mapping the driver is still holding. */
static void xud_batch_retire(XudBatch* batch, const XudDevice* device, XudLoader* loader, XudAllocator* alloc) {
    uint32_t index = 0u;

    if ((batch == NULL) || (loader == NULL) || (device == NULL)) {
        return;
    }

    if (batch->stagingMappedLive != 0u) {
        if ((loader->deviceTable.vkUnmapMemory != NULL) && (batch->stagingRange.memory != VK_NULL_HANDLE)) {
            (void)loader->deviceTable.vkUnmapMemory(device->device, batch->stagingRange.memory);
        }
        batch->stagingMapped = NULL;
        batch->stagingMappedLive = 0u;
    }
    if (batch->stagingBuffer != VK_NULL_HANDLE) {
        if (loader->deviceTable.vkDestroyBuffer != NULL) {
            (void)loader->deviceTable.vkDestroyBuffer(device->device, batch->stagingBuffer, NULL);
        }
        batch->stagingBuffer = VK_NULL_HANDLE;
    }
    if (batch->indexBuffer != VK_NULL_HANDLE) {
        if (loader->deviceTable.vkDestroyBuffer != NULL) {
            (void)loader->deviceTable.vkDestroyBuffer(device->device, batch->indexBuffer, NULL);
        }
        batch->indexBuffer = VK_NULL_HANDLE;
    }
    for (index = 0u; index < XUD_BATCH_STREAM_COUNT; index++) {
        if (batch->stream[index].buffer == VK_NULL_HANDLE) {
            continue;
        }
        if (loader->deviceTable.vkDestroyBuffer != NULL) {
            (void)loader->deviceTable.vkDestroyBuffer(device->device, batch->stream[index].buffer, NULL);
        }
        batch->stream[index].buffer = VK_NULL_HANDLE;
    }

    /* The ranges go free after the buffers that name them are gone - the ownership direction the
       allocator states for a buffer and the memory it is bound to. */
    if (batch->indexRange.size != 0u) {
        (void)xud_allocator_release(alloc, &batch->indexRange);
    }
    for (index = 0u; index < XUD_BATCH_STREAM_COUNT; index++) {
        if (batch->stream[index].range.size != 0u) {
            (void)xud_allocator_release(alloc, &batch->stream[index].range);
        }
    }
    if (batch->stagingRange.size != 0u) {
        (void)xud_allocator_release(alloc, &batch->stagingRange);
    }
}

/*-----------------------------------------------------------------------------------------------
 * The arena's layout - pure, and the whole arithmetic half of the rung
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_batch_layout(XudBatch* batch, uint32_t vertexCapacity) {
    VkDeviceSize running = 0u;
    uint32_t index = 0u;

    if (batch == NULL) {
        return XUD_INVALID_ARG;
    }
    if ((vertexCapacity == 0u) || (vertexCapacity > XUD_BATCH_MAX_VERTICES)) {
        return XUD_UNSUPPORTED;
    }

    /* The four streams laid out back to back in the arena, in stream order, each at its own
       element size times the one capacity. The offsets are what the appends, the copy regions
       and the buffer sizes are all read off, so the three cannot drift apart from each other. */
    for (index = 0u; index < XUD_BATCH_STREAM_COUNT; index++) {
        /* The element width and the format are written here rather than at the create: they are
           facts of the layout, not facts of the driver, and writing them in the pure half is what
           lets xud_batch_check_invariant() run on a headless batch that never created a buffer. */
        batch->stream[index].format = XUD_BATCH_STREAM_FORMAT(index);
        batch->stream[index].elementSize = (VkDeviceSize)XUD_BATCH_STREAM_ELEMENT(index);
        batch->stream[index].capacityBytes = (VkDeviceSize)vertexCapacity * batch->stream[index].elementSize;
        batch->streamOffset[index] = running;
        running += batch->stream[index].capacityBytes;
    }
    batch->indexOffset = running;
    running += (VkDeviceSize)XUD_BATCH_INDEX_CAPACITY(vertexCapacity) * (VkDeviceSize)XUD_BATCH_INDEX_SIZE;

    batch->vertexCapacity = vertexCapacity;
    batch->indexCapacity = XUD_BATCH_INDEX_CAPACITY(vertexCapacity);
    batch->stagingBytes = running;

    /* The live counts restart here: a layout is a new set of buffers, and a live count that
       survived into it would be a count naming vertices past the new buffers' contents. */
    batch->vertexCount = 0u;
    batch->indexCount = 0u;
    batch->pendingFirstIndex = 0u;
    batch->pendingIndexCount = 0u;

    return XUD_OK;
}

XudResult xud_batch_next_capacity(uint32_t vertexCapacity, uint32_t* outNext) {
    if (outNext == NULL) {
        return XUD_INVALID_ARG;
    }

    *outNext = 0u;

    if (vertexCapacity == 0u) {
        return XUD_UNSUPPORTED;
    }
    if (vertexCapacity > (XUD_BATCH_MAX_VERTICES / 2u)) {
        /* At or past the half-way point the next doubling would pass the bound, and the bound is
           the allocator's - so the answer is "there is no next capacity", not "there is a bigger
           arena than the allocator's blocks can serve". */
        return XUD_OK;
    }

    *outNext = vertexCapacity * 2u;
    if (*outNext > XUD_BATCH_MAX_VERTICES) {
        *outNext = XUD_BATCH_MAX_VERTICES;
    }
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The two vertex-input rows - pure, and gated on the caps
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_batch_fill_binding(VkVertexInputBindingDescription* outRow, const XudDeviceCaps* caps, uint32_t streamIndex) {
    const VkPhysicalDeviceLimits* limits = NULL;

    if ((outRow == NULL) || (caps == NULL) || (streamIndex >= XUD_BATCH_STREAM_COUNT)) {
        return XUD_INVALID_ARG;
    }

    /* Two hops, not one: the caps' field is itself named properties, and its type is the *2
       wrapper whose core struct is the one that holds the limits - the member-path trap xud_
       physical.c states and xud_descriptor.c reads the same way. */
    limits = &caps->properties.properties.limits;

    if (streamIndex >= limits->maxVertexInputBindings) {
        return XUD_UNSUPPORTED;
    }

    outRow->binding = streamIndex;
    outRow->stride = (uint32_t)XUD_BATCH_STREAM_ELEMENT(streamIndex);
    outRow->inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    if (outRow->stride > limits->maxVertexInputBindingStride) {
        return XUD_UNSUPPORTED;
    }
    return XUD_OK;
}

XudResult xud_batch_fill_attribute(VkVertexInputAttributeDescription* outRow, const XudDeviceCaps* caps, uint32_t streamIndex) {
    const VkPhysicalDeviceLimits* limits = NULL;

    if ((outRow == NULL) || (caps == NULL) || (streamIndex >= XUD_BATCH_STREAM_COUNT)) {
        return XUD_INVALID_ARG;
    }

    limits = &caps->properties.properties.limits;
    if (streamIndex >= limits->maxVertexInputAttributes) {
        return XUD_UNSUPPORTED;
    }

    outRow->location = streamIndex;
    outRow->binding = streamIndex;
    outRow->format = XUD_BATCH_STREAM_FORMAT(streamIndex);
    /* offset 0 is the whole point of SoA here: each stream is its own VkBuffer, so an attribute
       never sits at a non-zero offset inside one, and the maxVertexInputAttributeOffset limit
       has nothing to be checked against. */
    outRow->offset = 0u;

    return XUD_OK;
}

XudResult xud_batch_fill_vertex_input(XudBatch* batch, const XudDeviceCaps* caps) {
    uint32_t index = 0u;

    if (batch == NULL) {
        return XUD_INVALID_ARG;
    }

    for (index = 0u; index < XUD_BATCH_STREAM_COUNT; index++) {
        if (xud_batch_fill_binding(&batch->bindings[index], caps, index) != XUD_OK) {
            return XUD_UNSUPPORTED;
        }
        if (xud_batch_fill_attribute(&batch->attributes[index], caps, index) != XUD_OK) {
            return XUD_UNSUPPORTED;
        }
        /* The stream's own rows are the same bytes, kept beside the buffer they describe: the
           frame loop binds through these, the pipeline was built from the array above, and one
           set of bytes is the reason the two cannot disagree. */
        batch->stream[index].binding = batch->bindings[index];
        batch->stream[index].attribute = batch->attributes[index];
    }

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The five copy regions - pure
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_batch_fill_copy_regions(XudBatch* batch) {
    uint32_t index = 0u;
    VkDeviceSize liveBytes = 0u;

    if (batch == NULL) {
        return XUD_INVALID_ARG;
    }

    if ((batch->vertexCount == 0u) || (batch->indexCount == 0u)) {
        return XUD_UNSUPPORTED;
    }

    for (index = 0u; index < XUD_BATCH_COPY_REGION_COUNT; index++) {
        if (index < XUD_BATCH_STREAM_COUNT) {
            liveBytes = (VkDeviceSize)batch->vertexCount * batch->stream[index].elementSize;
            batch->copyRegions[index].srcOffset = batch->streamOffset[index];
            batch->copyRegions[index].dstOffset = 0u;
            batch->copyRegions[index].size = liveBytes;
            continue;
        }
        liveBytes = (VkDeviceSize)batch->indexCount * (VkDeviceSize)XUD_BATCH_INDEX_SIZE;
        batch->copyRegions[index].srcOffset = batch->indexOffset;
        batch->copyRegions[index].dstOffset = 0u;
        batch->copyRegions[index].size = liveBytes;
    }

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The invariant, the flush decision and the draw rows - pure
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_batch_check_invariant(const XudBatch* batch) {
    uint32_t index = 0u;

    if (batch == NULL) {
        return XUD_UNSUPPORTED;
    }
    if (batch->vertexCapacity == 0u) {
        return XUD_UNSUPPORTED;
    }

    /* The first half of the settled decision's invariant: every stream's capacity, divided by its
       own element size, is the batch's one vertex capacity. Read at the capacities rather than at
       a per-stream count - there is one live vertex count for the whole batch, and what makes one
       vertex index address all four buffers is that all four hold the same number of vertices. */
    for (index = 0u; index < XUD_BATCH_STREAM_COUNT; index++) {
        if (batch->stream[index].elementSize == 0u) {
            return XUD_UNSUPPORTED;
        }
        if ((batch->stream[index].capacityBytes % batch->stream[index].elementSize) != 0u) {
            return XUD_UNSUPPORTED;
        }
        if ((batch->stream[index].capacityBytes / batch->stream[index].elementSize) != batch->vertexCapacity) {
            return XUD_UNSUPPORTED;
        }
    }

    /* The second half: the live counts are inside the capacities, so an append that passed the
       capacity was refused rather than wrapped, and a draw that names them reads real bytes. */
    if ((batch->vertexCount > batch->vertexCapacity) || (batch->indexCount > batch->indexCapacity)) {
        return XUD_UNSUPPORTED;
    }
    if (batch->indexCount > 0u) {
        return XUD_OK;
    }
    return XUD_OK;
}

XudResult xud_batch_should_flush(uint32_t* outFlush, const XudBatch* batch, uint32_t requestedVariant, uint32_t requestedMaterial,
    uint32_t appendVertices, uint32_t appendIndices) {
    if ((outFlush == NULL) || (batch == NULL)) {
        return XUD_INVALID_ARG;
    }

    *outFlush = 0u;

    if (requestedVariant >= XUD_BATCH_VARIANT_COUNT) {
        return XUD_INVALID_ARG;
    }

    /* The rule, in the order the repo states it: a pipeline/material change flushes. The pending
       batch is empty when nothing has been accumulated under it, and an empty batch is not a
       batch to flush - the change still moves the current pair below. */
    if ((requestedVariant != batch->currentVariant) || (requestedMaterial != batch->currentMaterial)) {
        if (batch->pendingIndexCount != 0u) {
            *outFlush = 1u;
        }
        return XUD_OK;
    }

    /* The second half of the same rule: an append that would pass the capacity flushes rather
       than wrap, because a stream that wrapped would be a draw whose index range names bytes the
       next frame overwrites. */
    if ((batch->vertexCount > batch->vertexCapacity) || (batch->indexCount > batch->indexCapacity)) {
        /* A live count already past its capacity is a broken state of this rung's own record,
           reported rather than subtracted from - the subtraction below would wrap. */
        return XUD_UNSUPPORTED;
    }
    if ((appendVertices > (batch->vertexCapacity - batch->vertexCount)) || (appendIndices > (batch->indexCapacity - batch->indexCount))) {
        if (batch->pendingIndexCount != 0u) {
            *outFlush = 1u;
        }
        return XUD_OK;
    }

    return XUD_OK;
}

XudResult xud_batch_record_draw(XudBatch* batch, uint32_t variant, uint32_t materialIndex) {
    XudBatchDraw* rows = NULL;

    if (batch == NULL) {
        return XUD_INVALID_ARG;
    }
    if (variant >= XUD_BATCH_VARIANT_COUNT) {
        return XUD_INVALID_ARG;
    }

    if (batch->drawCount == batch->drawCapacity) {
        rows = realloc(batch->draws, (size_t)(batch->drawCapacity + XUD_BATCH_DRAW_MIN) * sizeof(*batch->draws));
        if (rows == NULL) {
            return XUD_OUT_OF_MEMORY;
        }
        batch->draws = rows;
        batch->drawCapacity += XUD_BATCH_DRAW_MIN;
    }

    batch->draws[batch->drawCount].firstIndex = batch->pendingFirstIndex;
    batch->draws[batch->drawCount].indexCount = batch->pendingIndexCount;
    batch->draws[batch->drawCount].variant = variant;
    batch->draws[batch->drawCount].materialIndex = materialIndex;
    batch->drawCount++;

    batch->pendingFirstIndex = batch->indexCount;
    batch->pendingIndexCount = 0u;

    return XUD_OK;
}

XudResult xud_batch_draw_get(const XudBatch* batch, uint32_t index, XudBatchDraw* outRow) {
    if ((batch == NULL) || (outRow == NULL) || (index >= batch->drawCount)) {
        return XUD_INVALID_ARG;
    }

    *outRow = batch->draws[index];
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The appends - pure arithmetic over the arena, plus the one host write into the mapped bytes
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_batch_append_vertex(XudBatch* batch, const float pos[3], const float uv[2], const uint8_t color[4], const float normal[3]) {
    uint8_t* arena = NULL;
    uint32_t index = 0u;

    if ((batch == NULL) || (pos == NULL) || (uv == NULL) || (color == NULL) || (normal == NULL)) {
        return XUD_INVALID_ARG;
    }
    if ((batch->stagingMapped == NULL) || (batch->stagingMappedLive == 0u)) {
        return XUD_OUT_OF_MEMORY;
    }
    if (batch->vertexCount >= batch->vertexCapacity) {
        return XUD_OUT_OF_MEMORY;
    }

    arena = (uint8_t*)batch->stagingMapped;

    /* The four streams, written at their own stride into their own window of the arena. The
       widths are the ones xud_batch_layout() built the offsets from, so a write that stays
       inside a window stays inside the arena, and the arena is exactly the sum of the windows. */
    memcpy(arena + batch->streamOffset[XUD_BATCH_STREAM_POS] + ((size_t)batch->vertexCount * XUD_BATCH_STREAM_POS_SIZE), pos,
        XUD_BATCH_STREAM_POS_SIZE);
    memcpy(arena + batch->streamOffset[XUD_BATCH_STREAM_UV] + ((size_t)batch->vertexCount * XUD_BATCH_STREAM_UV_SIZE), uv,
        XUD_BATCH_STREAM_UV_SIZE);
    memcpy(arena + batch->streamOffset[XUD_BATCH_STREAM_COLOR] + ((size_t)batch->vertexCount * XUD_BATCH_STREAM_COLOR_SIZE), color,
        XUD_BATCH_STREAM_COLOR_SIZE);
    memcpy(arena + batch->streamOffset[XUD_BATCH_STREAM_NORMAL] + ((size_t)batch->vertexCount * XUD_BATCH_STREAM_NORMAL_SIZE), normal,
        XUD_BATCH_STREAM_NORMAL_SIZE);

    batch->vertexCount++;

    for (index = 0u; index < XUD_BATCH_STREAM_COUNT; index++) {
        if (batch->stream[index].capacityBytes < ((VkDeviceSize)batch->vertexCount * batch->stream[index].elementSize)) {
            /* The capacity check belongs here rather than only in xud_batch_check_invariant(): a
               stream shorter than the live count is a write past that buffer, and the write above
               has to be refused before it happens, not reported after it. */
            return XUD_UNSUPPORTED;
        }
    }

    return XUD_OK;
}

XudResult xud_batch_append_index(XudBatch* batch, uint32_t index) {
    uint8_t* arena = NULL;

    if (batch == NULL) {
        return XUD_INVALID_ARG;
    }
    if ((batch->stagingMapped == NULL) || (batch->stagingMappedLive == 0u)) {
        return XUD_OUT_OF_MEMORY;
    }
    if (index >= batch->vertexCount) {
        /* An index that names no live vertex is a bad argument: it is the same rule vkCmdDraw
           Indexed's index range enforces on the driver side, checked here where it is testable. */
        return XUD_INVALID_ARG;
    }
    if (batch->indexCount >= batch->indexCapacity) {
        return XUD_OUT_OF_MEMORY;
    }

    arena = (uint8_t*)batch->stagingMapped;
    memcpy(arena + batch->indexOffset + ((size_t)batch->indexCount * XUD_BATCH_INDEX_SIZE), &index, XUD_BATCH_INDEX_SIZE);

    batch->indexCount++;
    batch->pendingIndexCount++;

    return XUD_OK;
}

XudResult xud_batch_reset(XudBatch* batch) {
    if (batch == NULL) {
        return XUD_INVALID_ARG;
    }

    batch->vertexCount = 0u;
    batch->indexCount = 0u;
    batch->drawCount = 0u;
    batch->pendingFirstIndex = 0u;
    batch->pendingIndexCount = 0u;
    batch->currentVariant = XUD_BATCH_VARIANT_OPAQUE;
    batch->currentMaterial = XUD_BATCH_NO_MATERIAL;

    return XUD_OK;
}

XudResult xud_batch_stats(const XudBatch* batch, uint32_t* outVertexCount, uint32_t* outIndexCount, uint32_t* outVertexCapacity,
    uint32_t* outDrawCount, VkDeviceSize* outStagingBytes) {
    if ((batch == NULL) || (outVertexCount == NULL) || (outIndexCount == NULL) || (outVertexCapacity == NULL) || (outDrawCount == NULL)
        || (outStagingBytes == NULL)) {
        return XUD_INVALID_ARG;
    }

    *outVertexCount = batch->vertexCount;
    *outIndexCount = batch->indexCount;
    *outVertexCapacity = batch->vertexCapacity;
    *outDrawCount = batch->drawCount;
    *outStagingBytes = batch->stagingBytes;

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The bring-up - the driver half
 *-----------------------------------------------------------------------------------------------*/

/* Pure, and the one place the "check before you use" rule is read at the memory rows: the mask of
   the caps' types that a CPU write can land in. Narrowing a buffer's own memoryTypeBits to this
   mask is a preference, not a lie - every type in the narrowed mask is also in the buffer's real
   one, so the bind below is still valid against the real requirements, and the type the acquire's
   own rule picks for the narrowed mask is re-checked against HOST_VISIBLE below rather than trus
   ted.
*/
XudResult xud_batch_host_visible_bits(const XudDeviceCaps* caps, uint32_t* outMemoryTypeBits) {
    const VkPhysicalDeviceMemoryProperties* mem = NULL;
    uint32_t index = 0u;
    uint32_t mask = 0u;

    if ((caps == NULL) || (outMemoryTypeBits == NULL)) {
        return XUD_INVALID_ARG;
    }

    mem = &caps->memoryProperties.memoryProperties;
    if (mem->memoryTypeCount > VK_MAX_MEMORY_TYPES) {
        return XUD_INVALID_ARG;
    }

    for (index = 0u; index < mem->memoryTypeCount; index++) {
        if ((mem->memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0u) {
            mask |= (1u << index);
        }
    }

    *outMemoryTypeBits = mask;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The bring-up - the driver half
 *-----------------------------------------------------------------------------------------------*/

static XudResult xud_batch_create_staging(
    XudBatch* batch, XudLoader* loader, const XudDevice* device, const XudDeviceCaps* caps, XudAllocator* alloc) {
    VkMemoryRequirements needs;
    VkMemoryRequirements narrowed;
    uint32_t hostVisibleMask = 0u;
    uint32_t memoryTypeIndex = 0u;
    XudResult result = XUD_INVALID_ARG;
    VkResult vk = VK_ERROR_UNKNOWN;
    void* mapped = NULL;

    if ((batch == NULL) || (loader == NULL) || (device == NULL) || (caps == NULL) || (alloc == NULL)) {
        return XUD_UNSUPPORTED;
    }
    if ((loader->deviceTable.vkCreateBuffer == NULL) || (loader->deviceTable.vkDestroyBuffer == NULL)
        || (loader->deviceTable.vkGetBufferMemoryRequirements == NULL) || (loader->deviceTable.vkBindBufferMemory == NULL)
        || (loader->deviceTable.vkMapMemory == NULL) || (loader->deviceTable.vkUnmapMemory == NULL)) {
        return XUD_UNSUPPORTED;
    }
    if (batch->stagingBytes == 0u) {
        return XUD_INVALID_ARG;
    }

    batch->stagingBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    batch->stagingBufferInfo.pNext = NULL;
    batch->stagingBufferInfo.flags = 0u;
    batch->stagingBufferInfo.size = batch->stagingBytes;
    /* TRANSFER_SRC alone: the arena is a source of every copy and never a destination of one, and
       it is not readable by any shader stage, so no usage bit past the transfer half is claimed
       for it. */
    batch->stagingBufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    batch->stagingBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    batch->stagingBufferInfo.queueFamilyIndexCount = 0u;
    batch->stagingBufferInfo.pQueueFamilyIndices = NULL;

    vk = loader->deviceTable.vkCreateBuffer(device->device, &batch->stagingBufferInfo, NULL, &batch->stagingBuffer);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK) {
        batch->stagingBuffer = VK_NULL_HANDLE;
        return result;
    }

    loader->deviceTable.vkGetBufferMemoryRequirements(device->device, batch->stagingBuffer, &needs);
    narrowed = needs;

    hostVisibleMask = 0u;
    result = xud_batch_host_visible_bits(caps, &hostVisibleMask);
    if (result != XUD_OK) {
        loader->deviceTable.vkDestroyBuffer(device->device, batch->stagingBuffer, NULL);
        batch->stagingBuffer = VK_NULL_HANDLE;
        return result;
    }
    if (hostVisibleMask == 0u) {
        /* No host-visible type at all: the arena cannot be written from the CPU, and the batch is
           a batch that has nowhere to append to. Reported, not worked around. */
        loader->deviceTable.vkDestroyBuffer(device->device, batch->stagingBuffer, NULL);
        batch->stagingBuffer = VK_NULL_HANDLE;
        return XUD_UNSUPPORTED;
    }
    narrowed.memoryTypeBits &= hostVisibleMask;
    if (narrowed.memoryTypeBits == 0u) {
        /* The buffer's own mask and the host-visible rows do not intersect: this buffer cannot be
           bound to host-visible memory on this driver. */
        loader->deviceTable.vkDestroyBuffer(device->device, batch->stagingBuffer, NULL);
        batch->stagingBuffer = VK_NULL_HANDLE;
        return XUD_UNSUPPORTED;
    }

    /* The arena is carved from the STATIC blocks, not from the allocator's transient ring, and the
       reason is the ring's own contract: a ring is a cursor that wraps, so anything it hands out
       at one lap is overwritten by a later one, and this arena holds the frame's bytes from the
       moment the appends wrote them to the moment the copies read them. A static carve is the one
       that lives that long. It is the repo's "allocate once, suballocate always" rule read at the
       buffer that made it largest - with the one exception that rule has: the arena is MAPPED, an
       d a mapping is per-VkDeviceMemory rather than per-range, so the acquire is the host-visible
       one and the arena owns the block it lives in. 2,688 B of arena is one block, not one vkAllo
       cateMemory per stream.
    */
    result = xud_allocator_acquire_host_visible(&batch->stagingRange, alloc, loader, device, caps, &narrowed);
    if (result != XUD_OK) {
        loader->deviceTable.vkDestroyBuffer(device->device, batch->stagingBuffer, NULL);
        batch->stagingBuffer = VK_NULL_HANDLE;
        return result;
    }

    /* The check-before-you-use rule read at the mapping rather than at the map: the type the acq
       uire actually allocated at has to be host-visible, and it is re-read from the allocator's o
       wn type rule rather than trusted from the mask that was asked for, because a mask is only a
       preference and the row it picked is the one vkMapMemory is called on.
    */
    memoryTypeIndex = 0u;
    result = xud_allocator_select_memory_type(caps, &narrowed, &memoryTypeIndex);
    if ((result != XUD_OK) || (memoryTypeIndex >= caps->memoryProperties.memoryProperties.memoryTypeCount)
        || ((caps->memoryProperties.memoryProperties.memoryTypes[memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            == 0u)) {
        xud_allocator_release(alloc, &batch->stagingRange);
        loader->deviceTable.vkDestroyBuffer(device->device, batch->stagingBuffer, NULL);
        batch->stagingBuffer = VK_NULL_HANDLE;
        return XUD_UNSUPPORTED;
    }

    vk = loader->deviceTable.vkBindBufferMemory(
        device->device, batch->stagingBuffer, batch->stagingRange.memory, batch->stagingRange.offset);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK) {
        xud_allocator_release(alloc, &batch->stagingRange);
        loader->deviceTable.vkDestroyBuffer(device->device, batch->stagingBuffer, NULL);
        batch->stagingBuffer = VK_NULL_HANDLE;
        return result;
    }

    /* The map, once, for the whole arena, for the whole life of the batch. flags 0 is the
       read/write pair the repo's rule asks for: the arena is written by the CPU and read by a
       transfer, and no cache-control flag is claimed for a type this layer has not asked to be
       cached. */
    vk = loader->deviceTable.vkMapMemory(
        device->device, batch->stagingRange.memory, batch->stagingRange.offset, batch->stagingRange.size, 0u, &mapped);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK) {
        xud_allocator_release(alloc, &batch->stagingRange);
        loader->deviceTable.vkDestroyBuffer(device->device, batch->stagingBuffer, NULL);
        batch->stagingBuffer = VK_NULL_HANDLE;
        return result;
    }

    batch->stagingMapped = mapped;
    batch->stagingMappedLive = 1u;

    return XUD_OK;
}

static XudResult xud_batch_create_one(
    XudBatch* batch, XudLoader* loader, const XudDevice* device, const XudDeviceCaps* caps, XudAllocator* alloc, uint32_t streamIndex) {
    VkMemoryRequirements needs;
    XudResult result = XUD_INVALID_ARG;
    VkResult vk = VK_ERROR_UNKNOWN;
    XudBatchStream* row = NULL;

    if ((batch == NULL) || (loader == NULL) || (device == NULL) || (caps == NULL) || (alloc == NULL)
        || (streamIndex >= XUD_BATCH_STREAM_COUNT)) {
        return XUD_UNSUPPORTED;
    }
    if ((loader->deviceTable.vkCreateBuffer == NULL) || (loader->deviceTable.vkDestroyBuffer == NULL)
        || (loader->deviceTable.vkGetBufferMemoryRequirements == NULL) || (loader->deviceTable.vkBindBufferMemory == NULL)) {
        return XUD_UNSUPPORTED;
    }

    row = &batch->stream[streamIndex];
    /* The width, the format and the capacity are xud_batch_layout()'s, read rather than
       recomputed: the buffer's size and the arena's window are one pair of numbers, and the
       append offsets are the third. */
    if ((row->elementSize == 0u) || (row->capacityBytes == 0u)) {
        return XUD_INVALID_ARG;
    }

    row->bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    row->bufferInfo.pNext = NULL;
    row->bufferInfo.flags = 0u;
    row->bufferInfo.size = row->capacityBytes;
    /* VERTEX_BUFFER is what the pipeline's attribute reads and TRANSFER_DST is what the copy
       writes, both claimed at the create rather than discovered later - the repo's rule that a
       buffer's usage is what it is used for, stated once. */
    row->bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    row->bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    row->bufferInfo.queueFamilyIndexCount = 0u;
    row->bufferInfo.pQueueFamilyIndices = NULL;

    vk = loader->deviceTable.vkCreateBuffer(device->device, &row->bufferInfo, NULL, &row->buffer);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK) {
        row->buffer = VK_NULL_HANDLE;
        return result;
    }

    loader->deviceTable.vkGetBufferMemoryRequirements(device->device, row->buffer, &needs);
    result = xud_allocator_acquire(&row->range, alloc, loader, device, caps, &needs);
    if (result != XUD_OK) {
        loader->deviceTable.vkDestroyBuffer(device->device, row->buffer, NULL);
        row->buffer = VK_NULL_HANDLE;
        return result;
    }

    vk = loader->deviceTable.vkBindBufferMemory(device->device, row->buffer, row->range.memory, row->range.offset);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK) {
        xud_allocator_release(alloc, &row->range);
        loader->deviceTable.vkDestroyBuffer(device->device, row->buffer, NULL);
        row->buffer = VK_NULL_HANDLE;
        return result;
    }

    return XUD_OK;
}

static XudResult xud_batch_create_index(
    XudBatch* batch, XudLoader* loader, const XudDevice* device, const XudDeviceCaps* caps, XudAllocator* alloc) {
    VkMemoryRequirements needs;
    XudResult result = XUD_INVALID_ARG;
    VkResult vk = VK_ERROR_UNKNOWN;

    if ((batch == NULL) || (loader == NULL) || (device == NULL) || (caps == NULL) || (alloc == NULL)) {
        return XUD_UNSUPPORTED;
    }
    if ((loader->deviceTable.vkCreateBuffer == NULL) || (loader->deviceTable.vkDestroyBuffer == NULL)
        || (loader->deviceTable.vkGetBufferMemoryRequirements == NULL) || (loader->deviceTable.vkBindBufferMemory == NULL)) {
        return XUD_UNSUPPORTED;
    }

    batch->indexBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    batch->indexBufferInfo.pNext = NULL;
    batch->indexBufferInfo.flags = 0u;
    batch->indexBufferInfo.size = (VkDeviceSize)batch->indexCapacity * (VkDeviceSize)XUD_BATCH_INDEX_SIZE;
    batch->indexBufferInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    batch->indexBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    batch->indexBufferInfo.queueFamilyIndexCount = 0u;
    batch->indexBufferInfo.pQueueFamilyIndices = NULL;

    vk = loader->deviceTable.vkCreateBuffer(device->device, &batch->indexBufferInfo, NULL, &batch->indexBuffer);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK) {
        batch->indexBuffer = VK_NULL_HANDLE;
        return result;
    }

    loader->deviceTable.vkGetBufferMemoryRequirements(device->device, batch->indexBuffer, &needs);
    result = xud_allocator_acquire(&batch->indexRange, alloc, loader, device, caps, &needs);
    if (result != XUD_OK) {
        loader->deviceTable.vkDestroyBuffer(device->device, batch->indexBuffer, NULL);
        batch->indexBuffer = VK_NULL_HANDLE;
        return result;
    }

    vk = loader->deviceTable.vkBindBufferMemory(device->device, batch->indexBuffer, batch->indexRange.memory, batch->indexRange.offset);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK) {
        xud_allocator_release(alloc, &batch->indexRange);
        loader->deviceTable.vkDestroyBuffer(device->device, batch->indexBuffer, NULL);
        batch->indexBuffer = VK_NULL_HANDLE;
        return result;
    }

    return XUD_OK;
}

XudResult xud_batch_open(XudBatch** outBatch, XudLoader* loader, const XudDevice* device, const XudDeviceCaps* caps, XudAllocator* alloc) {
    XudBatch* batch = NULL;
    uint32_t index = 0u;
    XudResult result = XUD_INVALID_ARG;

    if ((outBatch == NULL) || (*outBatch != NULL)) {
        return XUD_INVALID_ARG;
    }
    if ((loader == NULL) || (device == NULL) || (caps == NULL) || (alloc == NULL)) {
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

    /* Every destroy row before every create row: an object this layer cannot destroy is an object
       it must not create - the rule xud_window.c applies to a surface and xud_swapchain.c to a
       swapchain, read here for six buffers at once. The map and the unmap rows are checked
       together, because a mapped arena that cannot be unmapped is a mapping that leaks. */
    if ((loader->deviceTable.vkDestroyBuffer == NULL) || (loader->deviceTable.vkCreateBuffer == NULL)
        || (loader->deviceTable.vkGetBufferMemoryRequirements == NULL) || (loader->deviceTable.vkBindBufferMemory == NULL)
        || (loader->deviceTable.vkMapMemory == NULL) || (loader->deviceTable.vkUnmapMemory == NULL)) {
        return XUD_UNSUPPORTED;
    }

    /* The four vertex bindings and the four vertex attributes have to fit the caps before a single
       buffer is created: a device that answers fewer than four of either cannot hold this
       batch's vertex-input state at all, and creating six buffers it can never be drawn with is
       work for a refusal that the caps already tell us about. */
    if ((caps->properties.properties.limits.maxVertexInputBindings < XUD_BATCH_STREAM_COUNT)
        || (caps->properties.properties.limits.maxVertexInputAttributes < XUD_BATCH_STREAM_COUNT)) {
        return XUD_UNSUPPORTED;
    }

    batch = calloc(1u, sizeof(*batch));
    if (batch == NULL) {
        return XUD_OUT_OF_MEMORY;
    }

    batch->currentVariant = XUD_BATCH_VARIANT_OPAQUE;
    batch->currentMaterial = XUD_BATCH_NO_MATERIAL;

    result = xud_batch_layout(batch, XUD_BATCH_INITIAL_VERTICES);
    if (result != XUD_OK) {
        free(batch);
        return result;
    }

    result = xud_batch_fill_vertex_input(batch, caps);
    if (result != XUD_OK) {
        free(batch);
        return result;
    }

    result = xud_batch_create_staging(batch, loader, device, caps, alloc);
    if (result != XUD_OK) {
        free(batch);
        return result;
    }

    for (index = 0u; index < XUD_BATCH_STREAM_COUNT; index++) {
        result = xud_batch_create_one(batch, loader, device, caps, alloc, index);
        if (result != XUD_OK) {
            xud_batch_retire(batch, device, loader, alloc);
            free(batch);
            return result;
        }
    }

    result = xud_batch_create_index(batch, loader, device, caps, alloc);
    if (result != XUD_OK) {
        xud_batch_retire(batch, device, loader, alloc);
        free(batch);
        return result;
    }

    if (xud_batch_check_invariant(batch) != XUD_OK) {
        /* The invariant the settled decision states is checked here rather than assumed: a batch
           whose four streams do not hold the same vertex count is a batch whose appends would
           write past a buffer, and that is a state this rung's own record made. */
        xud_batch_retire(batch, device, loader, alloc);
        free(batch);
        return XUD_INTERNAL_ERROR;
    }

    *outBatch = batch;
    return XUD_OK;
}

XudResult xud_batch_grow(XudBatch* batch, XudLoader* loader, const XudDevice* device, const XudDeviceCaps* caps, XudAllocator* alloc) {
    uint32_t next = 0u;
    uint32_t index = 0u;
    XudResult result = XUD_INVALID_ARG;

    if ((batch == NULL) || (loader == NULL) || (device == NULL) || (caps == NULL) || (alloc == NULL)) {
        return XUD_UNSUPPORTED;
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
    if ((batch->stagingBuffer == VK_NULL_HANDLE) || (batch->indexBuffer == VK_NULL_HANDLE)) {
        return XUD_INVALID_ARG;
    }
    for (index = 0u; index < XUD_BATCH_STREAM_COUNT; index++) {
        if (batch->stream[index].buffer == VK_NULL_HANDLE) {
            return XUD_INVALID_ARG;
        }
    }

    result = xud_batch_next_capacity(batch->vertexCapacity, &next);
    if ((result != XUD_OK) || (next == 0u)) {
        /* At the bound the batch is full, and full is a decision the caller above reads rather
           than a growth step that quietly asks for an arena bigger than the allocator's block cei
           ling can serve. */
        return XUD_UNSUPPORTED;
    }

    /* The grow is a recreate at a frame boundary, never inside a recording: the repo's rule that
       no vkCreateBuffer runs inside a frame is what makes the frame loop's refusal of an
       over-capacity append the only honest answer mid-frame, and this call is where the frame
       loop is between two recordings. A grow that fails below here leaves the batch RETIRED - the
       recreate retires before it creates, so there is nothing half-built to leak, and the caller
       above closes the batch rather than appending into it. */
    xud_batch_retire(batch, device, loader, alloc);

    result = xud_batch_layout(batch, next);
    if (result != XUD_OK) {
        return result;
    }

    result = xud_batch_create_staging(batch, loader, device, caps, alloc);
    if (result != XUD_OK) {
        return result;
    }

    for (index = 0u; index < XUD_BATCH_STREAM_COUNT; index++) {
        result = xud_batch_create_one(batch, loader, device, caps, alloc, index);
        if (result != XUD_OK) {
            return result;
        }
    }

    result = xud_batch_create_index(batch, loader, device, caps, alloc);
    if (result != XUD_OK) {
        return result;
    }

    batch->growCount++;
    return XUD_OK;
}

XudResult xud_batch_upload(XudBatch* batch, XudLoader* loader, const XudDevice* device, VkCommandBuffer commandBuffer) {
    XudResult result = XUD_INVALID_ARG;
    VkBuffer destination = VK_NULL_HANDLE;
    uint32_t index = 0u;

    if ((batch == NULL) || (loader == NULL) || (device == NULL) || (commandBuffer == VK_NULL_HANDLE)) {
        return XUD_INVALID_ARG;
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
    if ((batch->stagingBuffer == VK_NULL_HANDLE) || (batch->indexBuffer == VK_NULL_HANDLE)) {
        return XUD_INVALID_ARG;
    }

    if ((batch->stagingMapped == NULL) || (batch->stagingMappedLive == 0u)) {
        return XUD_UNSUPPORTED;
    }
    if (loader->deviceTable.vkCmdCopyBuffer == NULL) {
        return XUD_UNSUPPORTED;
    }

    result = xud_batch_fill_copy_regions(batch);
    if (result != XUD_OK) {
        return result;
    }

    /* Every destination checked before the first copy is recorded: a half-recorded upload is a
       command buffer the frame loop will submit with some streams bound and some not, and this
       rung does not own that buffer to be able to say it is discarded. */
    for (index = 0u; index < XUD_BATCH_COPY_REGION_COUNT; index++) {
        destination = (index < XUD_BATCH_STREAM_COUNT) ? batch->stream[index].buffer : batch->indexBuffer;
        if (destination == VK_NULL_HANDLE) {
            return XUD_INVALID_ARG;
        }
    }

    /* Five calls, each carrying the one region for one destination. vkCmdCopyBuffer names ONE
       source and ONE destination buffer, so five destinations is five calls and the arena stays
       the single source of all five - which is what the SoA decision's "a stream is the unit of
       the copy" means read against these headers. The call is legal only OUTSIDE a render pass
       instance, which is the frame loop's job to honour - and the two barriers that make the host
       write visible to the transfer and the transfer visible to the vertex shader are its job too,
       because a barrier recorded into a command buffer this rung does not own is not a decision
       this rung can make. A transfer command has no stage, so no stage mask is an argument here
       at all. */
    for (index = 0u; index < XUD_BATCH_COPY_REGION_COUNT; index++) {
        destination = (index < XUD_BATCH_STREAM_COUNT) ? batch->stream[index].buffer : batch->indexBuffer;
        loader->deviceTable.vkCmdCopyBuffer(commandBuffer, batch->stagingBuffer, destination, 1u, &batch->copyRegions[index]);
    }

    return XUD_OK;
}

XudResult xud_batch_close(XudBatch** ioBatch, const XudDevice* device, XudLoader* loader, XudAllocator* alloc) {
    XudBatch* victim = NULL;

    if (ioBatch == NULL) {
        return XUD_INVALID_ARG;
    }
    if (*ioBatch == NULL) {
        return XUD_ALREADY;
    }
    victim = *ioBatch;

    if ((loader == NULL) || (device == NULL) || (alloc == NULL)) {
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

    /* A destroy row that is nil while the object it would destroy is live is a refusal that leaves
       the struct in place, because freeing it would leak six handles silently - the rule xud_
       frame_close() and xud_pipeline_close() state one rung up. */
    if (loader->deviceTable.vkDestroyBuffer == NULL) {
        if ((victim->stagingBuffer != VK_NULL_HANDLE) || (victim->indexBuffer != VK_NULL_HANDLE)) {
            return XUD_UNSUPPORTED;
        }
    }

    xud_batch_retire(victim, device, loader, alloc);

    free(victim->draws);
    victim->draws = NULL;
    victim->drawCount = 0u;
    victim->drawCapacity = 0u;

    free(victim);
    *ioBatch = NULL;

    return XUD_OK;
}
