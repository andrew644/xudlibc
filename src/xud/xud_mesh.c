/***********************************************************************************************
 *   xud_mesh.c - the mesh rung: the record behind a MESH-kind slot of the handle layer, the
 *                mesh's own five SoA buffers in one host-visible block, and the public
 *                upload/close pair xud.h declares above it
 *
 *   Owns : the nine rungs xud_mesh.h declares (xud_mesh_check(), xud_mesh_layout(), xud_mesh_
 *          caps_support(), xud_mesh_fill_buffer_info(), xud_mesh_write_arena(), xud_mesh_open(),
 *          xud_mesh_close(), xud_mesh_get(), xud_mesh_close_all()) and the two public calls xud_
 *          h declares above them (xud_upload_mesh(), xud_close_mesh()). The split is the point of
 *          the file: the mesh's byte layout, its argument checks, its caps gate and its write are
 *          pure and run with no driver present - a mesh's arena is a range of bytes and a test can
 *          hand the write its own heap buffer instead of one vkMapMemory wrote - and the creates,
 *          the binds, the map and the destroys are the driver half.
 *
 *   What the rung owns and what it borrows: it owns the record, the mesh's five VkBuffers and the
 *   ONE host-visible block their bytes live in. It borrows the handle table (the slot, and the rec
 *   ord field through the handle layer's own two accessors - the array is the handle layer's and it
 *   reallocs it as a unit, so nothing here indexes it), the allocator (the block - the mesh never ca
 *   lls vkAllocateMemory itself), the loader's device table and the device. It borrows nothing of t
 *   he renderer and nothing of the batch: a mesh is not a frame object, it is an object a caller cr
 *   eates once and holds for as long as it draws.
 *
 *   Why the write is a memcpy into a mapping rather than a transfer: the batch's arena is rewritten
 *   every frame and read by vkCmdCopyBuffer, so it needs a staging buffer, a command buffer, a sub
 *   mit and a fence. A mesh's arena is written ONCE, by the CPU, at the moment the mesh is created,
 *   and read by the vertex stage for the mesh's whole life. The repo's rule says prefer the copy pa
 *   th "for anything transient", and a mesh is not transient; the rule that forces the whole-block
 *   host-visible acquire is a Vulkan fact (vkMapMemory maps a VkDeviceMemory, not a range), and thi
 *   s rung spends exactly one block per mesh for exactly that reason.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud_mesh.h" /* Required for: XudMeshRecord and the nine rungs this file answers */
#include "xud.h" /* Required for: XudResult, XudHandle, XudMeshData, XUD_HANDLE_KIND_MESH - the public surface above */
#include "xud_allocator.h" /* Required for: xud_allocator_acquire_host_visible, xud_allocator_release, xud_allocator_select_memory_type - the block this rung owns */
#include "xud_batch.h" /* Required for: XUD_BATCH_STREAM_*, XUD_BATCH_VERTEX_SIZE, XUD_BATCH_INDEX_SIZE, xud_batch_host_visible_bits - the four streams and the host-visible mask, read rather than restated */
#include "xud_core.h" /* Required for: struct XudContext, whose handleTable/loader/device/allocator the public pair reaches */
#include "xud_device.h" /* Required for: XudDevice, xud_device_wait_idle - the wait-idle guard a buffer destroy owes */
#include "xud_handle.h" /* Required for: XudHandleTable, xud_handle_validate/alloc/free, xud_handle_record_get/set - the slot this record lives in */
#include "xud_loader.h" /* Required for: XudLoader, xud_loader_device_bound - the device-table rows and the bound-handle check */
#include "xud_physical.h" /* Required for: XudDeviceCaps, the vertex-input limits a mesh's streams are gated on */
#include "xud_vulkan.h" /* Required for: xud_result_from_vk - the one VkResult funnel, and why this rung uses the general one and never the swapchain's */
#include <assert.h> /* Required for: static_assert, the public token mirror below */
#include <stdlib.h> /* Required for: calloc and free - the one allocation a mesh spends */
#include <string.h> /* Required for: memcpy, the five writes into the arena */

/*-----------------------------------------------------------------------------------------------
 * The public token mirror, and why it is an assert rather than a translation
 *-----------------------------------------------------------------------------------------------*/

/* XUD_MESH_INDEX_SLOT is this rung's slot numbering and XUD_BATCH_STREAM_COUNT is the batch's
   stream count; the mesh's index buffer is one past the batch's four streams by construction, and
   the assert in xud_mesh.h is that fact. What is asserted HERE is the count the public input struct
   's arrays are sized by - a mesh's pos array is vertexCount*3 floats and its index array is index
   Count uint32s, and those two widths are the batch's, not a mesh's own. */
static_assert(
    XUD_BATCH_VERTEX_SIZE == 36u, "xud_mesh.c: the four streams a mesh owns are the batch's 36 B/vertex, read rather than remembered");
static_assert(XUD_MESH_BYTES_PER_VERTEX == (XUD_BATCH_VERTEX_SIZE + (3u * XUD_BATCH_INDEX_SIZE)),
    "xud_mesh.c: one vertex of a mesh's arena is its four streams plus the three indices that vertex owns");

/*-----------------------------------------------------------------------------------------------
 * The layout's arithmetic - pure, and the whole of it
 *-----------------------------------------------------------------------------------------------*/

VkDeviceSize xud_mesh_align_up(VkDeviceSize value, uint32_t alignment) {
    VkDeviceSize remainder = 0u;

    if (alignment == 0u) {
        /* An alignment of zero is not an alignment: every division below would fault. The layout
           callers never hand zero (xud_mesh_layout() refuses an alignment array past the ceiling and
           substitutes the floor for a NULL one), so this is the guard the helper's own contract state
           s rather than a policy of the layout.
        */
        return value;
    }

    remainder = value % (VkDeviceSize)alignment;
    if (remainder == 0u) {
        return value;
    }
    return value + ((VkDeviceSize)alignment - remainder);
}

XudResult xud_mesh_check(const XudMeshData* data, uint32_t* outTriangleCount) {
    uint32_t index = 0u;

    if (data == NULL) {
        return XUD_INVALID_ARG;
    }

    /* A mesh of no vertices is not a mesh: it names four streams of zero length and an index buffer
       of zero length, and a buffer of zero length is a buffer this rung must not create (the same
       zero-size rule xud_allocator_acquire() states beside its own guard). Refused as a bad argumen
       t, because it is the caller's count that is wrong, not a capability the device lacks.
    */
    if ((data->vertexCount == 0u) || (data->vertexCount > XUD_MESH_MAX_VERTICES)) {
        return XUD_INVALID_ARG;
    }

    /* One topology today: triangles. An index count that is not a whole number of three is not a
       value the row of the pipeline list can name, and it is refused here rather than at a draw tha
       t would read a half triangle.
    */
    if ((data->indexCount < 3u) || ((data->indexCount % 3u) != 0u)) {
        return XUD_INVALID_ARG;
    }

    /* The index array is the one source that cannot be defaulted: a mesh without indices is not a
       mesh with a default, it is a mesh whose topology is undefined.
    */
    if (data->index == NULL) {
        return XUD_INVALID_ARG;
    }

    /* Every index inside the vertex count. This is the check the whole accessor exists for: an out-
       of-range index is not something the driver would refuse later, it is undefined behaviour at
       the draw, and the repo's rule is that a bad request is caught rather than handed down. The
       loop is bounded by indexCount, which is bounded by 3 * XUD_MESH_MAX_VERTICES above.
    */
    for (index = 0u; index < data->indexCount; index++) {
        if (data->index[index] >= data->vertexCount) {
            return XUD_INVALID_ARG;
        }
    }

    if (outTriangleCount != NULL) {
        *outTriangleCount = data->indexCount / 3u;
    }

    return XUD_OK;
}

XudResult xud_mesh_caps_support(const XudDeviceCaps* caps, const XudMeshData* data) {
    const VkPhysicalDeviceLimits* limits = NULL;
    uint32_t stream = 0u;
    VkDeviceSize stride = 0u;

    if ((caps == NULL) || (data == NULL)) {
        return XUD_INVALID_ARG;
    }
    if (data->vertexCount == 0u) {
        /* The byte widths below are a product of the vertex count, and a count of zero is refused
           by xud_mesh_check() before this rung is ever reached - stated here so the products cannot
           be read as a gate that passes on nothing.
        */
        return XUD_INVALID_ARG;
    }

    /* The two-hop member path, the trap xud_physical.c states: the caps' field is itself named pro
       perties and its type is the VkPhysicalDeviceProperties2 wrapper, whose core struct is the .pr
       operties member.
    */
    limits = &caps->properties.properties.limits;

    /* The four streams a mesh binds are the four bindings and four attributes the pipeline's vertex
       -input state already declares, so a device that serves fewer of either is a device a mesh's
       streams cannot be bound on at all. The batch gates the same two numbers for its own streams;
       this rung gates them for its own, because binding is this rung's act and not the batch's.
    */
    if ((limits->maxVertexInputBindings < XUD_BATCH_STREAM_COUNT) || (limits->maxVertexInputAttributes < XUD_BATCH_STREAM_COUNT)) {
        return XUD_UNSUPPORTED;
    }

    /* The stride of each stream against the implementation's bound. This is the one place the caps
       can refuse a mesh as designed rather than refuse a smaller one: a stream whose element width
       the driver will not stride is a stream that cannot be bound through the pipeline's vertex-in
       put state whatever its length is, and the answer is UNSUPPORTED rather than a narrower forma
       t chosen behind the caller's back.
    */
    for (stream = 0u; stream < XUD_BATCH_STREAM_COUNT; stream++) {
        stride = (VkDeviceSize)XUD_BATCH_STREAM_ELEMENT(stream);
        if (stride > (VkDeviceSize)limits->maxVertexInputBindingStride) {
            return XUD_UNSUPPORTED;
        }
    }

    return XUD_OK;
}

XudResult xud_mesh_layout(const XudMeshData* data, const uint32_t* alignment, XudMeshRecord* outRecord) {
    VkDeviceSize running = 0u;
    uint32_t slot = 0u;
    uint32_t usedAlignment = XUD_MESH_ALIGN_MIN;

    if ((data == NULL) || (outRecord == NULL)) {
        return XUD_INVALID_ARG;
    }
    if ((data->vertexCount == 0u) || (data->vertexCount > XUD_MESH_MAX_VERTICES) || (data->indexCount < 3u)
        || ((data->indexCount % 3u) != 0u)) {
        return XUD_UNSUPPORTED;
    }

    /* The alignments are validated BEFORE anything is written, which is what makes a refusal a refus
       al that says nothing rather than a record that says half a mesh: the loop below writes the rec
       ord as it goes, so a guard that lived inside it would leave the earlier slots laid out for a m
       esh that does not exist.
    */
    for (slot = 0u; slot < XUD_MESH_BUFFER_COUNT; slot++) {
        usedAlignment = (alignment != NULL) ? alignment[slot] : XUD_MESH_ALIGN_MIN;
        if (usedAlignment == 0u) {
            return XUD_INVALID_ARG;
        }
        if (usedAlignment > XUD_MESH_ALIGN_MAX) {
            /* An alignment past the allocator's own ceiling is a request the block acquire would
               refuse; refusing it here is that reason stated before a block is asked for.
            */
            return XUD_UNSUPPORTED;
        }
    }

    /* The five byte counts first, and they are the whole of the arithmetic that does not depend on
       an alignment: a stream's length is the mesh's one vertex count times that stream's element w
       idth, and the invariant the settled decision states ("every stream holds the same number of
       vertices") is true by construction here rather than by a rule someone has to keep true later.
    */
    for (slot = 0u; slot < XUD_BATCH_STREAM_COUNT; slot++) {
        outRecord->streamBytes[slot] = (VkDeviceSize)data->vertexCount * (VkDeviceSize)XUD_BATCH_STREAM_ELEMENT(slot);
    }
    outRecord->indexBytes = (VkDeviceSize)data->indexCount * (VkDeviceSize)XUD_BATCH_INDEX_SIZE;

    /* The offsets, in slot order, each rounded up to that slot's own alignment. First-fit in row
       order is the same policy the allocator's free list uses, for the same reason: it is determ
       inistic, and it is what makes the write's offsets, the bind's offsets and the buffer sizes on
       e set of numbers rather than three that have to agree.
    */
    for (slot = 0u; slot < XUD_MESH_BUFFER_COUNT; slot++) {
        usedAlignment = (alignment != NULL) ? alignment[slot] : XUD_MESH_ALIGN_MIN;
        running = xud_mesh_align_up(running, usedAlignment);
        if (slot < XUD_BATCH_STREAM_COUNT) {
            outRecord->streamOffset[slot] = running;
            running += outRecord->streamBytes[slot];
        } else {
            outRecord->indexOffset = running;
            running += outRecord->indexBytes;
        }
    }
    outRecord->arenaBytes = running;

    outRecord->vertexCount = data->vertexCount;
    outRecord->indexCount = data->indexCount;
    outRecord->triangleCount = data->indexCount / 3u;

    return XUD_OK;
}

XudResult xud_mesh_fill_buffer_info(const XudMeshRecord* record, uint32_t slot, VkBufferCreateInfo* outInfo) {
    if ((record == NULL) || (outInfo == NULL) || (slot >= XUD_MESH_BUFFER_COUNT)) {
        return XUD_INVALID_ARG;
    }

    outInfo->sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    outInfo->pNext = NULL;
    outInfo->flags = 0u;
    outInfo->sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    outInfo->queueFamilyIndexCount = 0u;
    outInfo->pQueueFamilyIndices = NULL;

    if (slot < XUD_BATCH_STREAM_COUNT) {
        outInfo->size = record->streamBytes[slot];
        /* VERTEX_BUFFER alone, and no TRANSFER_DST: the four streams are read by the pipeline's
           attribute rows and nothing is copied into them - the write that fills them is the mapping
           above, not a transfer. Claiming a usage bit for a use this rung does not have is the opp
           osite of the repo's rule that a buffer's usage is what it is used for.
        */
        outInfo->usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    } else {
        outInfo->size = record->indexBytes;
        outInfo->usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    }

    return XUD_OK;
}

XudResult xud_mesh_write_arena(const XudMeshRecord* record, const XudMeshData* data, void* arena, VkDeviceSize arenaSize) {
    uint32_t vertex = 0u;
    uint32_t index = 0u;
    const float posDefault[3] = { 0.0f, 0.0f, 0.0f };
    const float uvDefault[2] = { 0.0f, 0.0f };
    const float normalDefault[3] = { 0.0f, 0.0f, 1.0f };
    const uint8_t colorDefault[4] = { 255u, 255u, 255u, 255u };

    if ((record == NULL) || (data == NULL) || (arena == NULL)) {
        return XUD_INVALID_ARG;
    }
    if (record->arenaBytes == 0u) {
        /* The offsets are xud_mesh_layout()'s, and a record that has not been laid out has offsets
           of zero for all five - writing there would write five times at the same place. Reported
           rather than laid out here, because the layout is the pure half above and this rung's job
           is the write.
        */
        return XUD_UNSUPPORTED;
    }
    if ((record->vertexCount != data->vertexCount) || (record->indexCount != data->indexCount)) {
        /* The record's counts and the description's have to be the same mesh's, because the write s
           izes its loops from the record and reads its arrays from the description - a record laid o
           ut for one mesh used to write another mesh's arrays is a write that reads past them.
        */
        return XUD_INVALID_ARG;
    }
    if (arenaSize < record->arenaBytes) {
        /* The destination is smaller than the layout's own total, so the offsets the write is abou
           t to use reach past it. The driver half hands this the mapping's size, which is the block'
           s size and therefore cannot be smaller than arenaBytes (the layout was recomputed at the
           driver's alignments and the acquire was asked for that total); a test hands this its own b
           uffer's size, which is the reason the check exists at all. Without it the write's offsets
           would be an assumption about a destination the rung never sees.
        */
        return XUD_INVALID_ARG;
    }

    /* The four attribute streams, each at its own offset, each vertexCount elements long. Every one
       of the five writes is a memcpy rather than a cast of the arena to the stream's element type:
       the arena is a range of bytes and nothing here knows its alignment better than the layout does,
       so a typed store through a cast pointer would be an aliasing rule this rung has no reason to
       take. A NULL source array is filled with the documented default rather than refused: the mesh's
       streams all have to exist, so "no uv array" is a mesh whose uv stream is zero, and what would
       have been a refusal is a mesh that draws white and flat-lit instead of a mesh that does not ex
       ist.
    */
    for (vertex = 0u; vertex < record->vertexCount; vertex++) {
        if (data->pos != NULL) {
            memcpy((char*)arena + record->streamOffset[XUD_BATCH_STREAM_POS]
                    + ((VkDeviceSize)vertex * (VkDeviceSize)XUD_BATCH_STREAM_POS_SIZE),
                &data->pos[(size_t)vertex * 3u], (size_t)XUD_BATCH_STREAM_POS_SIZE);
        } else {
            memcpy((char*)arena + record->streamOffset[XUD_BATCH_STREAM_POS]
                    + ((VkDeviceSize)vertex * (VkDeviceSize)XUD_BATCH_STREAM_POS_SIZE),
                posDefault, (size_t)XUD_BATCH_STREAM_POS_SIZE);
        }

        if (data->texcoords != NULL) {
            memcpy(
                (char*)arena + record->streamOffset[XUD_BATCH_STREAM_UV] + ((VkDeviceSize)vertex * (VkDeviceSize)XUD_BATCH_STREAM_UV_SIZE),
                &data->texcoords[(size_t)vertex * 2u], (size_t)XUD_BATCH_STREAM_UV_SIZE);
        } else {
            memcpy(
                (char*)arena + record->streamOffset[XUD_BATCH_STREAM_UV] + ((VkDeviceSize)vertex * (VkDeviceSize)XUD_BATCH_STREAM_UV_SIZE),
                uvDefault, (size_t)XUD_BATCH_STREAM_UV_SIZE);
        }

        if (data->color != NULL) {
            memcpy((char*)arena + record->streamOffset[XUD_BATCH_STREAM_COLOR]
                    + ((VkDeviceSize)vertex * (VkDeviceSize)XUD_BATCH_STREAM_COLOR_SIZE),
                &data->color[(size_t)vertex * 4u], (size_t)XUD_BATCH_STREAM_COLOR_SIZE);
        } else {
            /* Opaque white: the stream is R8G8B8A8_UNORM, so 255 is one, and white is the neutral
               value for a mesh that brought no colour array - the batch's per-draw colour has no
               equivalent at a mesh, so the neutral has to be in the stream.
            */
            memcpy((char*)arena + record->streamOffset[XUD_BATCH_STREAM_COLOR]
                    + ((VkDeviceSize)vertex * (VkDeviceSize)XUD_BATCH_STREAM_COLOR_SIZE),
                colorDefault, (size_t)XUD_BATCH_STREAM_COLOR_SIZE);
        }

        if (data->normal != NULL) {
            memcpy((char*)arena + record->streamOffset[XUD_BATCH_STREAM_NORMAL]
                    + ((VkDeviceSize)vertex * (VkDeviceSize)XUD_BATCH_STREAM_NORMAL_SIZE),
                &data->normal[(size_t)vertex * 3u], (size_t)XUD_BATCH_STREAM_NORMAL_SIZE);
        } else {
            /* +Z, unit: the frag stage renormalizes what it reads and refuses to divide by zero on
               a zero vector, so a default normal has to be a unit one - and +Z is the face turned at
               the viewer of a mesh that brought no normals.
            */
            memcpy((char*)arena + record->streamOffset[XUD_BATCH_STREAM_NORMAL]
                    + ((VkDeviceSize)vertex * (VkDeviceSize)XUD_BATCH_STREAM_NORMAL_SIZE),
                normalDefault, (size_t)XUD_BATCH_STREAM_NORMAL_SIZE);
        }
    }

    /* The index buffer, one uint32 per index, at the mesh's own index width - the width the frame
       loop's XUD_FRAME_INDEX_TYPE is pinned against.
    */
    for (index = 0u; index < record->indexCount; index++) {
        memcpy((char*)arena + record->indexOffset + ((VkDeviceSize)index * (VkDeviceSize)XUD_BATCH_INDEX_SIZE), &data->index[index],
            (size_t)XUD_BATCH_INDEX_SIZE);
    }

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The one teardown of the driver objects a record owns - shared by the close and by the rollback
 * above it, so a mesh that failed half-way up and a mesh released by a caller die the same way
 *-----------------------------------------------------------------------------------------------*/

static void xud_mesh_retire(XudMeshRecord* record, const XudDevice* device, XudLoader* loader, XudAllocator* alloc) {
    uint32_t slot = 0u;

    if ((record == NULL) || (loader == NULL) || (device == NULL)) {
        return;
    }

    /* The unmap first: a mapping is a promise about a VkDeviceMemory, and destroying the buffers
       whose bytes live in it while it is still mapped is the other order. The batch's arena and the
       descriptor's arena unmap in the same slot of their teardown for the same reason.
    */
    if ((record->arenaMappedLive != 0u) && (loader->deviceTable.vkUnmapMemory != NULL) && (record->arenaRange.memory != VK_NULL_HANDLE)) {
        (void)loader->deviceTable.vkUnmapMemory(device->device, record->arenaRange.memory);
    }
    record->arenaMapped = NULL;
    record->arenaMappedLive = 0u;

    for (slot = 0u; slot < XUD_MESH_BUFFER_COUNT; slot++) {
        if (record->buffer[slot] == VK_NULL_HANDLE) {
            continue;
        }
        if (loader->deviceTable.vkDestroyBuffer != NULL) {
            (void)loader->deviceTable.vkDestroyBuffer(device->device, record->buffer[slot], NULL);
        }
        record->buffer[slot] = VK_NULL_HANDLE;
    }

    /* The range goes free after the buffers that name it are gone - the ownership direction the
       allocator states for a buffer and the memory it is bound to, read at the one place a mesh's
       objects die.
    */
    if ((alloc != NULL) && (record->arenaRange.size != 0u)) {
        (void)xud_allocator_release(alloc, &record->arenaRange);
    }
}

/*-----------------------------------------------------------------------------------------------
 * The bring-up - driver
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_mesh_open(XudHandle* outHandle, XudHandleTable* table, XudLoader* loader, const XudDevice* device, const XudDeviceCaps* caps,
    XudAllocator* alloc, const XudMeshData* data) {
    XudMeshRecord* record = NULL;
    XudResult result = XUD_INVALID_ARG;
    VkResult vk = VK_ERROR_UNKNOWN;
    VkMemoryRequirements needs;
    VkMemoryRequirements arenaNeeds;
    uint32_t hostVisibleMask = 0u;
    uint32_t memoryTypeIndex = 0u;
    uint32_t alignments[XUD_MESH_BUFFER_COUNT];
    uint32_t maxAlignment = XUD_MESH_ALIGN_MIN;
    uint32_t slot = 0u;
    void* mapped = NULL;

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

    /* The destroy rows are checked before the creates, for the rule every rung in this tree states:
       a buffer this layer cannot destroy is a buffer it must not create. The fail ladder below is
       that ladder's teardown, and it destroys through these very rows, so a refusal that reached it
       with a missing row would leak the buffers the step above made.
    */
    if ((loader->deviceTable.vkCreateBuffer == NULL) || (loader->deviceTable.vkDestroyBuffer == NULL)
        || (loader->deviceTable.vkGetBufferMemoryRequirements == NULL) || (loader->deviceTable.vkBindBufferMemory == NULL)
        || (loader->deviceTable.vkMapMemory == NULL) || (loader->deviceTable.vkUnmapMemory == NULL)) {
        return XUD_UNSUPPORTED;
    }

    /* The argument check and the caps gate run before anything is spent, so a refusal leaves no slot
       and no record behind: XUD_INVALID_ARG is the caller's mesh and XUD_UNSUPPORTED is this device,
       and neither is a half-mesh.
    */
    if (xud_mesh_check(data, NULL) != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    result = xud_mesh_caps_support(caps, data);
    if (result != XUD_OK) {
        return result;
    }

    /* The slot first, then the record: the order is the material rung's, and the reason is the same
       one stated there - a slot allocated with no record is a slot whose record field is still the
       sentinel. Here that transient state is retired by the fail ladder below, which frees the slot
       on every path that does not reach the write.
    */
    result = xud_handle_alloc(outHandle, table, XUD_HANDLE_KIND_MESH);
    if (result != XUD_OK) {
        return result;
    }

    record = calloc(1u, sizeof(*record));
    if (record == NULL) {
        (void)xud_handle_free(table, *outHandle);
        return XUD_OUT_OF_MEMORY;
    }

    /* The zero state the driver needs before it answers, and before the fail ladder reads it: the
       five handles are VK_NULL_HANDLE until each create writes one (so a create that failed has not
       hing for the ladder to destroy), and the range is empty until the acquire writes it (so the la
       dder's release has nothing to release). calloc gives both, and the two loops below are what ma
       kes them explicit rather than what leaves them implied.
    */
    for (slot = 0u; slot < XUD_MESH_BUFFER_COUNT; slot++) {
        record->buffer[slot] = VK_NULL_HANDLE;
    }
    record->arenaRange.memory = VK_NULL_HANDLE;
    record->arenaRange.size = 0u;
    record->arenaRange.offset = 0u;
    record->arenaMapped = NULL;
    record->arenaMappedLive = 0u;

    /* The layout, at this layer's own alignment floor. This pass is the one that gives the five buf
       fer SIZES, which do not depend on what the driver reports; the alignment-dependent half (the o
       ffsets inside the arena) is recomputed below, once the driver's own alignments are in hand.
    */
    if (xud_mesh_layout(data, NULL, record) != XUD_OK) {
        result = XUD_UNSUPPORTED;
        goto fail;
    }

    /* The five buffers, in slot order, each created before its memory is asked for - the rule the ba
       tch follows one rung over, and the reason is a call signature: vkGetBufferMemoryRequirements t
       akes the buffer, so the requirements are a fact about a buffer that exists.
    */
    arenaNeeds.memoryTypeBits = 0xFFFFFFFFu;
    for (slot = 0u; slot < XUD_MESH_BUFFER_COUNT; slot++) {
        if (xud_mesh_fill_buffer_info(record, slot, &record->bufferInfo[slot]) != XUD_OK) {
            result = XUD_INVALID_ARG;
            goto fail;
        }

        vk = loader->deviceTable.vkCreateBuffer(device->device, &record->bufferInfo[slot], NULL, &record->buffer[slot]);
        result = xud_result_from_vk(vk);
        if (result != XUD_OK) {
            record->buffer[slot] = VK_NULL_HANDLE; /* the handle is untouched on failure in these headers, and the ladder below reads it */
            goto fail;
        }

        /* vkGetBufferMemoryRequirements returns VOID, so the zero-state-before-the-call rule this t
           ree follows for every void entry point: the row is cleared first, so a driver that wrote n
           othing leaves a row that says nothing rather than a row of whatever the stack held.
        */
        memset(&needs, 0, sizeof(needs));
        loader->deviceTable.vkGetBufferMemoryRequirements(device->device, record->buffer[slot], &needs);
        if (needs.size == 0u) {
            /* A buffer that asks for no memory is a buffer this rung cannot bind, and a zero-size r
               equest to the allocator is XUD_INVALID_ARG rather than a zero-size block.
            */
            result = XUD_VULKAN_ERROR;
            goto fail;
        }

        /* One block has to satisfy every one of the five buffers, so the five memory-type masks are
           ANDed rather than ORed - the intersection is the set of types that can back all five at o
           nce, and an empty intersection is refused below as UNSUPPORTED rather than resolved by ch
           oosing one buffer's type and letting the other four refuse their bind.
        */
        arenaNeeds.memoryTypeBits &= needs.memoryTypeBits;
        if (needs.alignment > maxAlignment) {
            maxAlignment = needs.alignment;
        }
        alignments[slot] = needs.alignment;
    }
    arenaNeeds.alignment = (maxAlignment > XUD_MESH_ALIGN_MAX) ? XUD_MESH_ALIGN_MAX : maxAlignment;

    /* The mask of the caps' host-visible memory types, read off the batch's own pure half rather th
       an restated here: "host-visible" is a requirement of this arena rather than a preference, and a
       caps that answers no host-visible type answers 0 bits, which is what makes the arena's allocat
       ion UNSUPPORTED rather than a mapping CPU code cannot read.
    */
    result = xud_batch_host_visible_bits(caps, &hostVisibleMask);
    if (result != XUD_OK) {
        goto fail;
    }
    if (hostVisibleMask == 0u) {
        result = XUD_UNSUPPORTED;
        goto fail;
    }
    arenaNeeds.memoryTypeBits &= hostVisibleMask;
    if (arenaNeeds.memoryTypeBits == 0u) {
        /* The five buffers' own masks and the host-visible rows do not intersect: this mesh cannot be
           bound to memory the CPU can write on this driver.
        */
        result = XUD_UNSUPPORTED;
        goto fail;
    }

    /* The offsets, recomputed at the alignments the driver actually reported. A layout that assumed a
       n alignment would be a layout that bound a buffer at an offset the driver then refused; taking
       the alignment as data is what makes the arithmetic a pure half a test can drive.
    */
    if (xud_mesh_layout(data, alignments, record) != XUD_OK) {
        result = XUD_UNSUPPORTED;
        goto fail;
    }
    arenaNeeds.size = record->arenaBytes;

    /* The one block, and it is the host-visible acquire rather than the suballocating one: a mappin
       g is per-VkDeviceMemory rather than per-range, so the one carve a rung maps owns the block it
       is mapped in. This is the repo's "allocate once, suballocate always" rule with its one except
       ion, and it is the same exception the batch's arena and the descriptor's arena spend.
    */
    result = xud_allocator_acquire_host_visible(&record->arenaRange, alloc, loader, device, caps, &arenaNeeds);
    if (result != XUD_OK) {
        goto fail;
    }

    /* The check-before-you-use rule read at the mapping rather than at the map: the type the acquire
       actually allocated at has to be host-visible, re-read from the allocator's own type rule rather
       than trusted from the mask that was asked for, because a mask is only a preference and the row
       it picked is the one vkMapMemory is called on.
    */
    result = xud_allocator_select_memory_type(caps, &arenaNeeds, &memoryTypeIndex);
    if ((result != XUD_OK) || (memoryTypeIndex >= caps->memoryProperties.memoryProperties.memoryTypeCount)
        || ((caps->memoryProperties.memoryProperties.memoryTypes[memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            == 0u)) {
        result = XUD_UNSUPPORTED;
        goto fail;
    }

    /* The five binds, each at its own offset inside the block the acquire handed back. The block's o
       wner offset is added rather than assumed zero, so a block that does not start at zero is still
       a block whose bytes are addressed correctly.
    */
    for (slot = 0u; slot < XUD_MESH_BUFFER_COUNT; slot++) {
        record->bindOffset[slot]
            = record->arenaRange.offset + ((slot < XUD_BATCH_STREAM_COUNT) ? record->streamOffset[slot] : record->indexOffset);
        vk = loader->deviceTable.vkBindBufferMemory(
            device->device, record->buffer[slot], record->arenaRange.memory, record->bindOffset[slot]);
        result = xud_result_from_vk(vk);
        if (result != XUD_OK) {
            goto fail;
        }
    }

    /* The map, once, for the whole arena, for the whole life of the mesh. flags 0 is the read/write
       pair the repo's rule asks for: the arena is written by the CPU and read by the vertex stage, a
       nd no cache-control flag is claimed for a type this layer has not asked to be cached.
    */
    vk = loader->deviceTable.vkMapMemory(
        device->device, record->arenaRange.memory, record->arenaRange.offset, record->arenaRange.size, 0u, &mapped);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK) {
        goto fail;
    }
    record->arenaMapped = mapped;
    record->arenaMappedLive = 1u;

    /* The write, last of the six because it is the only one of them that reads two objects created b
       y two different steps above it - the mapping above and the layout's offsets.
    */
    if (xud_mesh_write_arena(record, data, record->arenaMapped, record->arenaRange.size) != XUD_OK) {
        result = XUD_INVALID_ARG;
        goto fail;
    }

    /* The record is written through the handle layer's own accessor rather than by indexing the slot
       array from here: the array is the handle layer's, it reallocs it as a unit, and the accessor is
       what bounds-checks (kind, index) against the array that exists.
    */
    if (xud_handle_record_set(table, XUD_HANDLE_KIND_MESH, outHandle->index, record) != XUD_OK) {
        result = XUD_INVALID_ARG;
        goto fail;
    }

    return XUD_OK;

fail:
    /* Reverse creation order, in one place rather than at every refusal: the arena is unmapped and t
       he block released after the five buffers that name it are destroyed (the ownership direction t
       he allocator states for a buffer and the memory it is bound to), the record is freed after the
       field that names it is cleared, and the slot is freed last so a refusal leaves no half-mesh an
       d no mesh a caller can still name. The field is cleared BEFORE the free because xud_handle_free
       () leaves it as it was, and a free slot that still owns a record is a record two teardown path
       s would both free.
    */
    (void)xud_handle_record_set(table, XUD_HANDLE_KIND_MESH, outHandle->index, NULL);
    xud_mesh_retire(record, device, loader, alloc);
    if (record != NULL) {
        free(record);
    }
    (void)xud_handle_free(table, *outHandle);
    return (result != XUD_OK) ? result : XUD_INTERNAL_ERROR;
}

/*-----------------------------------------------------------------------------------------------
 * The accessor - pure
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_mesh_get(const XudHandleTable* table, XudHandle handle, const XudMeshRecord** outRecord) {
    void* record = NULL;

    if ((table == NULL) || (outRecord == NULL)) {
        return XUD_INVALID_ARG;
    }

    /* Validated against the MESH kind this rung is about rather than against the handle's own tag:
       that is what makes a material handle handed to a mesh call a refusal rather than a read of th
       e wrong array.
    */
    if (xud_handle_validate(table, handle, XUD_HANDLE_KIND_MESH) != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    if (xud_handle_record_get(table, XUD_HANDLE_KIND_MESH, handle.index, &record) != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    if (record == NULL) {
        /* A live MESH slot whose record field is the sentinel is a slot this rung did not allocate.
           Reported rather than handed back as a NULL record, because every caller of this accessor
           reads the record it points at.
        */
        return XUD_INVALID_ARG;
    }

    *outRecord = (const XudMeshRecord*)record;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The release - driver
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_mesh_close(XudHandleTable* table, XudHandle handle, XudLoader* loader, const XudDevice* device, XudAllocator* alloc) {
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
    if (table->slots[XUD_HANDLE_KIND_MESH] == NULL) {
        /* No MESH array was ever allocated, so no mesh of this table's was ever created and the
           handle in hand cannot name one - the same answer xud_mesh_close_all() gives for the same
           reason, and the reason xud_handle_validate() below would give anyway.
        */
        return XUD_INVALID_ARG;
    }

    /* Validated against the MESH kind before anything is destroyed: releasing is only ever about the
       array this rung's records live in, so a handle of another kind is refused rather than destroy
       ing a record of that other kind.
    */
    if (xud_handle_validate(table, handle, XUD_HANDLE_KIND_MESH) != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    if (xud_handle_record_get(table, XUD_HANDLE_KIND_MESH, handle.index, &record) != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    if (record == NULL) {
        return XUD_INVALID_ARG;
    }

    /* The wait-idle guard, and it is not decoration: a mesh's buffers are read by a submission, and
       VUID-vkDestroyBuffer-buffer-00007 ("all uses of the buffer object must have completed executi
       on") is the same rule the swapchain's recreate and the allocator's close state for their own
       objects. A guard that cannot run is a teardown this rung must not start, because freeing the
       record would leak every handle the driver still holds; a LOST device is still torn down and st
       ill reported, the rule xud_device_close() follows, because skipping the free would leak the b
       lock the driver still holds.
    */
    guard = xud_device_wait_idle(device, loader);
    if (guard == XUD_UNSUPPORTED) {
        return XUD_UNSUPPORTED;
    }
    if (guard != XUD_OK) {
        first = guard;
    }

    /* Cleared BEFORE the free, and before xud_handle_free() marks the slot free: the material rung's
       rule, and the reason is the same one - xud_handle_free() leaves the record field as it was, so
       a free slot can otherwise still own a record, and then two teardown paths (this one and the
       table's) would both destroy it.
    */
    if (xud_handle_record_set(table, XUD_HANDLE_KIND_MESH, handle.index, NULL) != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    xud_mesh_retire((XudMeshRecord*)record, device, loader, alloc);
    free(record);

    return xud_handle_free(table, handle) != XUD_OK ? XUD_INVALID_ARG : first;
}

/*-----------------------------------------------------------------------------------------------
 * The teardown rung above the handle table's own close - driver
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_mesh_close_all(XudHandleTable* table, XudLoader* loader, const XudDevice* device, XudAllocator* alloc, uint32_t* outClosed) {
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

    if (table->slots[XUD_HANDLE_KIND_MESH] == NULL) {
        /* No MESH array was ever allocated: nothing of this kind was ever created, which is a count
           of zero and not an error - the same answer the material's close_all gives for the same re
           ason.
        */
        return XUD_OK;
    }

    /* The guard once for the whole walk rather than once per mesh: a submission may still be reading
       any one of the buffers below, and one boundary ahead of the walk is the same boundary the wal
       k would otherwise ask for n times. The allocator's own wait-idle guard is xud_allocator_close
       ()'s, not this one's - this rung releases ranges inside blocks the allocator still owns, and t
       he allocator's close is what frees the blocks after its own guard.
    */
    guard = xud_device_wait_idle(device, loader);
    if (guard == XUD_UNSUPPORTED) {
        return XUD_UNSUPPORTED;
    }
    if (guard != XUD_OK) {
        first = guard;
    }

    /* Every row of the array is walked, live or free: xud_handle_free() leaves the record field as
       it was, so a slot a caller released still owns its record until this rung clears it, and a rec
       ord left behind is a leak the table's own close cannot see (it frees the array, not what the a
       rray points at). The walk is bounded by the array's own high mark rather than by its capacity,
       so the rows past the last slot ever handed out - which have never held a record - are not read
       .
    */
    for (index = 0u; index < table->slotCount[XUD_HANDLE_KIND_MESH]; index++) {
        if (xud_handle_record_get(table, XUD_HANDLE_KIND_MESH, index, &record) != XUD_OK) {
            return XUD_INVALID_ARG;
        }
        if (record == NULL) {
            continue;
        }
        if (xud_handle_record_set(table, XUD_HANDLE_KIND_MESH, index, NULL) != XUD_OK) {
            return XUD_INVALID_ARG;
        }
        xud_mesh_retire((XudMeshRecord*)record, device, loader, alloc);
        free(record);
        (*outClosed)++;
    }

    return first;
}

/*-----------------------------------------------------------------------------------------------
 * The public pair above the context - the object's lifetime
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_upload_mesh(XudHandle* outHandle, XudContext* context, const XudMeshData* mesh) {
    if ((outHandle == NULL) || (context == NULL) || (mesh == NULL)) {
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

    return xud_mesh_open(
        outHandle, context->handleTable, context->loader, context->device, &context->physicalDevice->caps, context->allocator, mesh);
}

XudResult xud_close_mesh(XudContext* context, XudHandle handle) {
    if (context == NULL) {
        return XUD_INVALID_ARG;
    }
    if ((context->handleTable == NULL) || (context->loader == NULL) || (context->device == NULL) || (context->allocator == NULL)) {
        return XUD_UNSUPPORTED;
    }

    return xud_mesh_close(context->handleTable, handle, context->loader, context->device, context->allocator);
}
