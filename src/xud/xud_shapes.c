/***********************************************************************************************
 *
 *   xud_shapes.c - the shapes rung and the public draw call above it
 *
 *   Owns : xud_shapes_append_triangle(), xud_shapes_append_cube(), xud_shapes_append_sphere(),
 *          xud_shapes_append_circle(), xud_shapes_append_ellipse(),
 *          xud_shapes_append_line(), xud_shapes_append_cube_wires(),
 *          xud_shapes_append_sphere_wires() and xud_shapes_append_glyph_quad(), the geometry the
 *          public draw calls name, the fifteen public calls above them (xud_draw_triangle_3d(),
 *          xud_draw_cube(), xud_draw_sphere(), xud_draw_line_3d(), xud_draw_point_3d(),
 *          xud_draw_cube_wires(), xud_draw_sphere_wires(), xud_draw_rectangle(), xud_draw_line(),
 *          xud_draw_pixel(), xud_draw_rectangle_lines(), xud_draw_triangle(),
 *          xud_draw_triangle_lines(), xud_draw_circle() and xud_draw_ellipse()) and the one guard those
 *          public calls share. Nothing else - no driver call, no loader row, no Vulkan or GLFW
 *          symbol the linker could see. The trigonometry the sphere reads is <math.h>'s, which is
 *          host arithmetic on the caller's floats rather than a driver row, and it is the only
 *          system header this file takes. The whole file is host work on the batch's own counters
 *          and the batch's own arena, which is what keeps it testable with no device present and
 *          keeps a machine with no Vulkan loader answering the same refusals as one with a loader
 *          and no window open.
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
#include <math.h> /* Required for: sinf/cosf, the sphere's latitude and longitude, and fabsf, the bounding box's absolute size - host arithmetic on the caller's floats, the one system header this file takes */

/*-----------------------------------------------------------------------------------------------
 * What a refused shape tells the batch
 *-----------------------------------------------------------------------------------------------*/

/* A refusal is not the end of the story: the frame boundary above can grow the batch for the next
   frame, and the only way it knows a frame wanted more is the ask the refusal records (see the
   capacity-ask note in xud_batch.h - an atomic refusal leaves the live counts BELOW capacity, so
   nothing else in the batch says a frame overflowed). The ask is capped at the batch's own bound
   rather than recorded as the caller's raw request: past the bound there is nothing to grow to,
   and a tessellation the caller invented reaches back through a uint32 - the sphere's need is a
   uint64 product of the caller's rings and slices for exactly that reason. */
static void xud_shapes_note_refused(XudBatch* batch, uint64_t requestedVertices, uint64_t requestedIndices) {
    const uint64_t vertexBound = (uint64_t)XUD_BATCH_MAX_VERTICES;
    const uint64_t indexBound = (uint64_t)XUD_BATCH_INDEX_CAPACITY(XUD_BATCH_MAX_VERTICES);

    (void)xud_batch_note_refused(batch, (uint32_t)((requestedVertices > vertexBound) ? vertexBound : requestedVertices),
        (uint32_t)((requestedIndices > indexBound) ? indexBound : requestedIndices));
}

/*-----------------------------------------------------------------------------------------------
 * The box's face tables: the six outward normals and the 24 corner signs
 *-----------------------------------------------------------------------------------------------*/

/* The cube's six faces as DATA rather than as twelve triangles of code: one row per face, the
   face's OUTWARD normal first and then its four corner SIGNS in counter-clockwise order seen from
   outside - the same counter-clockwise raylib's DrawTriangle3D documents (raylib.h 1588) and the
   pipeline's XUD_PIPELINE_FRONT_FACE names. The tests area re-derives all of it from the cross
   product of a face's two triangles against that face's normal rather than trusting this table
   being written right.

   The corners are signs, not coordinates: a vertex is the centre plus the sign times that axis's
   half-extent, so nothing about the caller's centre or extents is in here and the table is 6 rows
   of 4 corner triples of -1/+1 - and every corner of a box is at +/-half along ALL THREE axes, so
   a face's row carries that face's own normal sign on the normal's axis rather than a zero there,
   which is what keeps the 24 corners the 8 corners of one box rather than 12 points inside it. The
   winding rule the order obeys is the one this comment states: for a face whose tangent axes are u
   and v, the corners are -u-v, +u-v, +u+v, -u+v in that order and u x v IS the outward normal exac
    tly, not merely parallel to it, which is what makes that order counter-clockwise when the face
   is viewed from the side the normal points to and what lets one rule cover all six faces instead
   of a per-face exception - the u and v named beside each row below are the SIGNED choices that ma
   ke that equality hold, which is why the six rows differ in corner order rather than in rule. */
static const float XUD_SHAPES_CUBE_NORMAL[XUD_SHAPES_CUBE_FACE_COUNT][3] = {
    { 1.0f, 0.0f, 0.0f }, /* +X: u = +Y, v = +Z, u x v = +X */
    { -1.0f, 0.0f, 0.0f }, /* -X: u = +Z, v = +Y, u x v = -X */
    { 0.0f, 1.0f, 0.0f }, /* +Y: u = +Z, v = +X, u x v = +Y */
    { 0.0f, -1.0f, 0.0f }, /* -Y: u = +X, v = +Z, u x v = -Y */
    { 0.0f, 0.0f, 1.0f }, /* +Z: u = +X, v = +Y, u x v = +Z */
    { 0.0f, 0.0f, -1.0f }, /* -Z: u = +Y, v = +X, u x v = -Z */
};

static const float XUD_SHAPES_CUBE_CORNER[XUD_SHAPES_CUBE_FACE_COUNT][XUD_SHAPES_CUBE_FACE_VERTEX_COUNT][3] = {
    /* +X: u = +Y, v = +Z */
    { { 1.0f, -1.0f, -1.0f }, { 1.0f, 1.0f, -1.0f }, { 1.0f, 1.0f, 1.0f }, { 1.0f, -1.0f, 1.0f } },
    /* -X: u = +Z, v = +Y */
    { { -1.0f, -1.0f, -1.0f }, { -1.0f, -1.0f, 1.0f }, { -1.0f, 1.0f, 1.0f }, { -1.0f, 1.0f, -1.0f } },
    /* +Y: u = +Z, v = +X */
    { { -1.0f, 1.0f, -1.0f }, { -1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, -1.0f } },
    /* -Y: u = +X, v = +Z */
    { { -1.0f, -1.0f, -1.0f }, { 1.0f, -1.0f, -1.0f }, { 1.0f, -1.0f, 1.0f }, { -1.0f, -1.0f, 1.0f } },
    /* +Z: u = +X, v = +Y */
    { { -1.0f, -1.0f, 1.0f }, { 1.0f, -1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f }, { -1.0f, 1.0f, 1.0f } },
    /* -Z: u = +Y, v = +X */
    { { -1.0f, -1.0f, -1.0f }, { -1.0f, 1.0f, -1.0f }, { 1.0f, 1.0f, -1.0f }, { 1.0f, -1.0f, -1.0f } },
};

/* The two triangles of one quad, as corner indices into the four corners above: (0,1,2) and (0,2,
   3). Both are counter-clockwise in the same order the corners are, so the face is closed by two
   triangles that share the 0-to-2 diagonal and nothing about the face's shape is in here. */
static const uint32_t XUD_SHAPES_CUBE_TRIANGLE_CORNER[XUD_SHAPES_CUBE_FACE_INDEX_COUNT] = { 0u, 1u, 2u, 0u, 2u, 3u };

/*-----------------------------------------------------------------------------------------------
 * The sphere's vertex row: one unit direction, one radius, one centre
 *-----------------------------------------------------------------------------------------------*/

/* The sphere's vertices are all built the same way - the centre plus the radius times a UNIT direc
 *   tion - so the one place that spelling lives is one function rather than three copies of it in
 *   three loops. The direction is passed in rather than computed here because the three loops get
 *   it three ways: the two poles get it as an axis (exactly, so the poles are exact rather than
 *   sin(PI)-rounding), the rows between get it from a latitude and a longitude. The NORMAL is the
 *   same direction, unmodified: a sphere's normal is radial by definition, so the direction that
 *   places the vertex is the direction that shades it, and no division by the radius is needed to
 *   unitise it - which is why a zero radius is refused above rather than divided by here.
 */
static XudResult xud_shapes_sphere_vertex(XudBatch* batch, XudVector3 center, float radius, const float dir[3], XudColor color) {
    float pos[3];
    float uv[2];
    uint8_t rgba[4];

    uv[0] = XUD_SHAPES_UV_S;
    uv[1] = XUD_SHAPES_UV_T;
    rgba[0] = color.r;
    rgba[1] = color.g;
    rgba[2] = color.b;
    rgba[3] = color.a;

    pos[0] = center.x + (radius * dir[0]);
    pos[1] = center.y + (radius * dir[1]);
    pos[2] = center.z + (radius * dir[2]);

    return xud_batch_append_vertex(batch, pos, uv, rgba, dir);
}

static_assert(sizeof(XUD_SHAPES_CUBE_NORMAL) / (sizeof(XUD_SHAPES_CUBE_NORMAL[0][0]) * 3u) == XUD_SHAPES_CUBE_FACE_COUNT,
    "xud_shapes.c: the normal table has one row per face of the box");
static_assert(sizeof(XUD_SHAPES_CUBE_CORNER) / (sizeof(XUD_SHAPES_CUBE_CORNER[0][0][0]) * XUD_SHAPES_CUBE_FACE_VERTEX_COUNT * 3u)
        == XUD_SHAPES_CUBE_FACE_COUNT,
    "xud_shapes.c: the corner table has one row of four corners per face, which is the 24 the append's fit check counts");
static_assert(sizeof(XUD_SHAPES_CUBE_TRIANGLE_CORNER) / sizeof(XUD_SHAPES_CUBE_TRIANGLE_CORNER[0]) == XUD_SHAPES_CUBE_FACE_INDEX_COUNT,
    "xud_shapes.c: two triangles per face is six corner references per face, which is the 36 the append's index bound is written against");

/*-----------------------------------------------------------------------------------------------
 * The wire cube's tables: the 8 corners of one box and the 12 edges of the cube graph
 *-----------------------------------------------------------------------------------------------*/

/* The corners are numbered by their BIT PATTERN: corner i sits at x = +1 when bit 0 of i is set, at
   y = +1 when bit 1 is and at z = +1 when bit 2 is. The numbering is load-bearing rather than
   cosmetic - it is what makes the edge table below the graph rather than a list of twelve pairs to
   keep honest, because an edge is exactly a pair of corners that differ in one bit, and the twelve
   rows below are the four edges along x (bit 0 differing), the four along y (bit 1) and the four
   along z (bit 2). Every corner appears in exactly three rows and every edge appears once, which is
   the reason a wire cube is 12 edges rather than the 18 a per-face outline would trace with each
   shared edge drawn twice. The signs are the same -1/+1 spelling the filled box's corner table
   above uses, so a wire cube and a filled cube at the same centre and extents have the same eight
   corners in space - the wire is the outline OF that box rather than a second box. */
static const float XUD_SHAPES_WIRE_CUBE_CORNER[XUD_SHAPES_WIRE_CUBE_CORNER_COUNT][3] = {
    { -1.0f, -1.0f, -1.0f }, /* 0b000 */
    { 1.0f, -1.0f, -1.0f }, /* 0b001 */
    { -1.0f, 1.0f, -1.0f }, /* 0b010 */
    { 1.0f, 1.0f, -1.0f }, /* 0b011 */
    { -1.0f, -1.0f, 1.0f }, /* 0b100 */
    { 1.0f, -1.0f, 1.0f }, /* 0b101 */
    { -1.0f, 1.0f, 1.0f }, /* 0b110 */
    { 1.0f, 1.0f, 1.0f }, /* 0b111 */
};

/* The twelve edges, in the three groups of four the bit numbering above makes: the four along x,
   the four along y, the four along z. Each pair is ordered low-index-first, and the order is NOT
   load-bearing here the way a face's winding is above - a line has no front face, so LINE_LIST
   reads a pair as a segment rather than as an oriented one, and the pipeline's cull mode is NONE. */
static const uint32_t XUD_SHAPES_WIRE_CUBE_EDGE[XUD_SHAPES_WIRE_CUBE_EDGE_COUNT][2] = {
    { 0u, 1u },
    { 2u, 3u },
    { 4u, 5u },
    { 6u, 7u }, /* along x: bit 0 differs */
    { 0u, 2u },
    { 1u, 3u },
    { 4u, 6u },
    { 5u, 7u }, /* along y: bit 1 differs */
    { 0u, 4u },
    { 1u, 5u },
    { 2u, 6u },
    { 3u, 7u }, /* along z: bit 2 differs */
};

static_assert(sizeof(XUD_SHAPES_WIRE_CUBE_CORNER) / (sizeof(XUD_SHAPES_WIRE_CUBE_CORNER[0][0]) * 3u) == XUD_SHAPES_WIRE_CUBE_CORNER_COUNT,
    "xud_shapes.c: the corner table has one row per corner of one box, which is the 8 the append's vertex bound is written against");
static_assert(sizeof(XUD_SHAPES_WIRE_CUBE_EDGE) / (sizeof(XUD_SHAPES_WIRE_CUBE_EDGE[0][0]) * 2u) == XUD_SHAPES_WIRE_CUBE_EDGE_COUNT,
    "xud_shapes.c: the edge table has one row per edge of the cube graph, which is the 12 whose twice is the 24 the append's index bound "
    "is written against");
static_assert((XUD_SHAPES_WIRE_CUBE_EDGE_COUNT * 2u) == XUD_SHAPES_WIRE_CUBE_INDEX_COUNT,
    "xud_shapes.c: the edge table is read two indices per row, which is the 24 the append's index bound is written against - the table's "
    "own "
    "corner references being inside the eight corners is NOT asserted here because a C static_assert cannot name an array element, and the "
    "shapes area's graph check is what reads it every run instead");

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
        xud_shapes_note_refused(batch, vertexCount + XUD_SHAPES_TRIANGLE_VERTEX_COUNT, indexCount + XUD_SHAPES_TRIANGLE_INDEX_COUNT);
        return XUD_OUT_OF_MEMORY;
    }
    if ((indexCount + XUD_SHAPES_TRIANGLE_INDEX_COUNT) > batch->indexCapacity) {
        xud_shapes_note_refused(batch, vertexCount + XUD_SHAPES_TRIANGLE_VERTEX_COUNT, indexCount + XUD_SHAPES_TRIANGLE_INDEX_COUNT);
        return XUD_OUT_OF_MEMORY;
    }

    /* The topology this shape's indices are assembled as, moved before a byte is written. The
       batch's default is already TRIANGLES, so this is a no-op for the first shape of a frame; it
       is a call at all because a frame that drew a line before this triangle has to be two rows
       rather than one row drawn at the line's topology. */
    result = xud_batch_set_topology(batch, XUD_BATCH_TOPOLOGY_TRIANGLES);
    if (result != XUD_OK) {
        return result;
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
 * The plane: four corners, then the six indices that close them as two triangles
 *-----------------------------------------------------------------------------------------------*/

/* The four corners as their (x, z) SIGNS, in the order raylib's DrawPlane emits them (rmodels.c:
   -0.5,-0.5 then -0.5,+0.5 then +0.5,+0.5 then +0.5,-0.5). The order is load-bearing the way a
   face's winding is: it is what makes the right-hand rule answer +Y for both triangles, and +Y is
   the normal the plane carries and the axis the XZ ground lies across. The y of every corner is the
   centre's own - the plane has no thickness and no slope.
*/
static const float XUD_SHAPES_PLANE_CORNER[XUD_SHAPES_PLANE_VERTEX_COUNT][2] = {
    { -1.0f, -1.0f },
    { -1.0f, 1.0f },
    { 1.0f, 1.0f },
    { 1.0f, -1.0f },
};

/* The two triangles over those four corners, sharing the diagonal from corner 0 to corner 2 - the
   quad's own diagonal, not a fourth corner: a plane is one surface, so the diagonal is a tessellation
   of it and not one of its edges, and the two triangles' normals agree because both are the plane.
*/
static const uint32_t XUD_SHAPES_PLANE_INDEX[XUD_SHAPES_PLANE_INDEX_COUNT] = { 0u, 1u, 2u, 0u, 2u, 3u };

static_assert(sizeof(XUD_SHAPES_PLANE_CORNER) / (sizeof(XUD_SHAPES_PLANE_CORNER[0][0]) * 2u) == XUD_SHAPES_PLANE_VERTEX_COUNT,
    "xud_shapes.c: the corner table has one row per corner of one plane, which is the 4 the append's vertex bound is written against");
static_assert((sizeof(XUD_SHAPES_PLANE_INDEX) / sizeof(XUD_SHAPES_PLANE_INDEX[0])) == XUD_SHAPES_PLANE_INDEX_COUNT,
    "xud_shapes.c: the index table has one entry per index of one quad, which is the 6 the append's index bound is written against");

XudResult xud_shapes_append_plane(XudBatch* batch, XudVector3 center, XudVector2 size, XudColor color) {
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

    result = xud_batch_stats(batch, &vertexCount, &indexCount, &vertexCapacity, &drawCount, &stagingBytes);
    if (result != XUD_OK) {
        return result;
    }

    if ((vertexCount + XUD_SHAPES_PLANE_VERTEX_COUNT) > vertexCapacity) {
        xud_shapes_note_refused(batch, vertexCount + XUD_SHAPES_PLANE_VERTEX_COUNT, indexCount + XUD_SHAPES_PLANE_INDEX_COUNT);
        return XUD_OUT_OF_MEMORY;
    }
    if ((indexCount + XUD_SHAPES_PLANE_INDEX_COUNT) > batch->indexCapacity) {
        xud_shapes_note_refused(batch, vertexCount + XUD_SHAPES_PLANE_VERTEX_COUNT, indexCount + XUD_SHAPES_PLANE_INDEX_COUNT);
        return XUD_OUT_OF_MEMORY;
    }

    result = xud_batch_set_topology(batch, XUD_BATCH_TOPOLOGY_TRIANGLES);
    if (result != XUD_OK) {
        return result;
    }

    uv[0] = XUD_SHAPES_UV_S;
    uv[1] = XUD_SHAPES_UV_T;
    normal[0] = XUD_SHAPES_GROUND_NORMAL_X;
    normal[1] = XUD_SHAPES_GROUND_NORMAL_Y;
    normal[2] = XUD_SHAPES_GROUND_NORMAL_Z;
    rgba[0] = color.r;
    rgba[1] = color.g;
    rgba[2] = color.b;
    rgba[3] = color.a;

    /* The four corners, each at the centre plus its own signs times half the caller's size - the
       half because size is the plane's FULL extent along each axis, which is how raylib's own scale
       (rlScalef(size.x, 1, size.y) over a -0.5..+0.5 quad) reads it too.
    */
    first = vertexCount;

    for (row = 0u; row < XUD_SHAPES_PLANE_VERTEX_COUNT; row++) {
        pos[0] = center.x + (XUD_SHAPES_PLANE_HALF_EXTENT * XUD_SHAPES_PLANE_CORNER[row][0] * size.x);
        pos[1] = center.y;
        pos[2] = center.z + (XUD_SHAPES_PLANE_HALF_EXTENT * XUD_SHAPES_PLANE_CORNER[row][1] * size.y);

        result = xud_batch_append_vertex(batch, pos, uv, rgba, normal);
        if (result != XUD_OK) {
            return result;
        }
    }

    for (row = 0u; row < XUD_SHAPES_PLANE_INDEX_COUNT; row++) {
        result = xud_batch_append_index(batch, first + XUD_SHAPES_PLANE_INDEX[row]);
        if (result != XUD_OK) {
            return result;
        }
    }

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The box: 24 vertices, then the 36 indices that name them
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_shapes_append_cube(XudBatch* batch, XudVector3 center, float width, float height, float length, XudColor color) {
    float half[3];
    float pos[3];
    float uv[2];
    float normal[3];
    float corner[3];
    uint8_t rgba[4];
    uint32_t vertexCount = 0u;
    uint32_t indexCount = 0u;
    uint32_t vertexCapacity = 0u;
    uint32_t drawCount = 0u;
    VkDeviceSize stagingBytes = 0u;
    XudResult result = XUD_INVALID_ARG;
    uint32_t first = 0u;
    uint32_t face = 0u;
    uint32_t row = 0u;

    if (batch == NULL) {
        return XUD_INVALID_ARG;
    }

    /* The batch's counters, read the same way the triangle's append reads them: this is the half
       that makes the append atomic, so it has to be the same read the appends below make. */
    result = xud_batch_stats(batch, &vertexCount, &indexCount, &vertexCapacity, &drawCount, &stagingBytes);
    if (result != XUD_OK) {
        return result;
    }

    /* The whole box has to fit, or none of it is appended. Both bounds are checked: the vertex one
       against the accessor's capacity and the index one against the batch's own indexCapacity,
       which is the number xud_batch_layout() computed as 3n/2 for that capacity. Both, and not
       just the vertex one, because an index names a LIVE vertex rather than a new one - a batch wh
       ose existing content reuses vertices spends more than that 3n/2 and can be full on its index
       es while it still has twenty-four vertices' worth of room, and a refusal that checked only t
       he vertices would write a box's 24 vertices into a batch that cannot name them. Neither sum
       can overflow: the header's static_asserts bound the capacity at XUD_BATCH_MAX_VERTICES, four
       digits. */
    if ((vertexCount + XUD_SHAPES_CUBE_VERTEX_COUNT) > vertexCapacity) {
        xud_shapes_note_refused(batch, vertexCount + XUD_SHAPES_CUBE_VERTEX_COUNT, indexCount + XUD_SHAPES_CUBE_INDEX_COUNT);
        return XUD_OUT_OF_MEMORY;
    }
    if ((indexCount + XUD_SHAPES_CUBE_INDEX_COUNT) > batch->indexCapacity) {
        xud_shapes_note_refused(batch, vertexCount + XUD_SHAPES_CUBE_VERTEX_COUNT, indexCount + XUD_SHAPES_CUBE_INDEX_COUNT);
        return XUD_OUT_OF_MEMORY;
    }

    /* The topology moves before a byte is written, in the same shape as the triangle's append: a
       box's six faces are twelve triangles, so this is the shape that says the batch's default is
       still the default for a filled shape. */
    result = xud_batch_set_topology(batch, XUD_BATCH_TOPOLOGY_TRIANGLES);
    if (result != XUD_OK) {
        return result;
    }

    /* The extents are the caller's FULL sizes, halved here because the caller's position is the
       box's centre - raylib's DrawCube's own start = position - size*0.5 / end = position +
       size*0.5, restated here as the half-extent the corner signs are multiplied by. */
    half[0] = width * XUD_SHAPES_CUBE_HALF_EXTENT;
    half[1] = height * XUD_SHAPES_CUBE_HALF_EXTENT;
    half[2] = length * XUD_SHAPES_CUBE_HALF_EXTENT;

    uv[0] = XUD_SHAPES_UV_S;
    uv[1] = XUD_SHAPES_UV_T;
    rgba[0] = color.r;
    rgba[1] = color.g;
    rgba[2] = color.b;
    rgba[3] = color.a;

    first = vertexCount;

    /* The 24 vertices, six faces of four. The normal is read per face, not once for the shape:
       a face's normal belongs to that face alone, which is the reason the box carries 24 vertices
       rather than the 8 corners it shares between three faces. */
    for (face = 0u; face < XUD_SHAPES_CUBE_FACE_COUNT; face++) {
        normal[0] = XUD_SHAPES_CUBE_NORMAL[face][0];
        normal[1] = XUD_SHAPES_CUBE_NORMAL[face][1];
        normal[2] = XUD_SHAPES_CUBE_NORMAL[face][2];

        for (row = 0u; row < XUD_SHAPES_CUBE_FACE_VERTEX_COUNT; row++) {
            corner[0] = XUD_SHAPES_CUBE_CORNER[face][row][0];
            corner[1] = XUD_SHAPES_CUBE_CORNER[face][row][1];
            corner[2] = XUD_SHAPES_CUBE_CORNER[face][row][2];

            pos[0] = center.x + (corner[0] * half[0]);
            pos[1] = center.y + (corner[1] * half[1]);
            pos[2] = center.z + (corner[2] * half[2]);

            result = xud_batch_append_vertex(batch, pos, uv, rgba, normal);
            if (result != XUD_OK) {
                return result;
            }
        }
    }

    /* The indices name the 24 vertices this call just appended, four per face, and the two
       triangles of a face are the face's own four - so the index is the shape's first vertex plus
       the face's four plus the corner inside them, and xud_batch_append_index's own bound (an
       index at or above the live vertex count is a bad argument) is a bound these cannot cross. */
    for (face = 0u; face < XUD_SHAPES_CUBE_FACE_COUNT; face++) {
        for (row = 0u; row < XUD_SHAPES_CUBE_FACE_INDEX_COUNT; row++) {
            result
                = xud_batch_append_index(batch, first + (face * XUD_SHAPES_CUBE_FACE_VERTEX_COUNT) + XUD_SHAPES_CUBE_TRIANGLE_CORNER[row]);
            if (result != XUD_OK) {
                return result;
            }
        }
    }

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The sphere: 17 vertices, then the 90 indices that close it
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_shapes_append_sphere(XudBatch* batch, XudVector3 center, float radius, uint32_t rings, uint32_t slices, XudColor color) {
    uint32_t vertexCount = 0u;
    uint32_t indexCount = 0u;
    uint32_t vertexCapacity = 0u;
    uint32_t drawCount = 0u;
    uint64_t vertexNeed = 0ull;
    uint64_t indexNeed = 0ull;
    VkDeviceSize stagingBytes = 0u;
    XudResult result = XUD_INVALID_ARG;
    uint32_t first = 0u;
    uint32_t topPole = 0u;
    uint32_t bottomPole = 0u;
    uint32_t ring = 0u;
    uint32_t slice = 0u;
    uint32_t a = 0u;
    uint32_t b = 0u;
    uint32_t c = 0u;
    uint32_t d = 0u;
    float dir[3];
    float step = 0.0f;

    if (batch == NULL) {
        return XUD_INVALID_ARG;
    }

    /* The closed-surface minimums, refused rather than drawn degenerate: fewer than two rings is
       not a closed surface (there is no row between the two poles to close against) and fewer than
       three slices is a band whose faces lie on top of each other rather than around an axis. The
       radius test is written as "not greater" rather than "less or equal" so a NaN radius is refu
       sed by the same test rather than appended as a NaN position and a NaN normal. */
    if ((rings < XUD_SHAPES_SPHERE_RING_MIN) || (slices < XUD_SHAPES_SPHERE_SLICE_MIN)) {
        return XUD_INVALID_ARG;
    }
    if (!(radius > 0.0f)) {
        return XUD_INVALID_ARG;
    }

    result = xud_batch_stats(batch, &vertexCount, &indexCount, &vertexCapacity, &drawCount, &stagingBytes);
    if (result != XUD_OK) {
        return result;
    }

    /* The two counts, computed wide: rings and slices are the caller's uint32_t, so (rings-1)*slic
       es is a product that can wrap below the batch's own bound and a wrapped count is a shape tha
       t passes the fit check and then writes past the arena. Wide arithmetic makes the fit check a
       fact rather than an assumption, and the counts below are only used as uint32_t once they hav
       e passed against a uint32_t capacity. */
    vertexNeed = 2ull + (((uint64_t)rings - 1ull) * (uint64_t)slices);
    indexNeed = 6ull * (uint64_t)slices * ((uint64_t)rings - 1ull);

    /* Both bounds, in the same atomic shape as the triangle's and the box's. For a sphere the INDEX
       one is the one that trips - the sphere shares its vertices, so it spends the 6V-12 indices a
       closed MANIFOLD surface needs (90 over its 17 vertices, four times the 3n/2 per vertex CAPACI
       TY
       headroom, and the box's 1.5 per vertex is its SPLIT-per-face spelling rather than that bound)
       ,
       and the vertex bound has 47 slots to spare (xud_shapes.h's assert states the same fact the ot
       her
       way round). Neither bound is enough on its own here either: a batch that is nearly full on ve
       rtices
       can still have a sphere's indices, and one that is nearly full on indices can still have its
       vertices.
     */
    if (((uint64_t)vertexCount + vertexNeed) > (uint64_t)vertexCapacity) {
        xud_shapes_note_refused(batch, (uint64_t)vertexCount + vertexNeed, (uint64_t)indexCount + indexNeed);
        return XUD_OUT_OF_MEMORY;
    }
    if (((uint64_t)indexCount + indexNeed) > (uint64_t)batch->indexCapacity) {
        xud_shapes_note_refused(batch, (uint64_t)vertexCount + vertexNeed, (uint64_t)indexCount + indexNeed);
        return XUD_OUT_OF_MEMORY;
    }

    /* The topology moves before a byte is written, in the same shape as the triangle's and the
       box's appends. */
    result = xud_batch_set_topology(batch, XUD_BATCH_TOPOLOGY_TRIANGLES);
    if (result != XUD_OK) {
        return result;
    }

    /* The vertices, in the order the indices below number them: the top pole first, then the rows
       between the poles from the top down, SLICE_COUNT vertices a row, then the bottom pole. The poles
       are the axis direction rather than sinf/cosf of 0 and PI so their positions and normals are
       exact rather than sin(PI)-rounding - and sin(PI) is not zero, it is 1.2e-16, which is what
       makes a pole computed from it a pole 1.2e-16 radii off the axis rather than a pole.
     */
    first = vertexCount;
    topPole = first;
    bottomPole = first + 1u + ((rings - 1u) * slices);

    dir[0] = 0.0f;
    dir[1] = 1.0f;
    dir[2] = 0.0f;
    result = xud_shapes_sphere_vertex(batch, center, radius, dir, color);
    if (result != XUD_OK) {
        return result;
    }

    /* The longitude step, once: the longitudes are evenly spaced and the last one's neighbour is
       the first one, which is what makes the mesh closed without a seam column (see the note in
       xud_shapes.h on why there is no seam to duplicate here). */
    step = ((2.0f * XUD_SHAPES_SPHERE_PI) / (float)slices);

    for (ring = 1u; ring < rings; ring++) {
        const float latitude = XUD_SHAPES_SPHERE_PI * (float)ring / (float)rings;
        const float sinLatitude = sinf(latitude);
        const float cosLatitude = cosf(latitude);

        for (slice = 0u; slice < slices; slice++) {
            const float longitude = step * (float)slice;

            dir[0] = sinLatitude * cosf(longitude);
            dir[1] = cosLatitude;
            dir[2] = sinLatitude * sinf(longitude);

            result = xud_shapes_sphere_vertex(batch, center, radius, dir, color);
            if (result != XUD_OK) {
                return result;
            }
        }
    }

    dir[0] = 0.0f;
    dir[1] = -1.0f;
    dir[2] = 0.0f;
    result = xud_shapes_sphere_vertex(batch, center, radius, dir, color);
    if (result != XUD_OK) {
        return result;
    }

    /* The indices, in the three groups the closed mesh is made of, and the winding of all three is
       the same rule: a face is counter-clockwise seen from OUTSIDE, which is raylib's DrawTriangle
       3D wording (raylib.h 1588) and the pipeline's XUD_PIPELINE_FRONT_FACE, and the tests area re
       -derives it from each face's own cross product against the radial direction rather than tru
       sting this order. The two fans are what the two pole ROWS would have produced had the poles
       been rows of coincident vertices - one triangle per longitude rather than a quad whose half
       is degenerate - and the bands are the quads, each closed by the two triangles that share one
       diagonal. The longitude wraps, which is what makes the mesh closed with no seam column: the
       last slice's neighbour is the first one.
     */
    for (slice = 0u; slice < slices; slice++) {
        a = topPole;
        b = first + 1u + ((slice + 1u) % slices);
        c = first + 1u + slice;
        result = xud_batch_append_index(batch, a);
        if (result != XUD_OK) {
            return result;
        }
        result = xud_batch_append_index(batch, b);
        if (result != XUD_OK) {
            return result;
        }
        result = xud_batch_append_index(batch, c);
        if (result != XUD_OK) {
            return result;
        }
    }

    for (ring = 1u; (ring + 1u) < rings; ring++) {
        for (slice = 0u; slice < slices; slice++) {
            a = first + 1u + (((ring - 1u) * slices) + slice);
            b = first + 1u + (((ring - 1u) * slices) + ((slice + 1u) % slices));
            c = first + 1u + ((ring * slices) + ((slice + 1u) % slices));
            d = first + 1u + ((ring * slices) + slice);

            result = xud_batch_append_index(batch, a);
            if (result != XUD_OK) {
                return result;
            }
            result = xud_batch_append_index(batch, b);
            if (result != XUD_OK) {
                return result;
            }
            result = xud_batch_append_index(batch, c);
            if (result != XUD_OK) {
                return result;
            }
            result = xud_batch_append_index(batch, a);
            if (result != XUD_OK) {
                return result;
            }
            result = xud_batch_append_index(batch, c);
            if (result != XUD_OK) {
                return result;
            }
            result = xud_batch_append_index(batch, d);
            if (result != XUD_OK) {
                return result;
            }
        }
    }

    for (slice = 0u; slice < slices; slice++) {
        a = first + 1u + (((rings - 2u) * slices) + slice);
        b = first + 1u + (((rings - 2u) * slices) + ((slice + 1u) % slices));
        c = bottomPole;
        result = xud_batch_append_index(batch, a);
        if (result != XUD_OK) {
            return result;
        }
        result = xud_batch_append_index(batch, b);
        if (result != XUD_OK) {
            return result;
        }
        result = xud_batch_append_index(batch, c);
        if (result != XUD_OK) {
            return result;
        }
    }

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The circle: the centre and the 36 rim points, then the 108 indices that name the fan
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_shapes_append_circle(
    XudBatch* batch, uint32_t extentWidth, uint32_t extentHeight, float centerX, float centerY, float radius, XudColor color) {
    float pos[3];
    float uv[2];
    float normal[3];
    uint8_t rgba[4];
    float clipX = 0.0f;
    float clipY = 0.0f;
    float step = 0.0f;
    uint32_t vertexCount = 0u;
    uint32_t indexCount = 0u;
    uint32_t vertexCapacity = 0u;
    uint32_t drawCount = 0u;
    VkDeviceSize stagingBytes = 0u;
    XudResult result = XUD_INVALID_ARG;
    uint32_t first = 0u;
    uint32_t rim = 0u;

    if (batch == NULL) {
        return XUD_INVALID_ARG;
    }

    /* The radius test is the sphere's own: written "not greater" rather than "less or equal" so a
       NaN radius is refused by the same comparison rather than appended as NaN positions, and a
       circle with no radius has no rim to walk at all. */
    if (!(radius > 0.0f)) {
        return XUD_INVALID_ARG;
    }

    result = xud_batch_stats(batch, &vertexCount, &indexCount, &vertexCapacity, &drawCount, &stagingBytes);
    if (result != XUD_OK) {
        return result;
    }

    /* Both bounds, in the same atomic shape as the triangle's and the sphere's. For a circle the
       index one is the one that trips first, the way the sphere's is: the fan spends three indices
       per rim vertex, which is 2/3 over the 3n/2 the batch sizes its index headroom at, so a batch
       with 28 vertices of room left can have no room for the circle's 108 indices. */
    if ((vertexCount + XUD_SHAPES_CIRCLE_VERTEX_COUNT) > vertexCapacity) {
        xud_shapes_note_refused(batch, vertexCount + XUD_SHAPES_CIRCLE_VERTEX_COUNT, indexCount + XUD_SHAPES_CIRCLE_INDEX_COUNT);
        return XUD_OUT_OF_MEMORY;
    }
    if ((indexCount + XUD_SHAPES_CIRCLE_INDEX_COUNT) > batch->indexCapacity) {
        xud_shapes_note_refused(batch, vertexCount + XUD_SHAPES_CIRCLE_VERTEX_COUNT, indexCount + XUD_SHAPES_CIRCLE_INDEX_COUNT);
        return XUD_OUT_OF_MEMORY;
    }

    /* The topology this shape's indices are assembled as, moved before a byte is written, in the
       same shape as the other filled appends. */
    result = xud_batch_set_topology(batch, XUD_BATCH_TOPOLOGY_TRIANGLES);
    if (result != XUD_OK) {
        return result;
    }

    /* The two constant attribute rows, once, for the reasons stated beside each constant in
       xud_shapes.h: the uv names no texel a caller named, and the normal is the flat +Z a
       screen-facing shape faces along. */
    uv[0] = XUD_SHAPES_UV_S;
    uv[1] = XUD_SHAPES_UV_T;
    normal[0] = XUD_SHAPES_FLAT_NORMAL_X;
    normal[1] = XUD_SHAPES_FLAT_NORMAL_Y;
    normal[2] = XUD_SHAPES_FLAT_NORMAL_Z;
    rgba[0] = color.r;
    rgba[1] = color.g;
    rgba[2] = color.b;
    rgba[3] = color.a;

    first = vertexCount;

    /* The centre first, the vertex the 36 triangles all share, mapped through the same pixel to
       clip the rim points take below. */
    result = xud_shapes_pixel_to_clip(extentWidth, extentHeight, centerX, centerY, &clipX, &clipY);
    if (result != XUD_OK) {
        return result;
    }
    pos[0] = clipX;
    pos[1] = clipY;
    pos[2] = 0.0f;
    result = xud_batch_append_vertex(batch, pos, uv, rgba, normal);
    if (result != XUD_OK) {
        return result;
    }

    /* The rim, in the order the angles walk it. The step is 2 PI over the segment count, the same
       longitude step the sphere's append uses for its rows, and the rim is computed in pixel space
       at the caller's centre and radius with each point mapped to clip at the extent's own
       division: a circle in pixels is an ellipse in clip under the extent's non-uniform scale,
       which is why the mapping is per point rather than the centre mapped once and a radius
       scaled. */
    step = ((2.0f * XUD_SHAPES_SPHERE_PI) / (float)XUD_SHAPES_CIRCLE_SEGMENTS);
    for (rim = 0u; rim < XUD_SHAPES_CIRCLE_SEGMENTS; rim++) {
        const float angle = ((float)rim) * step;

        result = xud_shapes_pixel_to_clip(
            extentWidth, extentHeight, centerX + (cosf(angle) * radius), centerY + (sinf(angle) * radius), &clipX, &clipY);
        if (result != XUD_OK) {
            return result;
        }
        pos[0] = clipX;
        pos[1] = clipY;
        pos[2] = 0.0f;
        result = xud_batch_append_vertex(batch, pos, uv, rgba, normal);
        if (result != XUD_OK) {
            return result;
        }
    }

    /* The indices name the fan: each of the 36 triangles is the centre (the first vertex this call
       appended) and a pair of neighbouring rim points, the last wrapping back to the first to close
       the ring. */
    for (rim = 0u; rim < XUD_SHAPES_CIRCLE_SEGMENTS; rim++) {
        result = xud_batch_append_index(batch, first);
        if (result != XUD_OK) {
            return result;
        }
        result = xud_batch_append_index(batch, first + 1u + rim);
        if (result != XUD_OK) {
            return result;
        }
        result = xud_batch_append_index(batch, first + 1u + ((rim + 1u) % XUD_SHAPES_CIRCLE_SEGMENTS));
        if (result != XUD_OK) {
            return result;
        }
    }

    return XUD_OK;
}
/*---------------------------------------------------------------------------------------------------
 * The ellipse: the centre and the 36 rim points of the circle's fan, walked with two radii, then
 * the 108 indices that name the fan
 *---------------------------------------------------------------------------------------------------*/

XudResult xud_shapes_append_ellipse(XudBatch* batch, uint32_t extentWidth, uint32_t extentHeight, float centerX, float centerY,
    float radiusX, float radiusY, XudColor color) {
    float pos[3];
    float uv[2];
    float normal[3];
    uint8_t rgba[4];
    float clipX = 0.0f;
    float clipY = 0.0f;
    float step = 0.0f;
    uint32_t vertexCount = 0u;
    uint32_t indexCount = 0u;
    uint32_t vertexCapacity = 0u;
    uint32_t drawCount = 0u;
    VkDeviceSize stagingBytes = 0u;
    XudResult result = XUD_INVALID_ARG;
    uint32_t first = 0u;
    uint32_t rim = 0u;

    if (batch == NULL) {
        return XUD_INVALID_ARG;
    }

    /* The radius tests are the sphere's own: written "not greater" rather than "less or equal" so
       a NaN radius is refused by the same comparison rather than appended as NaN positions, and a
       fan with no radius on an axis has no rim to walk on it. */
    if (!(radiusX > 0.0f)) {
        return XUD_INVALID_ARG;
    }
    if (!(radiusY > 0.0f)) {
        return XUD_INVALID_ARG;
    }

    result = xud_batch_stats(batch, &vertexCount, &indexCount, &vertexCapacity, &drawCount, &stagingBytes);
    if (result != XUD_OK) {
        return result;
    }

    /* Both bounds, in the same atomic shape as the circle's, which this fan is: the constants are
       the circle's own, the reason being in xud_shapes.h's ellipse section. */
    if ((vertexCount + XUD_SHAPES_CIRCLE_VERTEX_COUNT) > vertexCapacity) {
        xud_shapes_note_refused(batch, vertexCount + XUD_SHAPES_CIRCLE_VERTEX_COUNT, indexCount + XUD_SHAPES_CIRCLE_INDEX_COUNT);
        return XUD_OUT_OF_MEMORY;
    }
    if ((indexCount + XUD_SHAPES_CIRCLE_INDEX_COUNT) > batch->indexCapacity) {
        xud_shapes_note_refused(batch, vertexCount + XUD_SHAPES_CIRCLE_VERTEX_COUNT, indexCount + XUD_SHAPES_CIRCLE_INDEX_COUNT);
        return XUD_OUT_OF_MEMORY;
    }

    /* The topology this shape's indices are assembled as, moved before a byte is written, in the
       same shape as the other filled appends. */
    result = xud_batch_set_topology(batch, XUD_BATCH_TOPOLOGY_TRIANGLES);
    if (result != XUD_OK) {
        return result;
    }

    /* The two constant attribute rows, once, for the reasons stated beside each constant in
       xud_shapes.h: the uv names no texel a caller named, and the normal is the flat +Z a
       screen-facing shape faces along. */
    uv[0] = XUD_SHAPES_UV_S;
    uv[1] = XUD_SHAPES_UV_T;
    normal[0] = XUD_SHAPES_FLAT_NORMAL_X;
    normal[1] = XUD_SHAPES_FLAT_NORMAL_Y;
    normal[2] = XUD_SHAPES_FLAT_NORMAL_Z;
    rgba[0] = color.r;
    rgba[1] = color.g;
    rgba[2] = color.b;
    rgba[3] = color.a;

    first = vertexCount;

    /* The centre first, the vertex the 36 triangles all share, mapped through the same pixel to
       clip the rim points take below. */
    result = xud_shapes_pixel_to_clip(extentWidth, extentHeight, centerX, centerY, &clipX, &clipY);
    if (result != XUD_OK) {
        return result;
    }
    pos[0] = clipX;
    pos[1] = clipY;
    pos[2] = 0.0f;
    result = xud_batch_append_vertex(batch, pos, uv, rgba, normal);
    if (result != XUD_OK) {
        return result;
    }

    /* The rim, in the order the angles walk it: the same walk the circle's takes, with the X
       offset the caller's radiusX and the Y offset the caller's radiusY rather than one radius
       for both, and each point mapped to clip at the extent's own division: an ellipse in pixels
       is a sheared ellipse in clip under the extent's non-uniform scale, which is why the mapping
       is per point rather than the centre mapped once and the radii scaled. */
    step = ((2.0f * XUD_SHAPES_SPHERE_PI) / (float)XUD_SHAPES_CIRCLE_SEGMENTS);
    for (rim = 0u; rim < XUD_SHAPES_CIRCLE_SEGMENTS; rim++) {
        const float angle = ((float)rim) * step;

        result = xud_shapes_pixel_to_clip(
            extentWidth, extentHeight, centerX + (cosf(angle) * radiusX), centerY + (sinf(angle) * radiusY), &clipX, &clipY);
        if (result != XUD_OK) {
            return result;
        }
        pos[0] = clipX;
        pos[1] = clipY;
        pos[2] = 0.0f;
        result = xud_batch_append_vertex(batch, pos, uv, rgba, normal);
        if (result != XUD_OK) {
            return result;
        }
    }

    /* The indices name the fan: each of the 36 triangles is the centre (the first vertex this call
       appended) and a pair of neighbouring rim points, the last wrapping back to the first to close
       the ring. */
    for (rim = 0u; rim < XUD_SHAPES_CIRCLE_SEGMENTS; rim++) {
        result = xud_batch_append_index(batch, first);
        if (result != XUD_OK) {
            return result;
        }
        result = xud_batch_append_index(batch, first + 1u + rim);
        if (result != XUD_OK) {
            return result;
        }
        result = xud_batch_append_index(batch, first + 1u + ((rim + 1u) % XUD_SHAPES_CIRCLE_SEGMENTS));
        if (result != XUD_OK) {
            return result;
        }
    }

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The line: two vertices, then the two indices that name them
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_shapes_append_line(XudBatch* batch, XudVector3 start, XudVector3 end, XudColor color) {
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

    result = xud_batch_stats(batch, &vertexCount, &indexCount, &vertexCapacity, &drawCount, &stagingBytes);
    if (result != XUD_OK) {
        return result;
    }

    /* Both bounds, read before anything at all changes - including the topology state below, which
       is why a line that does not fit cannot split a run. */
    if ((vertexCount + XUD_SHAPES_LINE_VERTEX_COUNT) > vertexCapacity) {
        xud_shapes_note_refused(batch, vertexCount + XUD_SHAPES_LINE_VERTEX_COUNT, indexCount + XUD_SHAPES_LINE_INDEX_COUNT);
        return XUD_OUT_OF_MEMORY;
    }
    if ((indexCount + XUD_SHAPES_LINE_INDEX_COUNT) > batch->indexCapacity) {
        xud_shapes_note_refused(batch, vertexCount + XUD_SHAPES_LINE_VERTEX_COUNT, indexCount + XUD_SHAPES_LINE_INDEX_COUNT);
        return XUD_OUT_OF_MEMORY;
    }

    /* The topology moves FIRST, and before a byte is written: the run that was pending under the
       old topology closes into a row of the same batch, and this shape's two indices start the next
       one. raylib's rlBegin(mode) does the same two things in the same order - close the current
       run, then set the mode - and this is why a frame that draws a sphere and a line is two rows
       through two pipelines rather than one row at whichever topology came last. */
    result = xud_batch_set_topology(batch, XUD_BATCH_TOPOLOGY_LINES);
    if (result != XUD_OK) {
        return result;
    }

    uv[0] = XUD_SHAPES_UV_S;
    uv[1] = XUD_SHAPES_UV_T;
    normal[0] = XUD_SHAPES_FLAT_NORMAL_X;
    normal[1] = XUD_SHAPES_FLAT_NORMAL_Y;
    normal[2] = XUD_SHAPES_FLAT_NORMAL_Z;
    rgba[0] = color.r;
    rgba[1] = color.g;
    rgba[2] = color.b;
    rgba[3] = color.a;

    first = vertexCount;

    pos[0] = start.x;
    pos[1] = start.y;
    pos[2] = start.z;
    result = xud_batch_append_vertex(batch, pos, uv, rgba, normal);
    if (result != XUD_OK) {
        return result;
    }

    pos[0] = end.x;
    pos[1] = end.y;
    pos[2] = end.z;
    result = xud_batch_append_vertex(batch, pos, uv, rgba, normal);
    if (result != XUD_OK) {
        return result;
    }

    /* The two indices name the two vertices this call just appended, in the caller's order - and
       LINE_LIST reads them two at a time, so the order is the line's direction rather than a
       winding (a line has no front face and the pipeline's cull mode is NONE anyway). */
    for (row = 0u; row < XUD_SHAPES_LINE_INDEX_COUNT; row++) {
        result = xud_batch_append_index(batch, first + row);
        if (result != XUD_OK) {
            return result;
        }
    }

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The wire cube: the 8 corners of one box, then the 24 indices that name the 12 edges
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_shapes_append_cube_wires(XudBatch* batch, XudVector3 center, float width, float height, float length, XudColor color) {
    float half[3];
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

    /* The batch's counters, read the same way every other shape's append reads them: this is the
       half that makes the append atomic. */
    result = xud_batch_stats(batch, &vertexCount, &indexCount, &vertexCapacity, &drawCount, &stagingBytes);
    if (result != XUD_OK) {
        return result;
    }

    /* Both bounds, before anything at all changes - including the topology state below. A wire cube
       is the shape that shows the batch's 3n/2 index headroom is a choice rather than a bound from
       a THIRD side: 24 indices over 8 vertices is 3 per vertex, twice the filled box's 1.5 (the
       sphere showed the same fact from above, at 90 over 17). So the index bound is the one that
       trips here while the vertex bound has 64 slots to spare, and a check that read only the
       vertices would be a wire cube written into a batch that cannot name its edges. */
    if ((vertexCount + XUD_SHAPES_WIRE_CUBE_VERTEX_COUNT) > vertexCapacity) {
        xud_shapes_note_refused(batch, vertexCount + XUD_SHAPES_WIRE_CUBE_VERTEX_COUNT, indexCount + XUD_SHAPES_WIRE_CUBE_INDEX_COUNT);
        return XUD_OUT_OF_MEMORY;
    }
    if ((indexCount + XUD_SHAPES_WIRE_CUBE_INDEX_COUNT) > batch->indexCapacity) {
        xud_shapes_note_refused(batch, vertexCount + XUD_SHAPES_WIRE_CUBE_VERTEX_COUNT, indexCount + XUD_SHAPES_WIRE_CUBE_INDEX_COUNT);
        return XUD_OUT_OF_MEMORY;
    }

    /* The topology moves before a byte is written, in the same shape as the line's append: an edge
       is a segment, so the wire cube's indices are assembled as LINE_LIST and the run that was
       pending under the old topology closes into a row of the same batch. This is raylib's DrawCube
       Wires shape read literally - the same box, a different rlBegin mode. */
    result = xud_batch_set_topology(batch, XUD_BATCH_TOPOLOGY_LINES);
    if (result != XUD_OK) {
        return result;
    }

    /* The extents are the caller's FULL sizes, halved here exactly as the filled box halves them
       above: the caller's position is the box's CENTRE, so a wire cube and a filled cube given the
       same arguments are the same box in space, one drawn as its outline. */
    half[0] = width * XUD_SHAPES_CUBE_HALF_EXTENT;
    half[1] = height * XUD_SHAPES_CUBE_HALF_EXTENT;
    half[2] = length * XUD_SHAPES_CUBE_HALF_EXTENT;

    uv[0] = XUD_SHAPES_UV_S;
    uv[1] = XUD_SHAPES_UV_T;
    normal[0] = XUD_SHAPES_FLAT_NORMAL_X;
    normal[1] = XUD_SHAPES_FLAT_NORMAL_Y;
    normal[2] = XUD_SHAPES_FLAT_NORMAL_Z;
    rgba[0] = color.r;
    rgba[1] = color.g;
    rgba[2] = color.b;
    rgba[3] = color.a;

    /* The eight corners, in the bit-pattern order the edge table is written against. The normal is
       the flat +Z constant rather than a face's normal, and that is the opposite of the filled box's
       reason for carrying 24 vertices: a wire has no face, so there is no normal a shared corner
       would have to contradict, and the corner is shared. */
    first = vertexCount;

    for (row = 0u; row < XUD_SHAPES_WIRE_CUBE_CORNER_COUNT; row++) {
        pos[0] = center.x + (XUD_SHAPES_WIRE_CUBE_CORNER[row][0] * half[0]);
        pos[1] = center.y + (XUD_SHAPES_WIRE_CUBE_CORNER[row][1] * half[1]);
        pos[2] = center.z + (XUD_SHAPES_WIRE_CUBE_CORNER[row][2] * half[2]);

        result = xud_batch_append_vertex(batch, pos, uv, rgba, normal);
        if (result != XUD_OK) {
            return result;
        }
    }

    /* The 24 indices, two per edge, naming the eight corners this call just appended. xud_batch
       _append_index refuses an index at or above the live vertex count, and the table's own bounds
       are asserted above against the eight corners, so these cannot cross that bound. */
    for (row = 0u; row < XUD_SHAPES_WIRE_CUBE_EDGE_COUNT; row++) {
        result = xud_batch_append_index(batch, first + XUD_SHAPES_WIRE_CUBE_EDGE[row][0]);
        if (result != XUD_OK) {
            return result;
        }
        result = xud_batch_append_index(batch, first + XUD_SHAPES_WIRE_CUBE_EDGE[row][1]);
        if (result != XUD_OK) {
            return result;
        }
    }

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The wire sphere: the 17 vertices the filled sphere places, then the 70 indices that name the grid
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_shapes_append_sphere_wires(
    XudBatch* batch, XudVector3 center, float radius, uint32_t rings, uint32_t slices, XudColor color) {
    uint32_t vertexCount = 0u;
    uint32_t indexCount = 0u;
    uint32_t vertexCapacity = 0u;
    uint32_t drawCount = 0u;
    uint64_t vertexNeed = 0ull;
    uint64_t indexNeed = 0ull;
    VkDeviceSize stagingBytes = 0u;
    XudResult result = XUD_INVALID_ARG;
    uint32_t first = 0u;
    uint32_t topPole = 0u;
    uint32_t bottomPole = 0u;
    uint32_t ring = 0u;
    uint32_t slice = 0u;
    uint32_t a = 0u;
    uint32_t b = 0u;
    float dir[3];
    float step = 0.0f;

    if (batch == NULL) {
        return XUD_INVALID_ARG;
    }

    /* The same three refusals the filled sphere makes above, because this form places the same
       vertices by the same directions: below either minimum there is no grid to draw (one ring is
       a pole and nothing between it), and a radius that is not strictly positive has no surface and
       no radial direction. The test is written "not greater" so a NaN radius is refused here rather
       than appended as a NaN position. */
    if ((rings < XUD_SHAPES_SPHERE_RING_MIN) || (slices < XUD_SHAPES_SPHERE_SLICE_MIN)) {
        return XUD_INVALID_ARG;
    }
    if (!(radius > 0.0f)) {
        return XUD_INVALID_ARG;
    }

    result = xud_batch_stats(batch, &vertexCount, &indexCount, &vertexCapacity, &drawCount, &stagingBytes);
    if (result != XUD_OK) {
        return result;
    }

    /* The two counts, computed WIDE in the same shape as the filled sphere's: rings and slices are
       the caller's uint32_t, so the product can wrap below the batch's own bound and a wrapped
       count is a shape that passes the fit check and then writes past the arena. The edge count is
       the grid's, not the mesh's: the latitude circles are (rings-1) rings of slices closing edges
       and the meridian polylines are slices chains of rings edges, the two poles included, which is
       slices*(2*rings-1) and which xud_shapes.h asserts both ways. */
    vertexNeed = 2ull + (((uint64_t)rings - 1ull) * (uint64_t)slices);
    indexNeed = 2ull * (uint64_t)slices * (((2ull * (uint64_t)rings)) - 1ull);

    /* Both bounds, and for a sphere the index one is the one that trips - the grid shares the
       sphere's vertices, so it spends 70 indices over the same 17 vertices, which is BELOW the
       filled sphere's 90 because the grid draws no band diagonal, and still four times the batch's
       3n/2 per-vertex headroom. */
    if (((uint64_t)vertexCount + vertexNeed) > (uint64_t)vertexCapacity) {
        xud_shapes_note_refused(batch, (uint64_t)vertexCount + vertexNeed, (uint64_t)indexCount + indexNeed);
        return XUD_OUT_OF_MEMORY;
    }
    if (((uint64_t)indexCount + indexNeed) > (uint64_t)batch->indexCapacity) {
        xud_shapes_note_refused(batch, (uint64_t)vertexCount + vertexNeed, (uint64_t)indexCount + indexNeed);
        return XUD_OUT_OF_MEMORY;
    }

    result = xud_batch_set_topology(batch, XUD_BATCH_TOPOLOGY_LINES);
    if (result != XUD_OK) {
        return result;
    }

    /* The vertices, in the order the indices below number them and by the same helper the filled
       sphere uses: the top pole first, then the rows between the poles from the top down, SLICE_CO
       UNT vertices a row, then the bottom pole. The two forms are ONE vertex layout rather than two
       to keep honest, which is what the header's assert states - and the normal is the radial direct
       ion the helper passes, because a wire sphere's vertices are the sphere's own surface vertices
       even though a wire cube's corners have no face to give them a normal. */
    first = vertexCount;
    topPole = first;
    bottomPole = first + 1u + ((rings - 1u) * slices);

    dir[0] = 0.0f;
    dir[1] = 1.0f;
    dir[2] = 0.0f;
    result = xud_shapes_sphere_vertex(batch, center, radius, dir, color);
    if (result != XUD_OK) {
        return result;
    }

    step = ((2.0f * XUD_SHAPES_SPHERE_PI) / (float)slices);

    for (ring = 1u; ring < rings; ring++) {
        const float latitude = XUD_SHAPES_SPHERE_PI * (float)ring / (float)rings;
        const float sinLatitude = sinf(latitude);
        const float cosLatitude = cosf(latitude);

        for (slice = 0u; slice < slices; slice++) {
            const float longitude = step * (float)slice;

            dir[0] = sinLatitude * cosf(longitude);
            dir[1] = cosLatitude;
            dir[2] = sinLatitude * sinf(longitude);

            result = xud_shapes_sphere_vertex(batch, center, radius, dir, color);
            if (result != XUD_OK) {
                return result;
            }
        }
    }

    dir[0] = 0.0f;
    dir[1] = -1.0f;
    dir[2] = 0.0f;
    result = xud_shapes_sphere_vertex(batch, center, radius, dir, color);
    if (result != XUD_OK) {
        return result;
    }

    /* The grid's edges, in the two groups the count above is made of, and every edge is named ONCE
       - the latitude circles first, each closing so the last slice's neighbour is the first one, in
       the same no-seam reading the filled sphere's note states. A ring's circle is drawn as slices
       segments rather than as a strip because LINE_LIST reads pairs, and a strip would be a second
       topology row of the list this rung grew rather than the one it already has. */
    for (ring = 1u; ring < rings; ring++) {
        for (slice = 0u; slice < slices; slice++) {
            a = first + 1u + (((ring - 1u) * slices) + slice);
            b = first + 1u + (((ring - 1u) * slices) + ((slice + 1u) % slices));
            result = xud_batch_append_index(batch, a);
            if (result != XUD_OK) {
                return result;
            }
            result = xud_batch_append_index(batch, b);
            if (result != XUD_OK) {
                return result;
            }
        }
    }

    /* The meridian polylines, one per longitude, in three groups: the top pole down to the first
       ring, the ring-to-ring runs, and the last ring down to the bottom pole. The middle loop is
       written ring + 1 < rings rather than ring < rings - 1 so rings == 2 (the closed-surface
       minimum) has no underflow to hide behind: there are no ring-to-ring runs at that tessellation
       and the two pole groups are the whole grid. */
    for (slice = 0u; slice < slices; slice++) {
        a = topPole;
        b = first + 1u + slice;
        result = xud_batch_append_index(batch, a);
        if (result != XUD_OK) {
            return result;
        }
        result = xud_batch_append_index(batch, b);
        if (result != XUD_OK) {
            return result;
        }
    }

    for (ring = 1u; (ring + 1u) < rings; ring++) {
        for (slice = 0u; slice < slices; slice++) {
            a = first + 1u + (((ring - 1u) * slices) + slice);
            b = first + 1u + ((ring * slices) + slice);
            result = xud_batch_append_index(batch, a);
            if (result != XUD_OK) {
                return result;
            }
            result = xud_batch_append_index(batch, b);
            if (result != XUD_OK) {
                return result;
            }
        }
    }

    for (slice = 0u; slice < slices; slice++) {
        a = first + 1u + (((rings - 2u) * slices) + slice);
        b = bottomPole;
        result = xud_batch_append_index(batch, a);
        if (result != XUD_OK) {
            return result;
        }
        result = xud_batch_append_index(batch, b);
        if (result != XUD_OK) {
            return result;
        }
    }

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The cylinder and the capsule: one axis, the rings about it, and the frame that puts them there
 *-----------------------------------------------------------------------------------------------*/

static void xud_shapes_cross(const float left[3], const float right[3], float* out) {
    out[0] = (left[1] * right[2]) - (left[2] * right[1]);
    out[1] = (left[2] * right[0]) - (left[0] * right[2]);
    out[2] = (left[0] * right[1]) - (left[1] * right[0]);
}

static float xud_shapes_length(const float v[3]) { return sqrtf((v[0] * v[0]) + (v[1] * v[1]) + (v[2] * v[2])); }

/* The frame a ring about the caller's axis is written in: the axis unitised and the two vectors
   across it. raylib's DrawCylinderEx builds the same frame (rmodels.c 651: b1 =
   Vector3Perpendicular(direction), b2 = Vector3CrossProduct(b1, direction)) and gets its
   perpendicular from the axis's least-aligned axis, which is the branch-free way to be sure the
   cross product has something to bite on. The difference is that a zero-length axis is refused here
   rather than divided by: an axis with no direction has no ring that can be round about it, and a
   NaN one fails the same comparison, so both reach the caller as XUD_INVALID_ARG instead of as a
   shape made of NaNs. side2 is cross(side1, axis) exactly as raylib's b2 is, which is what makes
   the winding the indices below are derived against the same for every axis and not upright only.
*/
static XudResult xud_shapes_axis_basis(XudVector3 start, XudVector3 end, float axis[3], float side1[3], float side2[3], float* outLength) {
    float delta[3];
    float least[3];
    float length;

    delta[0] = end.x - start.x;
    delta[1] = end.y - start.y;
    delta[2] = end.z - start.z;
    length = xud_shapes_length(delta);
    if (!(length > 0.0f)) {
        return XUD_INVALID_ARG;
    }
    *outLength = length;
    axis[0] = delta[0] / length;
    axis[1] = delta[1] / length;
    axis[2] = delta[2] / length;

    least[0] = 0.0f;
    least[1] = 0.0f;
    least[2] = 0.0f;
    if ((axis[0] * axis[0]) <= (axis[1] * axis[1]) && (axis[0] * axis[0]) <= (axis[2] * axis[2])) {
        least[0] = 1.0f;
    } else if ((axis[1] * axis[1]) <= (axis[2] * axis[2])) {
        least[1] = 1.0f;
    } else {
        least[2] = 1.0f;
    }

    xud_shapes_cross(axis, least, side1);
    length = xud_shapes_length(side1);
    if (!(length > 0.0f)) {
        return XUD_INVALID_ARG;
    }
    side1[0] /= length;
    side1[1] /= length;
    side1[2] /= length;
    xud_shapes_cross(side1, axis, side2);

    return XUD_OK;
}

/* One vertex of a shape that has no uv and one colour the whole shape shares: the uv and the rgba
   filled the way every other shape in this file fills them. The two round shapes below write a
   hundred and more of these out of three loops, which is why the spelling of one is a function.
*/
static XudResult xud_shapes_round_vertex(XudBatch* batch, const float pos[3], const float normal[3], XudColor color) {
    float uv[2];
    uint8_t rgba[4];

    uv[0] = XUD_SHAPES_UV_S;
    uv[1] = XUD_SHAPES_UV_T;
    rgba[0] = color.r;
    rgba[1] = color.g;
    rgba[2] = color.b;
    rgba[3] = color.a;

    return xud_batch_append_vertex(batch, pos, uv, rgba, normal);
}

/* A vertex of a ring, and the normal the slant of the side carries there: the centre plus RADIUS
   times the unit direction at ANGLE around the frame, and the radial direction corrected by SLOPE -
   (radius at the far end minus radius here) over the axis's length. Correcting rather than ignoring
   the slope is the difference between a cone that shades as one surface and one that shades as a
   stack of rings: raylib's immediate-mode cylinder hands the driver no normal and lets it light each
   triangle flat, which is a look this library cannot copy because solid.frag lights by the
   interpolated normal the vertex carries. The corrected vector is never zero - the radial direction
   is unit and lies across the axis, so no multiple of the axis cancels it - which is why the divide
   below needs no guard the slope's own finite-ness does not already give.
*/
static void xud_shapes_ring_point(float pos[3], float normal[3], const float center[3], float radius, float angle, const float axis[3],
    const float side1[3], const float side2[3], float slope) {
    float radial[3];
    float length;

    radial[0] = (sinf(angle) * side1[0]) + (cosf(angle) * side2[0]);
    radial[1] = (sinf(angle) * side1[1]) + (cosf(angle) * side2[1]);
    radial[2] = (sinf(angle) * side1[2]) + (cosf(angle) * side2[2]);

    pos[0] = center[0] + (radius * radial[0]);
    pos[1] = center[1] + (radius * radial[1]);
    pos[2] = center[2] + (radius * radial[2]);

    normal[0] = radial[0] - (slope * axis[0]);
    normal[1] = radial[1] - (slope * axis[1]);
    normal[2] = radial[2] - (slope * axis[2]);
    length = xud_shapes_length(normal);
    normal[0] /= length;
    normal[1] /= length;
    normal[2] /= length;
}

/* A vertex of a cap: the point at ANGLE around the frame and ALPHA out from the wide ring, on the
   side SIGN of the cap's own centre, with the normal it carries - the same unit direction, because
   a cap is a sphere about its own centre and the direction that places the vertex is the direction
   that shades it. No divide, so no guard: the two terms are unit and across each other.
*/
static void xud_shapes_cap_point(float pos[3], float normal[3], const float center[3], float radius, float angle, float alpha, float sign,
    const float axis[3], const float side1[3], const float side2[3]) {
    float dir[3];

    dir[0] = (sinf(alpha) * sign * axis[0]) + (cosf(alpha) * ((sinf(angle) * side1[0]) + (cosf(angle) * side2[0])));
    dir[1] = (sinf(alpha) * sign * axis[1]) + (cosf(alpha) * ((sinf(angle) * side1[1]) + (cosf(angle) * side2[1])));
    dir[2] = (sinf(alpha) * sign * axis[2]) + (cosf(alpha) * ((sinf(angle) * side1[2]) + (cosf(angle) * side2[2])));

    pos[0] = center[0] + (radius * dir[0]);
    pos[1] = center[1] + (radius * dir[1]);
    pos[2] = center[2] + (radius * dir[2]);
    normal[0] = dir[0];
    normal[1] = dir[1];
    normal[2] = dir[2];
}

/* One cap's vertices: RINGS-1 rings out from the wide ring the caller already appended, then the
   pole, which is the axis direction and not sin/cos of the last row (xud_shapes.c's sphere note:
   sin(PI) is 1.2e-16, and a pole that lands off-axis by that is a normal that lands off-axis too).
*/
static XudResult xud_shapes_append_cap_vertices(XudBatch* batch, const float center[3], float radius, float sign, float step,
    float alphaStep, uint32_t rings, uint32_t slices, const float axis[3], const float side1[3], const float side2[3], XudColor color) {
    float pos[3];
    float normal[3];
    uint32_t ring = 0u;
    uint32_t slice = 0u;
    XudResult result = XUD_OK;

    for (ring = 1u; ring < rings; ring++) {
        float alpha = alphaStep * (float)ring;
        for (slice = 0u; slice < slices; slice++) {
            xud_shapes_cap_point(pos, normal, center, radius, step * (float)slice, alpha, sign, axis, side1, side2);
            result = xud_shapes_round_vertex(batch, pos, normal, color);
            if (result != XUD_OK) {
                return result;
            }
        }
    }

    pos[0] = center[0] + (radius * sign * axis[0]);
    pos[1] = center[1] + (radius * sign * axis[1]);
    pos[2] = center[2] + (radius * sign * axis[2]);
    normal[0] = sign * axis[0];
    normal[1] = sign * axis[1];
    normal[2] = sign * axis[2];

    return xud_shapes_round_vertex(batch, pos, normal, color);
}

/* Both wide rings and both caps' vertices, in the order the filled capsule's counts assume: the
   start ring, the start cap, the end ring, the end cap. The two forms of the capsule share this one
   spelling because xud_shapes.h asserts they are ONE vertex layout - the wire draws lines between
   the vertices the fill shades, and two loops that had drifted by one row would be a wire drawn over
   a gap rather than over an edge.
*/
static XudResult xud_shapes_append_capsule_vertices(XudBatch* batch, const float startC[3], const float endC[3], float radius, float step,
    float alphaStep, uint32_t rings, uint32_t slices, const float axis[3], const float side1[3], const float side2[3], XudColor color) {
    const float* center = startC;
    float pos[3];
    float normal[3];
    uint32_t slice = 0u;
    XudResult result = XUD_OK;

    for (uint32_t cap = 0u; cap < 2u; cap++) {
        center = (cap == 0u) ? startC : endC;
        for (slice = 0u; slice < slices; slice++) {
            xud_shapes_ring_point(pos, normal, center, radius, step * (float)slice, axis, side1, side2, 0.0f);
            result = xud_shapes_round_vertex(batch, pos, normal, color);
            if (result != XUD_OK) {
                return result;
            }
        }
        result = xud_shapes_append_cap_vertices(
            batch, center, radius, (cap == 0u) ? -1.0f : 1.0f, step, alphaStep, rings, slices, axis, side1, side2, color);
        if (result != XUD_OK) {
            return result;
        }
    }

    return XUD_OK;
}

/* The vertex one row of a capsule's mesh is at: row 0 is the wide ring the band shares with the cap,
   row RINGS is the pole, and the rows between are the cap's own. One function because the wire's four
   loops and the filled form's index order both need the same answer.
*/
static uint32_t xud_shapes_capsule_vertex(
    uint32_t ringFirst, uint32_t capFirst, uint32_t row, uint32_t slice, uint32_t rings, uint32_t slices) {
    if (row == 0u) {
        return ringFirst + slice;
    }
    if (row == rings) {
        return capFirst + ((rings - 1u) * slices);
    }

    return capFirst + ((row - 1u) * slices) + slice;
}

XudResult xud_shapes_append_capsule_wires(XudBatch* batch, XudVector3 start, XudVector3 end, float radius, XudColor color) {
    float axis[3];
    float side1[3];
    float side2[3];
    float startC[3];
    float endC[3];
    float delta[3];
    float length = 0.0f;
    float step;
    float alphaStep;
    uint32_t vertexCount = 0u;
    uint32_t indexCount = 0u;
    uint32_t vertexCapacity = 0u;
    uint32_t drawCount = 0u;
    VkDeviceSize stagingBytes = 0u;
    XudResult result = XUD_INVALID_ARG;
    const uint32_t rings = XUD_SHAPES_CAPSULE_RING_COUNT;
    const uint32_t slices = XUD_SHAPES_CAPSULE_SLICE_COUNT;
    const uint32_t capVerts = ((rings - 1u) * slices) + 1u;
    uint32_t first = 0u;
    uint32_t startCap = 0u;
    uint32_t endRing = 0u;
    uint32_t endCap = 0u;
    uint32_t cap = 0u;
    uint32_t row = 0u;
    uint32_t slice = 0u;

    if ((batch == NULL) || !(radius > 0.0f)) {
        return XUD_INVALID_ARG;
    }

    delta[0] = end.x - start.x;
    delta[1] = end.y - start.y;
    delta[2] = end.z - start.z;
    length = xud_shapes_length(delta);
    if (!(length >= 0.0f)) {
        return XUD_INVALID_ARG;
    }
    if (length > 0.0f) {
        result = xud_shapes_axis_basis(start, end, axis, side1, side2, &length);
        if (result != XUD_OK) {
            return result;
        }
    } else {
        axis[0] = 0.0f;
        axis[1] = 1.0f;
        axis[2] = 0.0f;
        side1[0] = 1.0f;
        side1[1] = 0.0f;
        side1[2] = 0.0f;
        side2[0] = 0.0f;
        side2[1] = 0.0f;
        side2[2] = 1.0f;
    }

    result = xud_batch_stats(batch, &vertexCount, &indexCount, &vertexCapacity, &drawCount, &stagingBytes);
    if (result != XUD_OK) {
        return result;
    }
    if (((vertexCount + XUD_SHAPES_CAPSULE_WIRE_VERTEX_COUNT) > vertexCapacity)
        || ((indexCount + XUD_SHAPES_CAPSULE_WIRE_INDEX_COUNT) > batch->indexCapacity)) {
        xud_shapes_note_refused(
            batch, vertexCount + XUD_SHAPES_CAPSULE_WIRE_VERTEX_COUNT, indexCount + XUD_SHAPES_CAPSULE_WIRE_INDEX_COUNT);
        return XUD_OUT_OF_MEMORY;
    }

    result = xud_batch_set_topology(batch, XUD_BATCH_TOPOLOGY_LINES);
    if (result != XUD_OK) {
        return result;
    }

    step = ((2.0f * XUD_SHAPES_SPHERE_PI) / (float)slices);
    alphaStep = ((XUD_SHAPES_SPHERE_PI / 2.0f) / (float)rings);
    first = vertexCount;
    startCap = first + slices;
    endRing = startCap + capVerts;
    endCap = endRing + slices;
    startC[0] = start.x;
    startC[1] = start.y;
    startC[2] = start.z;
    endC[0] = end.x;
    endC[1] = end.y;
    endC[2] = end.z;

    result = xud_shapes_append_capsule_vertices(batch, startC, endC, radius, step, alphaStep, rings, slices, axis, side1, side2, color);
    if (result != XUD_OK) {
        return result;
    }

    /* Every edge of the filled mesh, and nothing that is not: a ring around each row of each cap, a
       longitudinal segment from each row to the next (the last row's meeting at the pole), and the
       band's own longitudinal. The band's ring edges ARE the two caps' row-0 rings, which is why they
       are not drawn twice here and why the count is the header's formula rather than that plus two
       more rings.
    */
    for (cap = 0u; cap < 2u; cap++) {
        const uint32_t ringFirst = (cap == 0u) ? first : endRing;
        const uint32_t capFirst = (cap == 0u) ? startCap : endCap;

        for (row = 0u; row < rings; row++) {
            for (slice = 0u; slice < slices; slice++) {
                result = xud_batch_append_index(batch, xud_shapes_capsule_vertex(ringFirst, capFirst, row, slice, rings, slices));
                if (result != XUD_OK) {
                    return result;
                }
                result = xud_batch_append_index(
                    batch, xud_shapes_capsule_vertex(ringFirst, capFirst, row, (slice + 1u) % slices, rings, slices));
                if (result != XUD_OK) {
                    return result;
                }
            }
        }
        for (row = 0u; row < rings; row++) {
            for (slice = 0u; slice < slices; slice++) {
                result = xud_batch_append_index(batch, xud_shapes_capsule_vertex(ringFirst, capFirst, row, slice, rings, slices));
                if (result != XUD_OK) {
                    return result;
                }
                result = xud_batch_append_index(batch, xud_shapes_capsule_vertex(ringFirst, capFirst, row + 1u, slice, rings, slices));
                if (result != XUD_OK) {
                    return result;
                }
            }
        }
    }

    for (slice = 0u; slice < slices; slice++) {
        result = xud_batch_append_index(batch, first + slice);
        if (result != XUD_OK) {
            return result;
        }
        result = xud_batch_append_index(batch, endRing + slice);
        if (result != XUD_OK) {
            return result;
        }
    }

    return XUD_OK;
}

/* One cap's triangles: the bands from the wide ring out, then the fan from the last row to the pole
   - which is the row of one, so the fan is the last band with its far edge collapsed, exactly as the
   sphere's two fans are. SIGN picks the winding: a cap on the −axis side is written in a frame whose
   handedness the sign flips, so every triangle is named in the opposite order there, which is what
   raylib's "NOTE: make sure cap triangle normals are facing outwards" branch (rmodels.c 636) is
   doing with its two copies of the same fan.
*/
static XudResult xud_shapes_append_cap_indices(
    XudBatch* batch, uint32_t ringFirst, uint32_t capFirst, float sign, uint32_t rings, uint32_t slices) {
    uint32_t row = 0u;
    uint32_t slice = 0u;
    uint32_t last = 0u;
    uint32_t pole = 0u;
    uint32_t lower = 0u;
    uint32_t upper = 0u;
    uint32_t next = 0u;
    uint32_t tri[6];
    XudResult result = XUD_OK;

    for (row = 0u; (row + 1u) < rings; row++) {
        lower = (row == 0u) ? ringFirst : (capFirst + ((row - 1u) * slices));
        upper = capFirst + (row * slices);
        for (slice = 0u; slice < slices; slice++) {
            next = (slice + 1u) % slices;
            if (sign > 0.0f) {
                tri[0] = lower + slice;
                tri[1] = lower + next;
                tri[2] = upper + next;
                tri[3] = lower + slice;
                tri[4] = upper + next;
                tri[5] = upper + slice;
            } else {
                tri[0] = lower + slice;
                tri[1] = upper + next;
                tri[2] = lower + next;
                tri[3] = lower + slice;
                tri[4] = upper + slice;
                tri[5] = upper + next;
            }
            for (uint32_t k = 0u; k < 6u; k++) {
                result = xud_batch_append_index(batch, tri[k]);
                if (result != XUD_OK) {
                    return result;
                }
            }
        }
    }

    last = (rings == 1u) ? ringFirst : (capFirst + ((rings - 2u) * slices));
    pole = capFirst + ((rings - 1u) * slices);
    for (slice = 0u; slice < slices; slice++) {
        next = (slice + 1u) % slices;
        tri[0] = pole;
        tri[1] = (sign > 0.0f) ? (last + slice) : (last + next);
        tri[2] = (sign > 0.0f) ? (last + next) : (last + slice);
        for (uint32_t k = 0u; k < 3u; k++) {
            result = xud_batch_append_index(batch, tri[k]);
            if (result != XUD_OK) {
                return result;
            }
        }
    }

    return XUD_OK;
}

XudResult xud_shapes_append_capsule(XudBatch* batch, XudVector3 start, XudVector3 end, float radius, XudColor color) {
    float axis[3];
    float side1[3];
    float side2[3];
    float startC[3];
    float endC[3];
    float delta[3];
    float length = 0.0f;
    float step;
    float alphaStep;
    uint32_t vertexCount = 0u;
    uint32_t indexCount = 0u;
    uint32_t vertexCapacity = 0u;
    uint32_t drawCount = 0u;
    VkDeviceSize stagingBytes = 0u;
    XudResult result = XUD_INVALID_ARG;
    const uint32_t rings = XUD_SHAPES_CAPSULE_RING_COUNT;
    const uint32_t slices = XUD_SHAPES_CAPSULE_SLICE_COUNT;
    const uint32_t capVerts = ((rings - 1u) * slices) + 1u;
    uint32_t first = 0u;
    uint32_t startCap = 0u;
    uint32_t endRing = 0u;
    uint32_t endCap = 0u;
    uint32_t slice = 0u;
    uint32_t next = 0u;

    if ((batch == NULL) || !(radius > 0.0f)) {
        return XUD_INVALID_ARG;
    }

    delta[0] = end.x - start.x;
    delta[1] = end.y - start.y;
    delta[2] = end.z - start.z;
    length = xud_shapes_length(delta);
    if (!(length >= 0.0f)) {
        return XUD_INVALID_ARG;
    }
    if (length > 0.0f) {
        result = xud_shapes_axis_basis(start, end, axis, side1, side2, &length);
        if (result != XUD_OK) {
            return result;
        }
    } else {
        /* raylib's sphereCase (rmodels.c 790): two centres meeting is a sphere, and the frame the
           two caps are written in is then the caller's to be nothing in particular - +Y is the one
           the band would have had, and the band closes to nothing either way.
        */
        axis[0] = 0.0f;
        axis[1] = 1.0f;
        axis[2] = 0.0f;
        side1[0] = 1.0f;
        side1[1] = 0.0f;
        side1[2] = 0.0f;
        side2[0] = 0.0f;
        side2[1] = 0.0f;
        side2[2] = 1.0f;
    }

    result = xud_batch_stats(batch, &vertexCount, &indexCount, &vertexCapacity, &drawCount, &stagingBytes);
    if (result != XUD_OK) {
        return result;
    }
    if (((vertexCount + XUD_SHAPES_CAPSULE_VERTEX_COUNT) > vertexCapacity)
        || ((indexCount + XUD_SHAPES_CAPSULE_INDEX_COUNT) > batch->indexCapacity)) {
        xud_shapes_note_refused(batch, vertexCount + XUD_SHAPES_CAPSULE_VERTEX_COUNT, indexCount + XUD_SHAPES_CAPSULE_INDEX_COUNT);
        return XUD_OUT_OF_MEMORY;
    }

    result = xud_batch_set_topology(batch, XUD_BATCH_TOPOLOGY_TRIANGLES);
    if (result != XUD_OK) {
        return result;
    }

    step = ((2.0f * XUD_SHAPES_SPHERE_PI) / (float)slices);
    alphaStep = ((XUD_SHAPES_SPHERE_PI / 2.0f) / (float)rings);
    first = vertexCount;
    startCap = first + slices;
    endRing = startCap + capVerts;
    endCap = endRing + slices;

    startC[0] = start.x;
    startC[1] = start.y;
    startC[2] = start.z;
    endC[0] = end.x;
    endC[1] = end.y;
    endC[2] = end.z;

    result = xud_shapes_append_capsule_vertices(batch, startC, endC, radius, step, alphaStep, rings, slices, axis, side1, side2, color);
    if (result != XUD_OK) {
        return result;
    }

    for (slice = 0u; slice < slices; slice++) {
        next = (slice + 1u) % slices;
        uint32_t band[6];

        band[0] = first + slice;
        band[1] = first + next;
        band[2] = endRing + next;
        band[3] = first + slice;
        band[4] = endRing + next;
        band[5] = endRing + slice;
        for (uint32_t k = 0u; k < 6u; k++) {
            result = xud_batch_append_index(batch, band[k]);
            if (result != XUD_OK) {
                return result;
            }
        }
    }

    result = xud_shapes_append_cap_indices(batch, first, startCap, -1.0f, rings, slices);
    if (result != XUD_OK) {
        return result;
    }

    return xud_shapes_append_cap_indices(batch, endRing, endCap, 1.0f, rings, slices);
}

/*-----------------------------------------------------------------------------------------------
 * The cylinder: two rings, two caps, and the 12 indices a side costs
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_shapes_append_cylinder(
    XudBatch* batch, XudVector3 start, XudVector3 end, float radiusStart, float radiusEnd, XudColor color) {
    float axis[3];
    float side1[3];
    float side2[3];
    float center[3];
    float pos[3];
    float normal[3];
    float slope = 0.0f;
    float length = 0.0f;
    float step;
    uint32_t vertexCount = 0u;
    uint32_t indexCount = 0u;
    uint32_t vertexCapacity = 0u;
    uint32_t drawCount = 0u;
    uint64_t vertexNeed = 0ull;
    uint64_t indexNeed = 0ull;
    VkDeviceSize stagingBytes = 0u;
    XudResult result = XUD_INVALID_ARG;
    uint32_t sides = XUD_SHAPES_CYLINDER_SIDE_COUNT;
    uint32_t first = 0u;
    uint32_t cap = 0u;
    uint32_t slot = 0u;
    uint32_t row = 0u;
    uint32_t next = 0u;

    if (batch == NULL) {
        return XUD_INVALID_ARG;
    }
    /* Both radii, read as "not at least zero" so a NaN is refused by the same test that refuses a
       negative one - a negative radius is a ring inside-out, and this rung refuses a mirrored solid
       rather than guessing the caller meant the absolute value (raylib's absf is that guess).
    */
    if (!(radiusStart >= 0.0f) || !(radiusEnd >= 0.0f)) {
        return XUD_INVALID_ARG;
    }

    result = xud_shapes_axis_basis(start, end, axis, side1, side2, &length);
    if (result != XUD_OK) {
        return result;
    }
    slope = (radiusEnd - radiusStart) / length;

    /* The two counts, exactly: a ring at each end of the axis always, and one cap per end THAT HAS
       A RADIUS TO CLOSE - a cone's apex is a point on the axis and the fan over it would be n
       triangles of zero area meeting at the apex the body already reaches.
    */
    vertexNeed = 2ull * sides;
    indexNeed = 6ull * sides;
    if (radiusStart > 0.0f) {
        vertexNeed += sides + 1ull;
        indexNeed += 3ull * sides;
    }
    if (radiusEnd > 0.0f) {
        vertexNeed += sides + 1ull;
        indexNeed += 3ull * sides;
    }

    result = xud_batch_stats(batch, &vertexCount, &indexCount, &vertexCapacity, &drawCount, &stagingBytes);
    if (result != XUD_OK) {
        return result;
    }
    if (((uint64_t)vertexCount + vertexNeed) > (uint64_t)vertexCapacity) {
        xud_shapes_note_refused(batch, (uint64_t)vertexCount + vertexNeed, (uint64_t)indexCount + indexNeed);
        return XUD_OUT_OF_MEMORY;
    }
    if (((uint64_t)indexCount + indexNeed) > (uint64_t)batch->indexCapacity) {
        xud_shapes_note_refused(batch, (uint64_t)vertexCount + vertexNeed, (uint64_t)indexCount + indexNeed);
        return XUD_OUT_OF_MEMORY;
    }

    result = xud_batch_set_topology(batch, XUD_BATCH_TOPOLOGY_TRIANGLES);
    if (result != XUD_OK) {
        return result;
    }

    step = ((2.0f * XUD_SHAPES_SPHERE_PI) / (float)sides);
    first = vertexCount;
    center[0] = start.x;
    center[1] = start.y;
    center[2] = start.z;

    for (row = 0u; row < sides; row++) {
        xud_shapes_ring_point(pos, normal, center, radiusStart, step * (float)row, axis, side1, side2, slope);
        result = xud_shapes_round_vertex(batch, pos, normal, color);
        if (result != XUD_OK) {
            return result;
        }
    }
    center[0] = end.x;
    center[1] = end.y;
    center[2] = end.z;
    for (row = 0u; row < sides; row++) {
        xud_shapes_ring_point(pos, normal, center, radiusEnd, step * (float)row, axis, side1, side2, slope);
        result = xud_shapes_round_vertex(batch, pos, normal, color);
        if (result != XUD_OK) {
            return result;
        }
    }

    /* The band: two triangles a side, (base_i, base_next, top_next) and (base_i, top_next, top_i).
       Derived once against the outward radial normal - the cross product of the first triangle's two
       edges at theta = 0 of an upright cylinder is +Z, which IS the outward normal there - and the
       frame above is the same right-handed one for every axis, so the order holds for a cylinder
       lying along any direction and not for the upright one only.
    */
    for (row = 0u; row < sides; row++) {
        next = first + ((row + 1u) % sides);
        result = xud_batch_append_index(batch, first + row);
        if (result != XUD_OK) {
            return result;
        }
        result = xud_batch_append_index(batch, next);
        if (result != XUD_OK) {
            return result;
        }
        result = xud_batch_append_index(batch, first + sides + ((row + 1u) % sides));
        if (result != XUD_OK) {
            return result;
        }
        result = xud_batch_append_index(batch, first + row);
        if (result != XUD_OK) {
            return result;
        }
        result = xud_batch_append_index(batch, first + sides + ((row + 1u) % sides));
        if (result != XUD_OK) {
            return result;
        }
        result = xud_batch_append_index(batch, first + sides + row);
        if (result != XUD_OK) {
            return result;
        }
    }

    /* The two caps, each a centre and its own rim, each fanned in the direction that answers its
       OWN outward normal: the start cap faces back down the axis, so its fan runs rim_next before
       rim; the end cap's runs the other way. Both are the same rule the plane's winding test reads
       (the cross product of a triangle's edges along the outward normal), applied to +-axis.
    */
    slot = first + (2u * sides);
    for (cap = 0u; cap < 2u; cap++) {
        float radius = (cap == 0u) ? radiusStart : radiusEnd;
        float facing[3];
        uint32_t centre = 0u;

        if (!(radius > 0.0f)) {
            continue;
        }
        center[0] = (cap == 0u) ? start.x : end.x;
        center[1] = (cap == 0u) ? start.y : end.y;
        center[2] = (cap == 0u) ? start.z : end.z;
        facing[0] = (cap == 0u) ? -axis[0] : axis[0];
        facing[1] = (cap == 0u) ? -axis[1] : axis[1];
        facing[2] = (cap == 0u) ? -axis[2] : axis[2];
        centre = slot;
        slot += sides + 1u;

        result = xud_shapes_round_vertex(batch, center, facing, color);
        if (result != XUD_OK) {
            return result;
        }
        for (row = 0u; row < sides; row++) {
            pos[0] = center[0] + (radius * ((sinf(step * (float)row) * side1[0]) + (cosf(step * (float)row) * side2[0])));
            pos[1] = center[1] + (radius * ((sinf(step * (float)row) * side1[1]) + (cosf(step * (float)row) * side2[1])));
            pos[2] = center[2] + (radius * ((sinf(step * (float)row) * side1[2]) + (cosf(step * (float)row) * side2[2])));
            result = xud_shapes_round_vertex(batch, pos, facing, color);
            if (result != XUD_OK) {
                return result;
            }
        }
        for (row = 0u; row < sides; row++) {
            next = (row + 1u) % sides;
            result = xud_batch_append_index(batch, centre);
            if (result != XUD_OK) {
                return result;
            }
            result = xud_batch_append_index(batch, centre + 1u + ((cap == 0u) ? next : row));
            if (result != XUD_OK) {
                return result;
            }
            result = xud_batch_append_index(batch, centre + 1u + ((cap == 0u) ? row : next));
            if (result != XUD_OK) {
                return result;
            }
        }
    }

    return XUD_OK;
}

XudResult xud_shapes_append_cylinder_wires(
    XudBatch* batch, XudVector3 start, XudVector3 end, float radiusStart, float radiusEnd, XudColor color) {
    float axis[3];
    float side1[3];
    float side2[3];
    float center[3];
    float pos[3];
    float normal[3];
    float slope = 0.0f;
    float length = 0.0f;
    float step;
    uint32_t vertexCount = 0u;
    uint32_t indexCount = 0u;
    uint32_t vertexCapacity = 0u;
    uint32_t drawCount = 0u;
    VkDeviceSize stagingBytes = 0u;
    XudResult result = XUD_INVALID_ARG;
    uint32_t sides = XUD_SHAPES_CYLINDER_SIDE_COUNT;
    uint32_t first = 0u;
    uint32_t row = 0u;
    uint32_t next = 0u;

    if (batch == NULL) {
        return XUD_INVALID_ARG;
    }
    if (!(radiusStart >= 0.0f) || !(radiusEnd >= 0.0f)) {
        return XUD_INVALID_ARG;
    }

    result = xud_shapes_axis_basis(start, end, axis, side1, side2, &length);
    if (result != XUD_OK) {
        return result;
    }
    slope = (radiusEnd - radiusStart) / length;

    result = xud_batch_stats(batch, &vertexCount, &indexCount, &vertexCapacity, &drawCount, &stagingBytes);
    if (result != XUD_OK) {
        return result;
    }
    if (((uint32_t)(vertexCount + XUD_SHAPES_CYLINDER_WIRE_VERTEX_COUNT) > vertexCapacity)
        || ((uint32_t)(indexCount + XUD_SHAPES_CYLINDER_WIRE_INDEX_COUNT) > batch->indexCapacity)) {
        xud_shapes_note_refused(
            batch, vertexCount + XUD_SHAPES_CYLINDER_WIRE_VERTEX_COUNT, indexCount + XUD_SHAPES_CYLINDER_WIRE_INDEX_COUNT);
        return XUD_OUT_OF_MEMORY;
    }

    result = xud_batch_set_topology(batch, XUD_BATCH_TOPOLOGY_LINES);
    if (result != XUD_OK) {
        return result;
    }

    step = ((2.0f * XUD_SHAPES_SPHERE_PI) / (float)sides);
    first = vertexCount;
    center[0] = start.x;
    center[1] = start.y;
    center[2] = start.z;
    for (row = 0u; row < sides; row++) {
        xud_shapes_ring_point(pos, normal, center, radiusStart, step * (float)row, axis, side1, side2, slope);
        result = xud_shapes_round_vertex(batch, pos, normal, color);
        if (result != XUD_OK) {
            return result;
        }
    }
    center[0] = end.x;
    center[1] = end.y;
    center[2] = end.z;
    for (row = 0u; row < sides; row++) {
        xud_shapes_ring_point(pos, normal, center, radiusEnd, step * (float)row, axis, side1, side2, slope);
        result = xud_shapes_round_vertex(batch, pos, normal, color);
        if (result != XUD_OK) {
            return result;
        }
    }

    /* Three lines a side, over the two rings only: the base rim, the vertical that leaves it, and the
       top rim the vertical arrives on - raylib's DrawCylinderWires (rmodels.c 706) line for line, the
       difference being that its six vertices a side are six and these two rings are shared.
    */
    for (row = 0u; row < sides; row++) {
        next = (row + 1u) % sides;
        result = xud_batch_append_index(batch, first + row);
        if (result != XUD_OK) {
            return result;
        }
        result = xud_batch_append_index(batch, first + next);
        if (result != XUD_OK) {
            return result;
        }
        result = xud_batch_append_index(batch, first + row);
        if (result != XUD_OK) {
            return result;
        }
        result = xud_batch_append_index(batch, first + sides + row);
        if (result != XUD_OK) {
            return result;
        }
        result = xud_batch_append_index(batch, first + sides + row);
        if (result != XUD_OK) {
            return result;
        }
        result = xud_batch_append_index(batch, first + sides + next);
        if (result != XUD_OK) {
            return result;
        }
    }

    return XUD_OK;
}

XudResult xud_shapes_append_grid(
    XudBatch* batch, XudVector3 center, uint32_t divisions, float spacing, XudColor centerColor, XudColor gridColor) {
    float pos[3];
    float normal[3];
    uint32_t vertexCount = 0u;
    uint32_t indexCount = 0u;
    uint32_t vertexCapacity = 0u;
    uint32_t drawCount = 0u;
    uint64_t vertexNeed = 0ull;
    uint64_t indexNeed = 0ull;
    VkDeviceSize stagingBytes = 0u;
    XudResult result = XUD_INVALID_ARG;
    float half = 0.0f;
    float offset = 0.0f;
    uint32_t line = 0u;
    uint32_t next = 0u;

    if ((batch == NULL) || (divisions < XUD_SHAPES_GRID_DIVISIONS_MIN) || !(spacing > 0.0f)) {
        return XUD_INVALID_ARG;
    }

    vertexNeed = 4ull * ((uint64_t)divisions + 1ull);
    indexNeed = vertexNeed;

    result = xud_batch_stats(batch, &vertexCount, &indexCount, &vertexCapacity, &drawCount, &stagingBytes);
    if (result != XUD_OK) {
        return result;
    }
    if ((((uint64_t)vertexCount + vertexNeed) > (uint64_t)vertexCapacity)
        || (((uint64_t)indexCount + indexNeed) > (uint64_t)batch->indexCapacity)) {
        xud_shapes_note_refused(batch, (uint64_t)vertexCount + vertexNeed, (uint64_t)indexCount + indexNeed);
        return XUD_OUT_OF_MEMORY;
    }

    result = xud_batch_set_topology(batch, XUD_BATCH_TOPOLOGY_LINES);
    if (result != XUD_OK) {
        return result;
    }

    normal[0] = XUD_SHAPES_GROUND_NORMAL_X;
    normal[1] = XUD_SHAPES_GROUND_NORMAL_Y;
    normal[2] = XUD_SHAPES_GROUND_NORMAL_Z;
    half = (spacing * (float)divisions) / 2.0f;
    /* The cursor the two vertices of each line are indexed against: the count the fit check read is
       the grid's first vertex, and every line after it starts two further on, so the pairs are the
       cursor and the cursor plus one rather than an offset rebuilt from the division's index.
    */
    next = vertexCount;

    /* Two passes over the same division count: lines parallel to x at each z, then lines parallel to
       z at each x. The centre line of each pass is the one whose offset is exactly zero - the test is
       on the index rather than on the offset's float so that the axis pair is the caller's colour for
       an even divisions and not whichever line happens to round closest to zero.
    */
    for (uint32_t way = 0u; way < 2u; way++) {
        for (line = 0u; line <= divisions; line++) {
            XudColor color = ((line * 2u) == divisions) ? centerColor : gridColor;
            offset = (half * -1.0f) + (spacing * (float)line);

            pos[0] = (way == 0u) ? (center.x - half) : (center.x + offset);
            pos[1] = center.y;
            pos[2] = (way == 0u) ? (center.z + offset) : (center.z - half);
            result = xud_shapes_round_vertex(batch, pos, normal, color);
            if (result != XUD_OK) {
                return result;
            }
            pos[0] = (way == 0u) ? (center.x + half) : (center.x + offset);
            pos[2] = (way == 0u) ? (center.z + offset) : (center.z + half);
            result = xud_shapes_round_vertex(batch, pos, normal, color);
            if (result != XUD_OK) {
                return result;
            }
            result = xud_batch_append_index(batch, next);
            if (result != XUD_OK) {
                return result;
            }
            result = xud_batch_append_index(batch, next + 1u);
            if (result != XUD_OK) {
                return result;
            }
            next += 2u;
        }
    }

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The glyph quad: the first shape here whose uv is the caller's rather than a constant
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_shapes_append_glyph_quad(
    XudBatch* batch, XudVector3 corner0, XudVector3 corner1, XudVector2 uv0, XudVector2 uv1, XudColor color) {
    float pos[3];
    float uv[2];
    float normal[3];
    float cornerX[XUD_SHAPES_GLYPH_VERTEX_COUNT];
    float cornerY[XUD_SHAPES_GLYPH_VERTEX_COUNT];
    float cornerU[XUD_SHAPES_GLYPH_VERTEX_COUNT];
    float cornerV[XUD_SHAPES_GLYPH_VERTEX_COUNT];
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

    /* The box has to be a box, and the two comparisons are the top-down reading the header's
       pairing note states: corner0 is the corner with the SMALLER x and the LARGER y, so a proper
       box is corner1.x above corner0.x AND corner0.y above corner1.y. A quad with no area has no
       front face and no texels of its own, and both comparisons refuse a NaN coordinate rather
       than letting one into a float stream - the reason the sphere's note gives for a NaN radius,
       one rung up. */
    if (!((corner1.x > corner0.x) && (corner0.y > corner1.y))) {
        return XUD_INVALID_ARG;
    }

    /* The four vertices carry the flat +Z normal, which is the surface's normal of a FLAT quad
       and of nothing else; a caller who wants a glyph that does not face the camera wants the
       transform step above this one, and a warped quad is a shape whose normal is a lie. The
       equality is exact rather than a tolerance because the caller passes the plane's z twice: a
       pair that is not the same number twice is not a rounding artefact, it is two different
       planes, and a NaN z is refused here too (NaN != NaN). */
    if (corner0.z != corner1.z) {
        return XUD_INVALID_ARG;
    }

    /* The batch's counters, read through the rung's own accessor for the same reason the other
       appends read them: this is the half that makes the append atomic. */
    result = xud_batch_stats(batch, &vertexCount, &indexCount, &vertexCapacity, &drawCount, &stagingBytes);
    if (result != XUD_OK) {
        return result;
    }

    /* Both bounds, before anything at all changes - including the topology state below, which is
       why a quad that does not fit cannot split a run. A glyph quad spends 4 vertices and 6
       indices, 1.5 per vertex, so it is the box's closed-per-face case rather than the triangle's
       1:1 one, and either bound can be the one that trips: a batch whose existing content reuses
       vertices is full on indices with four vertices still to spare. */
    if ((vertexCount + XUD_SHAPES_GLYPH_VERTEX_COUNT) > vertexCapacity) {
        xud_shapes_note_refused(batch, vertexCount + XUD_SHAPES_GLYPH_VERTEX_COUNT, indexCount + XUD_SHAPES_GLYPH_INDEX_COUNT);
        return XUD_OUT_OF_MEMORY;
    }
    if ((indexCount + XUD_SHAPES_GLYPH_INDEX_COUNT) > batch->indexCapacity) {
        xud_shapes_note_refused(batch, vertexCount + XUD_SHAPES_GLYPH_VERTEX_COUNT, indexCount + XUD_SHAPES_GLYPH_INDEX_COUNT);
        return XUD_OUT_OF_MEMORY;
    }

    /* The topology moves FIRST, and before a byte is written: the run that was pending under the
       old topology closes into a row of the same batch, and this quad's six indices start the next
       one. A frame of glyphs after a frame of lines is two rows, and the frame loop binds a
       pipeline beside each. */
    result = xud_batch_set_topology(batch, XUD_BATCH_TOPOLOGY_TRIANGLES);
    if (result != XUD_OK) {
        return result;
    }

    /* The four corners, spelled out once rather than in four append calls: the order is the
       counter-clockwise one the pipeline's front face names, seen from +Z, starting at the corner
       the caller's corner1 and uv1 pair (the bottom-right of the quad, the diagonally opposite
       corner to corner0/uv0) and walking right edge up, top edge left, left edge down. The two
       mixed corners take the uv of the corner on the same EDGE as them rather than the one on the
       same diagonal, which is what keeps the atlas box upright on the quad - see the pairing note
       beside the counts in the header for why the two boxes are both top-down. */
    cornerX[0] = corner1.x;
    cornerX[1] = corner1.x;
    cornerX[2] = corner0.x;
    cornerX[3] = corner0.x;
    cornerY[0] = corner1.y;
    cornerY[1] = corner0.y;
    cornerY[2] = corner0.y;
    cornerY[3] = corner1.y;
    cornerU[0] = uv1.x;
    cornerU[1] = uv1.x;
    cornerU[2] = uv0.x;
    cornerU[3] = uv0.x;
    cornerV[0] = uv1.y;
    cornerV[1] = uv0.y;
    cornerV[2] = uv0.y;
    cornerV[3] = uv1.y;

    normal[0] = XUD_SHAPES_FLAT_NORMAL_X;
    normal[1] = XUD_SHAPES_FLAT_NORMAL_Y;
    normal[2] = XUD_SHAPES_FLAT_NORMAL_Z;
    rgba[0] = color.r;
    rgba[1] = color.g;
    rgba[2] = color.b;
    rgba[3] = color.a;

    first = vertexCount;

    for (row = 0u; row < XUD_SHAPES_GLYPH_VERTEX_COUNT; row++) {
        pos[0] = cornerX[row];
        pos[1] = cornerY[row];
        pos[2] = corner0.z;
        uv[0] = cornerU[row];
        uv[1] = cornerV[row];
        result = xud_batch_append_vertex(batch, pos, uv, rgba, normal);
        if (result != XUD_OK) {
            return result;
        }
    }

    /* The same two triangles one face of a box is closed by, naming the four vertices this call
       just appended: a glyph quad and a box face are the same quad, which is the claim the header
       asserts about the two index counts. */
    for (row = 0u; row < XUD_SHAPES_GLYPH_INDEX_COUNT; row++) {
        result = xud_batch_append_index(batch, first + XUD_SHAPES_CUBE_TRIANGLE_CORNER[row]);
        if (result != XUD_OK) {
            return result;
        }
    }

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The pixel to clip - the screen-space draw's corner math
 *-----------------------------------------------------------------------------------------------*/

/* The inverse of the vertex stage's own final Y-flip, the way xud_ui_map_vertex states it: the
   top row of the framebuffer is the clip's +1 and the bottom its -1, and the X axis divides the
   same way with the left edge at -1. The divide is the extent's, not a constant, because the
   extent is the swapchain's imageExtent the window layer reports and it changes with the window,
   and the NaN a zero extent would make is refused by the check rather than divided into. */
XudResult xud_shapes_pixel_to_clip(uint32_t extentWidth, uint32_t extentHeight, float x, float y, float* outClipX, float* outClipY) {
    if ((extentWidth == 0u) || (extentHeight == 0u) || (outClipX == NULL) || (outClipY == NULL)) {
        return XUD_INVALID_ARG;
    }

    *outClipX = (2.0f * x) / (float)extentWidth - 1.0f;
    *outClipY = 1.0f - (2.0f * y) / (float)extentHeight;

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The one guard every public draw call above this rung needs
 *-----------------------------------------------------------------------------------------------*/

/* The guard is one function rather than one block per public call because every public draw call
   above this rung reaches the same two fields of the same owner and answers the same two codes:
   a NULL context is a bad argument and a context missing the rungs it needs is a context that
   cannot be drawn into at all. It is file-local because it is not the layer's contract - the
   contract is the shapes and the public calls, and this is the half they all share. */
static XudResult xud_shapes_context_batch(XudBatch** outBatch, XudContext* context) {
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

    *outBatch = context->renderer->batch;

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The public calls above it
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_draw_triangle_3d(XudContext* context, XudVector3 v0, XudVector3 v1, XudVector3 v2, XudColor color) {
    XudBatch* batch = NULL;
    XudResult result = XUD_INVALID_ARG;

    result = xud_shapes_context_batch(&batch, context);
    if (result != XUD_OK) {
        return result;
    }

    return xud_shapes_append_triangle(batch, v0, v1, v2, color);
}

XudResult xud_draw_cube(XudContext* context, XudVector3 position, float width, float height, float length, XudColor color) {
    XudBatch* batch = NULL;
    XudResult result = XUD_INVALID_ARG;

    result = xud_shapes_context_batch(&batch, context);
    if (result != XUD_OK) {
        return result;
    }

    return xud_shapes_append_cube(batch, position, width, height, length, color);
}

XudResult xud_draw_sphere(XudContext* context, XudVector3 centerPos, float radius, XudColor color) {
    XudBatch* batch = NULL;
    XudResult result = XUD_INVALID_ARG;

    result = xud_shapes_context_batch(&batch, context);
    if (result != XUD_OK) {
        return result;
    }

    return xud_shapes_append_sphere(batch, centerPos, radius, XUD_SHAPES_SPHERE_RING_COUNT, XUD_SHAPES_SPHERE_SLICE_COUNT, color);
}

XudResult xud_draw_line_3d(XudContext* context, XudVector3 startPos, XudVector3 endPos, XudColor color) {
    XudBatch* batch = NULL;
    XudResult result = XUD_INVALID_ARG;

    result = xud_shapes_context_batch(&batch, context);
    if (result != XUD_OK) {
        return result;
    }

    return xud_shapes_append_line(batch, startPos, endPos, color);
}

XudResult xud_draw_ray(XudContext* context, XudRay ray, XudColor color) {
    XudBatch* batch = NULL;
    XudVector3 end;
    XudResult result = XUD_INVALID_ARG;

    result = xud_shapes_context_batch(&batch, context);
    if (result != XUD_OK) {
        return result;
    }

    /* The line's own append, with the far end where the ray's direction puts it: no normalisation
       here, because a caller that handed an unnormalised direction asked for that length.
    */
    end.x = ray.position.x + (ray.direction.x * XUD_SHAPES_RAY_LENGTH);
    end.y = ray.position.y + (ray.direction.y * XUD_SHAPES_RAY_LENGTH);
    end.z = ray.position.z + (ray.direction.z * XUD_SHAPES_RAY_LENGTH);

    return xud_shapes_append_line(batch, ray.position, end, color);
}

XudResult xud_draw_point_3d(XudContext* context, XudVector3 position, XudColor color) {
    XudBatch* batch = NULL;
    XudVector3 start;
    XudVector3 end;
    XudResult result = XUD_INVALID_ARG;

    result = xud_shapes_context_batch(&batch, context);
    if (result != XUD_OK) {
        return result;
    }

    /* The point IS a line, either side of the caller's position along z - see the note beside
       XUD_SHAPES_POINT_HALF_LENGTH for why the point is a line at all and why the axis is z. */
    start = position;
    start.z -= XUD_SHAPES_POINT_HALF_LENGTH;
    end = position;
    end.z += XUD_SHAPES_POINT_HALF_LENGTH;

    return xud_shapes_append_line(batch, start, end, color);
}

XudResult xud_draw_cube_wires(XudContext* context, XudVector3 position, float width, float height, float length, XudColor color) {
    XudBatch* batch = NULL;
    XudResult result = XUD_INVALID_ARG;

    result = xud_shapes_context_batch(&batch, context);
    if (result != XUD_OK) {
        return result;
    }

    return xud_shapes_append_cube_wires(batch, position, width, height, length, color);
}

XudResult xud_draw_bounding_box(XudContext* context, XudBoundingBox box, XudColor color) {
    XudBatch* batch = NULL;
    XudVector3 center;
    XudVector3 size;
    XudResult result = XUD_INVALID_ARG;

    result = xud_shapes_context_batch(&batch, context);
    if (result != XUD_OK) {
        return result;
    }

    /* raylib's construction (rmodels.c 4145-4153): the absolute size on every axis and one wire cube
       over it, which is the whole of its DrawBoundingBox. The centre is the midpoint of the two
       corners rather than the minimum plus half that size - raylib's line 4151 lands a box whose
       corners came in the wrong order on the far side of its own minimum. A reversed pair still
       draws, and that is the one place this call is the collision rung's opposite: a test over a
       reversed box has no answer, so that rung refuses it, while a wire cube over an absolute size
       always has one.
    */
    size.x = fabsf(box.max.x - box.min.x);
    size.y = fabsf(box.max.y - box.min.y);
    size.z = fabsf(box.max.z - box.min.z);
    center.x = (box.min.x + box.max.x) * 0.5f;
    center.y = (box.min.y + box.max.y) * 0.5f;
    center.z = (box.min.z + box.max.z) * 0.5f;

    return xud_shapes_append_cube_wires(batch, center, size.x, size.y, size.z, color);
}

XudResult xud_draw_sphere_wires(XudContext* context, XudVector3 centerPos, float radius, XudColor color) {
    XudBatch* batch = NULL;
    XudResult result = XUD_INVALID_ARG;

    result = xud_shapes_context_batch(&batch, context);
    if (result != XUD_OK) {
        return result;
    }

    return xud_shapes_append_sphere_wires(batch, centerPos, radius, XUD_SHAPES_SPHERE_RING_COUNT, XUD_SHAPES_SPHERE_SLICE_COUNT, color);
}

XudResult xud_draw_plane(XudContext* context, XudVector3 centerPos, XudVector2 size, XudColor color) {
    XudBatch* batch = NULL;
    XudResult result = XUD_INVALID_ARG;

    result = xud_shapes_context_batch(&batch, context);
    if (result != XUD_OK) {
        return result;
    }

    return xud_shapes_append_plane(batch, centerPos, size, color);
}

XudResult xud_draw_cylinder(XudContext* context, XudVector3 position, float radiusTop, float radiusBottom, float height, XudColor color) {
    XudBatch* batch = NULL;
    XudVector3 start;
    XudVector3 end;
    XudResult result = XUD_INVALID_ARG;

    result = xud_shapes_context_batch(&batch, context);
    if (result != XUD_OK) {
        return result;
    }

    /* raylib's DrawCylinder (rmodels.c 588) is the upright form of the one shape below: its position
       is the BASE, its height runs up +Y, and the axis the append takes is the segment between.
    */
    start.x = position.x;
    start.y = position.y;
    start.z = position.z;
    end.x = position.x;
    end.y = position.y + height;
    end.z = position.z;

    return xud_shapes_append_cylinder(batch, start, end, radiusBottom, radiusTop, color);
}

XudResult xud_draw_cylinder_wires(
    XudContext* context, XudVector3 position, float radiusTop, float radiusBottom, float height, XudColor color) {
    XudBatch* batch = NULL;
    XudVector3 start;
    XudVector3 end;
    XudResult result = XUD_INVALID_ARG;

    result = xud_shapes_context_batch(&batch, context);
    if (result != XUD_OK) {
        return result;
    }

    start.x = position.x;
    start.y = position.y;
    start.z = position.z;
    end.x = position.x;
    end.y = position.y + height;
    end.z = position.z;

    return xud_shapes_append_cylinder_wires(batch, start, end, radiusBottom, radiusTop, color);
}

XudResult xud_draw_capsule(XudContext* context, XudVector3 startPos, XudVector3 endPos, float radius, XudColor color) {
    XudBatch* batch = NULL;
    XudResult result = XUD_INVALID_ARG;

    result = xud_shapes_context_batch(&batch, context);
    if (result != XUD_OK) {
        return result;
    }

    return xud_shapes_append_capsule(batch, startPos, endPos, radius, color);
}

XudResult xud_draw_capsule_wires(XudContext* context, XudVector3 startPos, XudVector3 endPos, float radius, XudColor color) {
    XudBatch* batch = NULL;
    XudResult result = XUD_INVALID_ARG;

    result = xud_shapes_context_batch(&batch, context);
    if (result != XUD_OK) {
        return result;
    }

    return xud_shapes_append_capsule_wires(batch, startPos, endPos, radius, color);
}

XudResult xud_draw_grid(
    XudContext* context, XudVector3 centerPos, uint32_t divisions, float spacing, XudColor centerColor, XudColor gridColor) {
    XudBatch* batch = NULL;
    XudResult result = XUD_INVALID_ARG;

    result = xud_shapes_context_batch(&batch, context);
    if (result != XUD_OK) {
        return result;
    }

    return xud_shapes_append_grid(batch, centerPos, divisions, spacing, centerColor, gridColor);
}

/* The plain half of the 2D surface: the caller's own rectangle as one screen-space quad at the
   caller's pixel position, through the pipeline list's UI column the same way xud_draw_texture()
   draws its texture's extent - the state swap moves the batch to the UI row for the quad and back
   when it is done, so the quad flushes as one row of its own and the shapes appended beside it
   draw with the state the call found. The quad's full box in uv space is the constant [0, 1], the
   whole of the slot-0 white texture the untextured row samples, which is why the uv is a constant
   here rather than a caller's box as in the glyph walk above. */
XudResult xud_draw_rectangle(XudContext* context, int32_t posX, int32_t posY, int32_t width, int32_t height, XudColor color) {
    XudBatch* batch = NULL;
    XudVector3 corner0;
    XudVector3 corner1;
    XudVector2 uv0;
    XudVector2 uv1;
    uint32_t oldVariant = 0u;
    uint32_t oldMaterial = 0u;
    uint32_t oldTexture = 0u;
    uint32_t extentWidth = 0u;
    uint32_t extentHeight = 0u;
    float clipX0 = 0.0f;
    float clipY0 = 0.0f;
    float clipX1 = 0.0f;
    float clipY1 = 0.0f;
    XudResult result = XUD_INVALID_ARG;

    result = xud_shapes_context_batch(&batch, context);
    if (result != XUD_OK) {
        return result;
    }

    /* The extent the caller's rectangle occupies: a zero or negative one is a degenerate quad the
       append would refuse corner-by-corner anyway, and it is refused here before any state is
       moved, for the same reason the textured half refuses a zero framebuffer extent - a degenerate
       number is a bad argument rather than a draw. */
    if ((width <= 0) || (height <= 0)) {
        return XUD_INVALID_ARG;
    }

    /* The extent the quad's pixels divide by is the framebuffer currently being drawn into: the
       active scope's own while a render target is open, the window's swapchain extent otherwise -
       the one rule xud_renderer_pixel_extent states. A zero extent is the state a window with no
       presentable extent is in, and a divide by it is an inf in a float stream rather than a
       draw. */
    result = xud_renderer_pixel_extent(context->renderer, context->width, context->height, &extentWidth, &extentHeight);
    if (result != XUD_OK) {
        return result;
    }
    if ((extentWidth == 0u) || (extentHeight == 0u)) {
        return XUD_INVALID_ARG;
    }

    /* The two corners in pixels, the top-left and the bottom-right of the caller's extent at the
       caller's position, mapped to clip before they become the corners the quad takes: the
       rectangle's full box in uv space is the quad's, so the uv is the whole of [0, 1]. */
    result = xud_shapes_pixel_to_clip(extentWidth, extentHeight, (float)posX, (float)posY, &clipX0, &clipY0);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_shapes_pixel_to_clip(extentWidth, extentHeight, (float)posX + (float)width, (float)posY + (float)height, &clipX1, &clipY1);
    if (result != XUD_OK) {
        return result;
    }
    corner0.x = clipX0;
    corner0.y = clipY0;
    corner0.z = 0.0f;
    corner1.x = clipX1;
    corner1.y = clipY1;
    corner1.z = 0.0f;
    uv0.x = 0.0f;
    uv0.y = 0.0f;
    uv1.x = 1.0f;
    uv1.y = 1.0f;

    result = xud_batch_state_get(batch, &oldVariant, &oldMaterial, &oldTexture);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_batch_set_material(batch, XUD_BATCH_NO_MATERIAL, XUD_BATCH_VARIANT_UI, XUD_BATCH_NO_TEXTURE);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_shapes_append_glyph_quad(batch, corner0, corner1, uv0, uv1, color);
    if (result != XUD_OK) {
        (void)xud_batch_set_material(batch, oldMaterial, oldVariant, oldTexture);
        return result;
    }
    result = xud_batch_set_material(batch, oldMaterial, oldVariant, oldTexture);

    return result;
}

/* The 2D line: the two endpoints at the caller's own pixels, mapped to clip at the framebuffer's
   extent and appended as the batch's one line shape through the pipeline list's UI column the
   same way the rectangle above does - the state swap moves the batch to the UI row for the line
   and back when it is done, so the line flushes as one row of its own and the shapes appended
   beside it draw with the state the call found. The append moves the batch's topology to the
   line's own before a byte is written, and it is the next shape the frame appends that moves it
   back, the same way the 3D line pair above does it, which is why the restore here takes back
   only the material triple the swap took and leaves the topology the append set. */
XudResult xud_draw_line(XudContext* context, int32_t startPosX, int32_t startPosY, int32_t endPosX, int32_t endPosY, XudColor color) {
    XudBatch* batch = NULL;
    XudVector3 start;
    XudVector3 end;
    uint32_t oldVariant = 0u;
    uint32_t oldMaterial = 0u;
    uint32_t oldTexture = 0u;
    uint32_t extentWidth = 0u;
    uint32_t extentHeight = 0u;
    float clipX = 0.0f;
    float clipY = 0.0f;
    XudResult result = XUD_INVALID_ARG;

    result = xud_shapes_context_batch(&batch, context);
    if (result != XUD_OK) {
        return result;
    }

    /* The extent the endpoints divide by is the framebuffer currently being drawn into, for the
       same reason the rectangle above refuses a zero one: a divide by it is an inf in a float
       stream rather than a draw. */
    result = xud_renderer_pixel_extent(context->renderer, context->width, context->height, &extentWidth, &extentHeight);
    if (result != XUD_OK) {
        return result;
    }
    if ((extentWidth == 0u) || (extentHeight == 0u)) {
        return XUD_INVALID_ARG;
    }

    result = xud_shapes_pixel_to_clip(extentWidth, extentHeight, (float)startPosX, (float)startPosY, &clipX, &clipY);
    if (result != XUD_OK) {
        return result;
    }
    start.x = clipX;
    start.y = clipY;
    start.z = 0.0f;
    result = xud_shapes_pixel_to_clip(extentWidth, extentHeight, (float)endPosX, (float)endPosY, &clipX, &clipY);
    if (result != XUD_OK) {
        return result;
    }
    end.x = clipX;
    end.y = clipY;
    end.z = 0.0f;

    result = xud_batch_state_get(batch, &oldVariant, &oldMaterial, &oldTexture);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_batch_set_material(batch, XUD_BATCH_NO_MATERIAL, XUD_BATCH_VARIANT_UI, XUD_BATCH_NO_TEXTURE);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_shapes_append_line(batch, start, end, color);
    if (result != XUD_OK) {
        (void)xud_batch_set_material(batch, oldMaterial, oldVariant, oldTexture);
        return result;
    }
    result = xud_batch_set_material(batch, oldMaterial, oldVariant, oldTexture);

    return result;
}

/* The pixel: one pixel of geometry, which the filled rectangle above is at a one-pixel extent,
   so the call is that rectangle at width and height one rather than a second append of the same
   quad: the extent check, the zero-extent refusal and the quad it draws are the rectangle's own.
 */
XudResult xud_draw_pixel(XudContext* context, int32_t posX, int32_t posY, XudColor color) {
    return xud_draw_rectangle(context, posX, posY, 1, 1, color);
}

/* The rectangle outline: the four edges of the caller's rectangle as the batch's four lines
   through the UI column the line above does, the four appends sharing one state swap rather than
   four, so the outline flushes as one row of its own instead of four, the same one-run agreement
   the glyph walk above states for a run of glyphs. The endpoints carry the same half-pixel inset
   raylib's outline applies - its 0.5 over the transform's scale, which at the screen's own
   transform is half a pixel - because an edge that sat on a whole pixel row would straddle the
   boundary of two rows, and the inset puts the line's centre on the row it names. An edge that
   does not fit is refused with the edges before it already appended, the same partial-draw
   agreement the glyph walk states, and the state the call found is restored on every path the
   append left it moved. */
XudResult xud_draw_rectangle_lines(XudContext* context, int32_t posX, int32_t posY, int32_t width, int32_t height, XudColor color) {
    XudBatch* batch = NULL;
    XudVector3 corner[4u];
    uint32_t oldVariant = 0u;
    uint32_t oldMaterial = 0u;
    uint32_t oldTexture = 0u;
    uint32_t extentWidth = 0u;
    uint32_t extentHeight = 0u;
    float clipX = 0.0f;
    float clipY = 0.0f;
    uint32_t edge = 0u;
    const float insetLeft = (float)posX + 0.5f;
    const float insetRight = (float)posX + (float)width - 0.5f;
    const float insetTop = (float)posY + 0.5f;
    const float insetBottom = (float)posY + (float)height - 0.5f;
    /* The corners in pixels, top-left first, the order raylib's own vertex sequence walks them:
       the top edge left to right, the right edge top to bottom, the bottom edge right to left,
       and the left edge bottom to top. */
    const float cornerX[4u] = { insetLeft, insetRight, insetRight, insetLeft };
    const float cornerY[4u] = { insetTop, insetTop, insetBottom, insetBottom };
    XudResult result = XUD_INVALID_ARG;

    result = xud_shapes_context_batch(&batch, context);
    if (result != XUD_OK) {
        return result;
    }

    /* A zero or negative extent is a degenerate outline the append would draw as overlapping or
       zero-length edges, and it is refused here before any state is moved, the same refusal the
       filled rectangle above makes of the same numbers. */
    if ((width <= 0) || (height <= 0)) {
        return XUD_INVALID_ARG;
    }

    result = xud_renderer_pixel_extent(context->renderer, context->width, context->height, &extentWidth, &extentHeight);
    if (result != XUD_OK) {
        return result;
    }
    if ((extentWidth == 0u) || (extentHeight == 0u)) {
        return XUD_INVALID_ARG;
    }

    for (edge = 0u; edge < 4u; edge++) {
        result = xud_shapes_pixel_to_clip(extentWidth, extentHeight, cornerX[edge], cornerY[edge], &clipX, &clipY);
        if (result != XUD_OK) {
            return result;
        }
        corner[edge].x = clipX;
        corner[edge].y = clipY;
        corner[edge].z = 0.0f;
    }

    result = xud_batch_state_get(batch, &oldVariant, &oldMaterial, &oldTexture);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_batch_set_material(batch, XUD_BATCH_NO_MATERIAL, XUD_BATCH_VARIANT_UI, XUD_BATCH_NO_TEXTURE);
    if (result != XUD_OK) {
        return result;
    }

    for (edge = 0u; edge < 4u; edge++) {
        result = xud_shapes_append_line(batch, corner[edge], corner[(edge + 1u) % 4u], color);
        if (result != XUD_OK) {
            (void)xud_batch_set_material(batch, oldMaterial, oldVariant, oldTexture);
            return result;
        }
    }

    result = xud_batch_set_material(batch, oldMaterial, oldVariant, oldTexture);

    return result;
}

/* The 2D triangle: the caller's three own screen-space points, in the order the caller named
   them, mapped to clip at the framebuffer's extent and appended as the batch's triangle shape
   through the pipeline list's UI column the rectangle and the line above do - the state swap
   moves the batch to the UI row for the triangle and back when it is done, so the triangle
   flushes as one row of its own and the shapes appended beside it draw with the state the call
   found. The append moves the batch's topology to the triangle's own before a byte is written,
   and it is the next shape the frame appends that moves it back, the same way the line above does
   it, which is why the restore here takes back only the material triple the swap took and leaves
   the topology the append set. The append is the 3D triangle's own: the three vertices in the
   order the caller named them and the three indices that name them, its constant uv and its flat
   +Z normal, which the untextured UI row samples to white as the rectangle above's quad does. */
XudResult xud_draw_triangle(XudContext* context, XudVector2 v0, XudVector2 v1, XudVector2 v2, XudColor color) {
    XudBatch* batch = NULL;
    XudVector3 p0;
    XudVector3 p1;
    XudVector3 p2;
    uint32_t oldVariant = 0u;
    uint32_t oldMaterial = 0u;
    uint32_t oldTexture = 0u;
    uint32_t extentWidth = 0u;
    uint32_t extentHeight = 0u;
    float clipX = 0.0f;
    float clipY = 0.0f;
    XudResult result = XUD_INVALID_ARG;

    result = xud_shapes_context_batch(&batch, context);
    if (result != XUD_OK) {
        return result;
    }

    /* The extent the points divide by is the framebuffer currently being drawn into, for the same
       reason the rectangle above refuses a zero one: a divide by it is an inf in a float stream
       rather than a draw. */
    result = xud_renderer_pixel_extent(context->renderer, context->width, context->height, &extentWidth, &extentHeight);
    if (result != XUD_OK) {
        return result;
    }
    if ((extentWidth == 0u) || (extentHeight == 0u)) {
        return XUD_INVALID_ARG;
    }

    result = xud_shapes_pixel_to_clip(extentWidth, extentHeight, v0.x, v0.y, &clipX, &clipY);
    if (result != XUD_OK) {
        return result;
    }
    p0.x = clipX;
    p0.y = clipY;
    p0.z = 0.0f;
    result = xud_shapes_pixel_to_clip(extentWidth, extentHeight, v1.x, v1.y, &clipX, &clipY);
    if (result != XUD_OK) {
        return result;
    }
    p1.x = clipX;
    p1.y = clipY;
    p1.z = 0.0f;
    result = xud_shapes_pixel_to_clip(extentWidth, extentHeight, v2.x, v2.y, &clipX, &clipY);
    if (result != XUD_OK) {
        return result;
    }
    p2.x = clipX;
    p2.y = clipY;
    p2.z = 0.0f;

    result = xud_batch_state_get(batch, &oldVariant, &oldMaterial, &oldTexture);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_batch_set_material(batch, XUD_BATCH_NO_MATERIAL, XUD_BATCH_VARIANT_UI, XUD_BATCH_NO_TEXTURE);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_shapes_append_triangle(batch, p0, p1, p2, color);
    if (result != XUD_OK) {
        (void)xud_batch_set_material(batch, oldMaterial, oldVariant, oldTexture);
        return result;
    }
    result = xud_batch_set_material(batch, oldMaterial, oldVariant, oldTexture);

    return result;
}

/* The triangle outline: the three edges of the caller's triangle as the batch's three lines
   through the UI column the line above does, the three appends sharing one state swap rather
   than three, so the outline is one run of its own instead of three, the same one-run agreement
   the rectangle outline above states for its four edges. The endpoints are the caller's own
   points, in the order the caller named them, at the same extent the triangle above divides by
   and with no inset of its own - raylib's triangle outline, unlike its rectangle one, carries
   no half-pixel, so the edges sit on the caller's points rather than inside the triangle they
   bound: the first edge from the first point to the second, the second edge from the second to
   the third, and the third edge from the third back to the first, the order raylib's own vertex
   sequence walks them. An edge that does not fit is refused with the edges before it already
   appended, the same partial-draw agreement the rectangle outline states, and the state the call
   found is restored on every path the append left it moved. */
XudResult xud_draw_triangle_lines(XudContext* context, XudVector2 v0, XudVector2 v1, XudVector2 v2, XudColor color) {
    XudBatch* batch = NULL;
    XudVector3 corner[3u];
    uint32_t oldVariant = 0u;
    uint32_t oldMaterial = 0u;
    uint32_t oldTexture = 0u;
    float clipX = 0.0f;
    float clipY = 0.0f;
    uint32_t edge = 0u;
    const float pointX[3u] = { v0.x, v1.x, v2.x };
    const float pointY[3u] = { v0.y, v1.y, v2.y };
    XudResult result = XUD_INVALID_ARG;

    result = xud_shapes_context_batch(&batch, context);
    if (result != XUD_OK) {
        return result;
    }

    /* The extent the points divide by is the framebuffer's own, for the same reason the triangle
       above refuses a zero one: a divide by it is an inf in a float stream rather than a draw. */
    if ((context->width == 0u) || (context->height == 0u)) {
        return XUD_INVALID_ARG;
    }

    for (edge = 0u; edge < 3u; edge++) {
        result = xud_shapes_pixel_to_clip(context->width, context->height, pointX[edge], pointY[edge], &clipX, &clipY);
        if (result != XUD_OK) {
            return result;
        }
        corner[edge].x = clipX;
        corner[edge].y = clipY;
        corner[edge].z = 0.0f;
    }

    result = xud_batch_state_get(batch, &oldVariant, &oldMaterial, &oldTexture);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_batch_set_material(batch, XUD_BATCH_NO_MATERIAL, XUD_BATCH_VARIANT_UI, XUD_BATCH_NO_TEXTURE);
    if (result != XUD_OK) {
        return result;
    }

    for (edge = 0u; edge < 3u; edge++) {
        result = xud_shapes_append_line(batch, corner[edge], corner[(edge + 1u) % 3u], color);
        if (result != XUD_OK) {
            (void)xud_batch_set_material(batch, oldMaterial, oldVariant, oldTexture);
            return result;
        }
    }

    result = xud_batch_set_material(batch, oldMaterial, oldVariant, oldTexture);

    return result;
}

/* The circle: the caller's centre and radius in the framebuffer's pixel space, the 36-segment fan
   the header states, drawn through the pipeline list's UI column the way the rectangle above does:
   the state swap moves the batch to the UI row for the fan and back when it is done, so the circle
   flushes as one row of its own and the shapes appended beside it draw with the state the call
   found. The radius is not checked here: the append owns the refusal the way the sphere's public
   call leaves its radius to its append, and a refused fan restores the state the call found before
   it answers. */
XudResult xud_draw_circle(XudContext* context, int32_t centerX, int32_t centerY, float radius, XudColor color) {
    XudBatch* batch = NULL;
    uint32_t oldVariant = 0u;
    uint32_t oldMaterial = 0u;
    uint32_t oldTexture = 0u;
    XudResult result = XUD_INVALID_ARG;

    result = xud_shapes_context_batch(&batch, context);
    if (result != XUD_OK) {
        return result;
    }

    /* The extent the fan's 37 points divide by is the framebuffer's own, for the same reason the
       rectangle above refuses a zero one: a divide by it is an inf in a float stream rather than
       a draw, and it is refused before any state is moved. */
    if ((context->width == 0u) || (context->height == 0u)) {
        return XUD_INVALID_ARG;
    }

    result = xud_batch_state_get(batch, &oldVariant, &oldMaterial, &oldTexture);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_batch_set_material(batch, XUD_BATCH_NO_MATERIAL, XUD_BATCH_VARIANT_UI, XUD_BATCH_NO_TEXTURE);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_shapes_append_circle(batch, context->width, context->height, (float)centerX, (float)centerY, radius, color);
    if (result != XUD_OK) {
        (void)xud_batch_set_material(batch, oldMaterial, oldVariant, oldTexture);
        return result;
    }
    result = xud_batch_set_material(batch, oldMaterial, oldVariant, oldTexture);

    return result;
}
/* The ellipse: the caller's centre and the two radii in the framebuffer's pixel space, the 36-segment
   fan the circle's section walks with the X offset radiusX and the Y offset radiusY, drawn through
   the pipeline list's UI column the way the circle above does: the state swap moves the batch to the
   UI row for the fan and back when it is done, so the ellipse flushes as one row of its own and the
   shapes appended beside it draw with the state the call found. The radii are not checked here: the
   append owns the refusals the way the sphere's public call leaves its radius to its append, and a
   refused fan restores the state the call found before it answers. */
XudResult xud_draw_ellipse(XudContext* context, int32_t centerX, int32_t centerY, float radiusX, float radiusY, XudColor color) {
    XudBatch* batch = NULL;
    uint32_t oldVariant = 0u;
    uint32_t oldMaterial = 0u;
    uint32_t oldTexture = 0u;
    XudResult result = XUD_INVALID_ARG;

    result = xud_shapes_context_batch(&batch, context);
    if (result != XUD_OK) {
        return result;
    }

    /* The extent the fan's 37 points divide by is the framebuffer's own, for the same reason the
       rectangle above refuses a zero one: a divide by it is an inf in a float stream rather than
       a draw, and it is refused before any state is moved. */
    if ((context->width == 0u) || (context->height == 0u)) {
        return XUD_INVALID_ARG;
    }

    result = xud_batch_state_get(batch, &oldVariant, &oldMaterial, &oldTexture);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_batch_set_material(batch, XUD_BATCH_NO_MATERIAL, XUD_BATCH_VARIANT_UI, XUD_BATCH_NO_TEXTURE);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_shapes_append_ellipse(batch, context->width, context->height, (float)centerX, (float)centerY, radiusX, radiusY, color);
    if (result != XUD_OK) {
        (void)xud_batch_set_material(batch, oldMaterial, oldVariant, oldTexture);
        return result;
    }
    result = xud_batch_set_material(batch, oldMaterial, oldVariant, oldTexture);

    return result;
}
