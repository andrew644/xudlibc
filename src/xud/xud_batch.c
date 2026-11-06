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
       survived into it would be a count naming vertices past the new buffers' contents. The cap
       acity ask restarts with them for the same reason - the ask was a refusal against the OLD
       capacities, and a batch that owns the new ones has not been refused anything yet. This is
       also where a grow retires the ask the frame boundary above asked for, which is what keeps a
       batch that now fits from doubling again next frame. */
    batch->vertexCount = 0u;
    batch->indexCount = 0u;
    batch->pendingFirstIndex = 0u;
    batch->pendingIndexCount = 0u;
    batch->wantedVertices = 0u;
    batch->wantedIndices = 0u;

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

    /* The third half, which only exists once a row can name geometry beside the arena: every
       recorded row names an entry of the geometry list that exists - the sentinel, or one of the
       entries handed out. A row naming past the list is a row the frame loop above would bind
       buffers from past the end of a heap array, and that is a broken state of this rung's own
       record rather than an argument a caller could have fixed. */
    for (index = 0u; index < batch->drawCount; index++) {
        if (batch->draws[index].geometryIndex > batch->geometryCount) {
            return XUD_UNSUPPORTED;
        }
    }

    return XUD_OK;
}

XudResult xud_batch_should_flush(uint32_t* outFlush, const XudBatch* batch, uint32_t requestedVariant, uint32_t requestedMaterial,
    uint32_t requestedTexture, uint32_t appendVertices, uint32_t appendIndices) {
    if ((outFlush == NULL) || (batch == NULL)) {
        return XUD_INVALID_ARG;
    }

    *outFlush = 0u;

    if (requestedVariant >= XUD_BATCH_VARIANT_COUNT) {
        return XUD_INVALID_ARG;
    }
    if ((requestedTexture != XUD_BATCH_NO_TEXTURE) && (requestedTexture >= XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE)) {
        return XUD_INVALID_ARG;
    }

    /* The rule, in the order the repo states it: a pipeline/material change flushes. The pending
       batch is empty when nothing has been accumulated under it, and an empty batch is not a
       batch to flush - the change still moves the current pair below. */
    if ((requestedVariant != batch->currentVariant) || (requestedMaterial != batch->currentMaterial)
        || (requestedTexture != batch->currentTexture)) {
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

/* The one capacity step both draw paths share: the draw list's doubling, in one place so the
   pending run's record and a mesh's record cannot grow the same array two ways. The step is the
   batch's own XUD_BATCH_DRAW_MIN rather than a doubling of drawCapacity because the sequence the
   rest of this tree uses is a step of the minimum, and a doubling of a capacity that started at
   0 would double 0 forever. */
static XudResult xud_batch_draw_reserve(XudBatch* batch) {
    XudBatchDraw* rows = NULL;

    if (batch->drawCount < batch->drawCapacity) {
        return XUD_OK;
    }
    rows = realloc(batch->draws, (size_t)(batch->drawCapacity + XUD_BATCH_DRAW_MIN) * sizeof(XudBatchDraw));
    if (rows == NULL) {
        return XUD_OUT_OF_MEMORY;
    }
    batch->draws = rows;
    batch->drawCapacity += XUD_BATCH_DRAW_MIN;

    return XUD_OK;
}

/* The same step for the geometry list. The list is the set of buffer sets this frame's rows bind,
   so its bound is the number of DISTINCT meshes a frame draws, not the number of draws - which is
   what a frame that draws one model a hundred times pays for. */
static XudResult xud_batch_geometry_reserve(XudBatch* batch) {
    XudBatchGeometry* rows = NULL;

    if (batch->geometryCount < batch->geometryCapacity) {
        return XUD_OK;
    }
    rows = realloc(batch->geometry, (size_t)(batch->geometryCapacity + XUD_BATCH_GEOMETRY_MIN) * sizeof(XudBatchGeometry));
    if (rows == NULL) {
        return XUD_OUT_OF_MEMORY;
    }
    batch->geometry = rows;
    batch->geometryCapacity += XUD_BATCH_GEOMETRY_MIN;

    return XUD_OK;
}

XudResult xud_batch_record_draw(XudBatch* batch, uint32_t topology, uint32_t variant, uint32_t materialIndex, uint32_t textureIndex) {
    XudResult result = XUD_OK;

    if (batch == NULL) {
        return XUD_INVALID_ARG;
    }
    if ((topology >= XUD_BATCH_TOPOLOGY_COUNT) || (variant >= XUD_BATCH_VARIANT_COUNT)) {
        return XUD_INVALID_ARG;
    }
    if ((textureIndex != XUD_BATCH_NO_TEXTURE) && (textureIndex >= XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE)) {
        return XUD_INVALID_ARG;
    }

    result = xud_batch_draw_reserve(batch);
    if (result != XUD_OK) {
        return result;
    }

    batch->draws[batch->drawCount].firstIndex = batch->pendingFirstIndex;
    batch->draws[batch->drawCount].indexCount = batch->pendingIndexCount;
    batch->draws[batch->drawCount].topology = topology;
    batch->draws[batch->drawCount].variant = variant;
    batch->draws[batch->drawCount].materialIndex = materialIndex;
    batch->draws[batch->drawCount].textureIndex = textureIndex;
    batch->draws[batch->drawCount].geometryIndex = XUD_BATCH_GEOMETRY_OWN;
    batch->drawCount++;

    batch->pendingFirstIndex = batch->indexCount;
    batch->pendingIndexCount = 0u;

    return XUD_OK;
}

/* The mesh's row, seen from the batch's side. A mesh is the settled SoA decision read one rung
   above: the same four streams, the same four widths, the same four formats, and an index buffer
   of its own. What changes here is therefore not the pipeline's vertex-input state - four bindings
   and four attributes, unchanged - but which buffers are bound at those four binding numbers
   beside this row, and binding is command-buffer state rather than pipeline state. The pending run
   of the batch's OWN arena is closed first, because a mesh appends nothing and the bytes a shape
   already wrote cannot end up in a row that binds a mesh's buffers; the mesh's own row is then
   recorded complete, its range being the whole of that mesh's index buffer. The five buffers are
   deduped against the list already recorded, so the frame loop rebinds once per distinct mesh
   rather than once per draw, and the list's indices are one past the array's positions because
   index 0 is the batch's own arena - a row xud_batch_geometry_get builds from the batch's live
   stream rows rather than a stored one, which is what keeps a grow that recreated the six buffers
   from leaving a stale row behind. */
XudResult xud_batch_record_mesh_draw(XudBatch* batch, uint32_t topology, uint32_t variant, uint32_t materialIndex, uint32_t textureIndex,
    const VkBuffer* buffers, const VkDeviceSize* offsets, uint32_t indexCount) {
    XudResult result = XUD_OK;
    uint32_t geometryIndex = XUD_BATCH_GEOMETRY_OWN;
    uint32_t found = 0u;
    uint32_t slot = 0u;
    uint32_t candidate = 0u;

    if ((batch == NULL) || (buffers == NULL) || (offsets == NULL)) {
        return XUD_INVALID_ARG;
    }
    if ((topology >= XUD_BATCH_TOPOLOGY_COUNT) || (variant >= XUD_BATCH_VARIANT_COUNT)) {
        return XUD_INVALID_ARG;
    }
    if ((textureIndex != XUD_BATCH_NO_TEXTURE) && (textureIndex >= XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE)) {
        return XUD_INVALID_ARG;
    }
    if (indexCount < 3u || ((indexCount % 3u) != 0u)) {
        return XUD_INVALID_ARG; /* the mesh rung's own rule, read again where the range is named */
    }
    for (slot = 0u; slot < XUD_BATCH_GEOMETRY_BUFFER_COUNT; slot++) {
        if (buffers[slot] == VK_NULL_HANDLE) {
            return XUD_INVALID_ARG; /* a row that binds a null buffer is a draw that reads nothing */
        }
    }

    if (batch->pendingIndexCount != 0u) {
        result = xud_batch_record_draw(batch, batch->currentTopology, batch->currentVariant, batch->currentMaterial, batch->currentTexture);
        if (result != XUD_OK) {
            return result;
        }
    }

    for (candidate = 0u; (found == 0u) && (candidate < batch->geometryCount); candidate++) {
        found = 1u;
        for (slot = 0u; slot < XUD_BATCH_GEOMETRY_BUFFER_COUNT; slot++) {
            if ((batch->geometry[candidate].buffer[slot] != buffers[slot]) || (batch->geometry[candidate].offset[slot] != offsets[slot])) {
                found = 0u;
                break;
            }
        }
        if (found != 0u) {
            geometryIndex = candidate + 1u;
        }
    }
    if (found == 0u) {
        result = xud_batch_geometry_reserve(batch);
        if (result != XUD_OK) {
            return result;
        }
        for (slot = 0u; slot < XUD_BATCH_GEOMETRY_BUFFER_COUNT; slot++) {
            batch->geometry[batch->geometryCount].buffer[slot] = buffers[slot];
            batch->geometry[batch->geometryCount].offset[slot] = offsets[slot];
        }
        batch->geometry[batch->geometryCount].indexCount = indexCount;
        geometryIndex = batch->geometryCount + 1u;
        batch->geometryCount++;
    }

    result = xud_batch_draw_reserve(batch);
    if (result != XUD_OK) {
        return result;
    }

    batch->draws[batch->drawCount].firstIndex = 0u;
    batch->draws[batch->drawCount].indexCount = indexCount;
    batch->draws[batch->drawCount].topology = topology;
    batch->draws[batch->drawCount].variant = variant;
    batch->draws[batch->drawCount].materialIndex = materialIndex;
    batch->draws[batch->drawCount].textureIndex = textureIndex;
    batch->draws[batch->drawCount].geometryIndex = geometryIndex;
    batch->drawCount++;

    return XUD_OK;
}

/* The row split: raylib's rlBegin(mode) on a mode change (reference/raylib/rlgl.h 1454-1478) closes
   the current run with drawCounter++ and writes the new mode into the new row. It does not draw
   and it does not end the batch, and neither does this: the batch stays open, the bytes its earlier
   rows name stay where they are, and the frame's upload still happens once at the boundary. The
   empty-run guard is raylib's own `if (vertexCount > 0)` beside that drawCounter++. */
XudResult xud_batch_set_topology(XudBatch* batch, uint32_t topology) {
    XudResult result = XUD_OK;

    if ((batch == NULL) || (topology >= XUD_BATCH_TOPOLOGY_COUNT)) {
        return XUD_INVALID_ARG;
    }
    if (topology == batch->currentTopology) {
        return XUD_OK;
    }

    if (batch->pendingIndexCount != 0u) {
        result = xud_batch_record_draw(batch, batch->currentTopology, batch->currentVariant, batch->currentMaterial, batch->currentTexture);
        if (result != XUD_OK) {
            return result;
        }
    }

    batch->currentTopology = topology;

    return XUD_OK;
}

/* The row split on its other axis: a material change closes the run that was pending under the
   previous state and starts a new one, exactly as a topology change does above. The two are one
   mechanism because in Vulkan both the topology (VkPipelineInputAssemblyStateCreateInfo::topolog
   y) and the blend attachment state (VkPipelineColorBlendAttachmentState) are PIPELINE state - a
   change of either selects a different row of the pipeline list for the draws that follow, and
   neither changes what has already been accumulated in the arena. That is why the pair moves toge
   ther in one call: the variant a material draws through is the material's own record member, so
   two separate state calls would be two splits, and the second would close a run that has no con
   tent in it. The index is not validated against a slot array here - the batch cannot name the
   handle layer - the public pair above validates the handle before it moves anything in. */
XudResult xud_batch_set_material(XudBatch* batch, uint32_t materialIndex, uint32_t variant, uint32_t textureIndex) {
    XudResult result = XUD_OK;

    if ((batch == NULL) || (variant >= XUD_BATCH_VARIANT_COUNT)) {
        return XUD_INVALID_ARG;
    }
    if ((textureIndex != XUD_BATCH_NO_TEXTURE) && (textureIndex >= XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE)) {
        return XUD_INVALID_ARG;
    }
    if ((materialIndex == batch->currentMaterial) && (variant == batch->currentVariant) && (textureIndex == batch->currentTexture)) {
        return XUD_OK;
    }

    if (batch->pendingIndexCount != 0u) {
        result = xud_batch_record_draw(batch, batch->currentTopology, batch->currentVariant, batch->currentMaterial, batch->currentTexture);
        if (result != XUD_OK) {
            return result;
        }
    }

    batch->currentMaterial = materialIndex;
    batch->currentVariant = variant;
    batch->currentTexture = textureIndex;

    return XUD_OK;
}

XudResult xud_batch_draw_get(const XudBatch* batch, uint32_t index, XudBatchDraw* outRow) {
    if ((batch == NULL) || (outRow == NULL) || (index >= batch->drawCount)) {
        return XUD_INVALID_ARG;
    }

    *outRow = batch->draws[index];
    return XUD_OK;
}

XudResult xud_batch_geometry_get(const XudBatch* batch, uint32_t index, XudBatchGeometry* outRow) {
    uint32_t slot = 0u;

    if ((batch == NULL) || (outRow == NULL) || (index > batch->geometryCount)) {
        return XUD_INVALID_ARG;
    }

    if (index == XUD_BATCH_GEOMETRY_OWN) {
        /* The batch's own arena, built from the stream rows that are live now rather than from a
           stored row: xud_batch_grow() recreates the six buffers, and a stored copy of the old
           ones would be a row the frame loop bound a draw from after the recreate. The offsets are
           0 because each stream is its own VkBuffer bound at its range's offset, so the vertex
           data starts at buffer offset 0 - the same fact the frame loop's first bind states.
        */
        for (slot = 0u; slot < XUD_BATCH_STREAM_COUNT; slot++) {
            outRow->buffer[slot] = batch->stream[slot].buffer;
            outRow->offset[slot] = 0u;
        }
        outRow->buffer[XUD_BATCH_STREAM_COUNT] = batch->indexBuffer;
        outRow->offset[XUD_BATCH_STREAM_COUNT] = 0u;
        outRow->indexCount = batch->indexCount;
        return XUD_OK;
    }

    *outRow = batch->geometry[index - 1u];
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
    batch->currentTopology = XUD_BATCH_TOPOLOGY_TRIANGLES;
    batch->currentVariant = XUD_BATCH_VARIANT_OPAQUE;
    batch->currentMaterial = XUD_BATCH_NO_MATERIAL;
    batch->currentTexture = XUD_BATCH_NO_TEXTURE;
    batch->geometryCount = 0u;

    return XUD_OK;
}

XudResult xud_batch_note_refused(XudBatch* batch, uint32_t requestedVertices, uint32_t requestedIndices) {
    if (batch == NULL) {
        return XUD_INVALID_ARG;
    }

    /* The ask is recorded against the batch's OWN capacities, and a request that fits records
       nothing: a batch that was never refused keeps answering 0/0, which is what makes the frame
       boundary's grow loop a no-op on an ordinary frame rather than a doubling every frame. The
       pair is a maxima and not a sum - a frame that refused several shapes refused them all
       against the same live counts, so the biggest ask is the one the next frame has to hold. */
    if ((requestedVertices > batch->wantedVertices) && (requestedVertices > batch->vertexCapacity)) {
        batch->wantedVertices = requestedVertices;
    }
    if ((requestedIndices > batch->wantedIndices) && (requestedIndices > batch->indexCapacity)) {
        batch->wantedIndices = requestedIndices;
    }

    return XUD_OK;
}

XudResult xud_batch_wanted(const XudBatch* batch, uint32_t* outVertices, uint32_t* outIndices) {
    if ((batch == NULL) || (outVertices == NULL) || (outIndices == NULL)) {
        return XUD_INVALID_ARG;
    }

    *outVertices = batch->wantedVertices;
    *outIndices = batch->wantedIndices;

    return XUD_OK;
}

XudResult xud_batch_state_get(const XudBatch* batch, uint32_t* outVariant, uint32_t* outMaterialIndex, uint32_t* outTextureIndex) {
    if ((batch == NULL) || (outVariant == NULL) || (outMaterialIndex == NULL) || (outTextureIndex == NULL)) {
        return XUD_INVALID_ARG;
    }

    *outVariant = batch->currentVariant;
    *outMaterialIndex = batch->currentMaterial;
    *outTextureIndex = batch->currentTexture;

    return XUD_OK;
}

XudResult xud_batch_growth_asked(const XudBatch* batch, uint32_t* outAsked) {
    if ((batch == NULL) || (outAsked == NULL)) {
        return XUD_INVALID_ARG;
    }

    /* Both halves, against the batch's OWN capacities: the vertex ask against vertexCapacity and
       the index ask against the indexCapacity xud_batch_layout computed as 3n/2 for it. Either
       half exceeding is enough - a frame that overflows the indices is a frame the next capacity
       has to hold, and the grow doubles both, so the ratio is kept rather than corrected. */
    *outAsked = ((batch->wantedVertices > batch->vertexCapacity) || (batch->wantedIndices > batch->indexCapacity)) ? 1u : 0u;

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

    batch->currentTopology = XUD_BATCH_TOPOLOGY_TRIANGLES;
    batch->currentVariant = XUD_BATCH_VARIANT_OPAQUE;
    batch->currentMaterial = XUD_BATCH_NO_MATERIAL;
    batch->currentTexture = XUD_BATCH_NO_TEXTURE;

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

    free(victim->geometry);
    victim->geometry = NULL;
    victim->geometryCount = 0u;
    victim->geometryCapacity = 0u;

    free(victim);
    *ioBatch = NULL;

    return XUD_OK;
}
