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
 *     - The depth RANGE. Vulkan's depth buffer is [0,1], the pipeline compares GREATER
 *       (xud_pipeline.h) and the pass clears to 0.0 (xud_renderpass.h), so the projection writes
 *       the REVERSED 1/z map - near plane to 1, far plane to near/far - because 1/z is where the
 *       depth precision lives at both ends of a wide frustum. The OpenGL convention neither maps
 *       to [-1,1] nor to the old near-to-0 linear form: the clip test still discards what leaves
 *       the range, and a surface nearer than the near plane still fails it above 1.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

/* xud_camera.c answers xud.h's public camera calls as well as the rung's own three, so the public
   header arrives through xud_camera.h and xud_core.h, not by itself.
*/
#include "xud_camera.h" /* Required for: XudCamera3D, the rung's constants and the rung's own three declarations */
#include "xud_core.h" /* Required for: struct XudContext, whose input and time rungs xud_update_camera() reads - the one public call here above the context, and the reason this file is the one that reaches above itself */
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
       pipeline's GREATER depth compare are both written against. Column-major storage: element
       index = column*4 + row.
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
       world to the sides rather than a stretched one. The two depth rows are the REVERSED 1/z
       mapping: the numerator is the constant near (the z column is zeroed and the translation is
       near) and the w row's -1 makes w_clip = -z_view = the distance in front of the eye, so
       z_ndc = near/distance - the near plane at 1, the far plane at near/far, and the space
       in between hyperbolically nearer to 1 the closer the surface is. This is the arrangement
       that spends the depth buffer where the precision is needed: the [0,1] linear-in-1/d map
       piles its resolution just past the near plane and leaves the far half of the frustum with
       so little that distant coplanar surfaces fight, while 1/z on the pass's D32_SFLOAT depth
       keeps every octave of distance about equally resolved. The pipeline's compare (LESS flipped
       to GREATER, xud_pipeline.h) and the pass's clear flipped to 0.0 (xud_renderpass.h) are the
       two other halves of the same convention, and a surface between the eye and the near plane
       lands above 1 and is clipped by w - the near clip survives the flip because it was always
       the clip test's, not the depth map's.
    */
    halfAngleRadians = (camera->fovy * 0.5f) / XUD_CAMERA_DEGREES_PER_RADIANS;
    focalLength = cosf(halfAngleRadians) / sinf(halfAngleRadians);
    projection[0u] = focalLength / aspect;
    projection[5u] = focalLength;
    projection[10u] = 0.0f;
    projection[14u] = camera->near;
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

/*-----------------------------------------------------------------------------------------------
 * The basis, read as three unit vectors - what every move and rotation below starts from
 *-----------------------------------------------------------------------------------------------*/

/* The same basis xud_camera_fill_view_proj() builds its view half from, in the same order and by
   the same three steps, so a move and a matrix can never disagree about which way the camera faces.
   The three degeneracies are refusals rather than NaNs: an eye on its target has no forward, a zero
   up has no axis, an up parallel to the forward has no right.
*/
static XudResult xud_camera_basis(const XudCamera3D* camera, float* forward, float* right, float* up) {
    float view[3];
    float rawUp[3];

    if (camera == NULL) {
        return XUD_INVALID_ARG;
    }

    view[0] = camera->target.x - camera->position.x;
    view[1] = camera->target.y - camera->position.y;
    view[2] = camera->target.z - camera->position.z;
    if (xud_camera_normalize(view, forward) != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    rawUp[0] = camera->up.x;
    rawUp[1] = camera->up.y;
    rawUp[2] = camera->up.z;
    if (xud_camera_normalize(rawUp, up) != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    xud_camera_cross(forward, up, view);

    return xud_camera_normalize(view, right);
}

/* The same question as a yes/no, for the half below that has to know whether the record it is about
   to WRITE still has a basis: every move and rotation computes its result into a copy and asks this
   of the copy, so a step that would walk the camera into a degeneracy (a pitch past the vertical with
   no clamp, a NaN distance) leaves the caller's record exactly as it was.
*/
static uint32_t xud_camera_basis_is_usable(const XudCamera3D* camera) {
    float forward[3];
    float right[3];
    float up[3];

    return (xud_camera_basis(camera, forward, right, up) == XUD_OK) ? 1u : 0u;
}

/* Rotate one vector around an axis by an angle in radians - the Euler-Rodrigues form raymath's Vec
   tor3RotateByAxisAngle uses (raymath.h), so the yaw, pitch and roll below turn a view vector the
   same way rcamera's do. The axis is normalised here rather than trusted, and a zero axis is a re
   fusion rather than the identity raymath silently answers (it substitutes a length of 1, which
   leaves the vector unchanged and hides the caller's degeneracy).
*/
static XudResult xud_camera_rotate_by_axis(const float* in, const float* axis, float angle, float* out) {
    float unit[3];
    float quaternion[3];
    float rotated[3];
    float twice[3];
    float sine = 0.0f;
    float cosine = 0.0f;
    uint32_t component = 0u;

    if (xud_camera_normalize(axis, unit) != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    sine = sinf(angle * 0.5f);
    cosine = cosf(angle * 0.5f);
    for (component = 0u; component < 3u; component++) {
        quaternion[component] = unit[component] * sine;
    }

    xud_camera_cross(quaternion, in, rotated);
    xud_camera_cross(quaternion, rotated, twice);

    for (component = 0u; component < 3u; component++) {
        out[component] = in[component] + (2.0f * cosine * rotated[component]) + (2.0f * twice[component]);
    }

    return XUD_OK;
}

/* The unsigned angle between two unit vectors, atan2 of the cross's length over the dot - the form
   that keeps its accuracy near zero and near pi, which is exactly where the pitch clamp below works.
*/
static float xud_camera_angle(const float* left, const float* right) {
    float cross[3];

    xud_camera_cross(left, right, cross);

    return atan2f(sqrtf(xud_camera_dot(cross, cross)), xud_camera_dot(left, right));
}

/*-----------------------------------------------------------------------------------------------
 * The basis, read by a caller - raylib's GetCameraForward / GetCameraUp / GetCameraRight
 *-----------------------------------------------------------------------------------------------*/

/// See xud.h. The three getters share one read: the basis the matrix is built from, in whichever
/// of its three vectors the caller asked for.
static XudResult xud_camera_get_axis(XudVector3* outAxis, const XudCamera3D* camera, uint32_t which) {
    float forward[3];
    float right[3];
    float up[3];
    const float* chosen = NULL;

    if (outAxis == NULL) {
        return XUD_INVALID_ARG;
    }
    *outAxis = (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 0.0f };

    if (xud_camera_basis(camera, forward, right, up) != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    chosen = (which == 0u) ? forward : ((which == 1u) ? up : right);

    outAxis->x = chosen[0];
    outAxis->y = chosen[1];
    outAxis->z = chosen[2];

    return XUD_OK;
}

XudResult xud_get_camera_forward(XudVector3* outForward, const XudCamera3D* camera) { return xud_camera_get_axis(outForward, camera, 0u); }

XudResult xud_get_camera_up(XudVector3* outUp, const XudCamera3D* camera) { return xud_camera_get_axis(outUp, camera, 1u); }

XudResult xud_get_camera_right(XudVector3* outRight, const XudCamera3D* camera) { return xud_camera_get_axis(outRight, camera, 2u); }

/*-----------------------------------------------------------------------------------------------
 * The two ways between the frame's pixels and the world the camera frames
 *-----------------------------------------------------------------------------------------------*/

/* The fold the two directions below share: the camera's basis, and the two scales a normalised
   device coordinate is measured in at one world unit of distance. The focal length is the same
   cotangent xud_camera_fill_view_proj() puts in the projection's diagonal and the vertical is
   cross(right, forward) rather than the record's up for the same reason: the view matrix takes the
   tilted up out of the frame's vertical, so a label placed by this fold lands where the geometry
   went rather than beside it.
*/
static XudResult xud_camera_fill_fold(const XudCamera3D* camera, float width, float height, float* forward, float* right, float* vertical,
    float* halfWidthAtUnit, float* halfHeightAtUnit) {
    float up[3];
    float halfAngleRadians = 0.0f;
    float focalLength = 0.0f;

    /* The not-greater form on both axes: a zero extent is a frame with no pixels to land on, a
       negative one is a fold that would put the frame's centre off the frame, and a NaN is refused
       by the same comparison rather than divided by.
    */
    if ((!(width > 0.0f)) || (!(height > 0.0f))) {
        return XUD_INVALID_ARG;
    }
    if (xud_camera_validate(camera) != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    if (xud_camera_basis(camera, forward, right, up) != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    xud_camera_cross(right, forward, vertical);

    halfAngleRadians = (camera->fovy * 0.5f) / XUD_CAMERA_DEGREES_PER_RADIANS;
    focalLength = cosf(halfAngleRadians) / sinf(halfAngleRadians);
    *halfHeightAtUnit = 1.0f / focalLength;
    *halfWidthAtUnit = (width / height) * (*halfHeightAtUnit);

    return XUD_OK;
}

XudResult xud_camera_fill_screen_point(const XudCamera3D* camera, float width, float height, XudVector3 position, XudVector2* outScreen) {
    float forward[3];
    float right[3];
    float vertical[3];
    float toPoint[3];
    float halfWidthAtUnit = 0.0f;
    float halfHeightAtUnit = 0.0f;
    float distance = 0.0f;

    if (outScreen == NULL) {
        return XUD_INVALID_ARG;
    }
    *outScreen = (XudVector2) { .x = 0.0f, .y = 0.0f };

    if (xud_camera_fill_fold(camera, width, height, forward, right, vertical, &halfWidthAtUnit, &halfHeightAtUnit) != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    toPoint[0] = position.x - camera->position.x;
    toPoint[1] = position.y - camera->position.y;
    toPoint[2] = position.z - camera->position.z;

    /* How far in front of the eye the point is: the view matrix's own w, which is the divisor the
       two device coordinates are put over. A point at the eye or behind it has no screen position,
       and the not-greater form refuses it along with a NaN distance - raylib divides blindly there
       and answers the mirrored position, which is a label drawn on the far side of the frame from
       the thing it labels.
    */
    distance = xud_camera_dot(forward, toPoint);
    if (!(distance > 0.0f)) {
        return XUD_INVALID_ARG;
    }

    /* The two device coordinates, then the viewport fold: the horizontal is the sideways offset
       over how wide the frame is AT THIS DISTANCE, so the distance cancels the way the pinhole's
       does, and the vertical is flipped on the way in because the frame's rows are counted from the
       top while the camera's are counted from the middle - the same flip raylib's own NDC fold
       applies (rcore.c 1543).
    */
    outScreen->x = ((xud_camera_dot(right, toPoint) / (distance * halfWidthAtUnit)) + 1.0f) * 0.5f * width;
    outScreen->y = (1.0f - (xud_camera_dot(vertical, toPoint) / (distance * halfHeightAtUnit))) * 0.5f * height;

    return XUD_OK;
}

XudResult xud_camera_fill_world_ray(const XudCamera3D* camera, float width, float height, XudVector2 screen, XudRay* outRay) {
    float forward[3];
    float right[3];
    float vertical[3];
    float halfWidthAtUnit = 0.0f;
    float halfHeightAtUnit = 0.0f;
    float deviceX = 0.0f;
    float deviceY = 0.0f;
    float direction[3] = { 0.0f, 0.0f, 0.0f };

    if (outRay == NULL) {
        return XUD_INVALID_ARG;
    }
    *outRay = (XudRay) { .position = { .x = 0.0f, .y = 0.0f, .z = 0.0f }, .direction = { .x = 0.0f, .y = 0.0f, .z = 0.0f } };

    if (xud_camera_fill_fold(camera, width, height, forward, right, vertical, &halfWidthAtUnit, &halfHeightAtUnit) != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    /* The pixel's own device coordinates, y flipped on the way in for the same reason the fold
       above flips it on the way out (rcore.c 1412, where raylib builds the same two numbers).
    */
    deviceX = ((2.0f * screen.x) / width) - 1.0f;
    deviceY = 1.0f - ((2.0f * screen.y) / height);

    /* One step along each of the three: the forward by one unit, the sideways pair by how far the
       frame reaches at that distance. Normalising the sum is what turns the pixel's position in the
       frame into a direction, and it is the same normalisation the matrix's basis went through, so a
       ray built here and a frame drawn there agree about the pixel the ray came from.
    */
    direction[0] = forward[0] + (right[0] * deviceX * halfWidthAtUnit) + (vertical[0] * deviceY * halfHeightAtUnit);
    direction[1] = forward[1] + (right[1] * deviceX * halfWidthAtUnit) + (vertical[1] * deviceY * halfHeightAtUnit);
    direction[2] = forward[2] + (right[2] * deviceX * halfWidthAtUnit) + (vertical[2] * deviceY * halfHeightAtUnit);
    if (xud_camera_normalize(direction, direction) != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    /* The eye is the origin, which is the one case of raylib's two (rcore.c 1454: the eye for a
       perspective camera, a point on the camera plane for an orthographic one) - this library's one
       camera is perspective, and xud.h says so beside the mode list.
    */
    outRay->position = camera->position;
    outRay->direction.x = direction[0];
    outRay->direction.y = direction[1];
    outRay->direction.z = direction[2];

    return XUD_OK;
}

XudResult xud_get_screen_to_world_ray_ex(XudRay* outRay, XudVector2 screen, const XudCamera3D* camera, float width, float height) {
    return xud_camera_fill_world_ray(camera, width, height, screen, outRay);
}

XudResult xud_get_screen_to_world_ray(XudRay* outRay, XudContext* context, XudVector2 screen, const XudCamera3D* camera) {
    if (context == NULL) {
        return XUD_INVALID_ARG;
    }
    if ((context->window == NULL) || (context->width == 0u) || (context->height == 0u)) {
        /* The extent a frame renders at is what the pixel is a fraction OF, and a context that never
           completed bring-up - or whose swapchain has no extent yet - has no such fraction.
        */
        return XUD_UNSUPPORTED;
    }

    return xud_camera_fill_world_ray(camera, (float)context->width, (float)context->height, screen, outRay);
}

XudResult xud_get_world_to_screen_ex(XudVector2* outScreen, XudVector3 position, const XudCamera3D* camera, float width, float height) {
    return xud_camera_fill_screen_point(camera, width, height, position, outScreen);
}

XudResult xud_get_world_to_screen(XudVector2* outScreen, XudContext* context, XudVector3 position, const XudCamera3D* camera) {
    if (context == NULL) {
        return XUD_INVALID_ARG;
    }
    if ((context->window == NULL) || (context->width == 0u) || (context->height == 0u)) {
        return XUD_UNSUPPORTED;
    }

    return xud_camera_fill_screen_point(camera, (float)context->width, (float)context->height, position, outScreen);
}

/*-----------------------------------------------------------------------------------------------
 * The moves - raylib's CameraMoveForward / MoveUp / MoveRight / MoveToTarget
 *
 * Every one of them moves the EYE and the TARGET together, because the record's direction is the
 * pair's difference and a move that touched only the eye would be a turn as well as a step. Each
 * computes its result into a copy and refuses that copy's basis before storing it, so a refusal -
 * a NaN distance, a step that degenerates the basis - leaves the caller's record untouched.
 *-----------------------------------------------------------------------------------------------*/

/* Project a step onto the plane perpendicular to the camera's own up, by dropping the component
   along the axis up is dominated by (raylib's fabsf(up.z) > 0.7071f ladder, rcamera.h 258) and
   re-normalising what is left. A step along the up axis itself has nothing left to re-normalise and
   is a refusal rather than a step of zero length dressed up as one.
*/
static XudResult xud_camera_project_on_world_plane(const XudCamera3D* camera, float* step) {
    const float absoluteX = fabsf(camera->up.x);
    const float absoluteZ = fabsf(camera->up.z);

    if (absoluteZ > XUD_CAMERA_PLANE_AXIS_DOMINANCE) {
        step[2] = 0.0f;
    } else if (absoluteX > XUD_CAMERA_PLANE_AXIS_DOMINANCE) {
        step[0] = 0.0f;
    } else {
        step[1] = 0.0f;
    }

    return xud_camera_normalize(step, step);
}

/* forward or right, scaled by the distance and optionally flattened onto the world plane - the one
   half CameraMoveForward and CameraMoveRight share, rcamera.h 251-307.
*/
static XudResult xud_camera_step(const XudCamera3D* camera, const float* axis, float distance, bool moveInWorldPlane, float* outStep) {
    float step[3];
    uint32_t component = 0u;

    for (component = 0u; component < 3u; component++) {
        step[component] = axis[component] * distance;
    }

    /* A distance of zero is NO STEP, and that has to be said rather than computed: the projection
       below re-normalises what is left of the step, and a step of zero length has no direction to
       normalise, so an unguarded projection refuses a caller that asked for nothing - which is what
       xud_update_camera_pro() would hit on every axis its caller left at zero. A NaN distance is
       NOT caught here (NaN compares false against zero) and travels on to the basis test.
    */
    if (moveInWorldPlane && (distance != 0.0f)) {
        if (xud_camera_project_on_world_plane(camera, step) != XUD_OK) {
            return XUD_INVALID_ARG;
        }
        for (component = 0u; component < 3u; component++) {
            step[component] *= distance;
        }
    }

    outStep[0] = step[0];
    outStep[1] = step[1];
    outStep[2] = step[2];

    return XUD_OK;
}

XudResult xud_camera_move_forward(XudCamera3D* camera, float distance, bool moveInWorldPlane) {
    XudCamera3D moved;
    float forward[3];
    float right[3];
    float up[3];
    float step[3];

    if (camera == NULL) {
        return XUD_INVALID_ARG;
    }
    if (xud_camera_basis(camera, forward, right, up) != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    if (xud_camera_step(camera, forward, distance, moveInWorldPlane, step) != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    moved = *camera;
    moved.position.x += step[0];
    moved.position.y += step[1];
    moved.position.z += step[2];
    moved.target.x += step[0];
    moved.target.y += step[1];
    moved.target.z += step[2];

    if (xud_camera_basis_is_usable(&moved) == 0u) {
        return XUD_INVALID_ARG;
    }

    *camera = moved;

    return XUD_OK;
}

XudResult xud_camera_move_right(XudCamera3D* camera, float distance, bool moveInWorldPlane) {
    XudCamera3D moved;
    float forward[3];
    float right[3];
    float up[3];
    float step[3];

    if (camera == NULL) {
        return XUD_INVALID_ARG;
    }
    if (xud_camera_basis(camera, forward, right, up) != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    if (xud_camera_step(camera, right, distance, moveInWorldPlane, step) != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    moved = *camera;
    moved.position.x += step[0];
    moved.position.y += step[1];
    moved.position.z += step[2];
    moved.target.x += step[0];
    moved.target.y += step[1];
    moved.target.z += step[2];

    if (xud_camera_basis_is_usable(&moved) == 0u) {
        return XUD_INVALID_ARG;
    }

    *camera = moved;

    return XUD_OK;
}

XudResult xud_camera_move_up(XudCamera3D* camera, float distance) {
    XudCamera3D moved;
    float forward[3];
    float right[3];
    float up[3];

    if (camera == NULL) {
        return XUD_INVALID_ARG;
    }
    if (xud_camera_basis(camera, forward, right, up) != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    moved = *camera;
    moved.position.x += up[0] * distance;
    moved.position.y += up[1] * distance;
    moved.position.z += up[2] * distance;
    moved.target.x += up[0] * distance;
    moved.target.y += up[1] * distance;
    moved.target.z += up[2] * distance;

    if (xud_camera_basis_is_usable(&moved) == 0u) {
        return XUD_INVALID_ARG;
    }

    *camera = moved;

    return XUD_OK;
}

XudResult xud_camera_move_to_target(XudCamera3D* camera, float delta) {
    XudCamera3D moved;
    float forward[3];
    float right[3];
    float up[3];
    float view[3];
    float distance = 0.0f;

    if (camera == NULL) {
        return XUD_INVALID_ARG;
    }
    if (xud_camera_basis(camera, forward, right, up) != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    view[0] = camera->position.x - camera->target.x;
    view[1] = camera->position.y - camera->target.y;
    view[2] = camera->position.z - camera->target.z;
    distance = sqrtf(xud_camera_dot(view, view)) + delta;

    /* The distance is a distance: rcamera.h 318 clamps a delta that reaches zero or past it to a
       hair above zero rather than letting the eye cross its target, which would put the view vector
       through the origin and flip the basis. The same floor, stated as this rung's own constant.
    */
    if (!(distance > XUD_CAMERA_MIN_TARGET_DISTANCE)) {
        distance = XUD_CAMERA_MIN_TARGET_DISTANCE;
    }

    moved = *camera;
    moved.position.x = camera->target.x - (forward[0] * distance);
    moved.position.y = camera->target.y - (forward[1] * distance);
    moved.position.z = camera->target.z - (forward[2] * distance);

    if (xud_camera_basis_is_usable(&moved) == 0u) {
        return XUD_INVALID_ARG;
    }

    *camera = moved;

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The rotations - raylib's CameraYaw / CameraPitch / CameraRoll
 *
 * All three rotate the VIEW VECTOR (target - position) around one of the camera's own axes, and the
 * rotateAroundTarget flag says which end of the record moves: the eye orbits the target (the third
 * person's and the orbital's reading) or the target swings under the eye (the first person's). The
 * angle is in radians, as raylib states it; degrees are the caller's or the mode's to convert.
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_camera_yaw(XudCamera3D* camera, float angle, bool rotateAroundTarget) {
    XudCamera3D turned;
    float forward[3];
    float right[3];
    float up[3];
    float view[3];
    float turnedView[3];

    if (camera == NULL) {
        return XUD_INVALID_ARG;
    }
    if (xud_camera_basis(camera, forward, right, up) != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    view[0] = camera->target.x - camera->position.x;
    view[1] = camera->target.y - camera->position.y;
    view[2] = camera->target.z - camera->position.z;

    if (xud_camera_rotate_by_axis(view, up, angle, turnedView) != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    turned = *camera;
    if (rotateAroundTarget) {
        turned.position.x = camera->target.x - turnedView[0];
        turned.position.y = camera->target.y - turnedView[1];
        turned.position.z = camera->target.z - turnedView[2];
    } else {
        turned.target.x = camera->position.x + turnedView[0];
        turned.target.y = camera->position.y + turnedView[1];
        turned.target.z = camera->position.z + turnedView[2];
    }

    if (xud_camera_basis_is_usable(&turned) == 0u) {
        return XUD_INVALID_ARG;
    }

    *camera = turned;

    return XUD_OK;
}

/* See xud.h. The clamp is the lockView half: the angle is cut to what leaves the view vector inside
   the cone between the camera's own up and its negation, minus this rung's margin, so a pitch held
   to the ceiling cannot put the view vector ON the up axis - the somersault rcamera.h 365 avoids the
   same way. Without the clamp a caller can still roll a camera past the vertical, and the resulting
   basis test below is what refuses it rather than a NaN matrix one call later.
*/
XudResult xud_camera_pitch(XudCamera3D* camera, float angle, bool lockView, bool rotateAroundTarget, bool rotateUp) {
    XudCamera3D turned;
    float forward[3];
    float right[3];
    float up[3];
    float view[3];
    float turnedView[3];
    float turnedUp[3];
    float maxUp = 0.0f;
    float maxDown = 0.0f;

    if (camera == NULL) {
        return XUD_INVALID_ARG;
    }
    if (xud_camera_basis(camera, forward, right, up) != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    view[0] = camera->target.x - camera->position.x;
    view[1] = camera->target.y - camera->position.y;
    view[2] = camera->target.z - camera->position.z;

    if (lockView) {
        const float down[3] = { -up[0], -up[1], -up[2] };

        maxUp = xud_camera_angle(up, view) - XUD_CAMERA_PITCH_CLAMP_MARGIN;
        if (angle > maxUp) {
            angle = maxUp;
        }

        maxDown = -xud_camera_angle(down, view) + XUD_CAMERA_PITCH_CLAMP_MARGIN;
        if (angle < maxDown) {
            angle = maxDown;
        }
    }

    if (xud_camera_rotate_by_axis(view, right, angle, turnedView) != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    turned = *camera;
    if (rotateAroundTarget) {
        turned.position.x = camera->target.x - turnedView[0];
        turned.position.y = camera->target.y - turnedView[1];
        turned.position.z = camera->target.z - turnedView[2];
    } else {
        turned.target.x = camera->position.x + turnedView[0];
        turned.target.y = camera->position.y + turnedView[1];
        turned.target.z = camera->position.z + turnedView[2];
    }

    turnedUp[0] = camera->up.x;
    turnedUp[1] = camera->up.y;
    turnedUp[2] = camera->up.z;
    if (rotateUp) {
        if (xud_camera_rotate_by_axis(turnedUp, right, angle, turnedUp) != XUD_OK) {
            return XUD_INVALID_ARG;
        }
        turned.up.x = turnedUp[0];
        turned.up.y = turnedUp[1];
        turned.up.z = turnedUp[2];
    }

    if (xud_camera_basis_is_usable(&turned) == 0u) {
        return XUD_INVALID_ARG;
    }

    *camera = turned;

    return XUD_OK;
}

XudResult xud_camera_roll(XudCamera3D* camera, float angle) {
    XudCamera3D turned;
    float forward[3];
    float right[3];
    float up[3];
    float turnedUp[3];
    float rawUp[3];

    if (camera == NULL) {
        return XUD_INVALID_ARG;
    }
    if (xud_camera_basis(camera, forward, right, up) != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    /* The caller's own up, rotated - not the normalised one. raylib's CameraRoll turns camera->up as
       given (rcamera.h 415), and the record's up LENGTH is the caller's: a camera whose up was
       handed in at twice a unit would otherwise find its first roll quietly rewriting that.
    */
    rawUp[0] = camera->up.x;
    rawUp[1] = camera->up.y;
    rawUp[2] = camera->up.z;
    if (xud_camera_rotate_by_axis(rawUp, forward, angle, turnedUp) != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    turned = *camera;
    turned.up.x = turnedUp[0];
    turned.up.y = turnedUp[1];
    turned.up.z = turnedUp[2];

    if (xud_camera_basis_is_usable(&turned) == 0u) {
        return XUD_INVALID_ARG;
    }

    *camera = turned;

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The two updates above the record
 *
 * xud_update_camera_pro() takes its movement, rotation and zoom from the caller and touches
 * nothing else; xud_update_camera() reads them from the context, which is raylib's UpdateCamera
 * read literally (GetFrameTime, GetMouseDelta, GetMouseWheelMove, IsKeyDown, IsKeyPressed and one
 * gamepad axis pair). The gamepad half is NOT here: this library owns no gamepad rung and vendored
 * GLFW's joystick subsystem is not wired to one (see xud_input.h's "What this layer does NOT own"),
 * so the axes a pad would supply are the caller's to hand to xud_update_camera_pro() instead.
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_update_camera_pro(XudCamera3D* camera, XudVector3 movement, XudVector3 rotation, float zoom) {
    XudCamera3D moved;

    if (camera == NULL) {
        return XUD_INVALID_ARG;
    }

    /* The whole record, not only its basis: this call exists to leave a camera that can be drawn
       with, so the aperture and the two planes are part of the question - and the ask happens on
       the caller's record BEFORE any of the steps below move it, so a refusal leaves it alone.
    */
    if (xud_camera_validate(camera) != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    moved = *camera;

    /* The seven steps in raylib's order (rcamera.h 549-559): pitch, yaw, roll, forward, right, up,
       then the pull toward the target. Every one of them is the rung's own primitive, so the sum is
       the same arithmetic a caller could write by hand - and every one of them works on the copy,
       so the caller's record changes only when all seven have landed.
    */
    if (xud_camera_pitch(&moved, -rotation.y, true, false, false) != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    if (xud_camera_yaw(&moved, -rotation.x, false) != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    if (xud_camera_roll(&moved, rotation.z) != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    if (xud_camera_move_forward(&moved, movement.x, true) != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    if (xud_camera_move_right(&moved, movement.y, true) != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    if (xud_camera_move_up(&moved, movement.z) != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    if (xud_camera_move_to_target(&moved, zoom) != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    *camera = moved;

    return XUD_OK;
}

/* One key's held state, as a yes/no. A refusal is answered "not held" rather than forwarded: the
   reach guard in xud_update_camera() has already ruled out the only state the input queries refuse
   on (a context with no input rung), and a camera mode is not the place to turn a key nobody is
   holding into an error code a caller would have to branch on four times over.
*/
static uint32_t xud_camera_key_held(XudContext* context, int key) {
    bool held = false;

    return ((xud_is_key_down(&held, context, key) == XUD_OK) && held) ? 1u : 0u;
}

static uint32_t xud_camera_key_tapped(XudContext* context, int key) {
    bool tapped = false;

    return ((xud_is_key_pressed(&tapped, context, key) == XUD_OK) && tapped) ? 1u : 0u;
}

XudResult xud_update_camera(XudContext* context, XudCamera3D* camera, uint32_t mode) {
    XudCamera3D moved;
    XudVector2 cursorDelta = { .x = 0.0f, .y = 0.0f };
    float frameTime = 0.0f;
    float wheel = 0.0f;
    float zoom = 0.0f;
    float moveSpeed = 0.0f;
    float rotationSpeed = 0.0f;
    float panSpeed = 0.0f;
    bool middleHeld = false;
    bool moveInWorldPlane = false;
    bool rotateAroundTarget = false;
    bool lockView = false;
    XudResult result = XUD_OK;

    if ((context == NULL) || (camera == NULL)) {
        return XUD_INVALID_ARG;
    }
    if (mode >= XUD_CAMERA_MODE_COUNT) {
        return XUD_INVALID_ARG;
    }
    if ((context->input == NULL) || (context->time == NULL)) {
        /* The two rungs this call reads: a context that never completed bring-up has neither, and
           the answer is the one every other public call above a missing rung gives.
        */
        return XUD_UNSUPPORTED;
    }

    /* Asked of the caller's record before anything below moves it, so a refused update is the same
       camera the caller arrived with - and the mode handlers below can then take the basis of the
       copy they work on for granted.
    */
    if (xud_camera_validate(camera) != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    moved = *camera;

    if (mode == XUD_CAMERA_MODE_CUSTOM) {
        /* The mode whose whole meaning is "the caller is driving" (rcamera.h 460: `if (mode ==
           CAMERA_CUSTOM) {}`). XUD_OK, because the call did what the mode asks.
        */
        return XUD_OK;
    }

    result = xud_get_frame_time(&frameTime, context);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_get_mouse_delta(&cursorDelta, context);
    if (result != XUD_OK) {
        return result;
    }

    /* The three flags raylib derives from the mode (rcamera.h 449-451) and the three speeds scaled
       by the frame that just completed. lockView is true for every mode that reaches here: the one
       mode it was false for is CUSTOM, which returned above. A first frame's span is zero, so the
       first update is a no-op rather than a jump - the same answer raylib's first GetFrameTime()
       gives.
    */
    moveInWorldPlane = ((mode == XUD_CAMERA_MODE_FIRST_PERSON) || (mode == XUD_CAMERA_MODE_THIRD_PERSON));
    rotateAroundTarget = ((mode == XUD_CAMERA_MODE_THIRD_PERSON) || (mode == XUD_CAMERA_MODE_ORBITAL));
    lockView = true;
    moveSpeed = XUD_CAMERA_MOVE_SPEED * frameTime;
    rotationSpeed = XUD_CAMERA_ROTATION_SPEED * frameTime;
    panSpeed = XUD_CAMERA_PAN_SPEED * frameTime;

    if (mode == XUD_CAMERA_MODE_ORBITAL) {
        /* The eye circles the target at the distance it already holds: the view vector swung around
           the camera's own up by the frame's slice of the orbital speed (rcamera.h 463-466). Keys
           and mouse are NOT read in this mode - the orbital camera has nothing but its own swing and
           the wheel below.
        */
        float forward[3];
        float right[3];
        float up[3];
        float view[3];
        float swung[3];

        if (xud_camera_basis(&moved, forward, right, up) != XUD_OK) {
            return XUD_INVALID_ARG;
        }
        view[0] = moved.position.x - moved.target.x;
        view[1] = moved.position.y - moved.target.y;
        view[2] = moved.position.z - moved.target.z;
        if (xud_camera_rotate_by_axis(view, up, XUD_CAMERA_ORBITAL_SPEED * frameTime, swung) != XUD_OK) {
            return XUD_INVALID_ARG;
        }
        moved.position.x = moved.target.x + swung[0];
        moved.position.y = moved.target.y + swung[1];
        moved.position.z = moved.target.z + swung[2];

        /* Asked of the SWUNG record: the question is whether the eye this frame put somewhere is
           still off its target and still has an axis to rotate over, not whether the one it came
           from had - which the validate at the top of this call already knowed.
        */
        if (xud_camera_basis_is_usable(&moved) == 0u) {
            return XUD_INVALID_ARG;
        }
    } else {
        /* The keyboard's rotations, then the mouse's, then the moves - raylib's order (rcamera.h
           471-499), so a frame that turns and steps in the same frame turns first.
        */
        if (xud_camera_key_held(context, XUD_KEY_DOWN) != 0u) {
            if (xud_camera_pitch(&moved, -rotationSpeed, lockView, rotateAroundTarget, false) != XUD_OK) {
                return XUD_INVALID_ARG;
            }
        }
        if (xud_camera_key_held(context, XUD_KEY_UP) != 0u) {
            if (xud_camera_pitch(&moved, rotationSpeed, lockView, rotateAroundTarget, false) != XUD_OK) {
                return XUD_INVALID_ARG;
            }
        }
        if (xud_camera_key_held(context, XUD_KEY_RIGHT) != 0u) {
            if (xud_camera_yaw(&moved, -rotationSpeed, rotateAroundTarget) != XUD_OK) {
                return XUD_INVALID_ARG;
            }
        }
        if (xud_camera_key_held(context, XUD_KEY_LEFT) != 0u) {
            if (xud_camera_yaw(&moved, rotationSpeed, rotateAroundTarget) != XUD_OK) {
                return XUD_INVALID_ARG;
            }
        }
        if (xud_camera_key_held(context, XUD_KEY_Q) != 0u) {
            if (xud_camera_roll(&moved, -rotationSpeed) != XUD_OK) {
                return XUD_INVALID_ARG;
            }
        }
        if (xud_camera_key_held(context, XUD_KEY_E) != 0u) {
            if (xud_camera_roll(&moved, rotationSpeed) != XUD_OK) {
                return XUD_INVALID_ARG;
            }
        }

        if ((mode == XUD_CAMERA_MODE_FREE) && (xud_is_mouse_button_down(&middleHeld, context, XUD_MOUSE_BUTTON_MIDDLE) == XUD_OK)
            && middleHeld) {
            /* Free flight's pan: the middle button drags the whole camera across the screen plane
               (rcamera.h 480-487) - the sign of the cursor delta, not its size, so the drag is the
               pan speed rather than an arbitrary multiple of the hand's jitter.
            */
            if (xud_camera_move_right(
                    &moved, ((cursorDelta.x > 0.0f) ? -panSpeed : ((cursorDelta.x < 0.0f) ? panSpeed : 0.0f)), moveInWorldPlane)
                != XUD_OK) {
                return XUD_INVALID_ARG;
            }
            if (xud_camera_move_up(&moved, ((cursorDelta.y > 0.0f) ? -panSpeed : ((cursorDelta.y < 0.0f) ? panSpeed : 0.0f))) != XUD_OK) {
                return XUD_INVALID_ARG;
            }
        } else {
            if (xud_camera_yaw(&moved, -cursorDelta.x * XUD_CAMERA_MOUSE_MOVE_SENSITIVITY, rotateAroundTarget) != XUD_OK) {
                return XUD_INVALID_ARG;
            }
            if (xud_camera_pitch(&moved, -cursorDelta.y * XUD_CAMERA_MOUSE_MOVE_SENSITIVITY, lockView, rotateAroundTarget, false)
                != XUD_OK) {
                return XUD_INVALID_ARG;
            }
        }

        if (xud_camera_key_held(context, XUD_KEY_W) != 0u) {
            if (xud_camera_move_forward(&moved, moveSpeed, moveInWorldPlane) != XUD_OK) {
                return XUD_INVALID_ARG;
            }
        }
        if (xud_camera_key_held(context, XUD_KEY_S) != 0u) {
            if (xud_camera_move_forward(&moved, -moveSpeed, moveInWorldPlane) != XUD_OK) {
                return XUD_INVALID_ARG;
            }
        }
        if (xud_camera_key_held(context, XUD_KEY_A) != 0u) {
            if (xud_camera_move_right(&moved, -moveSpeed, moveInWorldPlane) != XUD_OK) {
                return XUD_INVALID_ARG;
            }
        }
        if (xud_camera_key_held(context, XUD_KEY_D) != 0u) {
            if (xud_camera_move_right(&moved, moveSpeed, moveInWorldPlane) != XUD_OK) {
                return XUD_INVALID_ARG;
            }
        }
        if (mode == XUD_CAMERA_MODE_FREE) {
            if (xud_camera_key_held(context, XUD_KEY_SPACE) != 0u) {
                if (xud_camera_move_up(&moved, moveSpeed) != XUD_OK) {
                    return XUD_INVALID_ARG;
                }
            }
            if (xud_camera_key_held(context, XUD_KEY_LEFT_CONTROL) != 0u) {
                if (xud_camera_move_up(&moved, -moveSpeed) != XUD_OK) {
                    return XUD_INVALID_ARG;
                }
            }
        }
    }

    /* The wheel, for the three modes that hold a target at a distance (rcamera.h 521-527). The zoom
       is summed first and applied once, so a frame that scrolled AND tapped the keypad moves once
       by the total rather than twice by each half - and a frame with nothing to apply leaves the
       distance alone instead of rebuilding the position along the forward for a delta of zero.
    */
    if ((mode == XUD_CAMERA_MODE_THIRD_PERSON) || (mode == XUD_CAMERA_MODE_ORBITAL) || (mode == XUD_CAMERA_MODE_FREE)) {
        result = xud_get_mouse_wheel_move(&wheel, context);
        if (result != XUD_OK) {
            return result;
        }
        zoom = -wheel;
        if (xud_camera_key_tapped(context, XUD_KEY_KP_SUBTRACT) != 0u) {
            zoom += 2.0f;
        }
        if (xud_camera_key_tapped(context, XUD_KEY_KP_ADD) != 0u) {
            zoom -= 2.0f;
        }
        if (zoom != 0.0f) {
            if (xud_camera_move_to_target(&moved, zoom) != XUD_OK) {
                return XUD_INVALID_ARG;
            }
        }
    }

    /* Every step landed: the caller's record is the updated one, and nothing before this line could
       have written it.
    */
    *camera = moved;

    return XUD_OK;
}
