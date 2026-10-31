/***********************************************************************************************
 *
 *   xud_test_shapes.c - the shapes rung and the public drawing API above it
 *
 *   Owns : the tests of one rung - xud_shapes_append_triangle(), xud_shapes_append_cube(), xud
 *          _shapes_append_sphere() and xud_shapes_append_line(), the geometry xud_draw_triangle_3d
 *          (), xud_draw_cube(), xud_draw_sphere() and xud_draw_line_3d()/xud_draw_point_3d() name,
 *          and the public frame pair xud_begin_drawing()/xud_end_drawing().
 *          Nothing else - the batch's own appends are the batch area's, the frame loop's is the
 *          frame area's, and the context's lifecycle is the core area's.
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

#include <math.h> /* Required for: fabsf, the sphere's geometry checks are read against a tolerance rather than exactly, and this area's file is the one that says why */
#include <stdlib.h> /* Required for: free, the draw rows the topology split allocates in this area's line checks */
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

/* The radius the sphere checks use: dyadic, so the radius times a unit direction is an exact
   scaling and the only rounding in the positions below is the trigonometry's own. */
#define XUD_TEST_SHAPES_SPHERE_RADIUS 0.5f

/* The tolerance the sphere's geometry is read against, and why it is a tolerance at all where the
   box's geometry is read exactly: a sphere's positions and normals come from sinf/cosf, which are
   rounding, so no equality here can hold - and the tolerance is stated once here rather than sca
   ttered through the checks, so a check that fails by more than this is a shape bug rather than a
   rounding artefact. */
#define XUD_TEST_SHAPES_SPHERE_TOLERANCE 1e-5f

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
    xud_check(
        xud_shapes_append_sphere(NULL, center, 0.4f, XUD_SHAPES_SPHERE_RING_COUNT, XUD_SHAPES_SPHERE_SLICE_COUNT, color) == XUD_INVALID_ARG,
        "shapes/sphere-append-no-batch", "and so is a sphere - the guard is the same one, one rung above the same two codes");
    xud_check(xud_draw_sphere(NULL, center, 0.4f, color) == XUD_INVALID_ARG, "shapes/sphere-draw-no-context",
        "a public draw call with no context is refused");
    xud_check(xud_draw_sphere(&shell, center, 0.4f, color) == XUD_UNSUPPORTED, "shapes/sphere-draw-no-renderer",
        "a context with no renderer has no batch to append into, and the refusal is reported");
    xud_check(xud_shapes_append_line(NULL, v0, v1, color) == XUD_INVALID_ARG, "shapes/line-append-no-batch",
        "and so is a line - the guard is the same one, one rung above the same two codes");
    xud_check(xud_draw_line_3d(NULL, v0, v1, color) == XUD_INVALID_ARG, "shapes/line-draw-no-context",
        "a public draw call with no context is refused");
    xud_check(xud_draw_line_3d(&shell, v0, v1, color) == XUD_UNSUPPORTED, "shapes/line-draw-no-renderer",
        "a context with no renderer has no batch to append into, and the refusal is reported");
    xud_check(xud_draw_point_3d(NULL, center, color) == XUD_INVALID_ARG, "shapes/point-draw-no-context",
        "a public draw call with no context is refused");
    xud_check(xud_draw_point_3d(&shell, center, color) == XUD_UNSUPPORTED, "shapes/point-draw-no-renderer",
        "and the point is the same guard, one rung above the same two codes - it is a line, so it is "
        "refused as one");
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

    /*--- the sphere: the shape whose vertices are SHARED, and the first whose index spend is above
       the batch's own headroom ----------------------------------------------------------*/

    /* A batch at the capacity a context opens at, rather than one sized at the sphere's own vertex
       count: a batch of 17 vertices has an index headroom of 25 by the batch's own 3n/2 and could
       not hold the sphere's 90 indices at all, so the sphere is the shape that cannot be laid out
       at its own size - which is the same fact the box's layout check states when it sizes a batch
       at the box's vertex count and finds the headroom exactly right, seen from the other side. The
       four checks below are that fact read four ways: one sphere fits both bounds, two spheres do
       not fit the indices, two spheres DO fit the vertices, and the index spend is above the 3n/2
       the headroom is sized at - which together say the INDEX bound is the one that refuses a
       sphere and the vertex bound is not, the exact opposite of the triangle's and the box's. */
    xud_check(xud_batch_layout(&b, XUD_BATCH_INITIAL_VERTICES) == XUD_OK, "shapes/sphere-layout",
        "a batch at the capacity a context opens at lays out");
    xud_check(b.indexCapacity == XUD_BATCH_INDEX_CAPACITY(XUD_BATCH_INITIAL_VERTICES), "shapes/sphere-index-headroom",
        "and its index headroom is the batch's own 3n/2 of that capacity");
    xud_check((XUD_SHAPES_SPHERE_VERTEX_COUNT <= XUD_BATCH_INITIAL_VERTICES) && (XUD_SHAPES_SPHERE_INDEX_COUNT <= b.indexCapacity),
        "shapes/sphere-fits-the-initial-capacity",
        "one sphere fits both of that capacity's bounds, so drawing one asks nothing of the grow path");
    xud_check((2u * XUD_SHAPES_SPHERE_INDEX_COUNT) > b.indexCapacity, "shapes/two-spheres-do-not-fit-the-index-headroom",
        "two spheres do not fit its indices, which is what makes the refusal below a refusal of a whole second sphere rather than of half "
        "of one");
    xud_check((2u * XUD_SHAPES_SPHERE_VERTEX_COUNT) <= XUD_BATCH_INITIAL_VERTICES, "shapes/two-spheres-do-fit-the-vertex-bound",
        "and two spheres DO fit its vertices, which is what says the index bound is the one that refuses a sphere and the vertex bound is "
        "not - the opposite of the triangle's and the box's");
    xud_check(XUD_SHAPES_SPHERE_INDEX_COUNT == ((6u * XUD_SHAPES_SPHERE_VERTEX_COUNT) - 12u), "shapes/sphere-is-the-manifold-bound",
        "90 indices over 17 vertices is exactly 6V-12, the Euler bound for a closed manifold triangulated surface (F = 2V-4 faces, 3F "
        "indices), which is four times the 3n/2 per-vertex headroom the batch sizes at and the reason that note is a choice rather than a "
        "bound");
    xud_check(XUD_SHAPES_SPHERE_INDEX_COUNT > ((XUD_SHAPES_SPHERE_VERTEX_COUNT * 3u) / 2u), "shapes/sphere-spends-above-the-headroom",
        "90 indices over 17 vertices is above the 3n/2 per-vertex headroom the batch sizes at, which is the closed-mesh bound's "
        "counterexample and the reason a sphere shares vertices where a box cannot");

    b.stagingMapped = xudTestArena;
    b.stagingMappedLive = 1u;

    result = xud_shapes_append_sphere(
        &b, center, XUD_TEST_SHAPES_SPHERE_RADIUS, XUD_SHAPES_SPHERE_RING_COUNT, XUD_SHAPES_SPHERE_SLICE_COUNT, color);
    xud_check(result == XUD_OK, "shapes/sphere-append-ok", "the sphere appends");
    xud_check(xud_batch_stats(&b, &vertexCount, &indexCount, &capacity, &drawCount, &stagingBytes) == XUD_OK,
        "shapes/sphere-stats-after-append", "and the counters answer after it");
    xud_check(vertexCount == XUD_SHAPES_SPHERE_VERTEX_COUNT, "shapes/sphere-costs-17-vertices",
        "17 vertices, which is two single-vertex poles plus three rows of five - not the 30-vertex grid a seam-duplicated row and pole "
        "rows would give");
    xud_check(indexCount == XUD_SHAPES_SPHERE_INDEX_COUNT, "shapes/sphere-costs-90-indices",
        "90 indices, which is two fans of five triangles plus two bands of five quads, three indices per triangle");
    xud_check(indexCount == ((6u * vertexCount) - 12u), "shapes/sphere-live-count-is-the-manifold-bound",
        "and read off the batch's own counters rather than off the header, the live index count is 6V-12 for the live vertex count, so the "
        "mesh the append wrote is closed rather than a soup with a hole in it");
    xud_check(
        drawCount == 0u, "shapes/sphere-records-no-draw", "and appending records no draw row, as the triangle's and the box's appends do");
    xud_check(xud_batch_check_invariant(&b) == XUD_OK, "shapes/sphere-invariant", "and the batch's invariant still holds");

    /* The tessellation and radius refusals, at a tessellation the public call never asks for: the
       append takes rings and slices so the rung's own minimums are checkable, and the closed-surface
       minimums are refused rather than drawn degenerate. The NaN radius is refused by the same test
       the zero and the negative one are - the test is written as "not greater" rather than "less or
       equal" for exactly that reason, and a NaN radius divided into a normal would be a NaN in a
       float stream rather than a value the pipeline can read. */
    xud_check(xud_shapes_append_sphere(
                  &b, center, XUD_TEST_SHAPES_SPHERE_RADIUS, (XUD_SHAPES_SPHERE_RING_MIN - 1u), XUD_SHAPES_SPHERE_SLICE_COUNT, color)
            == XUD_INVALID_ARG,
        "shapes/sphere-rings-below-min", "one ring is not a closed surface, so it is refused rather than drawn as a degenerate one");
    xud_check(xud_shapes_append_sphere(
                  &b, center, XUD_TEST_SHAPES_SPHERE_RADIUS, XUD_SHAPES_SPHERE_RING_COUNT, (XUD_SHAPES_SPHERE_SLICE_MIN - 1u), color)
            == XUD_INVALID_ARG,
        "shapes/sphere-slices-below-min",
        "and two longitudes are a band whose faces lie on each other rather than around an axis, so they are refused too");
    xud_check(
        xud_shapes_append_sphere(&b, center, 0.0f, XUD_SHAPES_SPHERE_RING_COUNT, XUD_SHAPES_SPHERE_SLICE_COUNT, color) == XUD_INVALID_ARG,
        "shapes/sphere-zero-radius",
        "a sphere with no radius has no surface and no radial direction, and the normal is that direction divided by it");
    xud_check(
        xud_shapes_append_sphere(&b, center, -0.5f, XUD_SHAPES_SPHERE_RING_COUNT, XUD_SHAPES_SPHERE_SLICE_COUNT, color) == XUD_INVALID_ARG,
        "shapes/sphere-negative-radius", "and a negative one is inside-out rather than a sphere the caller asked for");
    xud_check(
        xud_shapes_append_sphere(&b, center, NAN, XUD_SHAPES_SPHERE_RING_COUNT, XUD_SHAPES_SPHERE_SLICE_COUNT, color) == XUD_INVALID_ARG,
        "shapes/sphere-nan-radius",
        "and a NaN radius is refused by the same test, which is why the test is written as not-greater rather than less-or-equal");
    xud_check(xud_batch_stats(&b, &vertexCount, &indexCount, &capacity, &drawCount, &stagingBytes) == XUD_OK,
        "shapes/sphere-stats-after-refusals",
        "and none of those refusals wrote anything, so the counters still answer at the sphere's own counts");
    xud_check((vertexCount == XUD_SHAPES_SPHERE_VERTEX_COUNT) && (indexCount == XUD_SHAPES_SPHERE_INDEX_COUNT),
        "shapes/sphere-refusals-wrote-nothing",
        "17 vertices and 90 indices, which is what says a refused tessellation or radius is atomic and not half a mesh");

    /* The geometry, read off the arena rather than off the loops the append was written from: every
       position has to be one radius from the centre, every normal has to be a unit vector pointing
       at that same position away from the centre (which is the sphere's own claim - a normal that
       is unit but not radial, or radial but not unit, fails one of the two checks and not both),
       every triangle has to be non-degenerate and wound counter-clockwise from outside, and every
       index has to name a vertex that exists and every vertex has to be named by at least one. The
       tolerance is XUD_TEST_SHAPES_SPHERE_TOLERANCE rather than an equality because a sphere's
       positions and normals come from sinf/cosf, which are rounding - unlike the box's dyadic ext
       ents, where every comparison above is exact. */
    {
        const uint8_t* arena = (const uint8_t*)b.stagingMapped;
        const float radius = XUD_TEST_SHAPES_SPHERE_RADIUS;
        const float tolerance = XUD_TEST_SHAPES_SPHERE_TOLERANCE;
        float centre[3];
        float pos[XUD_SHAPES_SPHERE_VERTEX_COUNT][3];
        float nrm[XUD_SHAPES_SPHERE_VERTEX_COUNT][3];
        uint32_t idx[XUD_SHAPES_SPHERE_INDEX_COUNT];
        uint32_t seen[XUD_SHAPES_SPHERE_VERTEX_COUNT];
        uint32_t v = 0u;
        uint32_t row = 0u;
        uint32_t onSphere = 0u;
        uint32_t unitNormal = 0u;
        uint32_t radialNormal = 0u;
        uint32_t zeroUv = 0u;
        uint32_t colourRows = 0u;
        uint32_t indexInRange = 0u;
        uint32_t vertexNamed = 0u;
        uint32_t distinctCorners = 0u;
        uint32_t nonDegenerate = 0u;
        uint32_t woundOutward = 0u;

        centre[0] = center.x;
        centre[1] = center.y;
        centre[2] = center.z;

        for (v = 0u; v < XUD_SHAPES_SPHERE_VERTEX_COUNT; v++) {
            seen[v] = 0u;
        }

        for (v = 0u; v < XUD_SHAPES_SPHERE_VERTEX_COUNT; v++) {
            float uvRow[2];
            float offset2 = 0.0f;
            float normal2 = 0.0f;
            float radial = 0.0f;
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
                zeroUv++;
            }

            for (k = 0u; k < 3u; k++) {
                const float off = pos[v][k] - centre[k];
                offset2 += off * off;
                normal2 += nrm[v][k] * nrm[v][k];
                radial += off * nrm[v][k];
            }
            if (fabsf(offset2 - (radius * radius)) <= tolerance) {
                onSphere++;
            }
            if (fabsf(normal2 - 1.0f) <= tolerance) {
                unitNormal++;
            }
            /* dot(offset, normal) is the radius only when the normal is unit AND points along the
               offset: a normal pointing anywhere else scores below, and one pointing back at the
               centre scores negative. That is the radial claim, checked rather than asserted. */
            if (fabsf(radial - radius) <= tolerance) {
                radialNormal++;
            }
        }

        for (row = 0u; row < XUD_SHAPES_SPHERE_INDEX_COUNT; row++) {
            memcpy(&idx[row], arena + b.indexOffset + ((size_t)row * XUD_BATCH_INDEX_SIZE), sizeof(idx[row]));
        }

        for (row = 0u; row < XUD_SHAPES_SPHERE_INDEX_COUNT; row++) {
            if (idx[row] < XUD_SHAPES_SPHERE_VERTEX_COUNT) {
                indexInRange++;
            }
        }
        for (v = 0u; v < XUD_SHAPES_SPHERE_VERTEX_COUNT; v++) {
            for (row = 0u; row < XUD_SHAPES_SPHERE_INDEX_COUNT; row++) {
                if (idx[row] == v) {
                    seen[v]++;
                }
            }
        }
        for (v = 0u; v < XUD_SHAPES_SPHERE_VERTEX_COUNT; v++) {
            if (seen[v] >= 1u) {
                vertexNamed++;
            }
        }

        for (row = 0u; row < XUD_SHAPES_SPHERE_TRIANGLE_COUNT; row++) {
            const uint32_t i0 = idx[row * 3u];
            const uint32_t i1 = idx[(row * 3u) + 1u];
            const uint32_t i2 = idx[(row * 3u) + 2u];
            const float* p0 = (i0 < XUD_SHAPES_SPHERE_VERTEX_COUNT) ? &pos[i0][0] : &pos[0][0];
            const float* p1 = (i1 < XUD_SHAPES_SPHERE_VERTEX_COUNT) ? &pos[i1][0] : &pos[0][0];
            const float* p2 = (i2 < XUD_SHAPES_SPHERE_VERTEX_COUNT) ? &pos[i2][0] : &pos[0][0];
            float e1[3];
            float e2[3];
            float cross[3];
            float outward[3];
            float cross2 = 0.0f;
            float facing = 0.0f;
            uint32_t k = 0u;

            if ((i0 != i1) && (i1 != i2) && (i0 != i2)) {
                distinctCorners++;
            }

            for (k = 0u; k < 3u; k++) {
                e1[k] = p1[k] - p0[k];
                e2[k] = p2[k] - p0[k];
            }
            cross[0] = (e1[1] * e2[2]) - (e1[2] * e2[1]);
            cross[1] = (e1[2] * e2[0]) - (e1[0] * e2[2]);
            cross[2] = (e1[0] * e2[1]) - (e1[1] * e2[0]);
            for (k = 0u; k < 3u; k++) {
                cross2 += cross[k] * cross[k];
                outward[k] = ((p0[k] + p1[k] + p2[k]) / 3.0f) - centre[k];
            }
            facing = (cross[0] * outward[0]) + (cross[1] * outward[1]) + (cross[2] * outward[2]);

            /* A degenerate triangle has a zero cross product, and the winding check below cannot
               read one: its facing value is zero too, which is neither positive nor negative. So
               the two checks are separate rather than one, and the degenerate case is the one the
               single-vertex poles exist to avoid. */
            if (cross2 > 0.0f) {
                nonDegenerate++;
            }
            if (facing > 0.0f) {
                woundOutward++;
            }
        }

        xud_check(colourRows == XUD_SHAPES_SPHERE_VERTEX_COUNT, "shapes/sphere-colour-at-every-vertex",
            "the caller's four bytes, in the caller's order, at all 17 vertices - the batch's COLOR stream is per-vertex and a sphere has "
            "one "
            "colour");
        xud_check(zeroUv == XUD_SHAPES_SPHERE_VERTEX_COUNT, "shapes/sphere-zero-uv-at-every-vertex",
            "the header's zero uv at all 17: a sphere has no texture to unwrap yet, and set 1 is still unallocated");
        xud_check(onSphere == XUD_SHAPES_SPHERE_VERTEX_COUNT, "shapes/sphere-vertices-at-the-radius",
            "every position is one radius from the centre, which is the caller's radius rather than one the shape invented - and it is "
            "read as a squared distance against a tolerance, because a sphere's positions come from sinf/cosf");
        xud_check(unitNormal == XUD_SHAPES_SPHERE_VERTEX_COUNT, "shapes/sphere-normals-are-unit",
            "every normal is a unit vector, which is what a lighting-ready NORMAL stream needs and what a normalised-but-not or a zero one "
            "would fail");
        xud_check(radialNormal == XUD_SHAPES_SPHERE_VERTEX_COUNT, "shapes/sphere-normals-are-radial",
            "and every one of them points at its own position away from the centre, which is the sphere's own claim and would fail on a "
            "normal copied from the flat +Z constant the triangle and the box's faces use");
        xud_check(indexInRange == XUD_SHAPES_SPHERE_INDEX_COUNT, "shapes/sphere-indices-in-range",
            "every index names a vertex that exists, so no index reaches past the sphere's own 17 into whatever the batch holds beside it");
        xud_check(vertexNamed == XUD_SHAPES_SPHERE_VERTEX_COUNT, "shapes/sphere-every-vertex-named",
            "and every vertex is named by at least one index, which is what says the mesh is not a row of vertices with a hole in it - the "
            "two poles are named by the five fan triangles each");
        xud_check(distinctCorners == XUD_SHAPES_SPHERE_TRIANGLE_COUNT, "shapes/sphere-triangles-have-three-corners",
            "every triangle names three different vertices, which is what a row of coincident pole vertices would fail and the "
            "single-vertex poles exist to give");
        xud_check(nonDegenerate == XUD_SHAPES_SPHERE_TRIANGLE_COUNT, "shapes/sphere-triangles-are-not-degenerate",
            "and every triangle has a non-zero cross product, which is the winding check's own precondition - a degenerate triangle's "
            "facing value is zero, which is neither outward nor inward, so it cannot be read as a winding");
        xud_check(woundOutward == XUD_SHAPES_SPHERE_TRIANGLE_COUNT, "shapes/sphere-triangles-wound-outward",
            "and all 30 of them are wound counter-clockwise seen from outside, which is the pipeline's front face and would fail on a fan "
            "or a band written in the other order");
    }

    /* The refusal half of the atomicity claim, at the shape whose index spend is the batch's counter
       example: a second sphere fits the vertex bound with 30 vertices to spare and does not fit the
       index headroom at all, so the refusal is an INDEX refusal and the check is the counters as
       well as the code. */
    result = xud_shapes_append_sphere(
        &b, center, XUD_TEST_SHAPES_SPHERE_RADIUS, XUD_SHAPES_SPHERE_RING_COUNT, XUD_SHAPES_SPHERE_SLICE_COUNT, color);
    xud_check(result == XUD_OUT_OF_MEMORY, "shapes/sphere-second-refused",
        "a second sphere does not fit the batch's index headroom and is refused with the batch's own code");
    xud_check(xud_batch_stats(&b, &vertexCount, &indexCount, &capacity, &drawCount, &stagingBytes) == XUD_OK,
        "shapes/sphere-stats-after-refusal", "and the counters are still readable after the refusal");
    xud_check(vertexCount == XUD_SHAPES_SPHERE_VERTEX_COUNT, "shapes/sphere-refusal-wrote-no-vertex",
        "the refused sphere wrote no vertex, so the live count is still 17 - and 47 vertices of room remain, which is what says the "
        "refusal is an index refusal and not a vertex one");
    xud_check(indexCount == XUD_SHAPES_SPHERE_INDEX_COUNT, "shapes/sphere-refusal-wrote-no-index",
        "and no index, so the live count is still 90, six slots short of the headroom's 96");
    xud_check(xud_batch_check_invariant(&b) == XUD_OK, "shapes/sphere-invariant-after-refusal",
        "and the batch's invariant still holds, which is what says nothing half-written is in it");

    /* A box beside a sphere in one batch: the two shapes' index spend is not one number, and the
       box is refused here for its indices while it still fits the sphere's leftover vertices - the
       sphere spends 90 indices over 17 vertices and the box needs 36 more over 24, which is 126
       against a headroom of 96. The same fact the triangle-and-box check above states, with the
       shapes the other way round. */
    result = xud_shapes_append_cube(&b, center, width, height, length, color);
    xud_check(result == XUD_OUT_OF_MEMORY, "shapes/sphere-plus-cube-refused",
        "a box beside a sphere does not fit the index headroom and is refused");
    xud_check((XUD_SHAPES_SPHERE_VERTEX_COUNT + XUD_SHAPES_CUBE_VERTEX_COUNT) <= XUD_BATCH_INITIAL_VERTICES,
        "shapes/sphere-plus-cube-fits-the-vertex-bound",
        "and it DOES fit the vertex bound, so the refusal is the index bound's and not the vertex one's");
    xud_check((XUD_SHAPES_SPHERE_INDEX_COUNT + XUD_SHAPES_CUBE_INDEX_COUNT) > XUD_BATCH_INDEX_CAPACITY(XUD_BATCH_INITIAL_VERTICES),
        "shapes/sphere-plus-cube-over-the-index-headroom", "90 plus 36 indices is over the 96-slot headroom the capacity sizes at 3n/2");
    xud_check(xud_batch_check_invariant(&b) == XUD_OK, "shapes/sphere-plus-cube-invariant",
        "and the batch's invariant still holds over the refusal, so the refused box left the sphere alone");

    /*--- the line: the shape on the OTHER row of the pipeline list ---------------------------*/

    /* The batch is still the sphere's: 17 vertices and 90 indices, at the capacity a context opens
       at. A line beside the sphere fits both bounds (92 of the 96-slot index headroom), and the
       append is the first thing in the tree that moves the batch's topology state - which makes
       THIS check the row split rather than a shape: the sphere's run closes into a row of the
       same batch and the line's two indices start the next one, so the frame's upload is still
       one upload and the frame's replay is two rows through two pipelines.
    */
    result = xud_shapes_append_line(&b, v0, v1, color);
    xud_check(result == XUD_OK, "shapes/line-beside-sphere-ok", "a line appends beside a sphere");
    xud_check(b.currentTopology == XUD_BATCH_TOPOLOGY_LINES, "shapes/line-moved-topology",
        "and the append moved the batch's topology state, which is the shape owning the state change "
        "rather than the caller");
    xud_check(b.drawCount == 1u, "shapes/line-split-one-row",
        "and closed the sphere's run into one row of the same batch, so a frame that draws a sphere "
        "and a line is two rows rather than one row at whichever topology came last");
    xud_check((b.draws[0u].topology == XUD_BATCH_TOPOLOGY_TRIANGLES) && (b.draws[0u].indexCount == XUD_SHAPES_SPHERE_INDEX_COUNT),
        "shapes/line-first-row-is-the-sphere's",
        "and that row is a TRIANGLE row over the sphere's 90 indices - the row names both axes and "
        "the pipeline list is indexed by the pair");
    xud_check(xud_batch_stats(&b, &vertexCount, &indexCount, &capacity, &drawCount, &stagingBytes) == XUD_OK,
        "shapes/line-stats-after-append", "and the counters answer after it");
    xud_check((vertexCount == (XUD_SHAPES_SPHERE_VERTEX_COUNT + XUD_SHAPES_LINE_VERTEX_COUNT))
            && (indexCount == (XUD_SHAPES_SPHERE_INDEX_COUNT + XUD_SHAPES_LINE_INDEX_COUNT)),
        "shapes/line-costs-two-and-two",
        "19 vertices and 92 indices, so a line costs two of each - the two indices are not decoration, "
        "vkCmdDrawIndexed names no buffer and no topology, so a line with no indices is a line the "
        "frame loop could not draw");

    /* The split back the other way, which is the fact the other half of the axis is a state rather
       than a one-way switch: a triangle after a line closes the LINE's run and starts a TRIANGLE
       one, so the frame's rows alternate with the shapes rather than with the calls.
    */
    result = xud_shapes_append_triangle(&b, v0, v1, v2, color);
    xud_check(result == XUD_OK, "shapes/triangle-after-line-ok", "a triangle appends after the line");
    xud_check(b.currentTopology == XUD_BATCH_TOPOLOGY_TRIANGLES, "shapes/triangle-moved-topology-back",
        "and the append moved the state back, so the axis is a state and not a switch");
    xud_check(b.drawCount == 2u, "shapes/triangle-split-second-row", "and closed the line's run into a second row");
    xud_check((b.draws[1u].topology == XUD_BATCH_TOPOLOGY_LINES) && (b.draws[1u].indexCount == XUD_SHAPES_LINE_INDEX_COUNT),
        "shapes/line-second-row-is-the-line's",
        "and that row is a LINE row over the line's two indices, which is what the LINE_LIST row of the "
        "pipeline list is drawn through");
    xud_check(xud_batch_stats(&b, &vertexCount, &indexCount, &capacity, &drawCount, &stagingBytes) == XUD_OK, "shapes/mixed-stats",
        "and the counters answer after all three shapes");
    xud_check((vertexCount == ((XUD_SHAPES_TRIANGLE_VERTEX_COUNT + XUD_SHAPES_SPHERE_VERTEX_COUNT) + XUD_SHAPES_LINE_VERTEX_COUNT))
            && (indexCount == ((XUD_SHAPES_TRIANGLE_INDEX_COUNT + XUD_SHAPES_SPHERE_INDEX_COUNT) + XUD_SHAPES_LINE_INDEX_COUNT)),
        "shapes/three-shapes-one-batch-counts",
        "22 vertices and 95 indices, which is the append being per shape rather than per row: the row "
        "split moved no count at all");
    xud_check(xud_batch_check_invariant(&b) == XUD_OK, "shapes/mixed-invariant",
        "and the invariant still holds across a row split, which is the half that says the split is a "
        "record rather than a re-allocation");

    /* The refusal half of the atomicity claim, on the axis rather than on the bytes: the batch has
       95 of the 96 index slots live, so a line's two more do not fit - and the refusal has to leave
       the TOPOLOGY state alone as well as the counts, because a refused line that moved the state
       first would split a run and record a row for a shape that was never drawn.
    */
    result = xud_shapes_append_line(&b, v0, v1, color);
    xud_check(result == XUD_OUT_OF_MEMORY, "shapes/line-over-index-headroom-refused",
        "a line that does not fit the index headroom is refused with the batch's own code");
    xud_check(b.currentTopology == XUD_BATCH_TOPOLOGY_TRIANGLES, "shapes/refused-line-left-topology-alone",
        "and the topology state is where it was, so a refused line cannot split a run");
    xud_check(b.drawCount == 2u, "shapes/refused-line-recorded-no-row", "and no row was recorded for it");
    xud_check(xud_batch_stats(&b, &vertexCount, &indexCount, &capacity, &drawCount, &stagingBytes) == XUD_OK,
        "shapes/stats-after-line-refusal", "and the counters are still readable after the refusal");
    xud_check((vertexCount == ((XUD_SHAPES_TRIANGLE_VERTEX_COUNT + XUD_SHAPES_SPHERE_VERTEX_COUNT) + XUD_SHAPES_LINE_VERTEX_COUNT))
            && (indexCount == ((XUD_SHAPES_TRIANGLE_INDEX_COUNT + XUD_SHAPES_SPHERE_INDEX_COUNT) + XUD_SHAPES_LINE_INDEX_COUNT)),
        "shapes/refused-line-wrote-nothing",
        "and the refused line wrote nothing, so the frame still draws its sphere, its line and its triangle");
    xud_check(
        xud_batch_check_invariant(&b) == XUD_OK, "shapes/invariant-after-line-refusal", "and the invariant still holds over the refusal");

    /* The rows the split above allocated are this area's to release: the batch is a stack record
       here rather than one xud_batch_open allocated, and the batch area frees its own rows the same
       way at the end of its pure half.
    */
    free(b.draws);
    b.draws = NULL;
    b.drawCapacity = 0u;
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

/* How many of those frames draw the box rather than the sphere. Two, and the last frame is the
   sphere's: a sphere's 90 indices beside the frame's two triangles' 6 is exactly the initial cap
   acity's 96-slot index headroom, so the sphere and the box cannot share a frame at that capacity
   and the GPU half has to spend one frame on each rather than doubling back on a batch it grew.
   That is the same fact the pure half checks, seen through a real submit and a real present. */
#define XUD_TEST_SHAPES_CUBE_FRAMES 2u

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
    XudVector3 sphereCentre = (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 0.0f };
    XudVector3 lineStart = (XudVector3) { .x = -0.8f, .y = -0.8f, .z = 0.0f };
    XudVector3 lineEnd = (XudVector3) { .x = 0.8f, .y = 0.8f, .z = 0.0f };
    XudColor white = (XudColor) { .r = 255u, .g = 255u, .b = 255u, .a = 255u };

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
           ngles went through. The line beside it is the shape on the OTHER row of the pipeline lis
           t, and the check under it is the row split: the append closes the triangles-and-box run
           into a row and starts the line's own, so this frame's flush replays TWO rows through TWO
           pipelines and the validation layer is installed in this build to say so if it does not. A
           line and a box cannot share a frame at the initial capacity with a sphere - see the note
           on XUD_TEST_SHAPES_CUBE_FRAMES above. */
        if (frame < XUD_TEST_SHAPES_CUBE_FRAMES) {
            result = xud_draw_cube(context, cubeCentre, 0.8f, 0.8f, 0.8f, blue);
            xud_check(result == XUD_OK, "gpu-shapes/cube-draw-ok", "the public box call appends into the context's own batch");
            xud_check(xud_batch_stats(context->renderer->batch, &vertexCount, &indexCount, &capacity, &drawCount, &stagingBytes) == XUD_OK,
                "gpu-shapes/stats-after-cube", "and the counters answer after it");
            xud_check(vertexCount == ((XUD_SHAPES_TRIANGLE_VERTEX_COUNT * 2u) + XUD_SHAPES_CUBE_VERTEX_COUNT),
                "gpu-shapes/cube-costs-24-vertices",
                "30 vertices in the frame, 24 of them the box's, which is the same count the pure half checked against a batch it built "
                "itself");
            xud_check(indexCount == ((XUD_SHAPES_TRIANGLE_INDEX_COUNT * 2u) + XUD_SHAPES_CUBE_INDEX_COUNT),
                "gpu-shapes/cube-costs-36-indices",
                "42 indices, 36 of them the box's - inside the initial capacity's headroom, so no grow was asked for mid-frame");

            result = xud_draw_line_3d(context, lineStart, lineEnd, white);
            xud_check(result == XUD_OK, "gpu-shapes/line-draw-ok", "the public line call appends into the same open frame");
            xud_check(xud_batch_stats(context->renderer->batch, &vertexCount, &indexCount, &capacity, &drawCount, &stagingBytes) == XUD_OK,
                "gpu-shapes/stats-after-line", "and the counters answer after it");
            xud_check(drawCount == 1u, "gpu-shapes/line-split-one-row-mid-frame",
                "one row recorded MID-frame, before the flush: the line's append closed the triangles-and-box "
                "run into it, so this frame's replay is two rows through two pipelines rather than one "
                "row at whichever topology came last - and the validation layer below is what reports "
                "it if the frame bound the wrong row");
            xud_check(
                vertexCount == ((XUD_SHAPES_TRIANGLE_VERTEX_COUNT * 2u) + (XUD_SHAPES_CUBE_VERTEX_COUNT + XUD_SHAPES_LINE_VERTEX_COUNT)),
                "gpu-shapes/line-costs-two-vertices", "32 vertices in the frame, 2 of them the line's, so the row split moved no count");
            xud_check(indexCount == ((XUD_SHAPES_TRIANGLE_INDEX_COUNT * 2u) + (XUD_SHAPES_CUBE_INDEX_COUNT + XUD_SHAPES_LINE_INDEX_COUNT)),
                "gpu-shapes/line-costs-two-indices", "44 indices, 2 of them the line's, inside the initial capacity's headroom");
        } else {
            result = xud_draw_sphere(context, sphereCentre, XUD_TEST_SHAPES_SPHERE_RADIUS, blue);
            xud_check(result == XUD_OK, "gpu-shapes/sphere-draw-ok", "the public sphere call appends into the context's own batch");
            xud_check(xud_batch_stats(context->renderer->batch, &vertexCount, &indexCount, &capacity, &drawCount, &stagingBytes) == XUD_OK,
                "gpu-shapes/stats-after-sphere", "and the counters answer after it");
            xud_check(vertexCount == ((XUD_SHAPES_TRIANGLE_VERTEX_COUNT * 2u) + XUD_SHAPES_SPHERE_VERTEX_COUNT),
                "gpu-shapes/sphere-costs-17-vertices",
                "23 vertices in the frame, 17 of them the sphere's, which is the shared-vertex count the pure half checked against a batch "
                "it "
                "built itself - not the 30 a seam-duplicated sphere would cost");
            xud_check(indexCount == ((XUD_SHAPES_TRIANGLE_INDEX_COUNT * 2u) + XUD_SHAPES_SPHERE_INDEX_COUNT),
                "gpu-shapes/sphere-costs-90-indices",
                "96 indices, 90 of them the sphere's, which is exactly the initial capacity's index headroom - the frame fills the batch's "
                "indices while 41 of its vertices are still free");
            xud_check(indexCount == XUD_BATCH_INDEX_CAPACITY(capacity), "gpu-shapes/sphere-fills-the-index-headroom",
                "and that is exactly the headroom the capacity sizes at 3n/2, so the sphere is the shape that spends it all");

            /* A line refused beside a sphere that filled the headroom, through a real context: the
               refusal is the batch's, and the check is the TOPOLOGY state as well as the counters,
               because a line that moved the state before refusing would have split the frame's run
               into a row for a shape that was never drawn - and this frame is the one the frame
               loop replays, so the row would be a real vkCmdDrawIndexed through the LINE_LIST pipel
               ine over two indices that were never written.
            */
            result = xud_draw_line_3d(context, lineStart, lineEnd, white);
            xud_check(result == XUD_OUT_OF_MEMORY, "gpu-shapes/line-beside-sphere-refused",
                "a line does not fit beside a sphere that filled the index headroom, and the batch's own code says so");
            xud_check(context->renderer->batch->currentTopology == XUD_BATCH_TOPOLOGY_TRIANGLES,
                "gpu-shapes/refused-line-left-topology-alone", "and the topology state is where it was, so the refused line split nothing");
            xud_check(xud_batch_stats(context->renderer->batch, &vertexCount, &indexCount, &capacity, &drawCount, &stagingBytes) == XUD_OK,
                "gpu-shapes/stats-after-line-refusal", "and the counters are still readable after the refusal");
            xud_check((vertexCount == ((XUD_SHAPES_TRIANGLE_VERTEX_COUNT * 2u) + XUD_SHAPES_SPHERE_VERTEX_COUNT))
                    && (indexCount == ((XUD_SHAPES_TRIANGLE_INDEX_COUNT * 2u) + XUD_SHAPES_SPHERE_INDEX_COUNT)),
                "gpu-shapes/refused-line-wrote-nothing",
                "and the refused line wrote nothing, so the frame still draws its two triangles and its sphere");

            /* A box refused beside a full sphere, through a real context: the refusal is the batch's
               and not the test's, and the check is the counters as well as the code, because a shape
               that wrote part of itself before refusing would leave a frame loop drawing half a box
               through a real pipeline. */
            result = xud_draw_cube(context, cubeCentre, 0.8f, 0.8f, 0.8f, blue);
            xud_check(result == XUD_OUT_OF_MEMORY, "gpu-shapes/cube-beside-sphere-refused",
                "a box does not fit beside a sphere that filled the index headroom, and the batch's own code says so");
            xud_check(xud_batch_stats(context->renderer->batch, &vertexCount, &indexCount, &capacity, &drawCount, &stagingBytes) == XUD_OK,
                "gpu-shapes/stats-after-refusal", "and the counters are still readable after the refusal");
            xud_check((vertexCount == ((XUD_SHAPES_TRIANGLE_VERTEX_COUNT * 2u) + XUD_SHAPES_SPHERE_VERTEX_COUNT))
                    && (indexCount == ((XUD_SHAPES_TRIANGLE_INDEX_COUNT * 2u) + XUD_SHAPES_SPHERE_INDEX_COUNT)),
                "gpu-shapes/refusal-wrote-nothing",
                "and the refused box wrote nothing, so the frame still draws its two triangles and its sphere");
        }

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
