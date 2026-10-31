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
        "a live camera is taken away, which is the state the next frame's block is written from");
    xud_check(renderer.cameraWanted == 0u, "camera/end-clears-the-flag",
        "the flag is cleared, so the next frame's block carries the identity matrix rather than whatever the last frame's camera wrote");
    xud_check(renderer.camera.fovy == 60.0f, "camera/end-leaves-the-record",
        "the record itself is left as it was - the flag is what governs, and a begin after here replaces the whole record");
    xud_check(xud_end_camera_3d(&shell) == XUD_ALREADY, "camera/end-no-camera-live",
        "and taking away a camera that is not live is the repo's idempotent ask rather than a refusal");
    xud_check(renderer.cameraWanted == 0u, "camera/end-twice-changes-nothing",
        "the second end changed nothing, so the ask is idempotent rather than a refusal that left state");
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

    result = xud_init_context(&context, 64u, 64u);
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

    xud_check(xud_begin_drawing(context) == XUD_OK, "gpu-camera/begin-drawing-after-end", "the next frame opens with no camera live");
    xud_check(xud_draw_cube(context, (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 0.0f }, 0.5f, 0.5f, 0.5f,
                  (XudColor) { .r = 60u, .g = 200u, .b = 60u, .a = 255u })
            == XUD_OK,
        "gpu-camera/cube-appended-no-camera", "a box is appended with no camera live");
    result = xud_end_drawing(context);
    xud_check(result == XUD_OK, "gpu-camera/end-drawing-no-camera", "and the frame closed");

    identity = 0u;
    for (element = 0u; element < XUD_CAMERA_MATRIX_ELEMENT_COUNT; element++) {
        if (context->renderer->descriptor->uniformBlock.viewProj[element] == 1.0f) {
            identity++;
        }
    }
    xud_check(identity == 4u, "gpu-camera/block-back-at-identity",
        "the block's matrix is the identity again, so the camera that began and ended inside these two frames left nothing behind in the "
        "frame that drew without one");

    if (context->validate != 0u) {
        xud_check(context->diagnostics->errorMessages == 0u, "gpu-camera/validation-clean",
            "two frames drew a real box through a real pipeline with a real camera and the messenger heard nothing");
    }

    result = xud_close_context(&context);
    xud_check(result == XUD_OK, "gpu-camera/close-clean", "the whole ladder tears back down with nothing refused");
    xud_check(context == NULL, "gpu-camera/close-clears", "and the out-param is cleared");
}

void xud_test_camera(void) {
    xud_test_camera_pure();
    if (xud_gpu_tests_enabled()) {
        xud_test_camera_gpu();
    }
}
