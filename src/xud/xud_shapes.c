/***********************************************************************************************
 *
 *   xud_shapes.c - the shapes rung and the public draw call above it
 *
 *   Owns : xud_shapes_append_triangle(), xud_shapes_append_cube(), xud_shapes_append_sphere(),
 *          xud_shapes_append_line(), xud_shapes_append_cube_wires() and xud_shapes_append_sphere
 *          _wires(), the geometry the public draw calls name, the seven public calls above them (xud
 *          _draw_triangle_3d(), xud_draw_cube(), xud_draw_sphere(), xud_draw_line_3d(), xud_draw
 *          _point_3d(), xud_draw_cube_wires(), xud_draw_sphere_wires()) and the one guard those
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
#include <math.h> /* Required for: sinf/cosf, the sphere's latitude and longitude - host arithmetic on the caller's floats, the one system header this file takes */

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

XudResult xud_draw_sphere_wires(XudContext* context, XudVector3 centerPos, float radius, XudColor color) {
    XudBatch* batch = NULL;
    XudResult result = XUD_INVALID_ARG;

    result = xud_shapes_context_batch(&batch, context);
    if (result != XUD_OK) {
        return result;
    }

    return xud_shapes_append_sphere_wires(batch, centerPos, radius, XUD_SHAPES_SPHERE_RING_COUNT, XUD_SHAPES_SPHERE_SLICE_COUNT, color);
}
