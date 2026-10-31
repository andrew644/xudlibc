/***********************************************************************************************
 *
 *   xud_camera.c - the camera rung: the camera's basis, its frustum and the one matrix set 0 carries
 *
 *   Owns : xud_camera_validate(), xud_camera_fill_view_proj() and xud_camera_apply(), the whole of
 *          the rung's arithmetic - the camera's orthonormal basis (forward, right, the true up), the
 *          symmetric perspective frustum at the caller's aspect and the product of the two, in the
 *          block's column-major float[16] order. Nothing else - no driver call, no loader row, no
 *          Vulkan or GLFW symbol the linker could see, no state: the rung's only system header is
 *          <math.h>, and every function here is host arithmetic on the caller's floats, which is
 *          what keeps the default test run able to assert the matrices against hand-computed rows
 *          with no device present.
 *   Layer: above the descriptor (xud_camera_apply() hands the matrix to the block through the
 *          descriptor rung's own setter, it does not reach into the block's field) and below the
 *          renderer, which is the owner that holds the camera and the extent a frame renders at.
 *
 *   The two conventions this rung has to get right, and the reason it can get them right at all:
 *
 *     - The matrix ORDER. The block's viewProj member is float[16] in column-major order (element
 *       index = column*4 + row), which is the order a GLSL mat4 reads a uniform buffer in, and the
 *       vertex shader multiplies it as `viewProj * modelMatrix * position` (shaders/solid.vert). So
 *       the matrix this rung produces is the PROJECTION times the VIEW, not the other way round:
 *       the point is a column vector, the model matrix is pushed per draw and the block's matrix is
 *       what maps the model-space point into clip space.
 *     - The depth RANGE. Vulkan's depth buffer is [0,1] and the pipeline compares LESS (xud_pipeline
 *       h), so nearer must be SMALLER, and the projection maps the near plane to 0 and the far plane
 *       to 1 rather than to -1 and +1 as the OpenGL convention does. A matrix built for [-1,1] is
 *       not "off by a scale of two": it puts the whole frustum below zero once clipped, which is
 *       what makes geometry past the near plane fail the depth test against the clear value of 1.0.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include "xud_camera.h" /* Required for: XudCamera3D, the rung's constants, the three declarations this file answers */
#include "xud_descriptor.h" /* Required for: XudDescriptor, xud_descriptor_set_view_proj - the setter that owns the block's viewProj member */
#include <math.h>

/*-----------------------------------------------------------------------------------------------
 * The basis arithmetic, internal to the rung
 *-----------------------------------------------------------------------------------------------*/

static void xud_camera_cross(const float* left, const float* right, float* out) {
    out[0] = (left[1] * right[2]) - (left[2] * right[1]);
    out[1] = (left[2] * right[0]) - (left[0] * right[2]);
    out[2] = (left[0] * right[1]) - (left[1] * right[0]);
}

static float xud_camera_dot(const float* left, const float* right) {
    return (left[0] * right[0]) + (left[1] * right[1]) + (left[2] * right[2]);
}

/* The squared length, compared against a squared floor rather than a length against a floor: the
   sqrt is what a normalising function needs and not what a well-formedness test needs, and a NaN
   component answers false against the floor either way.
*/
static uint32_t xud_camera_axis_is_usable(const float* axis) {
    const float floorSquared = XUD_CAMERA_AXIS_MIN_LENGTH * XUD_CAMERA_AXIS_MIN_LENGTH;

    return (xud_camera_dot(axis, axis) > floorSquared) ? 1u : 0u;
}

/* Normalising a vector the caller did not ask about: the length is the caller's arithmetic and a
   length of zero (or of NaN, which compares false against the floor) is the degenerate case the
   caller refuses rather than a division that leaves NaN in every basis vector.
*/
static XudResult xud_camera_normalize(const float* in, float* out) {
    const float length = sqrtf(xud_camera_dot(in, in));

    if (!(length > XUD_CAMERA_AXIS_MIN_LENGTH)) {
        return XUD_INVALID_ARG;
    }

    out[0] = in[0] / length;
    out[1] = in[1] / length;
    out[2] = in[2] / length;

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The camera record's own well-formedness - pure, and independent of the extent
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_camera_validate(const XudCamera3D* camera) {
    float forward[3] = { 0.0f, 0.0f, 0.0f };
    float up[3] = { 0.0f, 0.0f, 0.0f };
    float right[3] = { 0.0f, 0.0f, 0.0f };

    if (camera == NULL) {
        return XUD_INVALID_ARG;
    }

    /* Every range test is written as "not in range" rather than as "out of range" so a NaN fovy, a
       NaN near plane or a NaN far plane is refused by the SAME test as a zero one rather than conv
       erted to a NaN matrix that the driver then reads as a frustum containing the whole world.
    */
    if (!((camera->fovy > XUD_CAMERA_FOVY_MIN_DEG) && (camera->fovy < XUD_CAMERA_FOVY_MAX_DEG))) {
        return XUD_INVALID_ARG;
    }
    if (!(camera->near > XUD_CAMERA_PLANE_MIN_DISTANCE)) {
        return XUD_INVALID_ARG;
    }
    if (!(camera->far > camera->near)) {
        return XUD_INVALID_ARG;
    }

    /* The three basis degeneracies, in the order the basis is built: an eye sitting on its target
       has no forward vector, a zero up vector has no axis to rotate over, and an up PARALLEL to the
       forward has no right vector - the cross product is zero and normalising it divides by zero.
       raylib has no answer for any of the three (its camera is a global it never validates), so the
       refusal is this rung's own.
    */
    forward[0] = camera->target.x - camera->position.x;
    forward[1] = camera->target.y - camera->position.y;
    forward[2] = camera->target.z - camera->position.z;
    if (xud_camera_axis_is_usable(forward) == 0u) {
        return XUD_INVALID_ARG;
    }

    up[0] = camera->up.x;
    up[1] = camera->up.y;
    up[2] = camera->up.z;
    if (xud_camera_axis_is_usable(up) == 0u) {
        return XUD_INVALID_ARG;
    }

    xud_camera_cross(forward, up, right);
    if (xud_camera_axis_is_usable(right) == 0u) {
        return XUD_INVALID_ARG;
    }

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The camera's matrix - pure
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_camera_fill_view_proj(const XudCamera3D* camera, float aspect, float* outViewProj) {
    float forward[3] = { 0.0f, 0.0f, 0.0f };
    float up[3] = { 0.0f, 0.0f, 0.0f };
    float right[3] = { 0.0f, 0.0f, 0.0f };
    float eye[3] = { 0.0f, 0.0f, 0.0f };
    float view[XUD_CAMERA_MATRIX_ELEMENT_COUNT] = { 0.0f };
    float projection[XUD_CAMERA_MATRIX_ELEMENT_COUNT] = { 0.0f };
    float sum = 0.0f;
    float focalLength = 0.0f;
    float halfAngleRadians = 0.0f;
    uint32_t column = 0u;
    uint32_t row = 0u;
    uint32_t term = 0u;

    if ((camera == NULL) || (outViewProj == NULL)) {
        return XUD_INVALID_ARG;
    }

    /* The aspect is the extent a frame renders at, and it is checked apart from the camera because
       it is not part of the camera record - a zero or NaN aspect is a frame that has nothing to
       project onto.
    */
    if (!(aspect > 0.0f)) {
        return XUD_INVALID_ARG;
    }
    if (xud_camera_validate(camera) != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    eye[0] = camera->position.x;
    eye[1] = camera->position.y;
    eye[2] = camera->position.z;

    forward[0] = camera->target.x - eye[0];
    forward[1] = camera->target.y - eye[1];
    forward[2] = camera->target.z - eye[2];

    up[0] = camera->up.x;
    up[1] = camera->up.y;
    up[2] = camera->up.z;

    /* The basis, raylib's own three (rcamera.h: forward = normalize(target - position), up =
       normalize(camera.up), right = normalize(cross(forward, up))) and the fourth vector the basis
       needs, cross(right, forward), which replaces the caller's up so the basis is orthonormal. A
       tilted up therefore shears the frame rather than rotating it, which is what an orthonormal
       basis keeps it from - and what keeps the projection's focal length meaningful.
    */
    xud_camera_cross(forward, up, right);
    if (xud_camera_normalize(forward, forward) != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    if (xud_camera_normalize(up, up) != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    if (xud_camera_normalize(right, right) != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    xud_camera_cross(right, forward, up);

    /* The view half, as the three basis ROWS plus the translation: the camera's basis vectors are
       the columns of the camera's world transform, and a view transform is that transform's inverse
       - for an orthonormal basis that inverse is the transpose plus the eye pushed back through the
       rows, which is what the three dot products below are. The view space's own forward is -forwa
       rd, which is what makes the camera look down -Z, the convention the projection below and the
       pipeline's LESS compare are both written against. Column-major storage: element index = col
       *4 + row.
    */
    view[0u] = right[0];
    view[4u] = right[1];
    view[8u] = right[2];
    view[12u] = -xud_camera_dot(right, eye);
    view[1u] = up[0];
    view[5u] = up[1];
    view[9u] = up[2];
    view[13u] = -xud_camera_dot(up, eye);
    view[2u] = -forward[0];
    view[6u] = -forward[1];
    view[10u] = -forward[2];
    view[14u] = xud_camera_dot(forward, eye);
    view[3u] = 0.0f;
    view[7u] = 0.0f;
    view[11u] = 0.0f;
    view[15u] = 1.0f;

    /* The projection half. raylib's fovy is the FULL vertical aperture in DEGREES, so the focal
       length is the cotangent of the half-angle: a point at distance d and height h above the eye
       lands at h*focal/d in NDC, the same pinhole a caller's framing is stated against. The horizon
       tal scale is the vertical one divided by the extent's aspect, so a wider window shows more
       world to the sides rather than a stretched one. The two depth rows are the [0,1] mapping: at
       z_view = -near the numerator is zero and at z_view = -far it is w_clip, so the near plane is
       0 and the far plane is 1, and the -1 in the w row is what makes w_clip = -z_view = the dis
       tance in front of the eye.
    */
    halfAngleRadians = (camera->fovy * 0.5f) / XUD_CAMERA_DEGREES_PER_RADIANS;
    focalLength = cosf(halfAngleRadians) / sinf(halfAngleRadians);
    projection[0u] = focalLength / aspect;
    projection[5u] = focalLength;
    projection[10u] = camera->far / (camera->near - camera->far);
    projection[14u] = (camera->near * camera->far) / (camera->near - camera->far);
    projection[11u] = -1.0f;

    /* The product, projection times view - walked as a loop rather than spelled out: sixteen elems
       ents of host arithmetic once a frame, and the loop is where a transposition cannot hide be
       cause the two operands' element order is stated once, above, rather than four times.
    */
    for (column = 0u; column < 4u; column++) {
        for (row = 0u; row < 4u; row++) {
            sum = 0.0f;
            for (term = 0u; term < 4u; term++) {
                sum += projection[(term * 4u) + row] * view[(column * 4u) + term];
            }
            outViewProj[(column * 4u) + row] = sum;
        }
    }

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The camera's place in set 0's block - pure
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_camera_apply(XudDescriptor* descriptor, const XudCamera3D* camera, float aspect) {
    float viewProj[XUD_CAMERA_MATRIX_ELEMENT_COUNT] = { 0.0f };
    uint32_t element = 0u;

    if (descriptor == NULL) {
        return XUD_INVALID_ARG;
    }

    if (camera == NULL) {
        /* No camera live: the IDENTITY matrix, which is the one matrix xud_descriptor_fill_uniform_
           block() opens the block with. The block is rewritten every frame rather than left at
           whatever the previous frame's camera put in it, which is what makes a camera that began
           and ended inside one frame a no-op rather than a leak of state into the next frame. The
           diagonal test is element%5 on a 16-element array, which is the four diagonal elements and
           no others.
        */
        for (element = 0u; element < XUD_CAMERA_MATRIX_ELEMENT_COUNT; element++) {
            viewProj[element] = ((element % 5u) == 0u) ? 1.0f : 0.0f;
        }

        return xud_descriptor_set_view_proj(descriptor, viewProj);
    }

    if (xud_camera_fill_view_proj(camera, aspect, viewProj) != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    return xud_descriptor_set_view_proj(descriptor, viewProj);
}
