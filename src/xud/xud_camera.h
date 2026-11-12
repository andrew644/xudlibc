/***********************************************************************************************
 *
 *   xud_camera.h - the camera rung: the one perspective 3D camera a frame is drawn with
 *
 *   Owns : XudCamera3D, the camera a caller asks for (raylib's Camera3D - reference/raylib/
 *          rcamera.h 102: position, target, up, fovy - plus the two frustum planes this rung needs
 *          and raylib's Camera carries in its Camera aggregate (rcore.h, not in reference/)), and
 *          the arithmetic above it: xud_camera_fill_view_proj(), which turns the camera and
 *          the extent a frame renders at into the one matrix set 0's block carries, xud_camera_app
 *          ly(), which places that matrix in the block, and the rcamera movement + rotation half
 *          (the three basis getters, the four moves, yaw/pitch/roll and the two update calls rayl
 *          ib's UpdateCamera/UpdateCameraPro are) - the record's ORIENTATION and PLACE, as opposed
 *          to its matrix.
 *   Layer: above the descriptor (it writes the block through the descriptor rung's own setter) and
 *          below the renderer, which is the owner that holds the camera and the extent. All of it
 *          but xud_update_camera() is host arithmetic on the caller's floats - no driver call, no
 *          loader row, no Vulkan or GLFW symbol the linker could see - so the default test run as
 *          serts the matrices, the basis and every one of the moves against hand-computed values
 *          with no device present, and a machine with no Vulkan loader answers the same refusals as
 *          one with a loader and no window open. xud_update_camera() is the one call that reads the
 *          context - the input rung's keys, cursor delta and wheel, and the clock rung's frame span
 *          - because raylib's UpdateCamera reads GetMouseDelta, IsKeyDown and GetFrameTime, and in
 *          this library those three are per-context rungs rather than globals.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#ifndef XUD_CAMERA_H_
#define XUD_CAMERA_H_

#include "xud.h" /* Required for: XudResult, XudVector3, the public camera's vector members */
#include "xud_descriptor.h" /* Required for: XudDescriptor, set 0's block xud_camera_apply() writes through the descriptor rung's own setter */
#include <assert.h> /* Required for: static_assert, the matrix's member count */
#include <math.h> /* Required for: sinf/cosf/atanf/sqrtf/fabsf - host arithmetic on the caller's floats, the one system header this rung takes */
#include <stddef.h> /* Required for: sizeof, the matrix copy's length */
#include <stdint.h>

/*-----------------------------------------------------------------------------------------------
 * The rung's own constants
 *-----------------------------------------------------------------------------------------------*/

#define XUD_CAMERA_MATRIX_ELEMENT_COUNT                                                                                                    \
    16u /* The matrix's element count, in ONE place rather than as a 16 written at every loop.                                             \
          The block's viewProj member is float[16] (xud_descriptor.h) and the two matrices below                                           \
          are the same shape, so the count is a fact about the layout rather than a choice - and                                           \
          the assert under it is what keeps the two from drifting apart.                                                                   \
     */

#define XUD_CAMERA_FOVY_MIN_DEG                                                                                                            \
    0.0f /* The aperture floor, exclusive: a fovy of zero is a frustum with no interior, and the                                           \
           refusal below is written as "not greater" so a zero AND a NaN fovy both answer INVALID_                                         \
           ARG rather than a NaN matrix.                                                                                                   \
     */

#define XUD_CAMERA_FOVY_MAX_DEG                                                                                                            \
    180.0f /* The aperture ceiling, exclusive: at 180 degrees and past it the frustum's sides fold                                         \
             back over each other, so the half-angle's cosine is zero or negative and the matrix's                                         \
             scale flips sign - a frustum that contains the world behind the eye rather than the                                           \
             world in front of it.                                                                                                         \
          */

#define XUD_CAMERA_PLANE_MIN_DISTANCE                                                                                                      \
    0.0f /* The near plane's floor, exclusive: a near plane at zero or behind the eye is a frustum                                         \
           whose apex is not in front of the eye, and the depth mapping's denominator (near - far)                                         \
           and the visible-extent division both read it. Written as the floor of a "not greater"                                           \
           test so a NaN near answers INVALID_ARG rather than a NaN matrix.                                                                \
       */

#define XUD_CAMERA_DEGREES_PER_RADIANS                                                                                                     \
    (180.0f                                                                                                                                \
        / 3.1415926535897932f) /* The one degree-to-radian conversion this rung needs, because                                         \
                                     raylib's fovy is in degrees (rcamera.h 113: "field-of-view                                            \
                                     aperture in Y (degrees)") and <math.h>'s trigonometry takes                                           \
                                     radians. A double literal narrowed to float, which is what                                            \
                                     no assert pins it here, because a float comparison is not an                                          \
                                     integer constant expression in C11: the camera area's framing                                         \
                                     checks are what pin it, reading the cotangent of three apert-                                         \
                                     ures against the pinhole's own closed form, which a convert-                                          \
                                     sion off by an order of magnitude would fail by a mile.                                               \
                                 */

#define XUD_CAMERA_AXIS_MIN_LENGTH                                                                                                         \
    1e-6f /* The floor on a basis vector's length before it is called degenerate. A camera whose                                           \
            up vector is parallel to its forward vector has no right vector (the cross product is                                          \
            zero and normalising it divides by zero), and a camera whose eye sits on its target has                                        \
            no forward vector at all - both are refusals rather than a NaN matrix, and the floor is                                        \
            what makes a NaN length a refusal too (NaN compares false against the floor).                                                  \
         */

/* The five modes xud_update_camera() drives a camera with (XUD_CAMERA_MODE_*) and the range check
   they give are in xud.h, next to the declaration that takes one: a token a caller chooses is public
   vocabulary, the speeds below are arithmetic the library performs.
*/

/* The four speeds and the sensitivity, raylib's (rcamera.h 200-208), in world units per second and
   radians per second rather than per frame: each is multiplied by the frame's own span, which is
   what makes the same walk the same speed at 30 frames a second and at 144. A per-frame speed would
   make the machine's frame rate a rule of the scene.
*/
#define XUD_CAMERA_MOVE_SPEED 5.4f /* world units per second, raylib's CAMERA_MOVE_SPEED */
#define XUD_CAMERA_ROTATION_SPEED 0.03f /* radians per second of a held key, raylib's CAMERA_ROTATION_SPEED */
#define XUD_CAMERA_PAN_SPEED 2.0f /* world units per second of a held middle button, raylib's CAMERA_PAN_SPEED */
#define XUD_CAMERA_MOUSE_MOVE_SENSITIVITY 0.003f /* radians per pixel of cursor delta, raylib's CAMERA_MOUSE_MOVE_SENSITIVITY */
#define XUD_CAMERA_ORBITAL_SPEED 0.5f /* radians per second about the target, raylib's CAMERA_ORBITAL_SPEED */

/* The axis the world-plane projection drops: the component of a move vector along the axis the cam
   era's own up points along, which is the axis whose removal puts the move in the plane PERPENDICU
   LAR to up. 0.7071 is raylib's (rcamera.h 258, 294: fabsf(camera->up.z) > 0.7071f) - the sine of
   45 degrees, the point past which one component has to count as the dominant one. A unit axis is
   dominated by exactly one axis, because two components above 0.7071 each would make the squared
   length greater than one.
*/
#define XUD_CAMERA_PLANE_AXIS_DOMINANCE 0.7071f

/* The pitch clamp's margin, in radians: the angle the view vector is kept this far away from the
   camera's own up and from its negation, so a pitch held to the ceiling leaves a view vector that is
   NOT exactly parallel to up - the one degeneracy the basis cannot survive (XUD_CAMERA_AXIS_MIN_LE
   NGTH above). raylib's own margin (rcamera.h 372, 378: "avoid numerical errors").
*/
#define XUD_CAMERA_PITCH_CLAMP_MARGIN 0.001f

/* The floor on the eye-to-target distance, and the same number raylib's CameraMoveToTarget clamps
   to (rcamera.h 318: 0.001f, "Distance must be greater than 0"). A wheel pull that reaches the
   target has to stop this side of it: an eye ON its target has no forward vector at all, and the
   distance is what the next frame's basis is built from.
*/
#define XUD_CAMERA_MIN_TARGET_DISTANCE 0.001f

/*-----------------------------------------------------------------------------------------------
 * The camera a caller asks for
 *
 * The record itself is xud.h's - XudCamera3D sits in the public header beside XudVector3 and XudCol
 * or, in the bucket of value types a caller hands DOWN, because raylib's Camera3D is public API and
 * this rung's job is the arithmetic over it rather than the record's shape. What this header owns is
 * the constants the arithmetic is stated against and the functions below the record.
 *-----------------------------------------------------------------------------------------------*/

static_assert(XUD_CAMERA_MATRIX_ELEMENT_COUNT == 16u,
    "the block's viewProj member is float[16] (xud_descriptor.h) and the two matrices this rung produces are the same shape");

/*-----------------------------------------------------------------------------------------------
 * The three rung-internal steps below: the well-formedness test, the matrix and the block write.
 * The rcamera half this file also answers - the basis getters, the four moves, yaw/pitch/roll and
 * the two update calls - is PUBLIC API and so is declared in xud.h beside the camera record, the
 * same split xud_shapes.h makes between its appends and xud.h's draws: what a caller names lives in
 * one header, what the library's own rungs call lives here.
 *-----------------------------------------------------------------------------------------------*/

/// Pure: the camera record's own well-formedness, independent of the extent a frame renders at -
/// the two frustum planes, the aperture and the three basis degeneracies (an eye on its target, a
/// zero up vector, an up parallel to the eye's forward). The public begin call above the renderer
/// asks this BEFORE storing a camera, so a degenerate camera is refused at the call that asked for
/// it rather than at the frame that draws it; the matrix function below asks it again, because the
/// renderer's record is a value a caller could have tampered since.
XudResult xud_camera_validate(const XudCamera3D* camera);

/// Pure: the camera's view-projection matrix, as the block's float[16] column-major layout (element
/// index = column*4 + row, which is how the block's viewProj member is read as a GLSL mat4). The
/// view half is the camera's own basis - forward = normalize(target - position), right =
/// normalize(cross(forward, up)), up' = cross(right, forward), the same basis raylib's rcamera
/// builds (rcamera.h 244: right = normalize(cross(forward, up))) - and the projection half is the
/// symmetric frustum at the caller's aspect, mapped to Vulkan's [0,1] depth range rather than
/// OpenGL's [-1,1]. A degenerate camera (an eye on its target, a zero up vector, an up parallel to
/// the forward, a zero or NaN fovy, a near plane at or behind the eye, a far plane at or behind the
/// near) answers XUD_INVALID_ARG and leaves outViewProj alone; a camera that is usable answers XUD_
/// OK and fills all 16 elements.
XudResult xud_camera_fill_view_proj(const XudCamera3D* camera, float aspect, float* outViewProj);

/// Pure: the camera's matrix for one frame, placed in set 0's block through the descriptor rung's
/// own setter. A NULL camera is the "no camera live" state and writes the IDENTITY matrix, which is
/// the one matrix xud_descriptor_fill_uniform_block() opens the block with - so the block's matrix
/// is rewritten every frame from the camera state the renderer holds rather than being a value that
/// a previous frame left behind, which is what makes a camera that began and ended inside one frame
/// a no-op rather than a leak of state into the next frame. The aspect is the extent a frame renders
/// at (the framebuffer row's), and a zero or NaN aspect is refused rather than divided by.
XudResult xud_camera_apply(XudDescriptor* descriptor, const XudCamera3D* camera, float aspect);

#endif /* XUD_CAMERA_H_ */
