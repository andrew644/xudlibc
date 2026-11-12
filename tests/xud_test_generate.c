/***********************************************************************************************
 *
 *   xud_test_generate.c - the generation rung's two calls, over the eight solids
 *
 *   Owns : the tests of xud_gen_mesh_count() and xud_gen_mesh() - the counts each kind answers, the
 *          refusals both halves share, and what every solid it cuts must therefore be: indices that
 *          name a vertex, normals of unit length, triangles wound so their cross product points
 *          where the normal points, a bounding box the solid cannot escape, and a description the
 *          mesh rung will accept. Nothing else: the cast that walks these meshes is the collision
 *          area's, and the upload that turns one into a device buffer is the mesh area's.
 *   Layer: tests, above the generation rung, and no GPU half - the rung reads no context, so every
 *          fact about a solid is answerable with no driver present, which is the point of cutting
 *          geometry in a rung of its own.
 *
 *   What the expected values are. The counts are computed by hand from each solid's construction -
 *   a sphere of 8 slices and 6 stacks is 7 rows of 9 columns, 63 vertices, because the seam column
 *   is a real second vertex - never read back out of the implementation. The extents are the ones
 *   the shape's own numbers state: a cube of 2 x 1 x 2 fills -1..1 on x and z and -0.5..0.5 on y.
 *   The invariants that hold for ALL eight kinds (in-range indices, unit normals, winding against
 *   the normal, an accepted description) run over all eight rather than being spot-checked on one.
 *
 *   Deterministic: fixed shapes, no time, no randomness, no filesystem. The trigonometry is the
 *   same libm the rung calls, so the comparisons against a value that crossed a sinf are against
 *   the area's tolerance and the ones that did not are exact.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include <math.h> /* Required for: NAN, INFINITY, sqrtf, the normal's length */
#include <stddef.h> /* Required for: NULL */

#include "xud.h" /* Required for: XudResult, XudMeshData, XudMeshShape, XudMeshCounts, XudMeshBuffers, the kind axis, the two calls, and a mesh's bounds */
#include "xud_mesh.h" /* Required for: xud_mesh_check, the mesh rung's own acceptance of what was cut */
#include "xud_test_harness.h"

#define XUD_TEST_GENERATE_TOLERANCE                                                                                                        \
    1e-4f /* The reach of one sinf or cosf: a coordinate that crossed a trigonometric function                                              \
            is read against this, and one that did not is read exactly.                                                                     \
     */

#define XUD_TEST_GENERATE_VERTICES 192u /* The largest solid the area cuts, in vertices: the knot at its own sampling */
#define XUD_TEST_GENERATE_INDICES 1152u /* and in indices */

static float xudTestPosition[XUD_TEST_GENERATE_VERTICES * 3u];
static float xudTestTexcoord[XUD_TEST_GENERATE_VERTICES * 2u];
static float xudTestNormal[XUD_TEST_GENERATE_VERTICES * 3u];
static uint32_t xudTestIndex[XUD_TEST_GENERATE_INDICES];

/* The buffers every solid is cut into - the same block reused, which is what makes the "refused
   call writes nothing" checks mean something: the block still holds the PREVIOUS solid's bytes.
*/
static const XudMeshBuffers xudTestBuffers
    = { .pos = xudTestPosition, .texcoords = xudTestTexcoord, .normal = xudTestNormal, .index = xudTestIndex };

/* The samples every heightmap check reads: a tilted plane, 0.5x + 0.25z over the extent the shared
   shape spans (2 wide by 2 long), sampled on 5 columns by 3 rows so that every sample, every slope
   and every normal this area states is an exact binary number rather than one the tolerance has to
   forgive. Row 0 is the +z edge, the way a plane's rows run.
*/
static const float xudTestHeights[15]
    = { -0.25f, 0.0f, 0.25f, 0.5f, 0.75f, -0.5f, -0.25f, 0.0f, 0.25f, 0.5f, -0.75f, -0.5f, -0.25f, 0.0f, 0.25f };

/* A writable copy of the grid, for the refusals that poison one sample: the const grid above is what
   every geometry check reads, and it must stay readable after the poisoning.
*/
static float xudTestHeightsPoison[15];

static XudMeshShape xudTestShape(uint32_t kind) {
    return (XudMeshShape) { .kind = kind,
        .radius = 1.0f,
        .tubeRadius = 0.25f,
        .width = 2.0f,
        .height = 1.0f,
        .length = 2.0f,
        .slices = 8u,
        .stacks = 6u,
        .heights = xudTestHeights,
        .heightColumns = 5u,
        .heightRows = 3u };
}

static uint32_t xudTestNear(float value, float expected) {
    float difference = value - expected;

    if (difference < 0.0f) {
        difference = -difference;
    }

    return (difference <= XUD_TEST_GENERATE_TOLERANCE) ? 1u : 0u;
}

static float xudTestLength(const float* vector) {
    return sqrtf((vector[0] * vector[0]) + (vector[1] * vector[1]) + (vector[2] * vector[2]));
}

/*-----------------------------------------------------------------------------------------------
 * The refusals, which both halves share
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_generate_refusals(void) {
    XudMeshCounts counts;
    XudMeshData mesh;
    XudMeshShape shape = xudTestShape(XUD_MESH_KIND_SPHERE);
    XudMeshShape kindless = xudTestShape(XUD_MESH_KIND_COUNT);
    const XudMeshBuffers empty = { .pos = NULL, .texcoords = NULL, .normal = NULL, .index = NULL };
    const XudMeshBuffers noIndex = { .pos = xudTestPosition, .texcoords = NULL, .normal = NULL, .index = NULL };

    xud_check(xud_gen_mesh_count(NULL, &shape) == XUD_INVALID_ARG, "generate/count-null-out", "the count half refuses a NULL answer");
    xud_check(xud_gen_mesh_count(&counts, NULL) == XUD_INVALID_ARG, "generate/count-null-shape", "and a shape that is not there");
    xud_check(xud_gen_mesh(NULL, &shape, &xudTestBuffers) == XUD_INVALID_ARG, "generate/fill-null-out", "and the fill half does");
    xud_check(xud_gen_mesh(&mesh, NULL, &xudTestBuffers) == XUD_INVALID_ARG, "generate/fill-null-shape", "and so on the shape");
    xud_check(xud_gen_mesh(&mesh, &shape, NULL) == XUD_INVALID_ARG, "generate/fill-null-buffers", "and on no buffers at all");
    xud_check(xud_gen_mesh(&mesh, &shape, &empty) == XUD_INVALID_ARG, "generate/fill-no-position",
        "a mesh with no position stream has nothing to draw, and no index stream has nothing to name");
    xud_check(xud_gen_mesh(&mesh, &shape, &noIndex) == XUD_INVALID_ARG, "generate/fill-no-index",
        "the index array is the other stream with no default");

    xud_check(xud_gen_mesh_count(&counts, &kindless) == XUD_INVALID_ARG, "generate/count-unknown-kind",
        "a kind past the axis is refused rather than cut as whatever the last writer builds");
    xud_check(xud_gen_mesh(&mesh, &kindless, &xudTestBuffers) == XUD_INVALID_ARG, "generate/fill-unknown-kind",
        "and the fill refuses it before it dispatches, not after");

    shape.radius = 0.0f;
    xud_check(xud_gen_mesh_count(&counts, &shape) == XUD_INVALID_ARG, "generate/count-zero-radius",
        "a sphere of no radius has no surface to point a normal at");
    shape.radius = -1.0f;
    xud_check(xud_gen_mesh_count(&counts, &shape) == XUD_INVALID_ARG, "generate/count-negative-radius", "and a negative one is the same");
    shape.radius = NAN;
    xud_check(xud_gen_mesh_count(&counts, &shape) == XUD_INVALID_ARG, "generate/count-nan-radius",
        "and a NaN answers false against the floor rather than passing it quietly");
    shape.radius = INFINITY;
    xud_check(xud_gen_mesh_count(&counts, &shape) == XUD_INVALID_ARG, "generate/count-infinite-radius",
        "and an infinity is refused where the floor would have let it through");
    shape.radius = 1.0f;

    shape.slices = 2u;
    xud_check(xud_gen_mesh_count(&counts, &shape) == XUD_INVALID_ARG, "generate/count-two-slices", "two longitudes are a line, not a ring");
    shape.slices = 8u;
    shape.stacks = 1u;
    xud_check(xud_gen_mesh_count(&counts, &shape) == XUD_INVALID_ARG, "generate/count-one-stack",
        "and one stack of a sphere puts both poles on one ring, which is a ring of one point");
    shape.stacks = 6u;

    shape = xudTestShape(XUD_MESH_KIND_CUBE);
    shape.width = 0.0f;
    xud_check(xud_gen_mesh_count(&counts, &shape) == XUD_INVALID_ARG, "generate/count-flat-cube",
        "a cube with no width is two faces on one plane, which is not a box");
    shape.length = -2.0f;
    xud_check(xud_gen_mesh_count(&counts, &shape) == XUD_INVALID_ARG, "generate/count-negative-length", "and a negative extent is worse");

    shape = xudTestShape(XUD_MESH_KIND_PLANE);
    shape.slices = 0u;
    xud_check(xud_gen_mesh_count(&counts, &shape) == XUD_INVALID_ARG, "generate/count-empty-plane", "a plane of no columns is nothing");

    shape = xudTestShape(XUD_MESH_KIND_TORUS);
    shape.tubeRadius = 0.0f;
    xud_check(xud_gen_mesh_count(&counts, &shape) == XUD_INVALID_ARG, "generate/count-flat-torus",
        "a torus whose tube has no radius is the circle it was swept along");

    shape = xudTestShape(XUD_MESH_KIND_POLY);
    shape.slices = 2u;
    xud_check(xud_gen_mesh_count(&counts, &shape) == XUD_INVALID_ARG, "generate/count-two-sided-poly", "a polygon of two sides is a line");

    shape = xudTestShape(XUD_MESH_KIND_HEIGHTMAP);
    shape.heights = NULL;
    xud_check(xud_gen_mesh_count(&counts, &shape) == XUD_INVALID_ARG, "generate/count-heightmap-no-samples",
        "a heightmap with no samples has no surface, and no array to read a count out of");
    shape = xudTestShape(XUD_MESH_KIND_HEIGHTMAP);
    shape.heightColumns = 1u;
    xud_check(xud_gen_mesh_count(&counts, &shape) == XUD_INVALID_ARG, "generate/count-heightmap-one-column",
        "one column of samples is a line, which has no triangles to name");
    shape = xudTestShape(XUD_MESH_KIND_HEIGHTMAP);
    shape.heightRows = 0u;
    xud_check(xud_gen_mesh_count(&counts, &shape) == XUD_INVALID_ARG, "generate/count-heightmap-no-rows", "and no rows is nothing at all");
    shape = xudTestShape(XUD_MESH_KIND_HEIGHTMAP);
    shape.heightColumns = 65535u;
    shape.heightRows = 65535u;
    xud_check(xud_gen_mesh_count(&counts, &shape) == XUD_INVALID_ARG, "generate/count-heightmap-past-ceiling",
        "a sample grid past the mesh rung's vertex ceiling is refused before its samples are walked, rather than read");
    shape = xudTestShape(XUD_MESH_KIND_HEIGHTMAP);
    xudTestHeightsPoison[7] = NAN; /* the rest of the grid stays zero: finite, and not the point */
    shape.heights = xudTestHeightsPoison;
    xud_check(xud_gen_mesh_count(&counts, &shape) == XUD_INVALID_ARG, "generate/count-heightmap-nan-sample",
        "one NaN sample is refused: it is a vertex at NaN and six NaN normals around it");
    xud_check(xud_gen_mesh(&mesh, &shape, &xudTestBuffers) == XUD_INVALID_ARG, "generate/fill-heightmap-nan-sample",
        "and the fill half runs the same walk, so it cannot write the NaN the count refused");

    counts.vertexCount = 1234u;
    counts.indexCount = 5678u;
    shape = xudTestShape(XUD_MESH_KIND_CUBE);
    shape.height = NAN;
    xud_check(xud_gen_mesh_count(&counts, &shape) == XUD_INVALID_ARG, "generate/refusal-leaves-count",
        "a refused count call leaves the caller's two numbers as it found them");
    xud_check((counts.vertexCount == 1234u) && (counts.indexCount == 5678u), "generate/refusal-count-untouched",
        "which is to say it writes nothing it is refusing to answer");
}

/*-----------------------------------------------------------------------------------------------
 * The eight solids, and what every one of them must be
 *-----------------------------------------------------------------------------------------------*/

/* Every solid the area cuts is the default shape with its own kind, so the counts below are one
   hand-computed pair apiece: radius 1, tube 0.25, 2 x 1 x 2, 8 slices, 6 stacks.
*/
static const uint32_t xudTestKind[XUD_MESH_KIND_COUNT]
    = { XUD_MESH_KIND_PLANE, XUD_MESH_KIND_CUBE, XUD_MESH_KIND_SPHERE, XUD_MESH_KIND_HEMISPHERE, XUD_MESH_KIND_CYLINDER,
          XUD_MESH_KIND_TORUS, XUD_MESH_KIND_KNOT, XUD_MESH_KIND_POLY, XUD_MESH_KIND_HEIGHTMAP };
static const uint32_t xudTestVertices[XUD_MESH_KIND_COUNT] = { 63u, 24u, 63u, 63u, 38u, 63u, 48u, 10u, 15u };
static const uint32_t xudTestIndices[XUD_MESH_KIND_COUNT] = { 288u, 36u, 240u, 264u, 96u, 288u, 288u, 24u, 48u };

static void xud_test_generate_counts(void) {
    XudMeshCounts counts;
    XudMeshData mesh;
    uint32_t kind = 0u;

    for (kind = 0u; kind < XUD_MESH_KIND_COUNT; kind++) {
        XudMeshShape shape = xudTestShape(xudTestKind[kind]);

        xud_check(xud_gen_mesh_count(&counts, &shape) == XUD_OK, "generate/count-ok", "every kind answers its counts");
        xud_check((counts.vertexCount == xudTestVertices[kind]) && (counts.indexCount == xudTestIndices[kind]), "generate/count-table",
            "and the pair is the one its construction states");
        xud_check((counts.indexCount % 3u) == 0u, "generate/count-triangles", "and the index count is a whole number of triangles");

        xud_check(xud_gen_mesh(&mesh, &shape, &xudTestBuffers) == XUD_OK, "generate/fill-ok", "the same shape fills");
        xud_check((mesh.vertexCount == counts.vertexCount) && (mesh.indexCount == counts.indexCount), "generate/fill-agrees-with-count",
            "through the count the other half answered - the fill's writes are bounded by that answer");
        xud_check((mesh.pos == xudTestPosition) && (mesh.index == xudTestIndex) && (mesh.color == NULL), "generate/fill-streams",
            "and the description names the arrays it was handed, with the colour stream the generators never write left off");
    }
}

/* The invariants that hold for every solid: an index that names a vertex, a normal of unit length,
   a triangle whose cross product points where its own normals say out, and a description the mesh
   rung will take. A solid wound the wrong way looks identical in a render with nothing culled, which
   is exactly why it is checked here rather than looked at.
*/
static void xud_test_generate_invariants(void) {
    XudMeshData mesh;
    uint32_t kind = 0u;

    for (kind = 0u; kind < XUD_MESH_KIND_COUNT; kind++) {
        XudMeshShape shape = xudTestShape(xudTestKind[kind]);

        /* The knot at the sampling it is meant to be cut at: its curve turns three times around, so
           eight steps along it put the cross-sections further apart than the tube is wide, and the
           quads between two rows fold flat. The other seven solids take the default sampling.
        */
        if (xudTestKind[kind] == XUD_MESH_KIND_KNOT) {
            shape.slices = 24u;
            shape.stacks = 8u;
        }
        uint32_t triangle = 0u;
        uint32_t vertex = 0u;
        uint32_t inRange = 1u;
        uint32_t unitNormals = 1u;
        uint32_t wound = 1u;
        uint32_t triangles = 0u;

        xud_check(xud_gen_mesh(&mesh, &shape, &xudTestBuffers) == XUD_OK, "generate/invariant-fill", "the solid is cut");
        xud_check(xud_mesh_check(&mesh, &triangles) == XUD_OK, "generate/invariant-mesh-rung",
            "and the mesh rung accepts the description it made, which is the upload gate it must pass next");

        for (vertex = 0u; vertex < mesh.vertexCount; vertex++) {
            if (!xudTestNear(xudTestLength(&xudTestNormal[vertex * 3u]), 1.0f)) {
                unitNormals = 0u;
            }
        }

        for (triangle = 0u; triangle < (mesh.indexCount / 3u); triangle++) {
            const float* a = &xudTestPosition[xudTestIndex[triangle * 3u] * 3u];
            const float* b = &xudTestPosition[xudTestIndex[(triangle * 3u) + 1u] * 3u];
            const float* c = &xudTestPosition[xudTestIndex[(triangle * 3u) + 2u] * 3u];
            const float* na = &xudTestNormal[xudTestIndex[triangle * 3u] * 3u];
            float one[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
            float two[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
            float cross[3]
                = { (one[1] * two[2]) - (one[2] * two[1]), (one[2] * two[0]) - (one[0] * two[2]), (one[0] * two[1]) - (one[1] * two[0]) };
            float facing = (cross[0] * na[0]) + (cross[1] * na[1]) + (cross[2] * na[2]);

            if ((xudTestIndex[triangle * 3u] >= mesh.vertexCount) || (xudTestIndex[(triangle * 3u) + 1u] >= mesh.vertexCount)
                || (xudTestIndex[(triangle * 3u) + 2u] >= mesh.vertexCount)) {
                inRange = 0u;
            }
            if (facing <= 1e-9f) {
                wound = 0u;
            }
        }

        xud_check(inRange == 1u, "generate/invariant-index-range", "no index names a vertex the solid does not have");
        xud_check(unitNormals == 1u, "generate/invariant-unit-normals", "and no normal is a direction of any length but one");
        xud_check(wound == 1u, "generate/invariant-winding",
            "and every triangle is wound counter-clockwise seen from outside, which is the front face the pipelines name");
    }
}

/*-----------------------------------------------------------------------------------------------
 * The extents each kind states, and the streams it may leave out
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_generate_bounds(void) {
    XudMeshData mesh;
    XudMeshShape shape;
    XudVector3 min;
    XudVector3 max;
    uint32_t vertex = 0u;
    uint32_t widest = 0u;

    shape = xudTestShape(XUD_MESH_KIND_CUBE);
    xud_check(xud_gen_mesh(&mesh, &shape, &xudTestBuffers) == XUD_OK, "generate/bounds-cube", "the box is cut");
    xud_check(xud_get_mesh_bounding_box(&min, &max, &mesh) == XUD_OK, "generate/bounds-cube-box", "and its bounds answer");
    xud_check(xudTestNear(min.x, -1.0f) && xudTestNear(max.x, 1.0f), "generate/bounds-cube-x", "so a width of 2 spans -1 to 1");
    xud_check(xudTestNear(min.y, -0.5f) && xudTestNear(max.y, 0.5f), "generate/bounds-cube-y", "a height of 1 half a unit each way");
    xud_check(xudTestNear(min.z, -1.0f) && xudTestNear(max.z, 1.0f), "generate/bounds-cube-z", "and a length of 2 the width again");

    shape = xudTestShape(XUD_MESH_KIND_SPHERE);
    xud_check(xud_gen_mesh(&mesh, &shape, &xudTestBuffers) == XUD_OK, "generate/bounds-sphere", "the sphere is cut");
    xud_check(xud_get_mesh_bounding_box(&min, &max, &mesh) == XUD_OK, "generate/bounds-sphere-box", "and its bounds answer");
    xud_check(xudTestNear(min.x, -1.0f) && xudTestNear(max.x, 1.0f) && xudTestNear(min.y, -1.0f) && xudTestNear(max.y, 1.0f),
        "generate/bounds-sphere-reach", "and a radius of 1 reaches 1 along every axis, the poles included");

    shape = xudTestShape(XUD_MESH_KIND_PLANE);
    xud_check(xud_gen_mesh(&mesh, &shape, &xudTestBuffers) == XUD_OK, "generate/bounds-plane", "the floor is cut");
    xud_check(xud_get_mesh_bounding_box(&min, &max, &mesh) == XUD_OK, "generate/bounds-plane-box", "and its bounds answer");
    xud_check((min.y == 0.0f) && (max.y == 0.0f), "generate/bounds-plane-flat", "and it is flat: no vertex of a plane leaves y = 0");

    shape = xudTestShape(XUD_MESH_KIND_HEMISPHERE);
    xud_check(xud_gen_mesh(&mesh, &shape, &xudTestBuffers) == XUD_OK, "generate/bounds-hemisphere", "the dome is cut");
    xud_check(xud_get_mesh_bounding_box(&min, &max, &mesh) == XUD_OK, "generate/bounds-hemisphere-box", "and its bounds answer");
    xud_check((min.y == 0.0f) && xudTestNear(max.y, 1.0f), "generate/bounds-hemisphere-half",
        "and it stands on the ground plane its own row ends on, reaching the radius at the crown");

    shape = xudTestShape(XUD_MESH_KIND_CYLINDER);
    xud_check(xud_gen_mesh(&mesh, &shape, &xudTestBuffers) == XUD_OK, "generate/bounds-cylinder", "the drum is cut");
    xud_check(xud_get_mesh_bounding_box(&min, &max, &mesh) == XUD_OK, "generate/bounds-cylinder-box", "and its bounds answer");
    xud_check(xudTestNear(min.y, -0.5f) && xudTestNear(max.y, 0.5f), "generate/bounds-cylinder-height",
        "so a height of 1 is half a unit above and below the origin, the caps included");

    shape = xudTestShape(XUD_MESH_KIND_POLY);
    xud_check(xud_gen_mesh(&mesh, &shape, &xudTestBuffers) == XUD_OK, "generate/bounds-poly", "the prism is cut");
    widest = 1u;
    for (vertex = 1u; vertex < mesh.vertexCount; vertex++) {
        const float* point = &xudTestPosition[vertex * 3u];

        if (!xudTestNear(sqrtf((point[0] * point[0]) + (point[2] * point[2])), 1.0f)) {
            widest = 0u;
        }
    }
    xud_check(widest == 1u, "generate/bounds-poly-radius", "and every corner of a polygon of radius 1 is exactly 1 from the axis");

    shape = xudTestShape(XUD_MESH_KIND_TORUS);
    xud_check(xud_gen_mesh(&mesh, &shape, &xudTestBuffers) == XUD_OK, "generate/bounds-torus", "the ring is cut");
    xud_check(xud_get_mesh_bounding_box(&min, &max, &mesh) == XUD_OK, "generate/bounds-torus-box", "and its bounds answer");
    xud_check(xudTestNear(max.x, 1.25f) && (max.y <= 0.25f + XUD_TEST_GENERATE_TOLERANCE)
            && (min.y >= -(0.25f + XUD_TEST_GENERATE_TOLERANCE)) && (max.y > 0.2f),
        "generate/bounds-torus-reach", "so the reach is the circle's radius plus the tube's, and the tube stays inside its own diameter");

    shape = xudTestShape(XUD_MESH_KIND_KNOT);
    xud_check(xud_gen_mesh(&mesh, &shape, &xudTestBuffers) == XUD_OK, "generate/bounds-knot", "the trefoil is cut");
    xud_check(xud_get_mesh_bounding_box(&min, &max, &mesh) == XUD_OK, "generate/bounds-knot-box", "and its bounds answer");
    xud_check((max.x <= 1.25f + XUD_TEST_GENERATE_TOLERANCE) && (max.y <= 1.25f + XUD_TEST_GENERATE_TOLERANCE)
            && (max.z <= 1.25f + XUD_TEST_GENERATE_TOLERANCE) && (min.x >= -(1.25f + XUD_TEST_GENERATE_TOLERANCE)),
        "generate/bounds-knot-reach", "and nothing of it escapes the radius plus the tube's radius");
}

static void xud_test_generate_streams(void) {
    XudMeshData mesh;
    XudMeshShape shape = xudTestShape(XUD_MESH_KIND_SPHERE);
    const XudMeshBuffers positionsOnly = { .pos = xudTestPosition, .texcoords = NULL, .normal = NULL, .index = xudTestIndex };
    uint32_t triangles = 0u;

    mesh.vertexCount = 999u;
    mesh.pos = NULL;
    xud_check(xud_gen_mesh(&mesh, &shape, &positionsOnly) == XUD_OK, "generate/positions-only",
        "a caller that hands over only positions and indices gets a mesh, not a refusal");
    xud_check((mesh.texcoords == NULL) && (mesh.normal == NULL), "generate/positions-only-streams",
        "and the streams it did not offer are named as absent rather than as arrays of zeros");
    xud_check(xud_mesh_check(&mesh, &triangles) == XUD_OK, "generate/positions-only-check",
        "and the mesh rung uploads that as a vertex format with no normals, which is a light nothing, not a breakage");

    shape.radius = NAN;
    mesh.vertexCount = 777u;
    mesh.indexCount = 888u;
    xud_check(xud_gen_mesh(&mesh, &shape, &xudTestBuffers) == XUD_INVALID_ARG, "generate/refusal-fail", "a refused fill is refused");
    xud_check((mesh.vertexCount == 777u) && (mesh.indexCount == 888u) && (mesh.pos == xudTestPosition), "generate/refusal-fail-untouched",
        "and the caller's description is left exactly as it was, so a failed call cannot half-install a mesh");
}

/* The heightmap's own geometry: what one caller-supplied sample grid must be, vertex by vertex. The
   grid is a tilted plane, so every coordinate, texture coordinate and normal here is a number stated
   by the plane's equation rather than one read back out of the writer.
*/
static void xud_test_generate_heightmap(void) {
    XudMeshData mesh;
    XudMeshShape shape = xudTestShape(XUD_MESH_KIND_HEIGHTMAP);
    XudVector3 min;
    XudVector3 max;
    const float inverse = 1.0f / sqrtf(1.3125f); /* (-0.5, 1, -0.25) normalised: the tilt's own normal */
    uint32_t vertex = 0u;
    uint32_t rows = 1u;
    uint32_t triangles = 0u;

    xud_check(xud_gen_mesh(&mesh, &shape, &xudTestBuffers) == XUD_OK, "generate/heightmap-cut", "the tilted grid is cut");
    xud_check((mesh.vertexCount == 15u) && (mesh.indexCount == 48u), "generate/heightmap-counts",
        "5 columns by 3 rows is 15 vertices and 8 quads, one vertex per sample and none duplicated");
    xud_check(xud_mesh_check(&mesh, &triangles) == XUD_OK, "generate/heightmap-check", "and the mesh rung uploads the description");

    for (vertex = 0u; vertex < 15u; vertex++) {
        const uint32_t column = vertex % 5u;
        const uint32_t row = vertex / 5u;
        const float x = (((float)column / 4.0f) - 0.5f) * 2.0f;
        const float z = (0.5f - ((float)row / 2.0f)) * 2.0f;
        const float* point = &xudTestPosition[vertex * 3u];
        const float* texcoord = &xudTestTexcoord[vertex * 2u];
        const float* normal = &xudTestNormal[vertex * 3u];

        if (!xudTestNear(point[0], x) || !xudTestNear(point[1], xudTestHeights[vertex]) || !xudTestNear(point[2], z)) {
            rows = 0u;
        }
        if (!xudTestNear(texcoord[0], (float)column / 4.0f) || !xudTestNear(texcoord[1], (float)row / 2.0f)) {
            rows = 0u;
        }
        if (!xudTestNear(normal[0], -0.5f * inverse) || !xudTestNear(normal[1], inverse) || !xudTestNear(normal[2], -0.25f * inverse)) {
            rows = 0u;
        }
    }
    xud_check(rows == 1u, "generate/heightmap-vertices",
        "every vertex is its sample's height at its grid corner, its texture coordinate the grid position, and its normal the tilt's");

    /* The samples are world units: the shape's `height`, which no other kind uses for this, must not
       scale them, or a caller's measured terrain would be resized by a field it never filled.
    */
    shape.height = 7.0f;
    xud_check(
        xud_gen_mesh(&mesh, &shape, &xudTestBuffers) == XUD_OK, "generate/heightmap-height-setup", "the same grid, a taller height field");
    rows = 1u;
    for (vertex = 0u; vertex < 15u; vertex++) {
        if (!xudTestNear(xudTestPosition[(vertex * 3u) + 1u], xudTestHeights[vertex])) {
            rows = 0u;
        }
    }
    xud_check(rows == 1u, "generate/heightmap-height-unused", "and the y of every vertex is still the sample the caller wrote");

    shape.height = 1.0f;
    xud_check(xud_gen_mesh(&mesh, &shape, &xudTestBuffers) == XUD_OK, "generate/heightmap-bounds-setup", "and the bounds of the grid");
    xud_check(xud_get_mesh_bounding_box(&min, &max, &mesh) == XUD_OK, "generate/heightmap-bounds", "answer");
    xud_check(xudTestNear(min.y, -0.75f) && xudTestNear(max.y, 0.75f), "generate/heightmap-bounds-y",
        "the tilt reaches its lowest sample at (-1, -1) and its highest at (1, 1)");
    xud_check(xudTestNear(min.x, -1.0f) && xudTestNear(max.x, 1.0f) && xudTestNear(min.z, -1.0f) && xudTestNear(max.z, 1.0f),
        "generate/heightmap-bounds-plan", "and the grid still spans the width and length the shape states");
}

void xud_test_generate(void) {
    xud_test_generate_refusals();
    xud_test_generate_counts();
    xud_test_generate_invariants();
    xud_test_generate_bounds();
    xud_test_generate_heightmap();
    xud_test_generate_streams();
}
