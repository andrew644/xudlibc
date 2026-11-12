/***********************************************************************************************
 *
 *   xud_generate.c - the generation rung: eight solids cut into the caller's arrays
 *
 *   Owns : the validation and the count arithmetic the two public calls share, and the eight
 *          writers those calls dispatch to. See xud_generate.h for the layer, the floors, and the
 *          list of what is not cloned and why.
 *   Layer: pure arithmetic over the caller's floats. No context, no driver, no allocation: every
 *          byte lands in memory the caller sized from xud_gen_mesh_count(), which ran the same
 *          validation over the same shape, so a writer never has to ask whether it is in bounds.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud_generate.h" /* Required for: the floors, and the two declarations this file answers */

#include <math.h> /* Required for: sinf, cosf, sqrtf, the rings of every round solid */
#include <stdint.h> /* Required for: uint64_t, the count arithmetic that cannot be allowed to wrap */

/*-----------------------------------------------------------------------------------------------
 * The count half, shared by the two calls
 *-----------------------------------------------------------------------------------------------*/

/* One shape's two counts, in 64-bit space: a caller that asks for a sphere of a billion longitudes
   is refused by the ceiling below rather than by a product that wrapped into a small number, which
   is the difference between a refusal and a fill that writes past the buffers it was given.
*/
static uint32_t xud_generate_count(uint64_t* outVertices, uint64_t* outIndices, const XudMeshShape* shape) {
    uint64_t slices = (uint64_t)shape->slices;
    uint64_t stacks = (uint64_t)shape->stacks;
    uint64_t vertices = 0u;
    uint64_t indices = 0u;
    uint32_t floors = 1u;

    switch (shape->kind) {
    case XUD_MESH_KIND_PLANE:
        floors
            = ((shape->width >= XUD_GENERATE_MIN_EXTENT) && (shape->length >= XUD_GENERATE_MIN_EXTENT) && (slices >= 1u) && (stacks >= 1u))
            ? 1u
            : 0u;
        vertices = (slices + 1u) * (stacks + 1u);
        indices = slices * stacks * 6u;
        break;
    case XUD_MESH_KIND_CUBE:
        floors = ((shape->width >= XUD_GENERATE_MIN_EXTENT) && (shape->height >= XUD_GENERATE_MIN_EXTENT)
                     && (shape->length >= XUD_GENERATE_MIN_EXTENT))
            ? 1u
            : 0u;
        vertices = 24u;
        indices = 36u;
        break;
    case XUD_MESH_KIND_SPHERE:
        floors = ((shape->radius >= XUD_GENERATE_MIN_RADIUS) && (slices >= XUD_GENERATE_MIN_SIDES)
                     && (stacks >= XUD_GENERATE_MIN_SPHERE_STACKS))
            ? 1u
            : 0u;
        vertices = (stacks + 1u) * (slices + 1u);
        indices = (slices * 6u * (stacks - 2u)) + (slices * 6u);
        break;
    case XUD_MESH_KIND_HEMISPHERE:
        floors = ((shape->radius >= XUD_GENERATE_MIN_RADIUS) && (slices >= XUD_GENERATE_MIN_SIDES) && (stacks >= XUD_GENERATE_MIN_STACKS))
            ? 1u
            : 0u;
        vertices = (stacks + 1u) * (slices + 1u);
        indices = (slices * 3u) + (slices * 6u * (stacks - 1u));
        break;
    case XUD_MESH_KIND_CYLINDER:
        floors = ((shape->radius >= XUD_GENERATE_MIN_RADIUS) && (shape->height >= XUD_GENERATE_MIN_EXTENT)
                     && (slices >= XUD_GENERATE_MIN_SIDES))
            ? 1u
            : 0u;
        vertices = (2u * (slices + 1u)) + (2u * (slices + 2u));
        indices = slices * 12u;
        break;
    case XUD_MESH_KIND_TORUS:
        floors = ((shape->radius >= XUD_GENERATE_MIN_RADIUS) && (shape->tubeRadius >= XUD_GENERATE_MIN_RADIUS)
                     && (slices >= XUD_GENERATE_MIN_SIDES) && (stacks >= XUD_GENERATE_MIN_SIDES))
            ? 1u
            : 0u;
        vertices = (slices + 1u) * (stacks + 1u);
        indices = slices * stacks * 6u;
        break;
    case XUD_MESH_KIND_KNOT:
        floors
            = ((shape->tubeRadius >= XUD_GENERATE_MIN_RADIUS) && (slices >= XUD_GENERATE_MIN_SIDES) && (stacks >= XUD_GENERATE_MIN_SIDES))
            ? 1u
            : 0u;
        vertices = slices * stacks;
        indices = slices * stacks * 6u;
        break;
    case XUD_MESH_KIND_POLY:
        floors = ((shape->radius >= XUD_GENERATE_MIN_RADIUS) && (slices >= XUD_GENERATE_MIN_SIDES)) ? 1u : 0u;
        vertices = slices + 2u;
        indices = slices * 3u;
        break;
    default:
        floors = 0u;
        break;
    }

    if (floors == 0u) {
        return 0u;
    }
    if (vertices > (uint64_t)XUD_MESH_MAX_VERTICES) {
        return 0u;
    }

    *outVertices = vertices;
    *outIndices = indices;

    return 1u;
}

/* Every number the shape carries, finite. The floors in the count above compare against a floor,
   which a NaN answers false; this catches the infinity in a field the kind does NOT gate on, which
   would otherwise reach a writer as a vertex at infinity and a normal of NaN.
*/
static uint32_t xud_generate_shape_is_finite(const XudMeshShape* shape) {
    if ((isfinite(shape->radius) == 0) || (isfinite(shape->tubeRadius) == 0) || (isfinite(shape->width) == 0)
        || (isfinite(shape->height) == 0) || (isfinite(shape->length) == 0)) {
        return 0u;
    }

    return 1u;
}

static void xud_generate_vertex(
    float* pos, float* texcoord, float* normal, float x, float y, float z, float u, float v, float nx, float ny, float nz) {
    if (pos != NULL) {
        pos[0] = x;
        pos[1] = y;
        pos[2] = z;
    }
    if (texcoord != NULL) {
        texcoord[0] = u;
        texcoord[1] = v;
    }
    if (normal != NULL) {
        normal[0] = nx;
        normal[1] = ny;
        normal[2] = nz;
    }
}

/*-----------------------------------------------------------------------------------------------
 * The flat solids: a grid, a box, a fan
 *-----------------------------------------------------------------------------------------------*/

static void xud_generate_plane(const XudMeshShape* shape, const XudMeshBuffers* buffers) {
    const uint32_t columns = shape->slices + 1u;
    uint32_t row = 0u;
    uint32_t column = 0u;
    uint32_t quad = 0u;

    for (row = 0u; row <= shape->stacks; row++) {
        float v = (float)row / (float)shape->stacks;
        for (column = 0u; column < columns; column++) {
            float u = (float)column / (float)shape->slices;
            uint32_t vertex = (row * columns) + column;

            xud_generate_vertex(&buffers->pos[vertex * 3u], (buffers->texcoords != NULL) ? &buffers->texcoords[vertex * 2u] : NULL,
                (buffers->normal != NULL) ? &buffers->normal[vertex * 3u] : NULL, (u - 0.5f) * shape->width, 0.0f,
                (0.5f - v) * shape->length, u, v, 0.0f, 1.0f, 0.0f);
        }
    }

    for (row = 0u; row < shape->stacks; row++) {
        for (column = 0u; column < shape->slices; column++) {
            uint32_t a = (row * columns) + column;
            uint32_t b = a + 1u;
            uint32_t c = a + columns + 1u;
            uint32_t d = a + columns;

            buffers->index[quad * 6u] = a;
            buffers->index[(quad * 6u) + 1u] = b;
            buffers->index[(quad * 6u) + 2u] = d;
            buffers->index[(quad * 6u) + 3u] = b;
            buffers->index[(quad * 6u) + 4u] = c;
            buffers->index[(quad * 6u) + 5u] = d;
            quad++;
        }
    }
}

/* The cross product, at the one place the two frames that need one can reach it. */
static void xud_generate_cross(const float* left, const float* right, float* out) {
    out[0] = (left[1] * right[2]) - (left[2] * right[1]);
    out[1] = (left[2] * right[0]) - (left[0] * right[2]);
    out[2] = (left[0] * right[1]) - (left[1] * right[0]);
}

/* The axis a direction points along: a cube's in-plane directions are the world axes themselves, so
   the largest magnitude IS the axis and the other two are zero.
*/
static uint32_t xud_generate_axis_of(const float* direction) {
    float x = fabsf(direction[0]);
    float y = fabsf(direction[1]);
    float z = fabsf(direction[2]);

    return (x >= y) ? ((x >= z) ? 0u : 2u) : ((y >= z) ? 1u : 2u);
}

/* The six faces as their normals; the two in-plane directions of each are built from it below, so
   the winding of a face is the winding of its normal rather than six pairs of corners the file has
   to have got right by hand.
*/
static const float xudGenerateCubeNormal[6][3] = { { -1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f },
    { 0.0f, 0.0f, -1.0f }, { 0.0f, 0.0f, 1.0f } };

static void xud_generate_cube(const XudMeshShape* shape, const XudMeshBuffers* buffers) {
    const float half[3] = { shape->width / 2.0f, shape->height / 2.0f, shape->length / 2.0f };
    uint32_t face = 0u;

    for (face = 0u; face < 6u; face++) {
        const float* n = xudGenerateCubeNormal[face];
        float up[3] = { 0.0f, 0.0f, 0.0f };
        float tangent[3];
        float bitangent[3];
        float across = 0.0f;
        float along = 0.0f;
        uint32_t corner = 0u;

        /* The axis the normal is not along, which for these six normals is never the normal's own. */
        up[((n[0] != 0.0f) ? 1u : 0u)] = 1.0f;
        xud_generate_cross(up, n, tangent);
        xud_generate_cross(n, tangent, bitangent);
        across = half[xud_generate_axis_of(tangent)];
        along = half[xud_generate_axis_of(bitangent)];

        for (corner = 0u; corner < 4u; corner++) {
            float u = ((corner == 1u) || (corner == 2u)) ? 1.0f : 0.0f;
            float v = ((corner == 2u) || (corner == 3u)) ? 1.0f : 0.0f;
            uint32_t vertex = (face * 4u) + corner;
            uint32_t axis = 0u;
            float place[3] = { 0.0f, 0.0f, 0.0f };

            for (axis = 0u; axis < 3u; axis++) {
                place[axis] = (n[axis] * half[axis]) + (((2.0f * u) - 1.0f) * across * tangent[axis])
                    + (((2.0f * v) - 1.0f) * along * bitangent[axis]);
            }

            xud_generate_vertex(&buffers->pos[vertex * 3u], (buffers->texcoords != NULL) ? &buffers->texcoords[vertex * 2u] : NULL,
                (buffers->normal != NULL) ? &buffers->normal[vertex * 3u] : NULL, place[0], place[1], place[2], u, v, n[0], n[1], n[2]);
        }

        buffers->index[(face * 6u)] = face * 4u;
        buffers->index[(face * 6u) + 1u] = (face * 4u) + 1u;
        buffers->index[(face * 6u) + 2u] = (face * 4u) + 3u;
        buffers->index[(face * 6u) + 3u] = (face * 4u) + 1u;
        buffers->index[(face * 6u) + 4u] = (face * 4u) + 2u;
        buffers->index[(face * 6u) + 5u] = (face * 4u) + 3u;
    }
}

static void xud_generate_poly(const XudMeshShape* shape, const XudMeshBuffers* buffers) {
    uint32_t side = 0u;

    xud_generate_vertex(&buffers->pos[0], (buffers->texcoords != NULL) ? &buffers->texcoords[0] : NULL,
        (buffers->normal != NULL) ? &buffers->normal[0] : NULL, 0.0f, 0.0f, 0.0f, 0.5f, 0.5f, 0.0f, 1.0f, 0.0f);

    for (side = 0u; side <= shape->slices; side++) {
        float angle = (XUD_GENERATE_TWO_PI * (float)side) / (float)shape->slices;
        float x = shape->radius * cosf(angle);
        float y = shape->radius * sinf(angle);
        uint32_t vertex = side + 1u;

        xud_generate_vertex(&buffers->pos[vertex * 3u], (buffers->texcoords != NULL) ? &buffers->texcoords[vertex * 2u] : NULL,
            (buffers->normal != NULL) ? &buffers->normal[vertex * 3u] : NULL, x, 0.0f, y, (x / (2.0f * shape->radius)) + 0.5f,
            (y / (2.0f * shape->radius)) + 0.5f, 0.0f, 1.0f, 0.0f);
    }

    for (side = 0u; side < shape->slices; side++) {
        buffers->index[side * 3u] = 0u;
        buffers->index[(side * 3u) + 1u] = side + 2u;
        buffers->index[(side * 3u) + 2u] = side + 1u;
    }
}

/*-----------------------------------------------------------------------------------------------
 * The round solids: rings of latitude and longitude, and a tube swept along a curve
 *-----------------------------------------------------------------------------------------------*/

/* The UV sphere, and the hemisphere as its top half: stacks+1 rows of slices+1 columns, the last
   column a copy of the first so the seam has both u = 0 and u = 1 to sample from. Row 0 is the
   north pole with every column of it at the same point, and the index lists are what collapse it:
   the pole rows are fans of slices triangles and the rows between them quads, each wound so its
   cross product is the outward normal it carries. A hemisphere has one pole row, so it has one fan
   and its last row is the equator's ring, which is a real ring and gets real quads.
*/
static void xud_generate_sphere(const XudMeshShape* shape, const XudMeshBuffers* buffers, uint32_t hemisphere) {
    const uint32_t columns = shape->slices + 1u;
    const float sweep = (hemisphere != 0u) ? 0.5f : 1.0f;
    uint32_t row = 0u;
    uint32_t column = 0u;
    uint32_t triangle = 0u;

    for (row = 0u; row <= shape->stacks; row++) {
        float v = (float)row / (float)shape->stacks;
        float latitude = XUD_GENERATE_PI * (0.5f - (v * sweep));
        float ring = cosf(latitude);
        float y = sinf(latitude) * shape->radius;

        for (column = 0u; column < columns; column++) {
            float u = (float)column / (float)shape->slices;
            float longitude = XUD_GENERATE_TWO_PI * u;
            float nx = cosf(longitude) * ring;
            float ny = sinf(latitude);
            float nz = sinf(longitude) * ring;
            uint32_t vertex = (row * columns) + column;

            xud_generate_vertex(&buffers->pos[vertex * 3u], (buffers->texcoords != NULL) ? &buffers->texcoords[vertex * 2u] : NULL,
                (buffers->normal != NULL) ? &buffers->normal[vertex * 3u] : NULL, nx * shape->radius, y, nz * shape->radius, u, v, nx, ny,
                nz);
        }
    }

    for (row = 0u; row < shape->stacks; row++) {
        uint32_t last = ((hemisphere != 0u) || (row < (shape->stacks - 1u))) ? 1u : 0u;

        for (column = 0u; column < shape->slices; column++) {
            uint32_t a = (row * columns) + column;
            uint32_t b = a + 1u;
            uint32_t c = a + columns;
            uint32_t d = c + 1u;

            if (row == 0u) {
                buffers->index[triangle * 3u] = a;
                buffers->index[(triangle * 3u) + 1u] = d;
                buffers->index[(triangle * 3u) + 2u] = c;
                triangle++;
            } else {
                buffers->index[triangle * 3u] = a;
                buffers->index[(triangle * 3u) + 1u] = b;
                buffers->index[(triangle * 3u) + 2u] = c;
                triangle++;
                if (last != 0u) {
                    buffers->index[triangle * 3u] = b;
                    buffers->index[(triangle * 3u) + 1u] = d;
                    buffers->index[(triangle * 3u) + 2u] = c;
                    triangle++;
                }
            }
        }
    }
}

/* A tube of slices around a height, capped at both ends: two rings for the tube with the seam
   duplicated so it has both u values, and a cap of a centre and a ring apiece - a cap's fan cannot
   share the tube's vertices, because a cap's normal is the axis and the tube's is the radius.
*/
static void xud_generate_cylinder(const XudMeshShape* shape, const XudMeshBuffers* buffers) {
    const uint32_t columns = shape->slices + 1u;
    const uint32_t ring = 2u * columns;
    uint32_t column = 0u;

    for (column = 0u; column < columns; column++) {
        float u = (float)column / (float)shape->slices;
        float angle = XUD_GENERATE_TWO_PI * u;
        float nx = cosf(angle);
        float nz = sinf(angle);

        xud_generate_vertex(&buffers->pos[column * 3u], (buffers->texcoords != NULL) ? &buffers->texcoords[column * 2u] : NULL,
            (buffers->normal != NULL) ? &buffers->normal[column * 3u] : NULL, nx * shape->radius, shape->height / 2.0f, nz * shape->radius,
            u, 0.0f, nx, 0.0f, nz);
        xud_generate_vertex(&buffers->pos[(column + columns) * 3u],
            (buffers->texcoords != NULL) ? &buffers->texcoords[(column + columns) * 2u] : NULL,
            (buffers->normal != NULL) ? &buffers->normal[(column + columns) * 3u] : NULL, nx * shape->radius, -shape->height / 2.0f,
            nz * shape->radius, u, 1.0f, nx, 0.0f, nz);
    }

    for (column = 0u; column < shape->slices; column++) {
        uint32_t triangle = column * 2u;

        buffers->index[triangle * 3u] = column;
        buffers->index[(triangle * 3u) + 1u] = column + 1u;
        buffers->index[(triangle * 3u) + 2u] = column + columns;
        buffers->index[(triangle * 3u) + 3u] = column + 1u;
        buffers->index[(triangle * 3u) + 4u] = column + columns + 1u;
        buffers->index[(triangle * 3u) + 5u] = column + columns;
    }

    for (column = 0u; column < 2u; column++) {
        uint32_t centre = ring + (column * (columns + 1u));
        uint32_t cap = centre + 1u;
        float y = (column == 0u) ? 1.0f : -1.0f;
        uint32_t side = 0u;

        xud_generate_vertex(&buffers->pos[centre * 3u], (buffers->texcoords != NULL) ? &buffers->texcoords[centre * 2u] : NULL,
            (buffers->normal != NULL) ? &buffers->normal[centre * 3u] : NULL, 0.0f, y * (shape->height / 2.0f), 0.0f, 0.5f, 0.5f, 0.0f, y,
            0.0f);

        for (side = 0u; side < columns; side++) {
            float u = (float)side / (float)shape->slices;
            float angle = XUD_GENERATE_TWO_PI * u;
            uint32_t vertex = cap + side;

            xud_generate_vertex(&buffers->pos[vertex * 3u], (buffers->texcoords != NULL) ? &buffers->texcoords[vertex * 2u] : NULL,
                (buffers->normal != NULL) ? &buffers->normal[vertex * 3u] : NULL, cosf(angle) * shape->radius, y * (shape->height / 2.0f),
                sinf(angle) * shape->radius, cosf(angle) * 0.5f + 0.5f, sinf(angle) * 0.5f + 0.5f, 0.0f, y, 0.0f);

            if (side < shape->slices) {
                uint32_t triangle = (2u * shape->slices) + (column * shape->slices) + side;

                buffers->index[triangle * 3u] = centre;
                buffers->index[(triangle * 3u) + 1u] = (column == 0u) ? (cap + side + 1u) : (cap + side);
                buffers->index[(triangle * 3u) + 2u] = (column == 0u) ? (cap + side) : (cap + side + 1u);
            }
        }
    }
}

/* A tube of tubeRadius swept around a circle of radius: the main circle stepped slices times, the
   tube's own circle stacks times, and both seams duplicated so the mesh has an edge to close on.
   The normal of a torus vertex is the tube's own radius at it, which is the only direction that is
   out of the solid at both the fat side and the pinch.
*/
static void xud_generate_torus(const XudMeshShape* shape, const XudMeshBuffers* buffers) {
    const uint32_t columns = shape->stacks + 1u;
    uint32_t row = 0u;
    uint32_t column = 0u;
    uint32_t quad = 0u;

    for (row = 0u; row <= shape->slices; row++) {
        float u = (float)row / (float)shape->slices;
        float main = XUD_GENERATE_TWO_PI * u;
        float cosMain = cosf(main);
        float sinMain = sinf(main);

        for (column = 0u; column < columns; column++) {
            float v = (float)column / (float)shape->stacks;
            float tube = XUD_GENERATE_TWO_PI * v;
            float cosTube = cosf(tube);
            float sinTube = sinf(tube);
            float reach = shape->radius + (shape->tubeRadius * cosTube);
            uint32_t vertex = (row * columns) + column;

            xud_generate_vertex(&buffers->pos[vertex * 3u], (buffers->texcoords != NULL) ? &buffers->texcoords[vertex * 2u] : NULL,
                (buffers->normal != NULL) ? &buffers->normal[vertex * 3u] : NULL, reach * cosMain, shape->tubeRadius * sinTube,
                reach * sinMain, u, v, cosTube * cosMain, sinTube, cosTube * sinMain);
        }
    }

    for (row = 0u; row < shape->slices; row++) {
        for (column = 0u; column < shape->stacks; column++) {
            uint32_t a = (row * columns) + column;
            uint32_t b = a + 1u;
            uint32_t c = a + columns;
            uint32_t d = c + 1u;

            buffers->index[quad * 6u] = a;
            buffers->index[(quad * 6u) + 1u] = b;
            buffers->index[(quad * 6u) + 2u] = c;
            buffers->index[(quad * 6u) + 3u] = b;
            buffers->index[(quad * 6u) + 4u] = d;
            buffers->index[(quad * 6u) + 5u] = c;
            quad++;
        }
    }
}

/* The trefoil: a tube of tubeRadius swept along a (2, 3) torus knot. Each cross-section rides the
   frame the curve's tangent makes with the vertical - the fixed-axis frame par_shapes' trefoil uses -
   which turns only as far as the tangent's own azimuth does, and on this curve never collapses and
   never has to be re-signed, because the tangent is never vertical. Both directions wrap with no
   duplicate vertex - a knot has no seam to open - so the mesh is slices*stacks vertices for
   slices*stacks*6 indices, and the knot sits in the XZ plane like the rest of the solids rather than
   in par_shapes' XY one, so it stands on the floor beside them.
*/

/* The curve and its rate: the point scaled so a radius of r fits inside r of the origin, and the
   derivative that frames it. The scale cancels out of the direction, so it rides the point alone.
*/
static void xud_generate_knot_curve(float t, float scale, float* outPoint, float* outRate) {
    const float threeT = 3.0f * t;
    const float twoT = 2.0f * t;
    const float reach = 2.0f + cosf(threeT);

    outPoint[0] = scale * reach * cosf(twoT);
    outPoint[1] = scale * sinf(threeT);
    outPoint[2] = scale * reach * sinf(twoT);
    outRate[0] = -(3.0f * sinf(threeT) * cosf(twoT)) - (2.0f * reach * sinf(twoT));
    outRate[1] = 3.0f * cosf(threeT);
    outRate[2] = -(3.0f * sinf(threeT) * sinf(twoT)) + (2.0f * reach * cosf(twoT));
}

static void xud_generate_knot(const XudMeshShape* shape, const XudMeshBuffers* buffers) {
    const float scale = shape->radius / 3.0f;
    const uint32_t stacks = shape->stacks;
    const float vertical[3] = { 0.0f, 1.0f, 0.0f };
    uint32_t row = 0u;

    for (row = 0u; row < shape->slices; row++) {
        float t = XUD_GENERATE_TWO_PI * (float)row / (float)shape->slices;
        float curve[3];
        float rate[3];
        float tangent[3];
        float normal0[3];
        float binormal[3];
        float length = 0.0f;
        uint32_t column = 0u;

        xud_generate_knot_curve(t, scale, curve, rate);
        length = sqrtf((rate[0] * rate[0]) + (rate[1] * rate[1]) + (rate[2] * rate[2]));
        tangent[0] = rate[0] / length;
        tangent[1] = rate[1] / length;
        tangent[2] = rate[2] / length;
        xud_generate_cross(tangent, vertical, normal0);
        length = sqrtf((normal0[0] * normal0[0]) + (normal0[1] * normal0[1]) + (normal0[2] * normal0[2]));
        normal0[0] /= length;
        normal0[1] /= length;
        normal0[2] /= length;
        xud_generate_cross(tangent, normal0, binormal);

        for (column = 0u; column < stacks; column++) {
            float v = XUD_GENERATE_TWO_PI * (float)column / (float)stacks;
            float ring[3] = { ((cosf(v) * normal0[0]) + (sinf(v) * binormal[0])), ((cosf(v) * normal0[1]) + (sinf(v) * binormal[1])),
                ((cosf(v) * normal0[2]) + (sinf(v) * binormal[2])) };
            uint32_t vertex = (row * stacks) + column;

            xud_generate_vertex(&buffers->pos[vertex * 3u], (buffers->texcoords != NULL) ? &buffers->texcoords[vertex * 2u] : NULL,
                (buffers->normal != NULL) ? &buffers->normal[vertex * 3u] : NULL, curve[0] + (shape->tubeRadius * ring[0]),
                curve[1] + (shape->tubeRadius * ring[1]), curve[2] + (shape->tubeRadius * ring[2]), (float)row / (float)shape->slices,
                (float)column / (float)stacks, ring[0], ring[1], ring[2]);
        }
    }

    for (row = 0u; row < shape->slices; row++) {
        uint32_t nextRow = (row + 1u) % shape->slices;
        uint32_t column = 0u;

        for (column = 0u; column < stacks; column++) {
            uint32_t nextColumn = (column + 1u) % stacks;
            uint32_t a = (row * stacks) + column;
            uint32_t b = (row * stacks) + nextColumn;
            uint32_t c = (nextRow * stacks) + column;
            uint32_t d = (nextRow * stacks) + nextColumn;
            uint32_t quad = (row * stacks) + column;

            buffers->index[quad * 6u] = a;
            buffers->index[(quad * 6u) + 1u] = b;
            buffers->index[(quad * 6u) + 2u] = c;
            buffers->index[(quad * 6u) + 3u] = b;
            buffers->index[(quad * 6u) + 4u] = d;
            buffers->index[(quad * 6u) + 5u] = c;
        }
    }
}

/*-----------------------------------------------------------------------------------------------
 * The two calls
 *-----------------------------------------------------------------------------------------------*/

/* The one validation both halves run: the shape's numbers finite and its kind's floors met, and the
   counts that follow from it. The fill half calls this and nothing else, which is why its writes are
   in bounds - it writes exactly the counts this answers, into buffers the caller sized from the same
   answer.
*/
static uint32_t xud_generate_plan(uint64_t* outVertices, uint64_t* outIndices, const XudMeshShape* shape) {
    if (xud_generate_shape_is_finite(shape) == 0u) {
        return 0u;
    }

    return xud_generate_count(outVertices, outIndices, shape);
}

XudResult xud_gen_mesh_count(XudMeshCounts* outCounts, const XudMeshShape* shape) {
    uint64_t vertices = 0u;
    uint64_t indices = 0u;

    if ((outCounts == NULL) || (shape == NULL)) {
        return XUD_INVALID_ARG;
    }
    if (xud_generate_plan(&vertices, &indices, shape) == 0u) {
        return XUD_INVALID_ARG;
    }

    outCounts->vertexCount = (uint32_t)vertices;
    outCounts->indexCount = (uint32_t)indices;

    return XUD_OK;
}

XudResult xud_gen_mesh(XudMeshData* outMesh, const XudMeshShape* shape, const XudMeshBuffers* buffers) {
    uint64_t vertices = 0u;
    uint64_t indices = 0u;

    if ((outMesh == NULL) || (shape == NULL) || (buffers == NULL)) {
        return XUD_INVALID_ARG;
    }
    if ((buffers->pos == NULL) || (buffers->index == NULL)) {
        return XUD_INVALID_ARG;
    }
    if (xud_generate_plan(&vertices, &indices, shape) == 0u) {
        return XUD_INVALID_ARG;
    }

    switch (shape->kind) {
    case XUD_MESH_KIND_PLANE:
        xud_generate_plane(shape, buffers);
        break;
    case XUD_MESH_KIND_CUBE:
        xud_generate_cube(shape, buffers);
        break;
    case XUD_MESH_KIND_SPHERE:
        xud_generate_sphere(shape, buffers, 0u);
        break;
    case XUD_MESH_KIND_HEMISPHERE:
        xud_generate_sphere(shape, buffers, 1u);
        break;
    case XUD_MESH_KIND_CYLINDER:
        xud_generate_cylinder(shape, buffers);
        break;
    case XUD_MESH_KIND_TORUS:
        xud_generate_torus(shape, buffers);
        break;
    case XUD_MESH_KIND_KNOT:
        xud_generate_knot(shape, buffers);
        break;
    default:
        xud_generate_poly(shape, buffers);
        break;
    }

    outMesh->vertexCount = (uint32_t)vertices;
    outMesh->indexCount = (uint32_t)indices;
    outMesh->pos = buffers->pos;
    outMesh->texcoords = buffers->texcoords;
    outMesh->color = NULL;
    outMesh->normal = buffers->normal;
    outMesh->index = buffers->index;

    return XUD_OK;
}
