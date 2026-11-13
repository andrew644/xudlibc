/***********************************************************************************************
 *
 *   xud_test_camera.c - the camera rung and the public camera pair above it
 *
 *   Owns : the tests of one rung - xud_camera_validate(), xud_camera_fill_view_proj() and
 *          xud_camera_apply(), and the public pair xud_begin_camera_3d()/xud_end_camera_3d() -
 *          and nothing else: the descriptor block's own assembly is the descriptor area's, the
 *          frame loop that uploads the block is the frame area's, and the renderer that HOLDS a
 *          camera is the renderer area's.
 *   Layer: tests, above the shapes area and below the core area, which is the layer order: the
 *          matrix is checked against hand-computed rows with no context and no driver in sight,
 *          and the public pair is checked against a real context in the area's own GPU half. The
 *          pure half is where the whole of this rung's correctness lives - the rung is host arith
 *          metic on the caller's floats, so every one of its facts is answerable with no device
 *          present, and that is the reason the rung exists as a separate file at all.
 *
 *   What the checks are stated against, and not against: the matrix is read by TRANSFORMING world
 *   points and reading the clip coordinates back, never by comparing elements to a remembered ta
 *   ble. A check that compared m[11] to -1 would pass a matrix that is transposed-and-mirrored,
 *   which is the single most likely way to write a projection wrong; a check that the point two
 *   units in front of the eye lands at a depth of 2/3 with a w of 2 cannot. The expected values
 *   come from closed forms stated in the test's own words (the depth form far*(d-near)/(d*(far
 *   near)) is the pinhole's, not the matrix rows the implementation writes, and the framing form
 *   h*focal/(d*aspect) is the pinhole's too), so the test and the implementation agree about the
 *   GEOMETRY and disagree about the ALGEBRA, which is what makes a wrong product visible.
 *
 *   Deterministic: fixed cameras, fixed points, no time, no randomness, no filesystem. The float
 *   comparisons are read against a tolerance because every one of them crosses a sinf/cosf or a
 *   division; the tolerance is one constant stated once, so a failure past it is a geometry bug
 *   rather than a rounding artefact.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include <math.h> /* Required for: NAN and the test's own closed forms, which are the pinhole's rather than the matrix rows' */
#include <string.h> /* Required for: memset, the descriptor shell and the context shell this area builds itself */

#include "xud.h" /* Required for: XudResult, XudContext, XudCamera3D, XudVector3, the public camera pair */
#include "xud_camera.h" /* Required for: the rung's three functions and its constants */
#include "xud_core.h" /* Required for: struct XudContext, whose renderer field the pure half's shell fills */
#include "xud_descriptor.h" /* Required for: XudDescriptor, whose set 0 block xud_camera_apply() writes through the descriptor rung's setter */
#include "xud_renderer.h" /* Required for: XudRenderer, the record the public pair stores a camera into */
#include "xud_test_harness.h"
#include "xud_window.h" /* Required for: XudWindow, the record the fold's two context-taking calls refuse a NULL of */

/* The tolerance every float comparison in this area reads against, and why it is a tolerance at
   all where the batch area's counters are read exactly: every check below crosses a sinf/cosf or
   a division, so no equality can be promised - and stated once here rather than scattered through
   the checks, so a check that misses by more than this is a geometry bug rather than rounding.
*/
#define XUD_TEST_CAMERA_TOLERANCE 1e-4f

/* One world point through the matrix, read as the shader reads it: the point is a COLUMN vector
   (the block's matrix is column-major, element index = column*4 + row, and solid.vert multiplies
   `viewProj * modelMatrix * position`), so the row of the matrix is a dot product down a COLUMN
   of the array. The four clip components come back as they were before the perspective division,
   because the division is the rasteriser's and the checks below want to see w - a point behind the
   eye has a NEGATIVE w, and that is the one state a frustum check can see without a rasteriser.
*/
static void xudTestTransform(const float* m, float x, float y, float z, float* outClip) {
    outClip[0] = (m[0] * x) + (m[4] * y) + (m[8] * z) + m[12];
    outClip[1] = (m[1] * x) + (m[5] * y) + (m[9] * z) + m[13];
    outClip[2] = (m[2] * x) + (m[6] * y) + (m[10] * z) + m[14];
    outClip[3] = (m[3] * x) + (m[7] * y) + (m[11] * z) + m[15];
}

/* The depth the pinhole owes a point at distance d, stated as the pinhole states it rather than as
   the matrix rows state it: the near plane is 0, the far plane is 1 (Vulkan's range, which the pip
   eline's LESS compare is written against) and the mapping is hyperbolic rather than linear, which
   is why the mid-distance check below is NOT a half. Deriving the closed form from the two endpo
   ints and the pinhole's 1/d falloff is what makes it an independent statement of the same fact.
*/
static float xudTestExpectedDepth(float near, float far, float distance) { return (far * (distance - near)) / (distance * (far - near)); }

/* The framing the pinhole owes: a point h units off the eye's axis, at distance d, lands at h*foc
   al/d in the vertical NDC half and at h*focal/(d*aspect) in the horizontal one, where focal is
   the cotangent of the half-aperture. Stated in the test's own words rather than read off the mat
   rix, so a matrix that scales the two axes the same way (the stretched-window bug) fails the hor
   izontal check while passing the vertical one.
*/
static float xudTestExpectedFocal(float fovyDegrees) {
    const float halfRadians = (fovyDegrees * 0.5f) / XUD_CAMERA_DEGREES_PER_RADIANS;

    return cosf(halfRadians) / sinf(halfRadians);
}

static uint32_t xudTestNear(float actual, float expected) {
    const float difference = (actual > expected) ? (actual - expected) : (expected - actual);
    const float scale = (expected > 1.0f) ? expected : 1.0f;

    return (difference <= (XUD_TEST_CAMERA_TOLERANCE * scale)) ? 1u : 0u;
}

/*-----------------------------------------------------------------------------------------------
 * The pure half: the refusals, the block write and the matrix's geometry
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_camera_pure(void) {
    XudDescriptor descriptor;
    XudRenderer renderer;
    XudContext shell;
    XudCamera3D camera;
    XudCamera3D broken;
    XudCamera3D stored;
    float matrix[XUD_CAMERA_MATRIX_ELEMENT_COUNT];
    float clip[4];
    uint32_t element = 0u;
    uint32_t diagonal = 0u;
    uint32_t offDiagonal = 0u;

    memset(&descriptor, 0, sizeof(descriptor));
    memset(&renderer, 0, sizeof(renderer));
    memset(&shell, 0, sizeof(shell));
    shell.renderer = &renderer;

    /* The one camera every "the camera is fine" check below is the same camera seen through a dif
       ferent refusal: eye on the +Z axis looking at the origin, up +Y, a 90-degree aperture (so
       the focal length is exactly 1 and the framing arithmetic below is a division by the distance
       and nothing more) and planes at 1 and 4, which brackets the distances the checks use.
    */
    camera = (XudCamera3D) { .position = { .x = 0.0f, .y = 0.0f, .z = 2.0f },
        .target = { .x = 0.0f, .y = 0.0f, .z = 0.0f },
        .up = { .x = 0.0f, .y = 1.0f, .z = 0.0f },
        .fovy = 90.0f,
        .near = 1.0f,
        .far = 4.0f };

    /*--- the refusals ----------------------------------------------------------------------*/

    xud_check(xud_camera_validate(NULL) == XUD_INVALID_ARG, "camera/validate-no-camera",
        "a camera with no record is refused rather than read as a zero record");

    /* Each of the six degeneracies, one at a time, against the camera above: the record is copied
       and ONE field changed, so a refusal that came from a different field than the one the check
       names would be a refusal of the wrong thing.
    */
    broken = camera;
    broken.target = camera.position;
    xud_check(xud_camera_validate(&broken) == XUD_INVALID_ARG, "camera/validate-eye-on-target",
        "an eye sitting on what it looks at has no direction to look along, and the refusal is the "
        "basis's rather than a basis of NaNs");

    broken = camera;
    broken.up = (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 0.0f };
    xud_check(xud_camera_validate(&broken) == XUD_INVALID_ARG, "camera/validate-zero-up", "a zero up vector is no axis to rotate over");

    broken = camera;
    broken.up = (XudVector3) { .x = 0.0f, .y = 0.0f, .z = -1.0f };
    xud_check(xud_camera_validate(&broken) == XUD_INVALID_ARG, "camera/validate-up-parallel-forward",
        "an up parallel to the eye's forward gives a zero cross product and no right vector - the "
        "one degeneracy the other two tests cannot see");

    broken = camera;
    broken.fovy = 0.0f;
    xud_check(
        xud_camera_validate(&broken) == XUD_INVALID_ARG, "camera/validate-zero-fovy", "a zero aperture is a frustum with no interior");
    broken = camera;
    broken.fovy = 180.0f;
    xud_check(xud_camera_validate(&broken) == XUD_INVALID_ARG, "camera/validate-fovy-at-180",
        "at 180 degrees the frustum's sides fold back over each other and the focal length's sign "
        "flips");
    broken = camera;
    broken.fovy = NAN;
    xud_check(xud_camera_validate(&broken) == XUD_INVALID_ARG, "camera/validate-nan-fovy",
        "and a NaN aperture is refused by the SAME test, which is why the test is written as not-"
        "in-range rather than out-of-range");

    broken = camera;
    broken.near = 0.0f;
    xud_check(xud_camera_validate(&broken) == XUD_INVALID_ARG, "camera/validate-zero-near",
        "a near plane at the eye is a frustum with no apex in front of the eye, and the depth map's "
        "division reads it");
    broken = camera;
    broken.near = -1.0f;
    xud_check(xud_camera_validate(&broken) == XUD_INVALID_ARG, "camera/validate-negative-near",
        "and a near plane behind the eye is a frustum that contains the world behind the eye");
    broken = camera;
    broken.near = NAN;
    xud_check(xud_camera_validate(&broken) == XUD_INVALID_ARG, "camera/validate-nan-near", "and a NaN one is refused by the same test");

    broken = camera;
    broken.far = camera.near;
    xud_check(xud_camera_validate(&broken) == XUD_INVALID_ARG, "camera/validate-far-at-near",
        "a far plane on the near one is a frustum with no depth, and the depth map's denominator is "
        "near - far, which is zero there");
    broken = camera;
    broken.far = 0.5f;
    xud_check(
        xud_camera_validate(&broken) == XUD_INVALID_ARG, "camera/validate-far-inside-near", "and one inside it is an inverted frustum");
    broken = camera;
    broken.far = NAN;
    xud_check(xud_camera_validate(&broken) == XUD_INVALID_ARG, "camera/validate-nan-far", "and a NaN one is refused by the same test");

    xud_check(xud_camera_validate(&camera) == XUD_OK, "camera/validate-good-camera",
        "the camera every check below uses is a valid one, so none of the refusals above is a re"
        "fusal of the camera rather than of the field the check changed");

    xud_check(xud_camera_fill_view_proj(NULL, 1.0f, matrix) == XUD_INVALID_ARG, "camera/fill-no-camera",
        "the matrix function refuses the same way the validate above does");
    xud_check(xud_camera_fill_view_proj(&camera, 1.0f, NULL) == XUD_INVALID_ARG, "camera/fill-no-out-param",
        "and it refuses a matrix to be filled into nowhere");
    xud_check(xud_camera_fill_view_proj(&camera, 0.0f, matrix) == XUD_INVALID_ARG, "camera/fill-zero-aspect",
        "a frame with no height has nothing to project onto, and the aspect is checked apart from "
        "the camera because it is not part of the camera record");
    xud_check(xud_camera_fill_view_proj(&camera, NAN, matrix) == XUD_INVALID_ARG, "camera/fill-nan-aspect",
        "and a NaN aspect is refused by the same test");
    xud_check(xud_camera_fill_view_proj(&broken, 1.0f, matrix) == XUD_INVALID_ARG, "camera/fill-degenerate-camera",
        "the matrix function asks the validate above again, because the renderer's record is a value "
        "a caller could have tampered since the begin call validated it");

    xud_check(xud_camera_apply(NULL, &camera, 1.0f) == XUD_INVALID_ARG, "camera/apply-no-descriptor",
        "a camera with no block to be written into is refused");
    xud_check(xud_camera_apply(&descriptor, &broken, 1.0f) == XUD_INVALID_ARG, "camera/apply-degenerate-refused",
        "and so is a degenerate camera - the block is not written, which the check below reads");
    for (element = 0u; element < XUD_CAMERA_MATRIX_ELEMENT_COUNT; element++) {
        if (descriptor.uniformBlock.viewProj[element] == 0.0f) {
            offDiagonal++;
        }
    }
    xud_check(offDiagonal == XUD_CAMERA_MATRIX_ELEMENT_COUNT, "camera/apply-refusal-wrote-nothing",
        "the refused apply left the block at whatever it was, which is the state the frame loop "
        "would otherwise upload a frame late");

    /*--- the no-camera state ---------------------------------------------------------------*/

    xud_check(xud_camera_apply(&descriptor, NULL, 0.0f) == XUD_OK, "camera/apply-no-camera-ok",
        "no camera live is not a refusal: it is the identity matrix, and a zero aspect cannot make "
        "it one because an identity needs no aspect");
    diagonal = 0u;
    offDiagonal = 0u;
    for (element = 0u; element < XUD_CAMERA_MATRIX_ELEMENT_COUNT; element++) {
        if (descriptor.uniformBlock.viewProj[element] == 1.0f) {
            diagonal++;
        }
    }
    xud_check(diagonal == 4u, "camera/no-camera-block-is-identity",
        "the block's matrix is the four ones on the diagonal, which is the one matrix xud_descriptor"
        "_fill_uniform_block() opens the block with - so a camera that began and ended inside one "
        "frame is a no-op rather than a leak of state into the next");
    xud_check(
        xud_camera_apply(&descriptor, &camera, 1.0f) == XUD_OK, "camera/apply-good-camera", "a usable camera is written into the block");
    offDiagonal = 0u;
    for (element = 0u; element < XUD_CAMERA_MATRIX_ELEMENT_COUNT; element++) {
        if (descriptor.uniformBlock.viewProj[element] == 1.0f) {
            offDiagonal++;
        }
    }
    xud_check(offDiagonal < 4u, "camera/apply-replaced-the-identity",
        "and the identity above is gone, so the block carries the camera rather than the default");

    /*--- the matrix's geometry, read by transforming points ---------------------------------*/

    xud_check(
        xud_camera_fill_view_proj(&camera, 1.0f, matrix) == XUD_OK, "camera/fill-ok", "the square-aspect camera's matrix is produced");

    /* The eye's own point: at zero distance the clip w is zero, which is the one place the pinhole
       has no answer - and the check is that the matrix says so rather than that it says nothing.
    */
    xudTestTransform(matrix, camera.position.x, camera.position.y, camera.position.z, clip);
    xud_check(xudTestNear(clip[3], 0.0f), "camera/eye-is-at-zero-distance",
        "the eye's own point has a clip w of zero, which is the pinhole's w = the distance in front "
        "of the eye");

    /* A point behind the eye: a negative w, which is what a rasteriser clips away. A matrix built
       with the wrong sign in the w row would make this point look in FRONT of the eye instead, and
       every depth check below would then be reading a mirrored frustum.
    */
    xudTestTransform(matrix, camera.position.x, camera.position.y, camera.position.z + 1.0f, clip);
    xud_check(clip[3] < 0.0f, "camera/behind-the-eye-is-clipped",
        "a point one unit BEHIND the eye has a negative clip w, which is the sign the -Z view con"
        "vention and the LESS depth compare are both written against");

    /* The depth of a point straight ahead, at the two planes and between them, against the pinhole
      's own closed form: the near plane at 0, the far plane at 1 and the mid-distance at 2/3, which
       is NOT a half - the mapping is hyperbolic, and a check that expected 0.5 at the mid-distance
       would be a check of the wrong fact.
    */
    xudTestTransform(matrix, 0.0f, 0.0f, camera.position.z - camera.near, clip);
    xud_check(xudTestNear(clip[2] / clip[3], xudTestExpectedDepth(camera.near, camera.far, camera.near)), "camera/near-plane-is-zero-depth",
        "a point ON the near plane is at depth 0, which is the clear value's other end");
    xudTestTransform(matrix, 0.0f, 0.0f, camera.position.z - camera.far, clip);
    xud_check(xudTestNear(clip[2] / clip[3], xudTestExpectedDepth(camera.near, camera.far, camera.far)), "camera/far-plane-is-one-depth",
        "and a point ON the far plane is at depth 1, so nothing in the frustum is clamped");
    xudTestTransform(matrix, 0.0f, 0.0f, camera.position.z - 2.0f, clip);
    xud_check(xudTestNear(clip[2] / clip[3], xudTestExpectedDepth(camera.near, camera.far, 2.0f)),
        "camera/mid-distance-depth-is-the-pinhole's", "and the point two units ahead is at the pinhole's depth, not at a half");

    /* Nearer is smaller, which is the whole LESS-compare convention seen from the other side: three
       distances in order, three depths in order. A matrix that mapped the frustum to [-1,1] and
       then to [0,1] the wrong way round would put the near plane at 1 and fail this.
    */
    {
        float nearDepth = 0.0f;
        float midDepth = 0.0f;
        float farDepth = 0.0f;

        xudTestTransform(matrix, 0.0f, 0.0f, camera.position.z - 1.5f, clip);
        nearDepth = clip[2] / clip[3];
        xudTestTransform(matrix, 0.0f, 0.0f, camera.position.z - 2.0f, clip);
        midDepth = clip[2] / clip[3];
        xudTestTransform(matrix, 0.0f, 0.0f, camera.position.z - 3.0f, clip);
        farDepth = clip[2] / clip[3];
        xud_check((nearDepth < midDepth) && (midDepth < farDepth), "camera/nearer-is-smaller-depth",
            "three distances in order give three depths in order, so the pipeline's LESS compare "
            "keeps the nearer of two surfaces rather than the farther");
        xud_check((nearDepth >= 0.0f) && (farDepth <= 1.0f), "camera/frustum-stays-inside-the-range",
            "and the whole frustum stays inside [0,1], which is what says no part of the scene is "
            "being clamped to the clear value");
    }

    /* The framing, at the camera's own right and true-up axes: with a 90-degree aperture the focal
       length is 1, so a point half a unit off the axis at two units' distance lands a quarter of
       the way to the edge of the frame in both halves. The two axes are the camera's own (right =
       cross(forward, up) = +X world, true up = +Y world for this camera), which is what makes the
       check below a check of the basis as well as of the scale.
    */
    xudTestTransform(matrix, 0.5f, 0.0f, camera.position.z - 2.0f, clip);
    xud_check(xudTestNear(clip[0] / clip[3], (0.5f * xudTestExpectedFocal(camera.fovy)) / (2.0f * 1.0f)),
        "camera/horizontal-framing-is-the-pinhole's", "half a unit to the right at two units' distance is a quarter of a half-width");
    xud_check(clip[0] > 0.0f, "camera/right-is-positive-x",
        "a point to the camera's RIGHT is at a positive NDC x, so the basis is not mirrored - the "
        "one thing a cross product written in the other order would break");
    xudTestTransform(matrix, 0.0f, 0.5f, camera.position.z - 2.0f, clip);
    xud_check(xudTestNear(clip[1] / clip[3], (0.5f * xudTestExpectedFocal(camera.fovy)) / 2.0f), "camera/vertical-framing-is-the-pinhole's",
        "and half a unit up is a quarter of a half-height");

    /* The aspect, seen as the fact it is: doubling the aspect halves the horizontal NDC of the same
       point and leaves the vertical one alone, so a wider window shows more world to the sides rat
       her than stretching what it shows. The vertical check beside it is what makes a matrix that
       divided BOTH scales by the aspect fail one of the two rather than both.
    */
    xud_check(xud_camera_fill_view_proj(&camera, 2.0f, matrix) == XUD_OK, "camera/fill-wide-aspect", "the same camera at a 2:1 extent");
    xudTestTransform(matrix, 0.5f, 0.0f, camera.position.z - 2.0f, clip);
    xud_check(xudTestNear(clip[0] / clip[3], (0.5f * xudTestExpectedFocal(camera.fovy)) / (2.0f * 2.0f)),
        "camera/wider-aspect-shows-more-world", "the same point is at half the NDC x at twice the aspect");
    xudTestTransform(matrix, 0.0f, 0.5f, camera.position.z - 2.0f, clip);
    xud_check(xudTestNear(clip[1] / clip[3], (0.5f * xudTestExpectedFocal(camera.fovy)) / 2.0f),
        "camera/wider-aspect-leaves-the-vertical-alone",
        "and its NDC y is unchanged, so the two scales differ by the aspect and not by a stretch");

    /* The aperture, at three apertures: the framing form is the pinhole's, so a 60-degree camera
       frames a third more tightly than a 90-degree one and a 120-degree camera frames a third less
       - checked at the same point and the same distance, which is what makes the focal length's
       cotangent the thing under test rather than the framing's shape.
    */
    {
        const float apertures[3] = { 60.0f, 90.0f, 120.0f };
        uint32_t framed = 0u;

        for (element = 0u; element < 3u; element++) {
            broken = camera;
            broken.fovy = apertures[element];
            if (xud_camera_fill_view_proj(&broken, 1.0f, matrix) != XUD_OK) {
                continue;
            }
            xudTestTransform(matrix, 0.0f, 0.5f, camera.position.z - 2.0f, clip);
            if (xudTestNear(clip[1] / clip[3], (0.5f * xudTestExpectedFocal(apertures[element])) / 2.0f)) {
                framed++;
            }
        }
        xud_check(framed == 3u, "camera/aperture-frames-as-the-cotangent-says",
            "60, 90 and 120 degrees each frame the same point at their own cotangent's distance, so the aperture is the full angle's half "
            "and not the whole");
    }

    /* A rotated camera: the eye on the +X axis looking at the origin, up +Z. The basis is a per
       mutation of the axes above (forward = -X world, right = +Y world, true up = +Z world), so a
       matrix that built the view half from world axes rather than from the camera's own would pass
       the checks above and fail this one.
    */
    broken = (XudCamera3D) { .position = { .x = 2.0f, .y = 0.0f, .z = 0.0f },
        .target = { .x = 0.0f, .y = 0.0f, .z = 0.0f },
        .up = { .x = 0.0f, .y = 0.0f, .z = 1.0f },
        .fovy = 90.0f,
        .near = 1.0f,
        .far = 4.0f };
    xud_check(xud_camera_validate(&broken) == XUD_OK, "camera/rotated-camera-is-valid",
        "the rotated camera is a valid one, so the checks below are of its basis rather than of a refusal");
    xud_check(xud_camera_fill_view_proj(&broken, 1.0f, matrix) == XUD_OK, "camera/fill-rotated", "and its matrix is produced");
    /* The point one unit to the camera's right, two units ahead: +Y world for this camera. */
    xudTestTransform(matrix, broken.position.x - 2.0f, broken.position.y + 0.5f, broken.position.z, clip);
    xud_check(xudTestNear(clip[0] / clip[3], (0.5f * xudTestExpectedFocal(broken.fovy)) / (2.0f * 1.0f)),
        "camera/rotated-right-is-positive-x",
        "the point to the rotated camera's right is at a positive NDC x, so the right vector came from the camera's forward and not from "
        "the world's");
    xud_check(xudTestNear(clip[2] / clip[3], xudTestExpectedDepth(broken.near, broken.far, 2.0f)), "camera/rotated-depth-is-the-same",
        "and it is two units ahead, so the forward vector is the camera's own and not the world's -Z");

    /* A tilted up: the caller's up is (0,1,1), which is 45 degrees off the true up. The basis re
       places it (true up = cross(right, forward) = +Y world), so the framing of a point along the
       TRUE up is the same as the un-tilted camera's and its NDC x is zero - a basis that used the
       caller's up instead of re-orthogonalising would shear the frame and put that point at a non-"
       zero x.
    */
    broken = camera;
    broken.up = (XudVector3) { .x = 0.0f, .y = 1.0f, .z = 1.0f };
    xud_check(xud_camera_validate(&broken) == XUD_OK, "camera/tilted-camera-is-valid",
        "a tilted up is not a degeneracy - it is a tilt the basis has to undo");
    xud_check(xud_camera_fill_view_proj(&broken, 1.0f, matrix) == XUD_OK, "camera/fill-tilted", "and its matrix is produced");
    xudTestTransform(matrix, 0.0f, 0.5f, camera.position.z - 2.0f, clip);
    xud_check(xudTestNear(clip[1] / clip[3], (0.5f * xudTestExpectedFocal(broken.fovy)) / 2.0f),
        "camera/tilted-up-frames-like-an-untilted-one", "the point along the TRUE up is framed as the un-tilted camera frames it");
    xud_check(xudTestNear(clip[0] / clip[3], 0.0f), "camera/tilted-up-shears-nothing",
        "and it is dead ahead in x, so the tilt did not shear the frame - the basis is orthonormal rather than the caller's up read "
        "literally");

    /*--- the public pair, against a renderer shell -----------------------------------------*/

    xud_check(xud_begin_camera_3d(NULL, camera) == XUD_INVALID_ARG, "camera/begin-no-context",
        "a camera with no context to be stored in is refused");
    broken = camera;
    broken.near = 0.0f;
    xud_check(xud_begin_camera_3d(&shell, broken) == XUD_INVALID_ARG, "camera/begin-degenerate-refused",
        "and a degenerate camera is refused at the call that asked for it rather than at the frame that draws it");
    xud_check((renderer.cameraWanted == 0u), "camera/begin-refusal-stored-nothing",
        "the refused begin left the renderer with no camera live, so the next frame is drawn with the identity matrix rather than with a "
        "NaN one");

    stored = camera;
    xud_check(xud_begin_camera_3d(&shell, stored) == XUD_OK, "camera/begin-ok", "a valid camera is stored");
    xud_check(renderer.cameraWanted == 1u, "camera/begin-sets-the-flag",
        "and the renderer says a camera is live, which is the one state the frame loop reads");
    xud_check((renderer.camera.position.z == stored.position.z) && (renderer.camera.target.x == stored.target.x)
            && (renderer.camera.up.y == stored.up.y) && (renderer.camera.fovy == stored.fovy) && (renderer.camera.near == stored.near)
            && (renderer.camera.far == stored.far),
        "camera/begin-copied-the-record",
        "the record the renderer holds is the record the caller gave, field for field - a value rather than a pointer to a caller's local");

    stored.fovy = 60.0f;
    xud_check(xud_begin_camera_3d(&shell, stored) == XUD_OK, "camera/begin-replaces",
        "a second begin while one is live replaces it rather than stacking it - the scope is one camera per frame");
    xud_check(renderer.camera.fovy == 60.0f, "camera/begin-replacement-landed",
        "and the record that landed is the second one, so there is no camera stack here");

    broken = camera;
    broken.far = 0.5f;
    xud_check(xud_begin_camera_3d(&shell, broken) == XUD_INVALID_ARG, "camera/begin-degenerate-while-live",
        "a degenerate begin while a camera is live is refused");
    xud_check(renderer.camera.fovy == 60.0f, "camera/begin-refusal-left-the-live-camera",
        "and the camera that was live is still the one the renderer holds, so a refused begin is a frame drawn with the previous camera");

    xud_check(xud_end_camera_3d(NULL) == XUD_INVALID_ARG, "camera/end-no-context", "a camera cannot be taken away with no context");
    xud_check(xud_end_camera_3d(&shell) == XUD_OK, "camera/end-ok",
        "a live camera is taken away, which is the state the pending flush still projects from");
    xud_check(renderer.cameraWanted == 0u, "camera/end-clears-the-flag",
        "the live flag is cleared, so no frame after the pending flush is drawn with this camera");
    xud_check(renderer.cameraLatched == 1u, "camera/end-sets-the-latch",
        "and the close latches the record for the one flush the recorded draws wait behind");
    xud_check(renderer.cameraLatchDraw == renderer.drawCount, "camera/end-stamps-the-latch",
        "the latch is stamped with the draws so far: it belongs to the flush the close waited for, not to a later one");
    xud_check(renderer.camera.fovy == 60.0f, "camera/end-leaves-the-record",
        "the record itself is left as it was - the flags are what govern, and a begin after here replaces the whole record");
    xud_check(xud_end_camera_3d(&shell) == XUD_ALREADY, "camera/end-no-camera-live",
        "and taking away a camera that is not live is the repo's idempotent ask rather than a refusal");
    xud_check(renderer.cameraWanted == 0u, "camera/end-twice-changes-nothing",
        "the second end changed nothing, so the ask is idempotent rather than a refusal that left state");
}

/*-----------------------------------------------------------------------------------------------
 * The moves and the turns: the primitives xud_update_camera() drives a mode with
 *
 * Stated as GEOMETRY - where the eye ends up, which way it looks, how far the target is - and never
 * as the rotation algebra the implementation runs, for the reason the matrix checks above give: a
 * check that re-implements Rodrigues' formula asserts the same formula twice rather than asserting
 * that a right turn looks right. The expected numbers are the ones a hand states about a hand-built
 * camera: a camera looking down a diagonal walks flat when its step is flattened, and a quarter turn
 * of a camera looking down -Z looks along +X.
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_camera_moves(void) {
    const XudCamera3D eye = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, -1.0f }, { 0.0f, 1.0f, 0.0f }, 60.0f, 0.5f, 100.0f };
    const float pi = 4.0f * atanf(1.0f);
    XudVector3 axis;

    /*--- the basis: forward is the view, right is the cross, and a view of no length is refused --*/

    xud_check(xud_get_camera_forward(&axis, &eye) == XUD_OK, "camera/basis-forward-ok", "a posed camera answers its forward");
    xud_check(xudTestNear(axis.x, 0.0f) && xudTestNear(axis.y, 0.0f) && xudTestNear(axis.z, -1.0f), "camera/basis-forward-is-the-view",
        "forward is target - position, which is the direction the eye looks");
    xud_check(xud_get_camera_right(&axis, &eye) == XUD_OK, "camera/basis-right-ok", "and it answers its right");
    xud_check(xudTestNear(axis.x, 1.0f) && xudTestNear(axis.y, 0.0f) && xudTestNear(axis.z, 0.0f), "camera/basis-right-is-the-cross",
        "right is forward x up, which for an eye looking down -Z with +Y up is +X - the handedness every strafe in the example depends on");
    xud_check(xud_get_camera_up(&axis, &eye) == XUD_OK, "camera/basis-up-ok", "and its up");
    xud_check(xudTestNear(axis.x, 0.0f) && xudTestNear(axis.y, 1.0f) && xudTestNear(axis.z, 0.0f), "camera/basis-up-is-the-axes-third",
        "and the third axis completes the frame");

    /* A view two axes long is still one direction: the caller's numbers are not the basis. */
    {
        XudCamera3D longEye = eye;

        longEye.target = (XudVector3) { .x = 3.0f, .y = 0.0f, .z = -4.0f };
        xud_check(xud_get_camera_forward(&axis, &longEye) == XUD_OK, "camera/basis-long-view-ok", "a view of length 5 answers");
        xud_check(xudTestNear(axis.x, 0.6f) && xudTestNear(axis.y, 0.0f) && xudTestNear(axis.z, -0.8f), "camera/basis-view-normalised",
            "as the unit direction, not as the caller's (3, 0, -4)");
    }

    /* An eye looking at itself has no direction, and the rung says so rather than dividing by zero. */
    {
        XudCamera3D collapsed = eye;

        collapsed.target = collapsed.position;
        xud_check(xud_get_camera_forward(&axis, &collapsed) == XUD_INVALID_ARG, "camera/basis-collapsed-refused",
            "a camera whose target IS its position has no forward to hand out, and is refused");
        xud_check(xud_get_camera_right(&axis, &collapsed) == XUD_INVALID_ARG, "camera/basis-collapsed-right-refused", "and no right");
        xud_check(xud_get_camera_forward(NULL, &eye) == XUD_INVALID_ARG, "camera/basis-null-out",
            "an out-param that cannot be written is refused");
        xud_check(xud_get_camera_forward(&axis, NULL) == XUD_INVALID_ARG, "camera/basis-null-camera", "and no camera at all");
    }

    /* A posed camera's axes: forward and right are perpendicular whatever the pose was, and right is
       perpendicular to the record's up too - which is the whole cross-product claim. The up the getter
       hands out is the CALLER'S up at unit length, the vector raylib's GetCameraUp answers; it is the
       matrix that squares the frame, by re-deriving its own third row from that cross (the framing
       checks above are where that is proved), and a tilted up therefore shears nothing.
    */
    {
        const XudCamera3D posed = { { 1.0f, 2.0f, 3.0f }, { -2.0f, 0.5f, 1.0f }, { 0.3f, 0.9f, -0.2f }, 50.0f, 0.1f, 40.0f };
        XudVector3 forward;
        XudVector3 right;
        XudVector3 up;
        float dotFR = 0.0f;
        float dotRU = 0.0f;

        xud_check(xud_get_camera_forward(&forward, &posed) == XUD_OK, "camera/basis-posed-forward", "a posed camera answers its forward");
        xud_check(xud_get_camera_right(&right, &posed) == XUD_OK, "camera/basis-posed-right", "its right");
        xud_check(xud_get_camera_up(&up, &posed) == XUD_OK, "camera/basis-posed-up", "and its up");
        dotFR = (forward.x * right.x) + (forward.y * right.y) + (forward.z * right.z);
        dotRU = (right.x * up.x) + (right.y * up.y) + (right.z * up.z);
        xud_check(xudTestNear(dotFR, 0.0f), "camera/basis-square", "forward and right are perpendicular, whatever the pose was");
        xud_check(xudTestNear(dotRU, 0.0f), "camera/basis-right-perpendicular-to-up",
            "and right is perpendicular to the record's up as well, so a tilted up tilts the frame instead of skewing it");
        xud_check(xudTestNear((up.x * up.x) + (up.y * up.y) + (up.z * up.z), 1.0f), "camera/basis-up-unit",
            "and the up the rung hands out is a unit, whichever length the caller's was");
    }

    /*--- the three moves: a step along an axis, optionally flattened onto the world plane --------*/

    /* The 45-degree camera is the one that separates the two halves of the plane flag: its forward
       has as much y as z, so a flattened step and a free step part company at once.
    */
    {
        const XudCamera3D sloped = { { 0.0f, 5.0f, 5.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, 60.0f, 0.5f, 100.0f };
        XudCamera3D walked = sloped;
        XudCamera3D flew = sloped;
        XudVector3 before;
        XudVector3 after;

        xud_check(xud_get_camera_forward(&before, &sloped) == XUD_OK, "camera/move-sloped-basis", "the sloped camera has a forward");
        xud_check(xud_camera_move_forward(&walked, 2.0f, true) == XUD_OK, "camera/move-forward-plane-ok",
            "walking it forward in the plane answers OK");
        xud_check(
            xud_camera_move_forward(&flew, 2.0f, false) == XUD_OK, "camera/move-forward-free-ok", "and flying it forward answers OK too");
        xud_check(xudTestNear(walked.position.y, 5.0f), "camera/move-forward-plane-stays-level",
            "the walked eye is still 5 up: the flag flattens the step, which is the difference between walking and flying");
        xud_check(xudTestNear(walked.position.z, 3.0f), "camera/move-forward-plane-step",
            "and it moved the whole distance along z, because a flattened diagonal step is the full distance in the plane");
        xud_check(!xudTestNear(flew.position.y, 5.0f) && (flew.position.y < 5.0f), "camera/move-forward-free-descends",
            "the flown eye, on the other hand, comes down as it goes forward");

        xud_check(
            xud_get_camera_forward(&after, &walked) == XUD_OK, "camera/move-forward-direction-ok", "the walked camera still has a forward");
        xud_check(xudTestNear(after.x, before.x) && xudTestNear(after.y, before.y) && xudTestNear(after.z, before.z),
            "camera/move-forward-keeps-direction", "and it looks the same way it did: a step moves the whole frame, it does not steer it");
        xud_check(xudTestNear(walked.target.z, -2.0f) && xudTestNear(walked.target.y, 0.0f), "camera/move-forward-moves-target",
            "the target moves with the eye, so a walk does not quietly tilt the view the way moving only the eye would");

        /* A step sideways is perpendicular to the view and as long as the caller asked for. */
        {
            XudCamera3D strafed = sloped;
            float dx = 0.0f;
            float dy = 0.0f;
            float dz = 0.0f;
            float drift = 0.0f;
            float length = 0.0f;

            xud_check(xud_camera_move_right(&strafed, 1.5f, true) == XUD_OK, "camera/move-right-ok", "a step right answers OK");
            dx = strafed.position.x - sloped.position.x;
            dy = strafed.position.y - sloped.position.y;
            dz = strafed.position.z - sloped.position.z;
            drift = (dx * before.x) + (dy * before.y) + (dz * before.z);
            length = sqrtf((dx * dx) + (dy * dy) + (dz * dz));
            xud_check(xudTestNear(drift, 0.0f), "camera/move-right-perpendicular", "and it is perpendicular to where the camera looks");
            xud_check(
                xudTestNear(dy, 0.0f), "camera/move-right-stays-level", "and level, because the step was asked for in the world plane");
            xud_check(xudTestNear(length, 1.5f), "camera/move-right-step-length", "and it is exactly the distance asked for");
        }

        /* The plane's dominant axis is the one that gets dropped, so a camera whose up is the z axis
           walks a step of no z at all - the ladder's other branch, which a +Y camera never reaches.
        */
        {
            const XudCamera3D uprightZ = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.866f, -0.5f }, { 0.0f, 0.5f, 0.866f }, 60.0f, 0.5f, 100.0f };
            XudCamera3D moved = uprightZ;

            xud_check(xud_camera_move_forward(&moved, 1.0f, true) == XUD_OK, "camera/move-plane-z-up-ok",
                "a camera whose up is z-dominated walks too");
            xud_check(xudTestNear(moved.position.z, 0.0f) && xudTestNear(moved.position.y, 1.0f), "camera/move-plane-drops-dominant-axis",
                "and its flattened step drops z rather than y, which is the axis its own up is dominated by");
        }

        /* A distance of zero asks for nothing and gets nothing - including no refusal from the
           flattening, which has no direction to normalise out of a step of no length.
        */
        {
            XudCamera3D still = sloped;

            xud_check(xud_camera_move_forward(&still, 0.0f, true) == XUD_OK, "camera/move-zero-ok", "a step of zero answers OK");
            xud_check(xudTestNear(still.position.x, sloped.position.x) && xudTestNear(still.position.y, sloped.position.y)
                    && xudTestNear(still.position.z, sloped.position.z),
                "camera/move-zero-changes-nothing",
                "and leaves the eye exactly where it was, rather than refusing a caller that asked for nothing");
        }

        /* Straight up the camera's own up, which for +Y-up is the world's up: the mode that flies. */
        {
            XudCamera3D risen = sloped;

            xud_check(xud_camera_move_up(&risen, 3.0f) == XUD_OK, "camera/move-up-ok", "a step up answers OK");
            xud_check(xudTestNear(risen.position.y, 8.0f) && xudTestNear(risen.position.x, 0.0f) && xudTestNear(risen.position.z, 5.0f),
                "camera/move-up-is-along-up", "and it is 3 higher and nothing else, along the record's own up");
            xud_check(xudTestNear(risen.target.y, 3.0f), "camera/move-up-moves-target", "the target rises with it, so the view holds");
        }
    }

    /*--- the distance and the three turns -------------------------------------------------------*/

    {
        const XudCamera3D orbit = { { 0.0f, 0.0f, 5.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, 60.0f, 0.5f, 100.0f };
        XudCamera3D closer = orbit;
        XudCamera3D crossed = orbit;
        float distance = 0.0f;

        xud_check(xud_camera_move_to_target(&closer, -3.0f) == XUD_OK, "camera/move-to-target-ok", "pulling the eye in answers OK");
        distance = sqrtf(
            (closer.position.x * closer.position.x) + (closer.position.y * closer.position.y) + (closer.position.z * closer.position.z));
        xud_check(xudTestNear(distance, 2.0f), "camera/move-to-target-distance", "and the eye is 3 closer to the target than it was");
        xud_check(xudTestNear(closer.position.x, 0.0f) && xudTestNear(closer.position.y, 0.0f) && (closer.position.z > 0.0f),
            "camera/move-to-target-along-view", "along the view, not off it, and still on the same side of what it looks at");
        xud_check(xudTestNear(closer.target.x, 0.0f) && xudTestNear(closer.target.y, 0.0f) && xudTestNear(closer.target.z, 0.0f),
            "camera/move-to-target-target-fixed", "the target does not move: zooming is the eye's half of a third-person rig");

        /* The clamp: an eye pulled past its target would have its view vector through the origin, and
           the basis would flip. The floor is the rung's own distance, not zero.
        */
        xud_check(xud_camera_move_to_target(&crossed, -100.0f) == XUD_OK, "camera/move-to-target-clamped",
            "a pull past the target is answered, not refused");
        distance = sqrtf((crossed.position.x * crossed.position.x) + (crossed.position.y * crossed.position.y)
            + (crossed.position.z * crossed.position.z));
        xud_check(xudTestNear(distance, XUD_CAMERA_MIN_TARGET_DISTANCE), "camera/move-to-target-floor",
            "and the eye stops at the rung's floor above the target instead of crossing it");
        xud_check(xud_get_camera_forward(&axis, &crossed) == XUD_OK, "camera/move-to-target-basis-alive",
            "so the camera it leaves behind still has a basis to be drawn with");
    }

    /* A yaw of a quarter turn, from an eye looking down -Z with +Y up, looks along +X: the direction
       a rightward mouse move must send it, which is the sign the first-person example lives or dies by.
    */
    {
        XudCamera3D turned = eye;
        const XudCamera3D orbit = { { 0.0f, 0.0f, 5.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, 60.0f, 0.5f, 100.0f };
        XudCamera3D orbiting = orbit;

        xud_check(xud_camera_yaw(&turned, -(pi / 2.0f), false) == XUD_OK, "camera/yaw-ok", "a yaw about the camera's up answers OK");
        xud_check(xud_get_camera_forward(&axis, &turned) == XUD_OK, "camera/yaw-forward-ok", "and the turned camera has a forward");
        xud_check(xudTestNear(axis.x, 1.0f) && xudTestNear(axis.y, 0.0f) && xudTestNear(axis.z, 0.0f), "camera/yaw-turns-right",
            "a negative yaw turns an eye looking down -Z to look along +X, which is to its own right");
        xud_check(xudTestNear(turned.position.x, 0.0f) && xudTestNear(turned.position.y, 0.0f) && xudTestNear(turned.position.z, 0.0f),
            "camera/yaw-eye-fixed", "and the eye did not move: the first-person reading turns the head, not the body");
        xud_check(xudTestNear(sqrtf((turned.target.x * turned.target.x) + (turned.target.y * turned.target.y)
                                  + (turned.target.z * turned.target.z)),
                      1.0f),
            "camera/yaw-keeps-distance", "the target is still one unit out, so a turn is a rotation and not a slide");

        /* The other reading: the eye circles a target that stays put - third person and orbital. */
        xud_check(xud_camera_yaw(&orbiting, pi / 2.0f, true) == XUD_OK, "camera/yaw-around-target-ok", "the orbiting reading answers OK");
        xud_check(xudTestNear(orbiting.target.x, 0.0f) && xudTestNear(orbiting.target.z, 0.0f), "camera/yaw-around-target-target-fixed",
            "the target is where it was");
        xud_check(xudTestNear(orbiting.position.x, 5.0f) && xudTestNear(orbiting.position.z, 0.0f), "camera/yaw-around-target-eye-orbits",
            "and the eye has moved a quarter of the way round it, from +Z to +X, at the same distance");

        /* One whole turn is the camera back where it started: the trig is the only place this can fail. */
        {
            XudCamera3D full = eye;

            xud_check(xud_camera_yaw(&full, 2.0f * pi, false) == XUD_OK, "camera/yaw-full-turn-ok", "a whole turn answers OK");
            xud_check(xudTestNear(full.target.x, eye.target.x) && xudTestNear(full.target.y, eye.target.y)
                    && xudTestNear(full.target.z, eye.target.z),
                "camera/yaw-full-turn-identity", "and leaves the record on the view it started with");
        }

        /* A camera whose up is its own view has no axis to turn about, and is refused instead of being
           turned into a NaN one call later.
        */
        {
            XudCamera3D parallel = eye;

            parallel.up = (XudVector3) { .x = 0.0f, .y = 0.0f, .z = -1.0f };
            xud_check(xud_camera_yaw(&parallel, 0.3f, false) == XUD_INVALID_ARG, "camera/yaw-parallel-up-refused",
                "an up that IS the view gives no turn, and the record is refused rather than rewritten");
            xud_check(xudTestNear(parallel.target.x, eye.target.x) && xudTestNear(parallel.target.z, eye.target.z),
                "camera/yaw-refusal-leaves-record", "and the refused camera is the camera that was handed in");
            xud_check(xud_camera_yaw(NULL, 0.3f, false) == XUD_INVALID_ARG, "camera/yaw-null-camera", "no camera is refused");
        }
    }

    /*--- the pitch's clamp, the roll's axis, and the update that strings them together -----------*/

    {
        XudCamera3D locked = eye;
        XudCamera3D free = eye;

        /* The same angle, the flag on and off: with the lock the view stops short of the camera's own
           up, without it the view carries on past the vertical. A first-person camera is the lock and
           nothing else - the somersault is the bug the clamp exists to stop.
        */
        xud_check(xud_camera_pitch(&locked, 3.0f, true, false, false) == XUD_OK, "camera/pitch-locked-ok", "a pitched camera answers OK");
        xud_check(
            xud_camera_pitch(&free, 3.0f, false, false, false) == XUD_OK, "camera/pitch-free-ok", "and so is one pitched without the lock");
        xud_check(xud_get_camera_forward(&axis, &locked) == XUD_OK, "camera/pitch-locked-forward", "the locked camera has a forward");
        xud_check(xudTestNear(axis.y, 1.0f), "camera/pitch-locked-stops-short",
            "and it is stopped just short of straight up: the clamp, not the caller's 3 radians");
        xud_check(xud_get_camera_forward(&axis, &free) == XUD_OK, "camera/pitch-free-forward", "the unlocked one has one too");
        xud_check(xudTestNear(axis.y, sinf(3.0f)) && !xudTestNear(axis.y, 1.0f), "camera/pitch-free-passes-vertical",
            "and it went the whole 3 radians, which is past the vertical and back down the far side");

        /* Past the ceiling the clamp saturates rather than folding: a second identical pitch is the
           nothing the first one became, and not a rotation back down.
        */
        {
            XudCamera3D again = locked;

            xud_check(
                xud_camera_pitch(&again, 3.0f, true, false, false) == XUD_OK, "camera/pitch-saturated-ok", "the second pitch is answered");
            xud_check(xudTestNear(again.target.x, locked.target.x) && xudTestNear(again.target.y, locked.target.y)
                    && xudTestNear(again.target.z, locked.target.z),
                "camera/pitch-saturated-stops", "and stops the view where the first one stopped it, instead of folding it back down");
        }

        /* The record's up: held by default, carried along by the flag that says rotate it. */
        {
            XudCamera3D held = eye;
            XudCamera3D carried = eye;

            xud_check(
                xud_camera_pitch(&held, 0.5f, false, false, false) == XUD_OK, "camera/pitch-holds-up", "a pitch with no up flag answers");
            xud_check(xudTestNear(held.up.x, 0.0f) && xudTestNear(held.up.y, 1.0f) && xudTestNear(held.up.z, 0.0f),
                "camera/pitch-up-untouched", "and leaves the record's +Y up exactly as it was");
            xud_check(xud_camera_pitch(&carried, 0.5f, false, false, true) == XUD_OK, "camera/pitch-rotates-up", "the up flag answers too");
            xud_check(!xudTestNear(carried.up.y, 1.0f), "camera/pitch-up-rotated",
                "and there the up turns with the view, as a free camera's does");
        }

        /* Roll turns the record's up about the view and nothing else: the eye stays, the direction
           stays, and the up's LENGTH is the caller's - the one place the rung rotates a caller's
           vector without quietly normalising it first.
        */
        {
            XudCamera3D rolled = eye;
            float length = 0.0f;

            rolled.up = (XudVector3) { .x = 0.0f, .y = 2.0f, .z = 0.0f };
            xud_check(xud_camera_roll(&rolled, pi / 2.0f) == XUD_OK, "camera/roll-ok", "a roll answers OK");
            xud_check(xudTestNear(rolled.up.x, 2.0f) && xudTestNear(rolled.up.y, 0.0f) && xudTestNear(rolled.up.z, 0.0f),
                "camera/roll-turns-up", "and the up has turned a quarter of the way round the view");
            length = sqrtf((rolled.up.x * rolled.up.x) + (rolled.up.y * rolled.up.y) + (rolled.up.z * rolled.up.z));
            xud_check(xudTestNear(length, 2.0f), "camera/roll-keeps-up-length",
                "at the length the caller gave it, not at one the rung decided on");
            xud_check(xud_get_camera_forward(&axis, &rolled) == XUD_OK, "camera/roll-forward-ok", "the rolled camera has a forward");
            xud_check(xudTestNear(axis.x, 0.0f) && xudTestNear(axis.y, 0.0f) && xudTestNear(axis.z, -1.0f), "camera/roll-keeps-view",
                "and it still looks down -Z: a roll tilts the frame, it does not steer it");
        }
    }

    /* xud_update_camera_pro(): the seven steps in raylib's order over a caller's three numbers, with
       nothing read from a context. The checks are the same facts again, because the point of the call
       is that a caller can write the mode it wants out of the primitives above.
    */
    {
        const XudCamera3D orbit = { { 0.0f, 0.0f, 5.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, 60.0f, 0.5f, 100.0f };
        XudCamera3D idle = orbit;
        XudCamera3D walked = orbit;
        XudCamera3D spun = orbit;
        XudCamera3D zoomed = orbit;
        XudCamera3D broken = orbit;
        XudVector3 none = { .x = 0.0f, .y = 0.0f, .z = 0.0f };
        XudVector3 forwardMove = { .x = 1.0f, .y = 0.0f, .z = 0.0f };
        XudVector3 spin = { .x = pi / 2.0f, .y = 0.0f, .z = 0.0f };
        XudVector3 nanMove = { .x = NAN, .y = 0.0f, .z = 0.0f };
        XudVector3 lookDown = { .x = 0.0f, .y = 3.0f, .z = 0.0f };
        XudResult result = XUD_OK;

        xud_check(xud_update_camera_pro(NULL, none, none, 0.0f) == XUD_INVALID_ARG, "camera/pro-null-camera", "no camera is refused");

        broken.fovy = 0.0f;
        result = xud_update_camera_pro(&broken, forwardMove, none, 0.0f);
        xud_check(result == XUD_INVALID_ARG, "camera/pro-invalid-record-refused",
            "a record that could not be drawn with is refused before any of the seven steps runs");
        xud_check(xudTestNear(broken.position.z, 5.0f) && xudTestNear(broken.target.x, 0.0f), "camera/pro-refusal-leaves-record",
            "and the refusal left the record exactly as it was handed in");

        xud_check(
            xud_update_camera_pro(&idle, none, none, 0.0f) == XUD_OK, "camera/pro-idle-ok", "an update that asks for nothing answers OK");
        xud_check(xudTestNear(idle.position.x, 0.0f) && xudTestNear(idle.position.y, 0.0f) && xudTestNear(idle.position.z, 5.0f),
            "camera/pro-idle-leaves-eye", "and leaves the eye where it was");

        xud_check(xud_update_camera_pro(&walked, forwardMove, none, 0.0f) == XUD_OK, "camera/pro-move-ok",
            "a movement along the view answers OK");
        xud_check(xudTestNear(walked.position.y, 0.0f), "camera/pro-move-is-a-walk",
            "and the x movement is a WALK: the eye is still at the height it started at, because the step is flattened");
        xud_check(walked.position.z < 4.5f, "camera/pro-move-moves", "and it moved toward what it looks at");

        xud_check(
            xud_update_camera_pro(&spun, none, spin, 0.0f) == XUD_OK, "camera/pro-spin-ok", "a rotation about the vertical answers OK");
        xud_check(xud_get_camera_forward(&axis, &spun) == XUD_OK, "camera/pro-spin-forward-ok", "and the spun camera has a forward");
        xud_check(xudTestNear(axis.x, 1.0f) && xudTestNear(axis.z, 0.0f), "camera/pro-spin-turns-right",
            "a positive rotation.x turns the view to the right, which is what a mouse moving right means");

        xud_check(xud_update_camera_pro(&zoomed, none, none, -1.0f) == XUD_OK, "camera/pro-zoom-ok", "a zoom in answers OK");
        xud_check(xudTestNear(zoomed.position.z, 4.0f), "camera/pro-zoom-distance", "and the eye is one unit closer to the target");

        /* The lock the first-person mode runs on, reached through the update: a look past straight
           down stops at straight down and leaves a camera that can still be drawn with.
        */
        {
            XudCamera3D down = orbit;

            xud_check(
                xud_update_camera_pro(&down, none, lookDown, 0.0f) == XUD_OK, "camera/pro-look-down-ok", "a hard look down answers OK");
            xud_check(xud_get_camera_forward(&axis, &down) == XUD_OK, "camera/pro-look-down-forward", "and has a forward");
            xud_check(axis.y < -0.999f, "camera/pro-look-down-clamped", "straight down, and no further");
            xud_check(xud_camera_validate(&down) == XUD_OK, "camera/pro-look-down-still-drawable",
                "and the record that comes back is one the renderer will take");
        }

        /* One non-finite number anywhere in the ask refuses the WHOLE update: the caller's record is
           either moved or untouched, never half a step.
        */
        {
            XudCamera3D nanCamera = orbit;

            result = xud_update_camera_pro(&nanCamera, nanMove, none, 0.0f);
            xud_check(result == XUD_INVALID_ARG, "camera/pro-nan-refused", "a NaN distance refuses the whole update");
            xud_check(memcmp(&nanCamera, &orbit, sizeof(orbit)) == 0, "camera/pro-nan-leaves-record",
                "and the record that comes back is byte for byte the one that went in, so a refused update is not a half-taken step");
        }
    }
}

/*-----------------------------------------------------------------------------------------------
 * The GPU half: the camera that reaches the shader, through a real context
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_camera_gpu(void) {
    XudContext* context = NULL;
    XudResult result = XUD_OK;
    XudCamera3D camera;
    XudCamera3D broken;
    float matrix[XUD_CAMERA_MATRIX_ELEMENT_COUNT];
    uint32_t element = 0u;
    uint32_t match = 0u;
    uint32_t identity = 0u;
    float aspect = 0.0f;

    camera = (XudCamera3D) { .position = { .x = 0.0f, .y = 0.0f, .z = 2.0f },
        .target = { .x = 0.0f, .y = 0.0f, .z = 0.0f },
        .up = { .x = 0.0f, .y = 1.0f, .z = 0.0f },
        .fovy = 90.0f,
        .near = 1.0f,
        .far = 4.0f };

    result = xud_init_context(&context, 64u, 64u, "xud_test");
    xud_check(result == XUD_OK, "gpu-camera/bringup-completes",
        "this machine has a Vulkan loader and a window system, so the ladder must complete");
    if (context == NULL) {
        return;
    }

    xud_check(context->renderer->descriptor != NULL, "gpu-camera/descriptor-owned",
        "a context that completed owns the descriptor whose set 0 block the camera writes");
    xud_check(context->renderer->cameraWanted == 0u, "gpu-camera/no-camera-at-open",
        "a fresh renderer has no camera live, which is the state a window is drawn in before any caller asked for a camera");

    /* The identity at open, before any frame ran: the block's matrix is the descriptor rung's own
       default, and the camera rung has not been asked to write anything yet.
    */
    for (element = 0u; element < XUD_CAMERA_MATRIX_ELEMENT_COUNT; element++) {
        if (context->renderer->descriptor->uniformBlock.viewProj[element] == 1.0f) {
            identity++;
        }
    }
    xud_check(identity == 4u, "gpu-camera/block-opens-at-identity",
        "set 0's block carries the four ones on the diagonal, which is the one matrix the descriptor rung opens it with");

    xud_check(xud_begin_camera_3d(context, camera) == XUD_OK, "gpu-camera/begin-ok",
        "the public begin call stores the camera in the renderer the context owns");

    broken = camera;
    broken.near = 0.0f;
    xud_check(xud_begin_camera_3d(context, broken) == XUD_INVALID_ARG, "gpu-camera/begin-degenerate-refused",
        "and a degenerate one is refused against a real context as well as against a shell");
    xud_check(context->renderer->cameraWanted == 1u, "gpu-camera/refusal-kept-the-live-camera",
        "and the camera that was live is still the one the renderer holds");

    /* The aspect the frame will render at, read off the swapchain's own recorded extent rather than
       off the window's requested one - the same source the renderer's draw reads, and the reason a
       scaled session's two extents cannot disagree with the matrix that gets uploaded.
    */
    aspect = (context->swapchain->createInfo.imageExtent.height > 0u)
        ? ((float)context->swapchain->createInfo.imageExtent.width / (float)context->swapchain->createInfo.imageExtent.height)
        : 0.0f;
    xud_check(aspect > 0.0f, "gpu-camera/extent-gives-an-aspect",
        "the swapchain's recorded extent has a height, so there is an aspect to project by");

    xud_check(xud_begin_drawing(context) == XUD_OK, "gpu-camera/begin-drawing-ok", "the frame opens");
    xud_check(xud_draw_cube(context, (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 0.0f }, 1.0f, 1.0f, 1.0f,
                  (XudColor) { .r = 200u, .g = 60u, .b = 60u, .a = 255u })
            == XUD_OK,
        "gpu-camera/cube-appended", "a box at the origin, which is where the camera above is looking at");
    result = xud_end_drawing(context);
    xud_check(result == XUD_OK, "gpu-camera/end-drawing-ok", "and the frame closed, which ran one frame through the renderer");

    /* The matrix that reached the shader, read back out of the block: the renderer recomputed it at
       the extent this frame rendered at, so the block's matrix and the pure function's matrix at
       the same aspect are the same sixteen numbers. This is the wiring check - the pure half has
       already shown the matrix is the camera's, and this one shows the frame uploaded THAT matrix.
    */
    xud_check(xud_camera_fill_view_proj(&context->renderer->camera, aspect, matrix) == XUD_OK, "gpu-camera/pure-matrix-at-this-aspect",
        "the same camera at the same aspect, computed by the pure function");
    for (element = 0u; element < XUD_CAMERA_MATRIX_ELEMENT_COUNT; element++) {
        if (xudTestNear(context->renderer->descriptor->uniformBlock.viewProj[element], matrix[element])) {
            match++;
        }
    }
    xud_check(match == XUD_CAMERA_MATRIX_ELEMENT_COUNT, "gpu-camera/block-carries-the-camera",
        "all sixteen elements of the block's matrix are the camera's at this frame's extent, so the matrix the shader read is the matrix "
        "the caller asked for");

    xud_check(xud_end_camera_3d(context) == XUD_OK, "gpu-camera/end-ok", "the camera is taken away");
    xud_check(context->renderer->cameraWanted == 0u, "gpu-camera/flag-cleared", "and the renderer says no camera is live");
    xud_check(context->renderer->cameraLatched == 1u, "gpu-camera/close-latches",
        "but the close latches the record: the draws recorded under this camera flush at the next boundary, one step after here");

    xud_check(xud_begin_drawing(context) == XUD_OK, "gpu-camera/begin-drawing-latched", "the next frame opens with the camera closed");
    xud_check(xud_draw_cube(context, (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 0.0f }, 0.5f, 0.5f, 0.5f,
                  (XudColor) { .r = 60u, .g = 200u, .b = 60u, .a = 255u })
            == XUD_OK,
        "gpu-camera/cube-appended-latched", "a box is appended after the close, inside the frame the camera was closed in");
    result = xud_end_drawing(context);
    xud_check(result == XUD_OK, "gpu-camera/end-drawing-latched", "and the frame closed");

    /* The flush honoured the close rather than erasing it: the block carries the camera the draws
       were recorded under - the raylib BeginMode3D/EndMode3D-inside-the-frame order, which is the
       whole reason the latch exists.
    */
    xud_check(context->renderer->cameraLatched == 0u, "gpu-camera/latch-consumed", "the draw consumed the latch");
    xud_check(xud_camera_fill_view_proj(&context->renderer->camera, aspect, matrix) == XUD_OK, "gpu-camera/pure-matrix-latched",
        "the pure function at this aspect again");
    match = 0u;
    for (element = 0u; element < XUD_CAMERA_MATRIX_ELEMENT_COUNT; element++) {
        if (xudTestNear(context->renderer->descriptor->uniformBlock.viewProj[element], matrix[element])) {
            match++;
        }
    }
    xud_check(match == XUD_CAMERA_MATRIX_ELEMENT_COUNT, "gpu-camera/latched-flush-keeps-camera",
        "the frame drawn AFTER the close still projects with the camera closed inside it, all sixteen elements");

    xud_check(
        xud_begin_drawing(context) == XUD_OK, "gpu-camera/begin-drawing-after-latch", "and one frame later, nothing is live or latched");
    xud_check(xud_draw_cube(context, (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 0.0f }, 0.5f, 0.5f, 0.5f,
                  (XudColor) { .r = 60u, .g = 200u, .b = 60u, .a = 255u })
            == XUD_OK,
        "gpu-camera/cube-appended-no-camera", "a box is appended with no camera and nothing latched");
    result = xud_end_drawing(context);
    xud_check(result == XUD_OK, "gpu-camera/end-drawing-no-camera", "and that frame closed");

    identity = 0u;
    for (element = 0u; element < XUD_CAMERA_MATRIX_ELEMENT_COUNT; element++) {
        if (context->renderer->descriptor->uniformBlock.viewProj[element] == 1.0f) {
            identity++;
        }
    }
    xud_check(identity == 4u, "gpu-camera/block-back-at-identity",
        "the block's matrix is the identity again two frames on: the latched flush was the camera's last, and it leaked into no frame "
        "after it");

    if (context->validate != 0u) {
        xud_check(context->diagnostics->errorMessages == 0u, "gpu-camera/validation-clean",
            "three frames drew a real box through a real pipeline with a real camera and the messenger heard nothing");
    }

    /* xud_update_camera() against the context it reads: the mode's two rungs are bring-up's, so the
       call answers OK here, and the guards are the same ones every other public call above a context
       gives. With nothing pressed and the pointer still, a mode is a turn of the frame clock and
       nothing else - the eye stays where it is, which is the property a first-person loop needs before
       it trusts any of the input below it.
    */

    /* The pointer-still premise, stated the way the input rung can state it: a cursor delta belongs
       to the frame that carried it, and a window the compositor is still placing re-issues its
       cursor-entry jump into one of the early frames - a jump the modes below would then read as
       the caller's mouse. Empty frames run until one closes with nothing to report, which is the
       placement settling; from there the delta window the updates read is a quiet pair of polls.
    */
    {
        XudVector2 settle;
        uint32_t attempt = 0u;
        for (attempt = 0u; attempt < 16u; attempt++) {
            (void)xud_begin_drawing(context);
            (void)xud_end_drawing(context);
            if ((xud_get_mouse_delta(&settle, context) == XUD_OK) && (settle.x == 0.0f) && (settle.y == 0.0f)) {
                break;
            }
        }
    }
    xud_check(xud_update_camera(NULL, &camera, XUD_CAMERA_MODE_FREE) == XUD_INVALID_ARG, "gpu-camera/update-no-context",
        "an update with no context is refused");
    xud_check(xud_update_camera(context, NULL, XUD_CAMERA_MODE_FREE) == XUD_INVALID_ARG, "gpu-camera/update-no-camera",
        "and one with no camera to move");
    xud_check(xud_update_camera(context, &camera, XUD_CAMERA_MODE_COUNT) == XUD_INVALID_ARG, "gpu-camera/update-mode-past-list",
        "and a mode past the list, which is the number a caller's enum would run off the end at");

    /* CUSTOM is the mode that hands the record back exactly as it came: the caller is driving. */
    {
        XudCamera3D driven = camera;

        xud_check(xud_update_camera(context, &driven, XUD_CAMERA_MODE_CUSTOM) == XUD_OK, "gpu-camera/update-custom-ok",
            "the custom mode answers OK, because it did what the mode asks");
        xud_check(memcmp(&driven, &camera, sizeof(camera)) == 0, "gpu-camera/update-custom-touches-nothing",
            "and leaves every one of the twelve floats to the caller");
    }

    /* Every mode that drives from the window holds the record when the window has nothing to report:
       no key is held and the pointer is still, so an update is a frame's input rather than a frame's
       drift. The orbital mode is NOT in this list - its swing is the mode itself, checked on its own
       below - and CUSTOM returned above.
    */
    {
        const uint32_t idle[XUD_CAMERA_MODE_COUNT - 2u]
            = { XUD_CAMERA_MODE_FREE, XUD_CAMERA_MODE_FIRST_PERSON, XUD_CAMERA_MODE_THIRD_PERSON };
        uint32_t which = 0u;

        for (which = 0u; which < (XUD_CAMERA_MODE_COUNT - 2u); which++) {
            XudCamera3D updated = camera;

            result = xud_update_camera(context, &updated, idle[which]);
            xud_check(result == XUD_OK, "gpu-camera/update-each-mode", "every mode the library drives runs against a real context");
            xud_check(xud_camera_validate(&updated) == XUD_OK, "gpu-camera/update-leaves-drawable-camera",
                "and the record it leaves is one the renderer takes");
            xud_check(xudTestNear(updated.position.x, camera.position.x) && xudTestNear(updated.position.y, camera.position.y)
                    && xudTestNear(updated.position.z, camera.position.z) && xudTestNear(updated.target.x, camera.target.x)
                    && xudTestNear(updated.target.y, camera.target.y) && xudTestNear(updated.target.z, camera.target.z),
                "gpu-camera/update-idle-holds-record",
                "with no key held and the pointer still, the eye and what it looks at are the pair that went in");
        }
    }

    /* The orbital mode is the one mode whose eye moves while the window reports nothing, because the
       swing IS the mode: rcamera.h 463-466 turns the eye-to-target vector by cameraOrbitSpeed every
       frame. This library's copy is per SECOND, so the angle a frame turns is the constant times that
       frame's own span and the angle over a run of frames is the constant times the SUM of their spans
       - the chord below is measured against exactly that. The eye circles and never drifts: the radius
       and the target are untouched by any of it. The eye is put far out so the chord is thousands of
       floats wide rather than a few, which is what makes the comparison below a fact rather than noise.
    */
    {
        const XudCamera3D wide = { { 0.0f, 0.0f, 200.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, 60.0f, 0.5f, 1000.0f };
        XudCamera3D orbited = wide;
        float span = 0.0f;
        float seconds = 0.0f;
        float chord = 0.0f;
        float expected = 0.0f;
        float chordSquared = 0.0f;
        float radiusSquared = 0.0f;
        float radius = 0.0f;
        uint32_t frame = 0u;

        for (frame = 0u; frame < 8u; frame++) {
            xud_begin_drawing(context);
            xud_end_drawing(context);
            /* The span the update below is about to read: the clock answers the frame that just
               completed and nothing polls between here and there, so the two readings are one number.
            */
            result = xud_get_frame_time(&span, context);
            xud_check(
                result == XUD_OK, "gpu-camera/update-orbital-span", "the frame's span reads back, which is what the swing is scaled by");
            seconds += span;
            xud_check(xud_update_camera(context, &orbited, XUD_CAMERA_MODE_ORBITAL) == XUD_OK, "gpu-camera/update-orbital-ok",
                "and the orbital update runs against the real context");
        }

        chordSquared = ((orbited.position.x - wide.position.x) * (orbited.position.x - wide.position.x))
            + ((orbited.position.y - wide.position.y) * (orbited.position.y - wide.position.y))
            + ((orbited.position.z - wide.position.z) * (orbited.position.z - wide.position.z));
        radiusSquared = ((orbited.position.x - orbited.target.x) * (orbited.position.x - orbited.target.x))
            + ((orbited.position.y - orbited.target.y) * (orbited.position.y - orbited.target.y))
            + ((orbited.position.z - orbited.target.z) * (orbited.position.z - orbited.target.z));
        chord = sqrtf(chordSquared);
        expected = 2.0f * 200.0f * sinf(0.5f * XUD_CAMERA_ORBITAL_SPEED * seconds);
        radius = sqrtf(radiusSquared);
        xud_check(fabsf(chord - expected) <= ((0.01f * expected) + 1e-3f), "gpu-camera/update-orbital-sweeps-per-second",
            "eight frames sweep the constant times the eight frames' spans, which is the difference between a per-second speed and "
            "raylib's "
            "per-frame one: the same walk at 30 frames a second and at 144");
        xud_check(fabsf(radius - 200.0f) <= 0.01f, "gpu-camera/update-orbital-holds-radius",
            "and eight frames on, the eye is still exactly as far out of the target as it started");
        xud_check((orbited.target.x == 0.0f) && (orbited.target.y == 0.0f) && (orbited.target.z == 0.0f),
            "gpu-camera/update-orbital-holds-target",
            "and the target never stirred: the eye goes around it rather than it going with the eye");
    }

    result = xud_close_context(&context);
    xud_check(result == XUD_OK, "gpu-camera/close-clean", "the whole ladder tears back down with nothing refused");
    xud_check(context == NULL, "gpu-camera/close-clears", "and the out-param is cleared");
}

/*-----------------------------------------------------------------------------------------------
 * The fold between the frame's pixels and the world - the two public pairs above the camera record
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_camera_fold(void) {
    XudWindow window;
    XudContext shell;
    XudCamera3D camera;
    XudCamera3D broken;
    XudVector3 point;
    XudVector2 screen;
    XudRay ray;
    float matrix[XUD_CAMERA_MATRIX_ELEMENT_COUNT];
    float clip[4];
    float expected[2];
    XudResult result = XUD_INVALID_ARG;
    float toPoint[3];
    float cross[3];
    float toLength = 0.0f;
    float crossLength = 0.0f;
    float length = 0.0f;
    uint32_t badMatrix = 0u;
    uint32_t badLength = 0u;
    uint32_t bad = 0u;
    uint32_t ix = 0u;
    uint32_t iy = 0u;
    uint32_t iz = 0u;

    memset(&window, 0, sizeof(window));
    memset(&shell, 0, sizeof(shell));
    shell.window = &window;
    shell.width = 200u;
    shell.height = 100u;

    /* The same camera the matrix checks above use: eye on +Z looking at the origin over up +Y, with
       a 90-degree aperture so the focal length is exactly 1 and every closed form below is a
       division and nothing more. The extent is 200x100 rather than a square, because a square one
       lets a fold that forgets the aspect pass every check the horizontal has.
    */
    camera = (XudCamera3D) { .position = { .x = 0.0f, .y = 0.0f, .z = 2.0f },
        .target = { .x = 0.0f, .y = 0.0f, .z = 0.0f },
        .up = { .x = 0.0f, .y = 1.0f, .z = 0.0f },
        .fovy = 90.0f,
        .near = 1.0f,
        .far = 4.0f };
    broken = camera;

    /*--- the refusals ----------------------------------------------------------------------*/

    point = (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 0.0f };
    screen = (XudVector2) { .x = 100.0f, .y = 50.0f };

    xud_check(xud_camera_fill_screen_point(NULL, 200.0f, 100.0f, point, &screen) == XUD_INVALID_ARG, "camera/fold-no-camera",
        "a fold with no camera has no basis to fold through");
    xud_check(xud_camera_fill_screen_point(&camera, 200.0f, 100.0f, point, NULL) == XUD_INVALID_ARG, "camera/fold-no-out-param",
        "and no answer to write");
    xud_check(xud_camera_fill_world_ray(NULL, 200.0f, 100.0f, screen, &ray) == XUD_INVALID_ARG, "camera/ray-no-camera",
        "the same two refusals on the way back in");
    xud_check(xud_camera_fill_world_ray(&camera, 200.0f, 100.0f, screen, NULL) == XUD_INVALID_ARG, "camera/ray-no-out-param",
        "and the same missing answer");

    xud_check(xud_camera_fill_screen_point(&camera, 0.0f, 100.0f, point, &screen) == XUD_INVALID_ARG, "camera/fold-zero-width",
        "a viewport with no columns has no fraction of a frame to land on");
    xud_check(xud_camera_fill_screen_point(&camera, 200.0f, 0.0f, point, &screen) == XUD_INVALID_ARG, "camera/fold-zero-height",
        "and the same for no rows, which is the division the aspect is");
    xud_check(xud_camera_fill_screen_point(&camera, NAN, 100.0f, point, &screen) == XUD_INVALID_ARG, "camera/fold-nan-width",
        "and a NaN extent is refused by the same not-greater test rather than folded into NaN");
    xud_check(xud_camera_fill_world_ray(&camera, 200.0f, 0.0f, screen, &ray) == XUD_INVALID_ARG, "camera/ray-zero-height",
        "the trace refuses a zero extent before it divides the pixel by it");

    broken.target = camera.position;
    xud_check(xud_camera_fill_screen_point(&broken, 200.0f, 100.0f, point, &screen) == XUD_INVALID_ARG, "camera/fold-degenerate-camera",
        "an eye on its target has no frame to fold into");
    xud_check(xud_camera_fill_world_ray(&broken, 200.0f, 100.0f, screen, &ray) == XUD_INVALID_ARG, "camera/ray-degenerate-camera",
        "and no direction to trace");
    broken = camera;
    broken.fovy = 0.0f;
    xud_check(xud_camera_fill_world_ray(&broken, 200.0f, 100.0f, screen, &ray) == XUD_INVALID_ARG, "camera/ray-zero-aperture",
        "an aperture of zero has no frame at all, so the fold refuses it as the matrix does");
    broken = camera;

    point = (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 2.0f };
    xud_check(xud_camera_fill_screen_point(&camera, 200.0f, 100.0f, point, &screen) == XUD_INVALID_ARG, "camera/fold-point-at-eye",
        "a point AT the eye is the divide by zero the fold refuses rather than a pair of infinities");
    point = (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 4.0f };
    xud_check(xud_camera_fill_screen_point(&camera, 200.0f, 100.0f, point, &screen) == XUD_INVALID_ARG, "camera/fold-point-behind-eye",
        "and a point behind it has no screen position - raylib's blind divide answers the mirrored one here");

    screen = (XudVector2) { .x = NAN, .y = 50.0f };
    xud_check(xud_camera_fill_world_ray(&camera, 200.0f, 100.0f, screen, &ray) == XUD_INVALID_ARG, "camera/ray-nan-pixel",
        "a NaN pixel folds to a NaN direction, which the normalisation's floor refuses");

    /* The context-taking halves, over a shell whose window exists and whose extent is the 200x100
       above: the fold reads the extent and the window's presence and nothing else of either.
    */
    screen = (XudVector2) { .x = 100.0f, .y = 50.0f };
    point = (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 0.0f };
    xud_check(xud_get_world_to_screen(NULL, &shell, point, &camera) == XUD_INVALID_ARG, "camera/screen-no-out-param",
        "the public fold refuses a missing answer");
    xud_check(xud_get_world_to_screen(&screen, NULL, point, &camera) == XUD_INVALID_ARG, "camera/screen-no-context",
        "and a missing context, whose extent the fold has nothing to be a fraction of");
    xud_check(xud_get_world_to_screen(&screen, &shell, point, NULL) == XUD_INVALID_ARG, "camera/screen-no-camera", "and a missing camera");

    shell.window = NULL;
    xud_check(xud_get_world_to_screen(&screen, &shell, point, &camera) == XUD_UNSUPPORTED, "camera/screen-no-window",
        "a context that never completed bring-up has no extent a frame rendered at");
    xud_check(xud_get_screen_to_world_ray(&ray, &shell, screen, &camera) == XUD_UNSUPPORTED, "camera/ray-no-window",
        "and the trace gives the same answer for the same reason");
    shell.window = &window;

    xud_check(xud_get_world_to_screen(&screen, &shell, point, &camera) == XUD_OK, "camera/screen-with-extent",
        "with the window and its extent the fold answers from them");
    xud_check(xud_get_screen_to_world_ray(&ray, &shell, screen, &camera) == XUD_OK, "camera/ray-with-extent", "and so does the trace");

    /*--- the closed forms ------------------------------------------------------------------*/

    /* The frame's centre pixel traces the eye's own axis, and the origin the eye looks at is the
       frame's centre: the same fact stated from each end.
    */
    screen = (XudVector2) { .x = 100.0f, .y = 50.0f };
    point = (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 0.0f };
    result = xud_camera_fill_world_ray(&camera, 200.0f, 100.0f, screen, &ray);
    xud_check((result == XUD_OK) && xudTestNear(ray.direction.x, 0.0f) && xudTestNear(ray.direction.y, 0.0f)
            && xudTestNear(ray.direction.z, -1.0f),
        "camera/ray-centre-is-forward", "the middle pixel of the frame traces the axis the eye looks along");
    result = xud_camera_fill_screen_point(&camera, 200.0f, 100.0f, point, &screen);
    xud_check((result == XUD_OK) && xudTestNear(screen.x, 100.0f) && xudTestNear(screen.y, 50.0f), "camera/fold-centre-of-frame",
        "and the target the eye is aimed at lands in the middle of the frame");
    xud_check((ray.position.x == camera.position.x) && (ray.position.y == camera.position.y) && (ray.position.z == camera.position.z),
        "camera/ray-originates-at-eye", "and every trace starts at the eye, wherever on the frame it is aimed");

    /* One world unit off the axis, two units in front, at a 90-degree aperture and an aspect of 2:
       the vertical half of NDC is 1/2 and the horizontal is 1/(2*2), so the four points one unit off
       the axis land a quarter of the way out from the centre on each axis - 25 pixels along a row of
       200 and 25 along a column of 100. Y is the flipped one: the unit UP lands ABOVE the centre, at
       the smaller pixel row.
    */
    point = (XudVector3) { .x = 1.0f, .y = 0.0f, .z = 0.0f };
    result = xud_camera_fill_screen_point(&camera, 200.0f, 100.0f, point, &screen);
    xud_check((result == XUD_OK) && xudTestNear(screen.x, 125.0f) && xudTestNear(screen.y, 50.0f), "camera/fold-one-right",
        "a unit to the eye's right lands a quarter of the half-width out on the right");
    point = (XudVector3) { .x = -1.0f, .y = 0.0f, .z = 0.0f };
    result = xud_camera_fill_screen_point(&camera, 200.0f, 100.0f, point, &screen);
    xud_check((result == XUD_OK) && xudTestNear(screen.x, 75.0f) && xudTestNear(screen.y, 50.0f), "camera/fold-one-left",
        "and a unit to its left the same distance in the other direction, which is the sign a fold that forgot the flip would get wrong");
    point = (XudVector3) { .x = 0.0f, .y = 1.0f, .z = 0.0f };
    result = xud_camera_fill_screen_point(&camera, 200.0f, 100.0f, point, &screen);
    xud_check((result == XUD_OK) && xudTestNear(screen.x, 100.0f) && xudTestNear(screen.y, 25.0f), "camera/fold-one-up",
        "a unit up lands above the centre, at the SMALLER row - Vulkan's viewport has row zero on top while its NDC has +y up, and this "
        "is the one place the two are reconciled");
    point = (XudVector3) { .x = 0.0f, .y = -1.0f, .z = 0.0f };
    result = xud_camera_fill_screen_point(&camera, 200.0f, 100.0f, point, &screen);
    xud_check((result == XUD_OK) && xudTestNear(screen.x, 100.0f) && xudTestNear(screen.y, 75.0f), "camera/fold-one-down",
        "and a unit down below it");

    /* A square extent over the same camera moves the horizontal point, not the vertical one: half
       the columns is half the world's width across them, which is the aspect doing its job rather
       than a stretch.
    */
    point = (XudVector3) { .x = 1.0f, .y = 0.0f, .z = 0.0f };
    result = xud_camera_fill_screen_point(&camera, 100.0f, 100.0f, point, &screen);
    xud_check((result == XUD_OK) && xudTestNear(screen.x, 75.0f) && xudTestNear(screen.y, 50.0f), "camera/fold-square-extent",
        "the same point lands 3/4 across a square frame, where it was 5/8 across the wide one");

    /* The four corners of the frame, at 1/focal = 1 unit up and down and aspect/focal = 2 units
       sideways per unit of depth: the corner rays are the axis plus those offsets, normalised over
       the same sqrt(6).
    */
    screen = (XudVector2) { .x = 0.0f, .y = 0.0f };
    result = xud_camera_fill_world_ray(&camera, 200.0f, 100.0f, screen, &ray);
    xud_check((result == XUD_OK) && xudTestNear(ray.direction.x, -2.0f / 2.4494897f) && xudTestNear(ray.direction.y, 1.0f / 2.4494897f)
            && xudTestNear(ray.direction.z, -1.0f / 2.4494897f),
        "camera/ray-top-left-corner", "the top-left pixel traces the frustum's top-left edge");
    screen = (XudVector2) { .x = 200.0f, .y = 100.0f };
    result = xud_camera_fill_world_ray(&camera, 200.0f, 100.0f, screen, &ray);
    xud_check((result == XUD_OK) && xudTestNear(ray.direction.x, 2.0f / 2.4494897f) && xudTestNear(ray.direction.y, -1.0f / 2.4494897f)
            && xudTestNear(ray.direction.z, -1.0f / 2.4494897f),
        "camera/ray-bottom-right-corner", "and the bottom-right pixel the opposite edge");

    /*--- the fold against the matrix, and against itself -----------------------------------*/

    /* The fold is the CPU's own route from a world point to a pixel; the matrix is the route the
       shader takes. Two points on the axis, two off it and one above: the two routes have to put
       the same pixel on the same frame or the box a pick selects is not the box that was drawn.
    */
    {
        const float samples[4][3] = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.5f, -1.0f }, { -1.5f, -0.25f, 1.0f }, { 0.75f, 1.0f, -0.5f } };
        uint32_t sample = 0u;

        if (xud_camera_fill_view_proj(&camera, 2.0f, matrix) != XUD_OK) {
            badMatrix++;
        }

        for (sample = 0u; sample < 4u; sample++) {
            point.x = samples[sample][0];
            point.y = samples[sample][1];
            point.z = samples[sample][2];

            if (xud_camera_fill_screen_point(&camera, 200.0f, 100.0f, point, &screen) != XUD_OK) {
                badMatrix++;
                continue;
            }

            xudTestTransform(matrix, point.x, point.y, point.z, clip);
            if (clip[3] <= 0.0f) {
                badMatrix++;
                continue;
            }

            expected[0] = ((0.5f * clip[0] / clip[3]) + 0.5f) * 200.0f;
            expected[1] = (0.5f - (0.5f * clip[1] / clip[3])) * 100.0f;
            if (!xudTestNear(screen.x, expected[0]) || !xudTestNear(screen.y, expected[1])) {
                badMatrix++;
            }
        }
    }

    xud_check(badMatrix == 0u, "camera/fold-agrees-with-matrix",
        "four world points fold to the pixels the shader's own view-projection puts them at, so a pick cannot drift from what was drawn");

    /* Every point of a 3x3x3 block in front of the eye, out and back: the fold's pixel, traced back,
       has to come along the same line the eye-to-point vector is - the cross product of the two is
       zero to within the tolerance scaled by how far the point is - and has to point at it rather
       than away. The directions are unit, which is what the collision rungs below read without
       normalising them again.
    */
    for (iz = 0u; iz < 3u; iz++) {
        for (iy = 0u; iy < 3u; iy++) {
            for (ix = 0u; ix < 3u; ix++) {
                point = (XudVector3) { .x = -1.5f + (1.5f * (float)ix), .y = -1.0f + (1.0f * (float)iy), .z = -1.0f - (2.0f * (float)iz) };

                if (xud_camera_fill_screen_point(&camera, 200.0f, 100.0f, point, &screen) != XUD_OK) {
                    bad++;
                    continue;
                }

                if (xud_camera_fill_world_ray(&camera, 200.0f, 100.0f, screen, &ray) != XUD_OK) {
                    bad++;
                    continue;
                }

                length = sqrtf(
                    (ray.direction.x * ray.direction.x) + (ray.direction.y * ray.direction.y) + (ray.direction.z * ray.direction.z));
                if (fabsf(length - 1.0f) > XUD_TEST_CAMERA_TOLERANCE) {
                    badLength++;
                }

                toPoint[0] = point.x - camera.position.x;
                toPoint[1] = point.y - camera.position.y;
                toPoint[2] = point.z - camera.position.z;
                toLength = sqrtf((toPoint[0] * toPoint[0]) + (toPoint[1] * toPoint[1]) + (toPoint[2] * toPoint[2]));

                cross[0] = (ray.direction.y * toPoint[2]) - (ray.direction.z * toPoint[1]);
                cross[1] = (ray.direction.z * toPoint[0]) - (ray.direction.x * toPoint[2]);
                cross[2] = (ray.direction.x * toPoint[1]) - (ray.direction.y * toPoint[0]);
                crossLength = sqrtf((cross[0] * cross[0]) + (cross[1] * cross[1]) + (cross[2] * cross[2]));

                if (crossLength > (XUD_TEST_CAMERA_TOLERANCE * toLength)) {
                    bad++;
                }

                if (((ray.direction.x * toPoint[0]) + (ray.direction.y * toPoint[1]) + (ray.direction.z * toPoint[2])) <= 0.0f) {
                    bad++;
                }
            }
        }
    }

    xud_check(bad == 0u, "camera/fold-round-trip",
        "27 world points fold to a pixel and the trace back through that pixel comes along the same line the eye-to-point vector is, and "
        "towards the point rather than away from it");
    xud_check(badLength == 0u, "camera/ray-directions-are-unit",
        "and every direction the trace wrote is a unit vector, which is what the collision rungs read without normalising it again");
}

void xud_test_camera(void) {
    xud_test_camera_pure();
    xud_test_camera_fold();
    xud_test_camera_moves();
    if (xud_gpu_tests_enabled()) {
        xud_test_camera_gpu();
    }
}
