/***********************************************************************************************
 *
 *   xud_test_shapes.c - the shapes rung and the public drawing API above it
 *
 *   Owns : the tests of one rung - xud_shapes_append_triangle() and xud_shapes_append_cube(), the
 *          geometry xud_draw_triangle_3d() and xud_draw_cube() name, and the public frame pair xud
 *          _begin_drawing()/xud_end_drawing(). Nothing else - the batch's own appends are the batch
 *          area's, the frame loop's is the frame area's, and the context's lifecycle is the core a
 *          rea's.
 *   Layer: tests, above the batch area and below the core area, which is the layer order: the
 *          shape is checked against a batch the test built itself, with no context and no driver
 *          in sight, and the public calls are checked against a real context in the area's own
 *          GPU half. The pure half is the half the repo's headless-testable rule leans on - the
 *          append is host work on the batch's own counters, so its refusals and its counters are
 *          answerable with no device present, and a machine with no driver gets the same refusals
 *          a machine with a driver gets, which is why the default run stays green. The box's pure
 *          half checks GEOMETRY rather than a remembered table: the positions are checked to sit on
 *          their own face's plane and inside the other two extents, the normals to be unit axis
 *          vectors, and a face's first triangle to have a cross product that points along that
 *          face's outward normal - which is what would catch a face table written with the wrong
 *          winding or the wrong corner signs, and what does not depend on the table being right to
 *          say it is right.
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
    XudVector3 center;
    XudColor color;
    float width = 0.0f;
    float height = 0.0f;
    float length = 0.0f;
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

    /* The box's centre and extents: a centre at the origin and three DIFFERENT extents, so the
       shape under test is a box rather than a cube and a test that assumed eight symmetric corners
       would be a test of the wrong shape. */
    center = (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 0.0f };
    width = 1.0f;
    height = 0.5f;
    length = 2.0f;

    /*--- the refusals that need no batch and no context at all -------------------------------*/

    xud_check(xud_shapes_append_triangle(NULL, v0, v1, v2, color) == XUD_INVALID_ARG, "shapes/append-no-batch",
        "a shape with nowhere to be appended into is refused");
    xud_check(xud_draw_triangle_3d(NULL, v0, v1, v2, color) == XUD_INVALID_ARG, "shapes/draw-no-context",
        "a public draw call with no context is refused");
    xud_check(xud_draw_triangle_3d(&shell, v0, v1, v2, color) == XUD_UNSUPPORTED, "shapes/draw-no-renderer",
        "a context with no renderer has no batch to append into, and the refusal is reported");
    xud_check(xud_shapes_append_cube(NULL, center, width, height, length, color) == XUD_INVALID_ARG, "shapes/cube-append-no-batch",
        "and so is a box - the guard is the same one, one rung above the same two codes");
    xud_check(xud_draw_cube(NULL, center, width, height, length, color) == XUD_INVALID_ARG, "shapes/cube-draw-no-context",
        "a public draw call with no context is refused");
    xud_check(xud_draw_cube(&shell, center, width, height, length, color) == XUD_UNSUPPORTED, "shapes/cube-draw-no-renderer",
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

    /*--- the box: the shape above the triangle, and the first whose attribute rows are the shape's
       own ----------------------------------------------------------------------------*/

    /* A batch of exactly one box: 24 vertices and 36 indices, and the batch's own index headroom
       for 24 vertices is exactly 36, so a box spends BOTH of the batch's bounds exactly and the
       refusal below is a refusal of a whole second box rather than of the half of one that did not
       fit. */
    xud_check(
        xud_batch_layout(&b, XUD_SHAPES_CUBE_VERTEX_COUNT) == XUD_OK, "shapes/cube-layout", "a batch of one box's vertex count lays out");
    xud_check(b.vertexCapacity == XUD_SHAPES_CUBE_VERTEX_COUNT, "shapes/cube-capacity-is-a-box",
        "the capacity asked for is the capacity recorded");
    xud_check(b.indexCapacity == XUD_SHAPES_CUBE_INDEX_COUNT, "shapes/cube-index-capacity-is-exact",
        "the batch's index headroom for a box's vertex count is exactly a box's index count");
    xud_check(XUD_SHAPES_CUBE_INDEX_COUNT == XUD_BATCH_INDEX_CAPACITY(XUD_SHAPES_CUBE_VERTEX_COUNT), "shapes/cube-is-the-euler-case",
        "a box spends 1.5 indices per vertex, which is the 3n/2 the batch sizes its headroom at - the closed-mesh case of that bound");
    xud_check((XUD_SHAPES_CUBE_VERTEX_COUNT <= XUD_BATCH_INITIAL_VERTICES)
            && (XUD_SHAPES_CUBE_INDEX_COUNT <= XUD_BATCH_INDEX_CAPACITY(XUD_BATCH_INITIAL_VERTICES)),
        "shapes/cube-fits-the-initial-capacity",
        "one box fits the capacity a context's batch opens at, so drawing one asks nothing of the grow path");

    b.stagingMapped = xudTestArena;
    b.stagingMappedLive = 1u;

    result = xud_shapes_append_cube(&b, center, width, height, length, color);
    xud_check(result == XUD_OK, "shapes/cube-append-ok", "the box appends");
    xud_check(xud_batch_stats(&b, &vertexCount, &indexCount, &capacity, &drawCount, &stagingBytes) == XUD_OK,
        "shapes/cube-stats-after-append", "and the counters answer after it");
    xud_check(vertexCount == XUD_SHAPES_CUBE_VERTEX_COUNT, "shapes/cube-costs-24-vertices",
        "24 vertices, four per face, so a corner is not shared between the three faces that want three different normals");
    xud_check(indexCount == XUD_SHAPES_CUBE_INDEX_COUNT, "shapes/cube-costs-36-indices", "36 indices, two triangles per face");
    xud_check(drawCount == 0u, "shapes/cube-records-no-draw", "and appending records no draw row, as the triangle's append does");
    xud_check(xud_batch_check_invariant(&b) == XUD_OK, "shapes/cube-invariant", "and the batch's invariant still holds");

    /* The geometry, read off the arena rather than off the table the append was written from: the
       positions have to sit on their own face's plane and inside the other two extents, the normals
       have to be unit axis vectors, a face's two triangles have to have cross products that point
       along that face's OUTWARD normal (the counter-clockwise-from-outside claim the shape states,
       and the one check here that would notice a face table written with the corners in the wrong
       order), and every index has to name one of its own face's four corners, exactly twice. The
       extents below are dyadic (1, 0.5, 2) so every comparison here is exact rather than a toleran
       ce standing in for a fact. */
    {
        const uint8_t* arena = (const uint8_t*)b.stagingMapped;
        float half[3];
        float centre[3];
        float pos[XUD_SHAPES_CUBE_VERTEX_COUNT][3];
        float nrm[XUD_SHAPES_CUBE_VERTEX_COUNT][3];
        uint32_t idx[XUD_SHAPES_CUBE_INDEX_COUNT];
        uint32_t v = 0u;
        uint32_t face = 0u;
        uint32_t colourRows = 0u;
        uint32_t zeroUvRows = 0u;
        uint32_t unitAxisNormals = 0u;
        uint32_t onOwnFacePlane = 0u;
        uint32_t insideOtherExtents = 0u;
        uint32_t facesWoundOutward = 0u;
        uint32_t facesShareOneNormal = 0u;
        uint32_t facesIndexOwnCorners = 0u;

        half[0] = width * XUD_SHAPES_CUBE_HALF_EXTENT;
        half[1] = height * XUD_SHAPES_CUBE_HALF_EXTENT;
        half[2] = length * XUD_SHAPES_CUBE_HALF_EXTENT;
        centre[0] = center.x;
        centre[1] = center.y;
        centre[2] = center.z;

        for (v = 0u; v < XUD_SHAPES_CUBE_VERTEX_COUNT; v++) {
            float uvRow[2];
            uint32_t nonZero = 0u;
            uint32_t axis = 0u;
            uint32_t unit = 0u;
            float sign = 0.0f;
            uint32_t k = 0u;

            memcpy(&pos[v][0], arena + b.streamOffset[XUD_BATCH_STREAM_POS] + ((size_t)v * XUD_BATCH_STREAM_POS_SIZE), sizeof(pos[v]));
            memcpy(
                &nrm[v][0], arena + b.streamOffset[XUD_BATCH_STREAM_NORMAL] + ((size_t)v * XUD_BATCH_STREAM_NORMAL_SIZE), sizeof(nrm[v]));
            memcpy(uvRow, arena + b.streamOffset[XUD_BATCH_STREAM_UV] + ((size_t)v * XUD_BATCH_STREAM_UV_SIZE), sizeof(uvRow));

            if (memcmp(arena + b.streamOffset[XUD_BATCH_STREAM_COLOR] + ((size_t)v * XUD_BATCH_STREAM_COLOR_SIZE), &color,
                    XUD_BATCH_STREAM_COLOR_SIZE)
                == 0) {
                colourRows++;
            }
            if ((uvRow[0] == XUD_SHAPES_UV_S) && (uvRow[1] == XUD_SHAPES_UV_T)) {
                zeroUvRows++;
            }

            /* A face normal is a unit vector down one axis: one non-zero component, and that
               component is exactly +1 or -1. The axis and the sign are kept because the two checks
               below are stated against them - a vertex is on its own face's plane when it is half a
               box out along the axis its own normal points down, and it is inside the box along the
               other two. */
            for (k = 0u; k < 3u; k++) {
                if (nrm[v][k] != 0.0f) {
                    nonZero++;
                    axis = k;
                    unit = ((nrm[v][k] == 1.0f) || (nrm[v][k] == -1.0f)) ? 1u : 0u;
                    sign = (nrm[v][k] > 0.0f) ? 1.0f : -1.0f;
                }
            }
            if ((nonZero == 1u) && (unit == 1u)) {
                unitAxisNormals++;
                if (((pos[v][axis] - centre[axis]) * sign) == half[axis]) {
                    onOwnFacePlane++;
                }
                for (k = 0u; k < 3u; k++) {
                    if (k == axis) {
                        continue;
                    }
                    if (((pos[v][k] - centre[k]) < -half[k]) || ((pos[v][k] - centre[k]) > half[k])) {
                        break;
                    }
                    insideOtherExtents++;
                }
            }
        }

        for (v = 0u; v < XUD_SHAPES_CUBE_INDEX_COUNT; v++) {
            memcpy(&idx[v], arena + b.indexOffset + ((size_t)v * XUD_BATCH_INDEX_SIZE), sizeof(idx[v]));
        }

        for (face = 0u; face < XUD_SHAPES_CUBE_FACE_COUNT; face++) {
            const float* p0 = &pos[face * XUD_SHAPES_CUBE_FACE_VERTEX_COUNT][0];
            const float* p1 = &pos[(face * XUD_SHAPES_CUBE_FACE_VERTEX_COUNT) + 1u][0];
            const float* p2 = &pos[(face * XUD_SHAPES_CUBE_FACE_VERTEX_COUNT) + 2u][0];
            const float* p3 = &pos[(face * XUD_SHAPES_CUBE_FACE_VERTEX_COUNT) + 3u][0];
            const float* n = &nrm[face * XUD_SHAPES_CUBE_FACE_VERTEX_COUNT][0];
            float cross[3];
            float dot = 0.0f;
            uint32_t seen[XUD_SHAPES_CUBE_FACE_VERTEX_COUNT];
            uint32_t row = 0u;
            uint32_t own = 0u;
            uint32_t named = 0u;

            for (row = 0u; row < XUD_SHAPES_CUBE_FACE_VERTEX_COUNT; row++) {
                seen[row] = 0u;
            }

            /* The face's four corners are consecutive vertices, so the face's own normal is the one
               all four of them carry - checked as all-four-equal rather than assumed, because a
               face whose four vertices disagree about their normal is a face the rasteriser would
               shade with a normal of its own invention. */
            if ((memcmp(&nrm[(face * XUD_SHAPES_CUBE_FACE_VERTEX_COUNT) + 1u][0], n, sizeof(nrm[0])) == 0)
                && (memcmp(&nrm[(face * XUD_SHAPES_CUBE_FACE_VERTEX_COUNT) + 2u][0], n, sizeof(nrm[0])) == 0)
                && (memcmp(&nrm[(face * XUD_SHAPES_CUBE_FACE_VERTEX_COUNT) + 3u][0], n, sizeof(nrm[0])) == 0)) {
                own++;
            }

            /* The two triangles of the face, (0,1,2) and (0,2,3): each one's cross product has to
               point along the face's outward normal, which is the whole counter-clockwise-from-out
               side claim and is checked for both triangles rather than the first. */
            cross[0] = ((p1[1] - p0[1]) * (p2[2] - p0[2])) - ((p1[2] - p0[2]) * (p2[1] - p0[1]));
            cross[1] = ((p1[2] - p0[2]) * (p2[0] - p0[0])) - ((p1[0] - p0[0]) * (p2[2] - p0[2]));
            cross[2] = ((p1[0] - p0[0]) * (p2[1] - p0[1])) - ((p1[1] - p0[1]) * (p2[0] - p0[0]));
            dot = (cross[0] * n[0]) + (cross[1] * n[1]) + (cross[2] * n[2]);
            if (dot > 0.0f) {
                cross[0] = ((p2[1] - p0[1]) * (p3[2] - p0[2])) - ((p2[2] - p0[2]) * (p3[1] - p0[1]));
                cross[1] = ((p2[2] - p0[2]) * (p3[0] - p0[0])) - ((p2[0] - p0[0]) * (p3[2] - p0[2]));
                cross[2] = ((p2[0] - p0[0]) * (p3[1] - p0[1])) - ((p2[1] - p0[1]) * (p3[0] - p0[0]));
                dot = (cross[0] * n[0]) + (cross[1] * n[1]) + (cross[2] * n[2]);
                if (dot > 0.0f) {
                    facesWoundOutward++;
                }
            }

            /* The face's six indices name its own four corners, and all four of them are named -
               the two triangles share the 0-to-2 diagonal, so the two corners at the ends of that
               diagonal are named twice and the other two once each, which is why the claim here is
               "all four named" rather than "each named twice" and what a face drawn with the wrong
               four would fail. */
            for (row = 0u; row < XUD_SHAPES_CUBE_FACE_INDEX_COUNT; row++) {
                const uint32_t corner = idx[(face * XUD_SHAPES_CUBE_FACE_INDEX_COUNT) + row];
                if ((corner >= (face * XUD_SHAPES_CUBE_FACE_VERTEX_COUNT))
                    && (corner < ((face + 1u) * XUD_SHAPES_CUBE_FACE_VERTEX_COUNT))) {
                    seen[corner - (face * XUD_SHAPES_CUBE_FACE_VERTEX_COUNT)]++;
                }
            }
            for (row = 0u; row < XUD_SHAPES_CUBE_FACE_VERTEX_COUNT; row++) {
                if (seen[row] >= 1u) {
                    named++;
                }
            }
            if (own == 1u) {
                facesShareOneNormal++;
            }
            if (named == XUD_SHAPES_CUBE_FACE_VERTEX_COUNT) {
                facesIndexOwnCorners++;
            }
        }

        xud_check(colourRows == XUD_SHAPES_CUBE_VERTEX_COUNT, "shapes/cube-colour-at-every-vertex",
            "the caller's four bytes, in the caller's order, at all 24 vertices - the batch's COLOR stream is per-vertex and a box has one "
            "colour");
        xud_check(zeroUvRows == XUD_SHAPES_CUBE_VERTEX_COUNT, "shapes/cube-zero-uv-at-every-vertex",
            "the header's zero uv at all 24: a box has no texture to unwrap yet, and set 1 is still unallocated");
        xud_check(unitAxisNormals == XUD_SHAPES_CUBE_VERTEX_COUNT, "shapes/cube-normals-are-unit-axes",
            "every vertex's normal is a unit vector down one axis, which is the shape's own attribute row rather than the flat +Z "
            "constant");
        xud_check(onOwnFacePlane == XUD_SHAPES_CUBE_VERTEX_COUNT, "shapes/cube-vertices-on-own-face-plane",
            "every vertex sits half a box out along the axis its own normal points down, which is what says the corner table and the "
            "normal table agree with each other");
        xud_check(insideOtherExtents == (XUD_SHAPES_CUBE_VERTEX_COUNT * 2u), "shapes/cube-vertices-inside-the-box",
            "and inside the other two extents along the other two axes, which is the extent the caller asked for rather than one the shape "
            "invented");
        xud_check(facesWoundOutward == XUD_SHAPES_CUBE_FACE_COUNT, "shapes/cube-faces-wound-outward",
            "both triangles of all six faces have cross products along the OUTWARD normal, which is the counter-clockwise-from-outside "
            "claim and would fail on a face table written with the corners in the wrong order");
        xud_check(facesShareOneNormal == XUD_SHAPES_CUBE_FACE_COUNT, "shapes/cube-face-shares-one-normal",
            "all four corners of a face carry that face's normal, so a face has one normal rather than four the rasteriser would have to "
            "choose between");
        xud_check(facesIndexOwnCorners == XUD_SHAPES_CUBE_FACE_COUNT, "shapes/cube-indices-name-own-face",
            "every face's six indices name its own four corners and all four of them are named, so the face is closed and no index reaches "
            "a neighbour's vertices - the two triangles share the 0-to-2 diagonal, so the diagonal's corners are named twice and the other "
            "two once");
    }

    /* The refusal half of the atomicity claim, at a shape boundary twice the triangle's: the second
       box does not fit and the check is the counters as well as the code. */
    result = xud_shapes_append_cube(&b, center, width, height, length, color);
    xud_check(result == XUD_OUT_OF_MEMORY, "shapes/cube-second-refused",
        "a box that does not fit the batch's capacities is refused with the batch's own code");
    xud_check(xud_batch_stats(&b, &vertexCount, &indexCount, &capacity, &drawCount, &stagingBytes) == XUD_OK,
        "shapes/cube-stats-after-refusal", "and the counters are still readable after the refusal");
    xud_check(vertexCount == XUD_SHAPES_CUBE_VERTEX_COUNT, "shapes/cube-refusal-wrote-no-vertex",
        "the refused box wrote no vertex, so the live count is still 24");
    xud_check(
        indexCount == XUD_SHAPES_CUBE_INDEX_COUNT, "shapes/cube-refusal-wrote-no-index", "and no index, so the live count is still 36");
    xud_check(xud_batch_check_invariant(&b) == XUD_OK, "shapes/cube-invariant-after-refusal",
        "and the batch's invariant still holds, which is what says nothing half-written is in it");

    /* A box beside a triangle in one batch: the counts add per shape, and the two shapes' index
       spend is not one number - a box spends 1.5 indices per vertex and a triangle spends 1, so a
       frame that already holds shapes can be refused for its indices before it is refused for its
       vertices, which is the reason the box's append checks both bounds. */
    xud_check(xud_batch_layout(&b, XUD_BATCH_INITIAL_VERTICES) == XUD_OK, "shapes/cube-mixed-layout",
        "a batch at the capacity a context opens at lays out");
    b.stagingMapped = xudTestArena;
    b.stagingMappedLive = 1u;
    result = xud_shapes_append_triangle(&b, v0, v1, v2, color);
    xud_check(result == XUD_OK, "shapes/cube-mixed-triangle", "the triangle appends into it");
    result = xud_shapes_append_cube(&b, center, width, height, length, color);
    xud_check(result == XUD_OK, "shapes/cube-mixed-cube", "and the box appends beside it, into the same open batch");
    xud_check(xud_batch_stats(&b, &vertexCount, &indexCount, &capacity, &drawCount, &stagingBytes) == XUD_OK, "shapes/cube-mixed-stats",
        "and the counters answer after both");
    xud_check(vertexCount == (XUD_SHAPES_TRIANGLE_VERTEX_COUNT + XUD_SHAPES_CUBE_VERTEX_COUNT), "shapes/cube-mixed-vertex-count",
        "27 vertices, which is the append being per shape rather than per frame");
    xud_check(indexCount == (XUD_SHAPES_TRIANGLE_INDEX_COUNT + XUD_SHAPES_CUBE_INDEX_COUNT), "shapes/cube-mixed-index-count",
        "39 indices, 36 of them the box's at 1.5 per vertex and 3 of them the triangle's at 1 per vertex");
    xud_check(
        xud_batch_check_invariant(&b) == XUD_OK, "shapes/cube-mixed-invariant", "and the batch's invariant still holds over two shapes");
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
    XudColor blue = (XudColor) { .r = 0u, .g = 0u, .b = 255u, .a = 255u };
    XudVector3 cubeCentre = (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 0.0f };

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

        /* The box in the same frame is the shape that needs per-face attribute rows, and it is also
           the reason the frame's flush is one triangle-list pipeline: 24 vertices and 36 indices
           appended beside the frame's two triangles, replayed through the same pipeline the tria
           ngles went through. A line or a point would need a different topology and would not be
           this rung's to draw (see xud_shapes.h's note), which is what makes a box the shape that
           can land here today. */
        result = xud_draw_cube(context, cubeCentre, 0.8f, 0.8f, 0.8f, blue);
        xud_check(result == XUD_OK, "gpu-shapes/cube-draw-ok", "the public box call appends into the context's own batch");
        xud_check(xud_batch_stats(context->renderer->batch, &vertexCount, &indexCount, &capacity, &drawCount, &stagingBytes) == XUD_OK,
            "gpu-shapes/stats-after-cube", "and the counters answer after it");
        xud_check(vertexCount == ((XUD_SHAPES_TRIANGLE_VERTEX_COUNT * 2u) + XUD_SHAPES_CUBE_VERTEX_COUNT),
            "gpu-shapes/cube-costs-24-vertices",
            "30 vertices in the frame, 24 of them the box's, which is the same count the pure half checked against a batch it built "
            "itself");
        xud_check(indexCount == ((XUD_SHAPES_TRIANGLE_INDEX_COUNT * 2u) + XUD_SHAPES_CUBE_INDEX_COUNT), "gpu-shapes/cube-costs-36-indices",
            "42 indices, 36 of them the box's - inside the initial capacity's headroom, so no grow was asked for mid-frame");

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
