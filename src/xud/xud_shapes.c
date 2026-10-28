/***********************************************************************************************
 *
 *   xud_shapes.c - the shapes rung and the public draw call above it
 *
 *   Owns : xud_shapes_append_triangle(), the geometry one public draw call names, and xud_draw_
 *          triangle_3d(), the public call that names it. Nothing else - no driver call, no loader
 *          row, no Vulkan or GLFW symbol the linker could see. The whole file is host work on the
 *          batch's own counters and the batch's own arena, which is what keeps it testable with
 *          no device present and keeps a machine with no Vulkan loader answering the same refu
 *          sals as one with a loader and no window open.
 *   Layer: shapes (above the batch, below the public API). The public call sits in this file for
 *          the same reason raylib's DrawTriangle3D sits in rshapes.c: the shape IS the public cal
 *          l's job, and the public header is the declaration of it rather than its implementa
 *          tion. The internal function next to it is the half a test can run against a batch it
 *          built itself, with no context and no driver in sight - which is the reason the two are
 *          split rather than one public function doing both jobs.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include "xud_shapes.h"
#include "xud.h" /* Required for: XudResult, XudVector3, XudColor */
#include "xud_batch.h" /* Required for: XudBatch, xud_batch_stats, xud_batch_append_vertex/xud_batch_append_index */
#include "xud_core.h" /* Required for: struct XudContext, the one owner whose renderer's batch a public draw call appends into */

/*-----------------------------------------------------------------------------------------------
 * The shape: three vertices, then the three indices that name them
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_shapes_append_triangle(XudBatch* batch, XudVector3 v0, XudVector3 v1, XudVector3 v2, XudColor color) {
    float pos[3];
    float uv[2];
    float normal[3];
    uint8_t rgba[4];
    uint32_t vertexCount = 0u;
    uint32_t indexCount = 0u;
    uint32_t vertexCapacity = 0u;
    uint32_t drawCount = 0u;
    VkDeviceSize stagingBytes = 0u;
    XudResult result = XUD_INVALID_ARG;
    uint32_t first = 0u;
    uint32_t row = 0u;

    if (batch == NULL) {
        return XUD_INVALID_ARG;
    }

    /* The batch's counters, read through the rung's own accessor rather than off the fields: this
       is the half that makes the append atomic, so it has to be the same read the appends below
       will make, and xud_batch_stats is the pure accessor that states which fields those are. */
    result = xud_batch_stats(batch, &vertexCount, &indexCount, &vertexCapacity, &drawCount, &stagingBytes);
    if (result != XUD_OK) {
        return result;
    }

    /* The whole shape has to fit, or none of it is appended. The vertex bound is the accessor's
       own capacity; the index bound is the batch's field, which is the number xud_batch_layout()
       computed as 3n/2 for that capacity - xud_batch_stats does not hand it out, and this rung is
       inside the library, so it reads it rather than re-deriving it. Neither sum can overflow: the
       header's static_asserts bound the capacity at XUD_BATCH_MAX_VERTICES, four digits. */
    if ((vertexCount + XUD_SHAPES_TRIANGLE_VERTEX_COUNT) > vertexCapacity) {
        return XUD_OUT_OF_MEMORY;
    }
    if ((indexCount + XUD_SHAPES_TRIANGLE_INDEX_COUNT) > batch->indexCapacity) {
        return XUD_OUT_OF_MEMORY;
    }

    /* The two constant attribute rows, once. The batch's appends refuse a NULL array rather than
       zero-filling one, so a stream this shape has nothing to say about still has to be handed
       something - see the reasons stated beside each constant in xud_shapes.h. */
    uv[0] = XUD_SHAPES_UV_S;
    uv[1] = XUD_SHAPES_UV_T;
    normal[0] = XUD_SHAPES_FLAT_NORMAL_X;
    normal[1] = XUD_SHAPES_FLAT_NORMAL_Y;
    normal[2] = XUD_SHAPES_FLAT_NORMAL_Z;
    rgba[0] = color.r;
    rgba[1] = color.g;
    rgba[2] = color.b;
    rgba[3] = color.a;

    /* The three vertices, in the order the caller named them. The loop reads the three by-value
       structs field-wise rather than pretending they are an array: they are three parameters, and
       an array of them would be a copy of the caller's three on the stack for no reason. */
    first = vertexCount;

    pos[0] = v0.x;
    pos[1] = v0.y;
    pos[2] = v0.z;
    result = xud_batch_append_vertex(batch, pos, uv, rgba, normal);
    if (result != XUD_OK) {
        return result;
    }

    pos[0] = v1.x;
    pos[1] = v1.y;
    pos[2] = v1.z;
    result = xud_batch_append_vertex(batch, pos, uv, rgba, normal);
    if (result != XUD_OK) {
        return result;
    }

    pos[0] = v2.x;
    pos[1] = v2.y;
    pos[2] = v2.z;
    result = xud_batch_append_vertex(batch, pos, uv, rgba, normal);
    if (result != XUD_OK) {
        return result;
    }

    /* The indices name the three vertices this call just appended, so they are the live count the
       read above recorded and the three slots after it - and xud_batch_append_index refuses an in
       dex at or above the live count, which after the three appends above is a bound these three
       cannot cross. */
    for (row = 0u; row < XUD_SHAPES_TRIANGLE_INDEX_COUNT; row++) {
        result = xud_batch_append_index(batch, first + row);
        if (result != XUD_OK) {
            return result;
        }
    }

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The public call above it
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_draw_triangle_3d(XudContext* context, XudVector3 v0, XudVector3 v1, XudVector3 v2, XudColor color) {
    if (context == NULL) {
        return XUD_INVALID_ARG;
    }
    if ((context->renderer == NULL) || (context->renderer->batch == NULL)) {
        /* A context that never completed bring-up, or a renderer whose batch rung was retired by a
           resize that failed part-way, is a context with no batch to append into. xud_init_context
           () hands back NULL for the first case and xud_renderer_close() tears the second down, so
           this is a guard against a caller holding on rather than a state the library leaves behi
           nd - and it is reported, per the repo's rule that nothing is silently ignored. */
        return XUD_UNSUPPORTED;
    }

    return xud_shapes_append_triangle(context->renderer->batch, v0, v1, v2, color);
}
