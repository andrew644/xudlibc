/***********************************************************************************************
 *
 *   xud_test_shapes.c - the shapes rung and the public drawing API above it
 *
 *   Owns : the tests of one rung - xud_shapes_append_triangle(), the geometry xud_draw_triangle_
 *          3d() names, and the public frame pair xud_begin_drawing()/xud_end_drawing(). Nothing
 *          else - the batch's own appends are the batch area's, the frame loop's is the frame a
 *          rea's, and the context's lifecycle is the core area's.
 *   Layer: tests, above the batch area and below the core area, which is the layer order: the
 *          shape is checked against a batch the test built itself, with no context and no driver
 *          in sight, and the public calls are checked against a real context in the area's own
 *          GPU half. The pure half is the half the repo's headless-testable rule leans on - the
 *          append is host work on the batch's own counters, so its refusals and its counters are
 *          answerable with no device present, and a machine with no driver gets the same refusals
 *          a machine with a driver gets, which is why the default run stays green.
 *
 *   Deterministic: fixed loop counts, no time, no randomness, no filesystem.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include <string.h> /* Required for: memset, the batch shell and the context shell this area builds itself */

#include "xud.h" /* Required for: XudResult, XudContext, XudVector3, XudColor, the public frame pair and xud_draw_triangle_3d */
#include "xud_batch.h" /* Required for: XudBatch, xud_batch_layout/xud_batch_stats/xud_batch_check_invariant, the capacity macros */
#include "xud_core.h" /* Required for: struct XudContext, whose renderer/frame/batch fields the GPU half reads */
#include "xud_shapes.h" /* Required for: the shapes rung's own constants and its internal append */
#include "xud_test_harness.h"

/* The host arena the pure half's batch writes into, the same stand-in the batch area uses: the
   batch's own mapping is a driver fact and the GPU half's to check, so the headless run hands the
   append a block of host memory and checks the bytes it wrote. Nothing here is mapped.
*/
static uint8_t xudTestArena[(XUD_BATCH_MAX_VERTICES * XUD_BATCH_VERTEX_SIZE)
    + (XUD_BATCH_INDEX_CAPACITY(XUD_BATCH_MAX_VERTICES) * XUD_BATCH_INDEX_SIZE)];

/*-----------------------------------------------------------------------------------------------
 * The pure half: the shape against a batch this area built itself
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_shapes_pure(void) {
    XudBatch b;
    XudContext shell;
    XudVector3 v0;
    XudVector3 v1;
    XudVector3 v2;
    XudColor color;
    XudResult result = XUD_OK;
    uint32_t vertexCount = 0u;
    uint32_t indexCount = 0u;
    uint32_t capacity = 0u;
    uint32_t drawCount = 0u;
    VkDeviceSize stagingBytes = 0u;

    memset(&b, 0, sizeof(b));
    memset(&shell, 0, sizeof(shell));

    v0 = (XudVector3) { .x = -0.5f, .y = -0.5f, .z = 0.0f };
    v1 = (XudVector3) { .x = 0.5f, .y = -0.5f, .z = 0.0f };
    v2 = (XudVector3) { .x = 0.0f, .y = 0.5f, .z = 0.0f };
    color = (XudColor) { .r = 255u, .g = 0u, .b = 0u, .a = 255u };

    /*--- the refusals that need no batch and no context at all -------------------------------*/

    xud_check(xud_shapes_append_triangle(NULL, v0, v1, v2, color) == XUD_INVALID_ARG, "shapes/append-no-batch",
        "a shape with nowhere to be appended into is refused");
    xud_check(xud_draw_triangle_3d(NULL, v0, v1, v2, color) == XUD_INVALID_ARG, "shapes/draw-no-context",
        "a public draw call with no context is refused");
    xud_check(xud_draw_triangle_3d(&shell, v0, v1, v2, color) == XUD_UNSUPPORTED, "shapes/draw-no-renderer",
        "a context with no renderer has no batch to append into, and the refusal is reported");
    xud_check(xud_begin_drawing(NULL) == XUD_INVALID_ARG, "frames/begin-no-context", "a frame boundary with no context is refused");
    xud_check(xud_begin_drawing(&shell) == XUD_UNSUPPORTED, "frames/begin-no-renderer", "and so is one whose renderer rung is gone");
    xud_check(xud_end_drawing(NULL) == XUD_INVALID_ARG, "frames/end-no-context", "a frame boundary cannot be closed with no context");
    xud_check(xud_end_drawing(&shell) == XUD_UNSUPPORTED, "frames/end-no-rungs",
        "and a context missing every rung the frame and the rebuild need says so rather than drawing nothing silently");

    /* The two value types are one number each, and the colour is the stream's byte order: the
       check is the public type against the batch's own element size, which is the pair of facts
       xud.h's static_assert and xud_batch.h's COLOR element state from the other side. */
    xud_check(sizeof(XudColor) == XUD_BATCH_STREAM_ELEMENT(XUD_BATCH_STREAM_COLOR), "shapes/colour-is-the-stream-element",
        "a caller's colour is the four bytes the COLOR stream holds, read field-wise rather than converted");

    /*--- the shape against a batch this area laid out itself ---------------------------------*/

    /* Six vertices is two triangles exactly, which is the smallest capacity that lets the fits
       half of the atomicity claim be checked at all: the third triangle has to be refused and
       the two that fit have to leave the counters at six. */
    xud_check(xud_batch_layout(&b, XUD_SHAPES_TRIANGLE_VERTEX_COUNT * 2u) == XUD_OK, "shapes/layout-two-triangles",
        "a batch of six vertices is a batch of two triangles exactly");
    xud_check(b.vertexCapacity == (XUD_SHAPES_TRIANGLE_VERTEX_COUNT * 2u), "shapes/capacity-is-the-request",
        "the capacity asked for is the capacity recorded");
    xud_check(b.indexCapacity == XUD_BATCH_INDEX_CAPACITY(XUD_SHAPES_TRIANGLE_VERTEX_COUNT * 2u), "shapes/index-capacity-is-3-over-2",
        "the index headroom is the Euler bound the batch header states, not a guessed one");

    /* The 1:1 spend is the reason the vertex bound is the one a triangle trips: a triangle costs
       three vertices and three indices, and the batch's index headroom is 3n/2 for n vertices,
       so a batch that still has room for three vertices still has room for three indices. The
       arithmetic is the whole argument and it is checked here rather than assumed, because it is
       what makes the vertex bound the only one of the two that can ever refuse a triangle. */
    xud_check(b.indexCapacity >= (b.vertexCapacity + XUD_SHAPES_TRIANGLE_INDEX_COUNT), "shapes/vertex-bound-trips-first",
        "the index headroom is 3n/2 and a triangle spends both at 1:1, so the vertex bound is the bound a triangle trips first");

    xud_check(xud_batch_stats(&b, &vertexCount, &indexCount, &capacity, &drawCount, &stagingBytes) == XUD_OK, "shapes/stats-empty-ok",
        "the fresh batch's counters are readable");
    xud_check((vertexCount == 0u) && (indexCount == 0u) && (drawCount == 0u), "shapes/empty-at-layout",
        "a laid-out batch has appended nothing and recorded nothing");

    /* The host arena above the layout: the append writes through what a mapping handed back, so a
       batch whose arena is not mapped refuses rather than writing into nothing. The batch area's
       own checks set this up the same way, and the point here is the shape's writes, not the map.
    */
    b.stagingMapped = xudTestArena;
    b.stagingMappedLive = 1u;

    result = xud_shapes_append_triangle(&b, v0, v1, v2, color);
    xud_check(result == XUD_OK, "shapes/append-first", "the first triangle appends");
    xud_check(xud_batch_stats(&b, &vertexCount, &indexCount, &capacity, &drawCount, &stagingBytes) == XUD_OK, "shapes/stats-after-first",
        "and the counters answer after it");
    xud_check(vertexCount == XUD_SHAPES_TRIANGLE_VERTEX_COUNT, "shapes/first-triangle-costs-three-vertices",
        "one triangle costs three vertices, which is the shape's own count rather than a count of one shape");
    xud_check(
        indexCount == XUD_SHAPES_TRIANGLE_INDEX_COUNT, "shapes/first-triangle-costs-three-indices", "and the three indices that name them");
    xud_check(drawCount == 0u, "shapes/append-records-no-draw",
        "appending a shape does not record a draw row - the frame loop's flush is what records one");
    xud_check(xud_batch_check_invariant(&b) == XUD_OK, "shapes/invariant-after-first",
        "the batch's own invariant still holds after the append, which is the atomicity claim seen from the batch's side");

    /* The bytes, not only the counts: a count-only check would pass a shape that wrote the wrong
       bytes. The colour has to be the caller's four bytes in the caller's order, the normal the
       header's flat +Z, the uv the header's zero pair and the indices the three slots this appen
       d just made - which is the shape's own claim, read off the arena rather than off the code.
    */
    {
        const uint8_t* arena = (const uint8_t*)b.stagingMapped;
        float normalRow[3];
        float uvRow[2];
        uint32_t indexRow[3];

        memcpy(normalRow, arena + b.streamOffset[XUD_BATCH_STREAM_NORMAL], sizeof(normalRow));
        memcpy(uvRow, arena + b.streamOffset[XUD_BATCH_STREAM_UV], sizeof(uvRow));
        memcpy(indexRow, arena + b.indexOffset, sizeof(indexRow));

        xud_check(memcmp(arena + b.streamOffset[XUD_BATCH_STREAM_COLOR], &color, XUD_BATCH_STREAM_COLOR_SIZE) == 0,
            "shapes/colour-landed-in-order", "the caller's four bytes, in the caller's R,G,B,A order");
        xud_check((normalRow[0] == XUD_SHAPES_FLAT_NORMAL_X) && (normalRow[1] == XUD_SHAPES_FLAT_NORMAL_Y)
                && (normalRow[2] == XUD_SHAPES_FLAT_NORMAL_Z),
            "shapes/flat-normal-landed", "the header's flat +Z, in the NORMAL stream's window");
        xud_check((uvRow[0] == XUD_SHAPES_UV_S) && (uvRow[1] == XUD_SHAPES_UV_T), "shapes/zero-uv-landed",
            "the header's zero uv, in the UV stream's window");
        xud_check((indexRow[0] == 0u) && (indexRow[1] == 1u) && (indexRow[2] == 2u), "shapes/indices-name-the-shape",
            "the three indices name the three vertices this append just made, in the caller's order");
        xud_check(memcmp(arena + b.streamOffset[XUD_BATCH_STREAM_POS], &v0, XUD_BATCH_STREAM_POS_SIZE) == 0,
            "shapes/first-point-landed-first", "the first point the caller named is the first row of the POS window");
    }

    result = xud_shapes_append_triangle(&b, v0, v1, v2, color);
    xud_check(result == XUD_OK, "shapes/append-second", "the second triangle appends, which fills the batch exactly");
    xud_check(xud_batch_stats(&b, &vertexCount, &indexCount, &capacity, &drawCount, &stagingBytes) == XUD_OK, "shapes/stats-after-second",
        "and the counters answer after it");
    xud_check(
        vertexCount == (XUD_SHAPES_TRIANGLE_VERTEX_COUNT * 2u), "shapes/two-triangles-fill-it", "six vertices, which is the capacity");

    /* The refusal half of the atomicity claim: the third triangle does not fit, and the check is
       not only the code but the counters - a shape that wrote part of itself before refusing wou
       ld answer six vertices and a non-empty arena, which is the state a frame loop would then
       draw half a triangle from. */
    result = xud_shapes_append_triangle(&b, v0, v1, v2, color);
    xud_check(result == XUD_OUT_OF_MEMORY, "shapes/append-third-refused",
        "a triangle that does not fit the batch's capacities is refused with the batch's own code");
    xud_check(xud_batch_stats(&b, &vertexCount, &indexCount, &capacity, &drawCount, &stagingBytes) == XUD_OK, "shapes/stats-after-refusal",
        "and the counters are still readable after the refusal");
    xud_check(vertexCount == (XUD_SHAPES_TRIANGLE_VERTEX_COUNT * 2u), "shapes/refusal-wrote-nothing",
        "the refused triangle wrote no vertex, so the live count is still six");
    xud_check(indexCount == (XUD_SHAPES_TRIANGLE_INDEX_COUNT * 2u), "shapes/refusal-wrote-no-index",
        "and no index, so the live count is still six");
    xud_check(xud_batch_check_invariant(&b) == XUD_OK, "shapes/invariant-after-refusal",
        "and the batch's invariant still holds, which is what says nothing half-written is in it");
}

/*-----------------------------------------------------------------------------------------------
 * The GPU half: the public pair against a real context
 *-----------------------------------------------------------------------------------------------*/

/* How many frames the GPU half draws. Three, not more: each one is a real acquire, a real
   command buffer, a real submission and a real presentation on this machine, and the verdict thi
   s half exists for is that the pair runs a frame at all and that the messenger heard nothing a
   bout it - the validation layer is installed in this build, so a frame loop that bound the wro
   ng pipeline row, bound a set against the wrong layout or pushed past the recorded range would
   be reported here rather than only suspected. */
#define XUD_TEST_SHAPES_FRAMES 3u

static void xud_test_shapes_gpu(void) {
    XudContext* context = NULL;
    XudResult result = XUD_OK;
    uint32_t vertexCount = 0u;
    uint32_t indexCount = 0u;
    uint32_t capacity = 0u;
    uint32_t drawCount = 0u;
    VkDeviceSize stagingBytes = 0u;
    uint32_t frame = 0u;
    XudVector3 v0 = (XudVector3) { .x = -0.5f, .y = -0.5f, .z = 0.0f };
    XudVector3 v1 = (XudVector3) { .x = 0.5f, .y = -0.5f, .z = 0.0f };
    XudVector3 v2 = (XudVector3) { .x = 0.0f, .y = 0.5f, .z = 0.0f };
    XudColor red = (XudColor) { .r = 255u, .g = 0u, .b = 0u, .a = 255u };
    XudColor green = (XudColor) { .r = 0u, .g = 255u, .b = 0u, .a = 255u };

    result = xud_init_context(&context, 64u, 64u);
    xud_check(result == XUD_OK, "gpu-shapes/bringup-completes",
        "this machine has a Vulkan loader and a window system, so the ladder must complete");
    if (context == NULL) {
        return;
    }

    xud_check(context->renderer->batch != NULL, "gpu-shapes/batch-owned",
        "a context that completed owns a batch, which is what a public draw call appends into");
    xud_check(xud_batch_stats(context->renderer->batch, &vertexCount, &indexCount, &capacity, &drawCount, &stagingBytes) == XUD_OK,
        "gpu-shapes/stats-at-open", "and its counters answer");
    xud_check((vertexCount == 0u) && (indexCount == 0u), "gpu-shapes/empty-at-open", "and nothing has been appended to it yet");
    xud_check(capacity == XUD_BATCH_INITIAL_VERTICES, "gpu-shapes/capacity-is-the-default",
        "the batch was opened at the header's initial capacity, which is the one capacity no caller asked for");

    for (frame = 0u; frame < XUD_TEST_SHAPES_FRAMES; frame++) {
        result = xud_begin_drawing(context);
        xud_check(result == XUD_OK, "gpu-frames/begin-ok", "the public frame boundary opens");

        result = xud_draw_triangle_3d(context, v0, v1, v2, red);
        xud_check(result == XUD_OK, "gpu-shapes/draw-ok", "the public draw call appends into the context's own batch");
        if (frame == 0u) {
            xud_check(xud_batch_stats(context->renderer->batch, &vertexCount, &indexCount, &capacity, &drawCount, &stagingBytes) == XUD_OK,
                "gpu-shapes/stats-after-one-draw", "and the counters answer after it");
            xud_check(vertexCount == XUD_SHAPES_TRIANGLE_VERTEX_COUNT, "gpu-shapes/one-draw-costs-three-vertices",
                "one public draw call costs three vertices, the same count the pure half checked against a batch it built itself");
        }

        /* The second shape in the same frame is the fact that a frame is more than one shape: the
           batch is still open, so the append lands beside the first one and the frame loop's flu
           sh records both as one run. */
        result = xud_draw_triangle_3d(context, v0, v1, v2, green);
        xud_check(result == XUD_OK, "gpu-shapes/two-draws-one-frame", "a second shape appends into the same open frame");
        xud_check(xud_batch_stats(context->renderer->batch, &vertexCount, &indexCount, &capacity, &drawCount, &stagingBytes) == XUD_OK,
            "gpu-shapes/stats-after-two-draws", "and the counters answer after it");
        xud_check(vertexCount == (XUD_SHAPES_TRIANGLE_VERTEX_COUNT * 2u), "gpu-shapes/two-draws-cost-six-vertices",
            "six vertices in one frame, which is the append being per shape rather than per frame");

        result = xud_end_drawing(context);
        xud_check(result == XUD_OK, "gpu-frames/end-ok",
            "the boundary closed, which ran one frame: acquire, record, upload, bind, draw the batch's rows, submit, present");

        xud_check(xud_batch_stats(context->renderer->batch, &vertexCount, &indexCount, &capacity, &drawCount, &stagingBytes) == XUD_OK,
            "gpu-shapes/stats-after-end", "and the counters answer after the frame");
        xud_check((vertexCount == 0u) && (indexCount == 0u), "gpu-shapes/batch-empty-after-frame",
            "the frame's flush emptied the batch, so the next frame starts from an empty one");
        xud_check(context->renderer->frame->recreateWanted == 0u, "gpu-frames/no-recreate-ask",
            "this machine's swapchain matches its surface, so the acquire answered VK_SUCCESS and nothing was asked for");
    }

    if (context->validate != 0u) {
        xud_check(context->diagnostics->errorMessages == 0u, "gpu-shapes/validation-clean",
            "the frames drew a real triangle through a real pipeline and the messenger heard nothing - "
            "not just that nothing crashed");
    }

    result = xud_close_context(&context);
    xud_check(result == XUD_OK, "gpu-shapes/close-clean", "the whole ladder tears back down with nothing refused");
    xud_check(context == NULL, "gpu-shapes/close-clears", "and the out-param is cleared");
}

void xud_test_shapes(void) {
    xud_test_shapes_pure();
    if (xud_gpu_tests_enabled()) {
        xud_test_shapes_gpu();
    }
}
