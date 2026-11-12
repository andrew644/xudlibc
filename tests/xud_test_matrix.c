/***********************************************************************************************
 *
 *   xud_test_matrix.c - the matrix rung's eight builders, over the sixteen floats they share
 *
 *   Owns : the tests of xud_matrix_identity/translate/scale/rotate/rotate_xyz/multiply/compose/
 *          transform_point - what each transform DOES to a point, the order the factors are spent
 *          in, the axis-free normalisation, the alias-free product, and the refusals. Nothing else:
 *          the camera's own view-projection is the camera area's, and the draw that spends a posed
 *          matrix is the batch area's.
 *   Layer: tests, above the matrix rung, and no GPU half - the rung reads no context, so every
 *          rotation is answerable with no driver present.
 *
 *   What the expected values are. Rotations are pinned by where they send a basis vector, computed
 *   by hand from the right-handed rule the rung states (a positive quarter turn about +z takes +x to
 *   +y), never by reading the implementation's product back. The factor order is pinned twice over:
 *   once by a two-angle euler that lands a basis vector on a third axis, and once by a
 *   translate-times-rotate whose point lands at the rotated position plus the translation. The euler
 *   is additionally compared against raymath's own MatrixRotateXYZ (raymath.h 1897, transposed into
 *   this library's column layout) because the rung claims numeric equivalence to it rather than a
 *   similar rotation.
 *
 *   Deterministic: fixed angles - quarters, halves, and one 37 degrees - no time, no randomness, no
 *   filesystem. Values that crossed a sinf/cosf are read against the area's tolerance; the ones that
 *   did not (a translate, a scale, a product of two quarter turns' exact zeros) are read exactly.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include <math.h> /* Required for: NAN, INFINITY, sqrtf, and the quarters the angles are made of */
#include <stddef.h> /* Required for: NULL */

#include "xud.h" /* Required for: XudResult, XudVector3, the eight calls */
#include "xud_test_harness.h"

#define XUD_TEST_MATRIX_TOLERANCE                                                                                                          \
    1e-5f /* The reach of one sinf/cosf round trip: a matrix element that crossed a trigonometric                                            \
            function is read against this, and one that did not is read exactly.                                                            \
     */

#define XUD_TEST_MATRIX_ELEMENTS 16u /* The length of every matrix the area builds */

static float xudTestMatrixA[XUD_TEST_MATRIX_ELEMENTS];
static float xudTestMatrixB[XUD_TEST_MATRIX_ELEMENTS];
static float xudTestMatrixC[XUD_TEST_MATRIX_ELEMENTS];
static float xudTestMatrixD[XUD_TEST_MATRIX_ELEMENTS];
static XudVector3 xudTestPoint;

static const XudVector3 xudTestOrigin = { 0.0f, 0.0f, 0.0f };
static const XudVector3 xudTestUnitX = { 1.0f, 0.0f, 0.0f };
static const XudVector3 xudTestUnitY = { 0.0f, 1.0f, 0.0f };
static const XudVector3 xudTestUnitZ = { 0.0f, 0.0f, 1.0f };

#define XUD_TEST_MATRIX_PI 3.1415926535897932f /* raylib's and every other rung's own literal: M_PI is not C11 */
#define XUD_TEST_MATRIX_QUARTER (XUD_TEST_MATRIX_PI / 2.0f) /* the three right angles every basis check uses */
#define XUD_TEST_MATRIX_HALF XUD_TEST_MATRIX_PI /* a half turn, which lands every basis vector on its own line */
#define XUD_TEST_MATRIX_ODD                                                                                                               \
    (37.0f * XUD_TEST_MATRIX_PI / 180.0f) /* one angle that is a special case of nothing, in                                              \
                                            radians                                                                                       \
                                         */

static uint32_t xud_test_matrix_close(const float* left, const float* right, float tolerance) {
    uint32_t element = 0u;

    for (element = 0u; element < XUD_TEST_MATRIX_ELEMENTS; ++element) {
        const float delta = left[element] - right[element];

        if (((delta < 0.0f) ? -delta : delta) > tolerance) {
            return 0u;
        }
    }

    return 1u;
}

static uint32_t xud_test_matrix_is_zero(const float* matrix) {
    uint32_t element = 0u;

    for (element = 0u; element < XUD_TEST_MATRIX_ELEMENTS; ++element) {
        if (matrix[element] != 0.0f) {
            return 0u;
        }
    }

    return 1u;
}

static uint32_t xud_test_point_close(XudVector3 point, XudVector3 expected) {
    const float deltas[3] = { point.x - expected.x, point.y - expected.y, point.z - expected.z };
    uint32_t axis = 0u;

    for (axis = 0u; axis < 3u; ++axis) {
        if (((deltas[axis] < 0.0f) ? -deltas[axis] : deltas[axis]) > XUD_TEST_MATRIX_TOLERANCE) {
            return 0u;
        }
    }

    return 1u;
}

/* raymath's own MatrixRotateXYZ, in raymath's own row-major layout and with raymath's own negated
   angles (raymath.h 1897-1926), written out here so the comparison is against the reference and not
   against this rung's arithmetic. The caller gets the TRANSPOSE, which is the same transform read as
   columns - the layout claim the rung's banner makes.
*/
static void xud_test_matrix_raymath_xyz(float* outMatrix, float angleX, float angleY, float angleZ) {
    const float cosZ = cosf(-angleZ);
    const float sinZ = sinf(-angleZ);
    const float cosY = cosf(-angleY);
    const float sinY = sinf(-angleY);
    const float cosX = cosf(-angleX);
    const float sinX = sinf(-angleX);
    float rows[3][3];

    rows[0][0] = cosZ * cosY;
    rows[0][1] = (cosZ * sinY * sinX) - (sinZ * cosX);
    rows[0][2] = (cosZ * sinY * cosX) + (sinZ * sinX);

    rows[1][0] = sinZ * cosY;
    rows[1][1] = (sinZ * sinY * sinX) + (cosZ * cosX);
    rows[1][2] = (sinZ * sinY * cosX) - (cosZ * sinX);

    rows[2][0] = -sinY;
    rows[2][1] = cosY * sinX;
    rows[2][2] = cosY * cosX;

    /* The transpose, stated once: this layout's slot for column c, row r holds raymath's element at
       row c, column r. Reading rows[r][c] into that slot instead is the same bytes as raymath's
       struct, and the inverse rotation.
    */
    for (uint32_t row = 0u; row < 3u; ++row) {
        for (uint32_t column = 0u; column < 3u; ++column) {
            outMatrix[(column * 4u) + row] = rows[column][row];
        }
    }

    outMatrix[3] = 0.0f;
    outMatrix[7] = 0.0f;
    outMatrix[11] = 0.0f;
    outMatrix[12] = 0.0f;
    outMatrix[13] = 0.0f;
    outMatrix[14] = 0.0f;
    outMatrix[15] = 1.0f;
}

/*-----------------------------------------------------------------------------------------------
 * The three placements, and the point that reads them
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_matrix_placement(void) {
    const XudVector3 shift = { 4.0f, -2.5f, 0.75f };
    const XudVector3 stretch = { 2.0f, 0.5f, 10.0f };
    const XudVector3 corner = { 1.0f, 2.0f, 3.0f };
    XudVector3 expected = { 0.0f, 0.0f, 0.0f };

    xud_check(xud_matrix_identity(xudTestMatrixA) == XUD_OK, "matrix/identity-ok", "the identity builds with nothing to refuse");
    xud_check(xudTestMatrixA[0] == 1.0f, "matrix/identity-diagonal", "one on the diagonal at 0");
    xud_check((xudTestMatrixA[5] == 1.0f) && (xudTestMatrixA[10] == 1.0f) && (xudTestMatrixA[15] == 1.0f), "matrix/identity-diagonal-rest",
        "and at 5, 10 and 15");
    xud_check((xudTestMatrixA[1] == 0.0f) && (xudTestMatrixA[4] == 0.0f) && (xudTestMatrixA[12] == 0.0f) && (xudTestMatrixA[3] == 0.0f)
                  && (xudTestMatrixA[7] == 0.0f) && (xudTestMatrixA[11] == 0.0f),
        "matrix/identity-off-diagonal", "zero everywhere else, the fourth row included");

    xud_check(xud_matrix_translate(xudTestMatrixA, shift) == XUD_OK, "matrix/translate-ok", "a shift builds");
    xud_check((xudTestMatrixA[12] == shift.x) && (xudTestMatrixA[13] == shift.y) && (xudTestMatrixA[14] == shift.z),
        "matrix/translate-column", "the point lands in the fourth COLUMN at 12, 13, 14, not in a row");
    xud_check((xudTestMatrixA[0] == 1.0f) && (xudTestMatrixA[5] == 1.0f) && (xudTestMatrixA[10] == 1.0f), "matrix/translate-basis",
        "and the three basis columns are untouched");

    xud_check(xud_matrix_transform_point(&xudTestPoint, xudTestMatrixA, xudTestOrigin) == XUD_OK, "matrix/transform-ok", "a point goes through");
    xud_check(xud_test_point_close(xudTestPoint, shift), "matrix/transform-origin", "the origin comes out at the translation");
    xud_check((xud_matrix_transform_point(&xudTestPoint, xudTestMatrixA, corner) == XUD_OK)
                  && xud_test_point_close(xudTestPoint, (XudVector3){ corner.x + shift.x, corner.y + shift.y, corner.z + shift.z }),
        "matrix/transform-point", "a point comes out shifted by the same amount");

    xud_check(xud_matrix_scale(xudTestMatrixA, stretch) == XUD_OK, "matrix/scale-ok", "a stretch builds");
    xud_check((xud_matrix_transform_point(&xudTestPoint, xudTestMatrixA, xudTestUnitX) == XUD_OK)
                  && xud_test_point_close(xudTestPoint, (XudVector3){ stretch.x, 0.0f, 0.0f }),
        "matrix/scale-basis-x", "+x lands on the x stretch, the other two coordinates staying zero");
    xud_check((xud_matrix_transform_point(&xudTestPoint, xudTestMatrixA, xudTestUnitY) == XUD_OK)
                  && xud_test_point_close(xudTestPoint, (XudVector3){ 0.0f, stretch.y, 0.0f }),
        "matrix/scale-basis-y", "+y lands on the y stretch");
    xud_check((xud_matrix_transform_point(&xudTestPoint, xudTestMatrixA, xudTestUnitZ) == XUD_OK)
                  && xud_test_point_close(xudTestPoint, (XudVector3){ 0.0f, 0.0f, stretch.z }),
        "matrix/scale-basis-z", "and +z on the z stretch, so the three columns carry the three numbers");
    xud_check(xud_matrix_scale(xudTestMatrixA, (XudVector3){ 0.0f, 0.0f, 0.0f }) == XUD_OK, "matrix/scale-zero",
        "a scale of zero is a flattening the caller asked for, not a refusal");
    xud_check(xud_test_matrix_is_zero(xudTestMatrixA) == 0u, "matrix/scale-zero-writes", "and it still writes its matrix");

    /* The affine half only: the rung states that the fourth row is never read, which is what makes it a
       placement rather than a projection, and the only way to tell the two apart is to put a number there.
    */
    xud_check(xud_matrix_identity(xudTestMatrixA) == XUD_OK, "matrix/transform-clip-setup", "the identity, then a fourth row filled in");
    xudTestMatrixA[3] = 5.0f;
    xudTestMatrixA[7] = -2.0f;
    xudTestMatrixA[11] = 9.0f;
    xudTestMatrixA[15] = 3.0f;
    xud_check((xud_matrix_transform_point(&xudTestPoint, xudTestMatrixA, corner) == XUD_OK) && xud_test_point_close(xudTestPoint, corner),
        "matrix/transform-ignores-fourth-row", "a corner still comes out of a matrix whose w row says otherwise");

    expected = corner;
    xud_check(xud_matrix_identity(xudTestMatrixA) == XUD_OK, "matrix/transform-identity-setup", "the identity again, for the neutral case");
    xud_check((xud_matrix_transform_point(&xudTestPoint, xudTestMatrixA, expected) == XUD_OK) && xud_test_point_close(xudTestPoint, expected),
        "matrix/transform-identity", "the identity leaves a point where it found it");
}

/*-----------------------------------------------------------------------------------------------
 * The axis-angle rotation
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_matrix_rotate(void) {
    const XudVector3 arm = { 3.0f, 0.0f, 0.0f };
    float length = 0.0f;

    xud_check((xud_matrix_rotate(xudTestMatrixA, xudTestUnitZ, XUD_TEST_MATRIX_QUARTER) == XUD_OK)
                  && (xud_matrix_transform_point(&xudTestPoint, xudTestMatrixA, xudTestUnitX) == XUD_OK)
                  && xud_test_point_close(xudTestPoint, xudTestUnitY),
        "matrix/rotate-z-quarter", "a positive quarter about +z takes +x to +y");
    xud_check((xud_matrix_rotate(xudTestMatrixA, xudTestUnitX, XUD_TEST_MATRIX_QUARTER) == XUD_OK)
                  && (xud_matrix_transform_point(&xudTestPoint, xudTestMatrixA, xudTestUnitY) == XUD_OK)
                  && xud_test_point_close(xudTestPoint, xudTestUnitZ),
        "matrix/rotate-x-quarter", "and about +x takes +y to +z");
    xud_check((xud_matrix_rotate(xudTestMatrixA, xudTestUnitY, XUD_TEST_MATRIX_QUARTER) == XUD_OK)
                  && (xud_matrix_transform_point(&xudTestPoint, xudTestMatrixA, xudTestUnitZ) == XUD_OK)
                  && xud_test_point_close(xudTestPoint, xudTestUnitX),
        "matrix/rotate-y-quarter", "and about +y takes +z to +x: right-handed all the way round");

    xud_check(xud_matrix_rotate(xudTestMatrixA, xudTestUnitX, 0.0f) == XUD_OK, "matrix/rotate-zero-ok", "a turn of zero builds");
    xud_check(xud_test_matrix_close(xudTestMatrixA, (const float[16]){ 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                                          0.0f, 0.0f, 0.0f, 1.0f },
                          0.0f)
            == 1u,
        "matrix/rotate-zero-identity", "and is the identity exactly, sines and all");

    /* The axis needs no normalising: (0, 0, 4) points the same way (0, 0, 1) does, and a rung that
       divided by the wrong thing would answer a different rotation for the two.
    */
    xud_check((xud_matrix_rotate(xudTestMatrixA, (XudVector3){ 0.0f, 0.0f, 4.0f }, XUD_TEST_MATRIX_ODD) == XUD_OK)
                  && (xud_matrix_rotate(xudTestMatrixB, xudTestUnitZ, XUD_TEST_MATRIX_ODD) == XUD_OK)
                  && (xud_test_matrix_close(xudTestMatrixA, xudTestMatrixB, XUD_TEST_MATRIX_TOLERANCE) == 1u),
        "matrix/rotate-axis-normalised", "a long axis and a unit axis answer the same rotation");

    xud_check((xud_matrix_rotate(xudTestMatrixA, (XudVector3){ 1.0f, 1.0f, 1.0f }, XUD_TEST_MATRIX_ODD) == XUD_OK)
                  && (xud_matrix_transform_point(&xudTestPoint, xudTestMatrixA, arm) == XUD_OK),
        "matrix/rotate-oblique-ok", "a diagonal axis and an off-axis arm both go in");
    length = sqrtf((xudTestPoint.x * xudTestPoint.x) + (xudTestPoint.y * xudTestPoint.y) + (xudTestPoint.z * xudTestPoint.z));
    xud_check((length > (3.0f - (10.0f * XUD_TEST_MATRIX_TOLERANCE))) && (length < (3.0f + (10.0f * XUD_TEST_MATRIX_TOLERANCE))),
        "matrix/rotate-keeps-length", "and a rotation leaves the arm's length at three");

    /* Two quarters about one axis are a half turn about it: the closed form is checked against a
       second, longer one rather than against itself.
    */
    xud_check((xud_matrix_rotate(xudTestMatrixA, xudTestUnitY, XUD_TEST_MATRIX_QUARTER) == XUD_OK)
                  && (xud_matrix_rotate(xudTestMatrixB, xudTestUnitY, XUD_TEST_MATRIX_QUARTER) == XUD_OK)
                  && (xud_matrix_multiply(xudTestMatrixC, xudTestMatrixA, xudTestMatrixB) == XUD_OK)
                  && (xud_matrix_rotate(xudTestMatrixD, xudTestUnitY, XUD_TEST_MATRIX_HALF) == XUD_OK)
                  && (xud_test_matrix_close(xudTestMatrixC, xudTestMatrixD, 1e-4f) == 1u),
        "matrix/rotate-two-quarters", "two quarters about +y are the half turn they add up to");
}



/*-----------------------------------------------------------------------------------------------
 * The three angles at once, and the order they are spent in
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_matrix_euler(void) {
    xud_check(xud_matrix_rotate_xyz(xudTestMatrixA, (XudVector3){ 0.0f, 0.0f, 0.0f }) == XUD_OK, "matrix/euler-zero-ok",
        "three zero angles build");
    xud_check(xud_test_matrix_close(xudTestMatrixA, (const float[16]){ 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                                          0.0f, 0.0f, 0.0f, 1.0f },
                          0.0f)
            == 1u,
        "matrix/euler-zero-identity", "and are the identity, exactly");

    /* One angle at a time is the single-axis builder the caller could have used instead. */
    xud_check((xud_matrix_rotate_xyz(xudTestMatrixA, (XudVector3){ 0.0f, XUD_TEST_MATRIX_ODD, 0.0f }) == XUD_OK)
                  && (xud_matrix_rotate(xudTestMatrixB, xudTestUnitY, XUD_TEST_MATRIX_ODD) == XUD_OK)
                  && (xud_test_matrix_close(xudTestMatrixA, xudTestMatrixB, XUD_TEST_MATRIX_TOLERANCE) == 1u),
        "matrix/euler-yaw-only", "yaw alone is a rotation about +y");
    xud_check((xud_matrix_rotate_xyz(xudTestMatrixA, (XudVector3){ XUD_TEST_MATRIX_ODD, 0.0f, 0.0f }) == XUD_OK)
                  && (xud_matrix_rotate(xudTestMatrixB, xudTestUnitX, XUD_TEST_MATRIX_ODD) == XUD_OK)
                  && (xud_test_matrix_close(xudTestMatrixA, xudTestMatrixB, XUD_TEST_MATRIX_TOLERANCE) == 1u),
        "matrix/euler-pitch-only", "pitch alone is a rotation about +x");
    xud_check((xud_matrix_rotate_xyz(xudTestMatrixA, (XudVector3){ 0.0f, 0.0f, XUD_TEST_MATRIX_ODD }) == XUD_OK)
                  && (xud_matrix_rotate(xudTestMatrixB, xudTestUnitZ, XUD_TEST_MATRIX_ODD) == XUD_OK)
                  && (xud_test_matrix_close(xudTestMatrixA, xudTestMatrixB, XUD_TEST_MATRIX_TOLERANCE) == 1u),
        "matrix/euler-roll-only", "and roll alone is a rotation about +z");

    /* The order, pinned by a case only one order can answer: x and z a quarter each, y nothing. The
       third angle is spent first, so +x goes to +y and then +y goes to +z; a rung that multiplied
       Rx*Ry*Rz the other way round would leave the arm lying on +y.
    */
    xud_check((xud_matrix_rotate_xyz(xudTestMatrixA, (XudVector3){ XUD_TEST_MATRIX_QUARTER, 0.0f, XUD_TEST_MATRIX_QUARTER }) == XUD_OK)
                  && (xud_matrix_transform_point(&xudTestPoint, xudTestMatrixA, xudTestUnitX) == XUD_OK)
                  && xud_test_point_close(xudTestPoint, xudTestUnitZ),
        "matrix/euler-order", "pitch then roll sends +x to +z, which is roll first and pitch last");

    /* The equivalence the rung's banner claims, checked against raymath's own arithmetic rather than
       against this file's: three angles no symmetry helps, read through all sixteen slots.
    */
    xud_check(xud_matrix_rotate_xyz(xudTestMatrixA, (XudVector3){ 0.3f, -1.1f, 2.4f }) == XUD_OK, "matrix/euler-raymath-setup",
        "an odd triple builds");
    xud_test_matrix_raymath_xyz(xudTestMatrixB, 0.3f, -1.1f, 2.4f);
    xud_check(xud_test_matrix_close(xudTestMatrixA, xudTestMatrixB, XUD_TEST_MATRIX_TOLERANCE) == 1u, "matrix/euler-raymath-xyz",
        "and every slot matches raymath's MatrixRotateXYZ transposed into this layout");

    xud_check(xud_matrix_rotate_xyz(xudTestMatrixA, (XudVector3){ 1.0f, -0.5f, 0.25f }) == XUD_OK, "matrix/euler-raymath-again-setup",
        "a second triple, this one with the middle angle negative");
    xud_test_matrix_raymath_xyz(xudTestMatrixB, 1.0f, -0.5f, 0.25f);
    xud_check(xud_test_matrix_close(xudTestMatrixA, xudTestMatrixB, XUD_TEST_MATRIX_TOLERANCE) == 1u, "matrix/euler-raymath-again",
        "and it matches too, which a transpose bug would not survive");
}


/*-----------------------------------------------------------------------------------------------
 * The product, the compose, and what a refusal leaves behind
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_matrix_product(void) {
    const XudVector3 shift = { 1.0f, 2.0f, 3.0f };
    const XudVector3 stretch = { 2.0f, 3.0f, 4.0f };
    float bracket[XUD_TEST_MATRIX_ELEMENTS];

    xud_check(xud_matrix_identity(xudTestMatrixA) == XUD_OK, "matrix/product-setup", "the identity, to multiply against");
    xud_check(xud_matrix_rotate(xudTestMatrixB, xudTestUnitY, XUD_TEST_MATRIX_ODD) == XUD_OK, "matrix/product-setup-rotation",
        "and a rotation to multiply it by");
    xud_check((xud_matrix_multiply(xudTestMatrixC, xudTestMatrixA, xudTestMatrixB) == XUD_OK)
                  && (xud_test_matrix_close(xudTestMatrixC, xudTestMatrixB, 0.0f) == 1u),
        "matrix/product-identity-left", "identity on the left leaves the factor alone");
    xud_check((xud_matrix_multiply(xudTestMatrixC, xudTestMatrixB, xudTestMatrixA) == XUD_OK)
                  && (xud_test_matrix_close(xudTestMatrixC, xudTestMatrixB, 0.0f) == 1u),
        "matrix/product-identity-right", "and identity on the right leaves it alone too");

    /* translate * rotate is the order every placement uses, and the order a point can tell: the arm
       goes to where the rotation puts it and THEN travels by the translation. The other order travels
       first and rotates the travel, which would answer (-1, 1, 3) here rather than (1, 3, 3).
    */
    xud_check((xud_matrix_translate(xudTestMatrixA, shift) == XUD_OK)
                  && (xud_matrix_rotate(xudTestMatrixB, xudTestUnitZ, XUD_TEST_MATRIX_QUARTER) == XUD_OK)
                  && (xud_matrix_multiply(xudTestMatrixC, xudTestMatrixA, xudTestMatrixB) == XUD_OK)
                  && (xud_matrix_transform_point(&xudTestPoint, xudTestMatrixC, xudTestUnitX) == XUD_OK)
                  && xud_test_point_close(xudTestPoint, (XudVector3){ 1.0f, 3.0f, 3.0f }),
        "matrix/product-order", "translate times rotate rotates the point and then moves it");

    /* Associativity, because the compose below is checked against two multiplies: if the two ways of
       bracketing a triple disagreed, that check would pin whichever bracket the implementation likes.
    */
    xud_check((xud_matrix_translate(xudTestMatrixA, shift) == XUD_OK) && (xud_matrix_scale(xudTestMatrixB, stretch) == XUD_OK)
                  && (xud_matrix_rotate(xudTestMatrixC, (XudVector3){ 1.0f, 2.0f, 3.0f }, XUD_TEST_MATRIX_ODD) == XUD_OK)
                  && (xud_matrix_multiply(xudTestMatrixD, xudTestMatrixB, xudTestMatrixC) == XUD_OK)
                  && (xud_matrix_multiply(xudTestMatrixD, xudTestMatrixA, xudTestMatrixD) == XUD_OK)
                  && (xud_matrix_multiply(bracket, xudTestMatrixA, xudTestMatrixB) == XUD_OK)
                  && (xud_matrix_multiply(bracket, bracket, xudTestMatrixC) == XUD_OK)
                  && (xud_test_matrix_close(xudTestMatrixD, bracket, 1e-4f) == 1u),
        "matrix/product-associative", "(translate*scale)*rotate and translate*(scale*rotate) are one matrix");

    /* The out pointer may be one of the factors: the product is built in a local first, and a rung
       that wrote slot by slot into out would corrupt the factor it was still reading.
    */
    xud_check((xud_matrix_translate(xudTestMatrixA, shift) == XUD_OK) && (xud_matrix_scale(xudTestMatrixB, stretch) == XUD_OK)
                  && (xud_matrix_multiply(xudTestMatrixC, xudTestMatrixA, xudTestMatrixB) == XUD_OK),
        "matrix/product-alias-setup", "the same product, into a third buffer");
    xud_check(xud_matrix_multiply(xudTestMatrixA, xudTestMatrixA, xudTestMatrixB) == XUD_OK, "matrix/product-alias-ok", "and in place");
    xud_check(xud_test_matrix_close(xudTestMatrixA, xudTestMatrixC, 0.0f) == 1u, "matrix/product-alias-in-place",
        "the in-place product is the other product, slot for slot");
    xud_check(xud_matrix_multiply(xudTestMatrixB, xudTestMatrixA, xudTestMatrixB) == XUD_OK, "matrix/product-alias-right-ok",
        "aliasing the RIGHT factor works as well");
}

static void xud_test_matrix_compose(void) {
    const XudVector3 shift = { 1.0f, 2.0f, 3.0f };
    const XudVector3 yaw = { 0.0f, XUD_TEST_MATRIX_QUARTER, 0.0f };
    const XudVector3 stretch = { 2.0f, 2.0f, 2.0f };

    /* The compose is the two multiplies the caller would otherwise write, so it is checked against
       exactly those two multiplies rather than against a hand-written matrix.
    */
    xud_check((xud_matrix_compose(xudTestMatrixA, shift, yaw, stretch) == XUD_OK) && (xud_matrix_translate(xudTestMatrixB, shift) == XUD_OK)
                  && (xud_matrix_rotate_xyz(xudTestMatrixC, yaw) == XUD_OK) && (xud_matrix_scale(xudTestMatrixD, stretch) == XUD_OK)
                  && (xud_matrix_multiply(xudTestMatrixB, xudTestMatrixB, xudTestMatrixC) == XUD_OK)
                  && (xud_matrix_multiply(xudTestMatrixB, xudTestMatrixB, xudTestMatrixD) == XUD_OK)
                  && (xud_test_matrix_close(xudTestMatrixA, xudTestMatrixB, XUD_TEST_MATRIX_TOLERANCE) == 1u),
        "matrix/compose-is-translate-rotate-scale", "compose equals translate*rotate*scale, multiplied out");

    xud_check((xud_matrix_compose(xudTestMatrixA, shift, yaw, stretch) == XUD_OK)
                  && (xud_matrix_transform_point(&xudTestPoint, xudTestMatrixA, xudTestUnitX) == XUD_OK)
                  && xud_test_point_close(xudTestPoint, (XudVector3){ 1.0f, 2.0f, 1.0f }),
        "matrix/compose-point", "a doubled arm yawed a quarter and moved lands at (1, 2, 1)");

    xud_check((xud_matrix_compose(xudTestMatrixA, shift, (XudVector3){ 0.0f, 0.0f, 0.0f }, (XudVector3){ 1.0f, 1.0f, 1.0f }) == XUD_OK)
                  && (xud_matrix_translate(xudTestMatrixB, shift) == XUD_OK)
                  && (xud_test_matrix_close(xudTestMatrixA, xudTestMatrixB, 0.0f) == 1u),
        "matrix/compose-neutral", "a compose with no angle and no stretch is the translate alone");
}


/*-----------------------------------------------------------------------------------------------
 * The refusals, and the zeros they leave behind
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_matrix_refusals(void) {
    const float nan = (float)NAN;
    const float infinite = (float)INFINITY;

    xud_check(xud_matrix_identity(NULL) == XUD_INVALID_ARG, "matrix/refuse-identity", "no answer, no identity");
    xud_check(xud_matrix_translate(NULL, xudTestOrigin) == XUD_INVALID_ARG, "matrix/refuse-translate", "the shift refuses a NULL out");
    xud_check(xud_matrix_scale(NULL, xudTestUnitX) == XUD_INVALID_ARG, "matrix/refuse-scale", "so does the stretch");
    xud_check(xud_matrix_rotate(NULL, xudTestUnitZ, 1.0f) == XUD_INVALID_ARG, "matrix/refuse-rotate", "so does the axis-angle turn");
    xud_check(xud_matrix_rotate_xyz(NULL, xudTestOrigin) == XUD_INVALID_ARG, "matrix/refuse-rotate-xyz", "so do the three angles");
    xud_check(xud_matrix_multiply(NULL, xudTestMatrixA, xudTestMatrixB) == XUD_INVALID_ARG, "matrix/refuse-multiply-out",
        "the product refuses a NULL out");
    xud_check(xud_matrix_multiply(xudTestMatrixA, NULL, xudTestMatrixB) == XUD_INVALID_ARG, "matrix/refuse-multiply-left",
        "a NULL left factor");
    xud_check(xud_matrix_multiply(xudTestMatrixA, xudTestMatrixB, NULL) == XUD_INVALID_ARG, "matrix/refuse-multiply-right",
        "and a NULL right one");
    xud_check(xud_matrix_compose(NULL, xudTestOrigin, xudTestOrigin, xudTestUnitX) == XUD_INVALID_ARG, "matrix/refuse-compose",
        "the compose refuses a NULL out");
    xud_check(xud_matrix_transform_point(NULL, xudTestMatrixA, xudTestOrigin) == XUD_INVALID_ARG, "matrix/refuse-point-out",
        "the point query refuses a NULL answer");
    xud_check(xud_matrix_transform_point(&xudTestPoint, NULL, xudTestOrigin) == XUD_INVALID_ARG, "matrix/refuse-point-matrix",
        "and a matrix that is not there");

    /* Not-a-number inputs, one per argument the rung reads: a NaN that reached sinf or a multiply
       would spread through all sixteen slots and reach the vertex stage, where it is a triangle that
       exists nowhere and a scissor test that clips nothing.
    */
    xud_check(xud_matrix_translate(xudTestMatrixA, (XudVector3){ nan, 0.0f, 0.0f }) == XUD_INVALID_ARG, "matrix/refuse-translate-nan",
        "a NaN translation is refused");
    xud_check(xud_matrix_scale(xudTestMatrixA, (XudVector3){ 1.0f, infinite, 1.0f }) == XUD_INVALID_ARG, "matrix/refuse-scale-inf",
        "an infinite scale is refused");
    xud_check(xud_matrix_rotate(xudTestMatrixA, xudTestUnitZ, nan) == XUD_INVALID_ARG, "matrix/refuse-rotate-angle-nan",
        "a NaN angle is refused");
    xud_check(xud_matrix_rotate(xudTestMatrixA, xudTestUnitZ, infinite) == XUD_INVALID_ARG, "matrix/refuse-rotate-angle-inf",
        "and so is an infinite one, which has no remainder to take");
    xud_check(xud_matrix_rotate(xudTestMatrixA, (XudVector3){ 1.0f, nan, 0.0f }, 1.0f) == XUD_INVALID_ARG, "matrix/refuse-rotate-axis-nan",
        "a NaN axis component is refused");
    xud_check(xud_matrix_rotate_xyz(xudTestMatrixA, (XudVector3){ 0.0f, 0.0f, infinite }) == XUD_INVALID_ARG, "matrix/refuse-rotate-xyz-inf",
        "and so is an infinite euler angle");
    xud_check(xud_matrix_compose(xudTestMatrixA, (XudVector3){ 0.0f, 0.0f, nan }, xudTestOrigin, xudTestUnitX) == XUD_INVALID_ARG,
        "matrix/refuse-compose-nan", "the compose refuses whichever of its three vectors carries a NaN");

    /* An axis with no direction in it. The floor is on the SQUARED length, so an axis of one
       ten-millionth - long enough to divide by, too short to be a direction - is refused rather than
       normalised into a spin about rounding error.
    */
    xud_check(xud_matrix_rotate(xudTestMatrixA, xudTestOrigin, 1.0f) == XUD_INVALID_ARG, "matrix/refuse-rotate-zero-axis",
        "a zero axis has no direction to rotate about");
    xud_check(xud_matrix_rotate(xudTestMatrixA, (XudVector3){ 1e-7f, 0.0f, 0.0f }, 1.0f) == XUD_INVALID_ARG, "matrix/refuse-rotate-tiny-axis",
        "and so has one of one ten-millionth, under the squared floor");

    /* A matrix argument is read whole, so one poisoned slot poisons the call. */
    xud_check(xud_matrix_identity(xudTestMatrixA) == XUD_OK, "matrix/refuse-poison-setup", "an identity to poison");
    xudTestMatrixA[7] = nan;
    xud_check(xud_matrix_multiply(xudTestMatrixB, xudTestMatrixA, xudTestMatrixA) == XUD_INVALID_ARG, "matrix/refuse-multiply-nan-left",
        "the product refuses a factor with a NaN in it");
    xud_check(xud_matrix_multiply(xudTestMatrixB, xudTestMatrixB, xudTestMatrixA) == XUD_INVALID_ARG, "matrix/refuse-multiply-nan-right",
        "whichever side the NaN is on");
    xud_check(xud_matrix_transform_point(&xudTestPoint, xudTestMatrixA, xudTestUnitX) == XUD_INVALID_ARG, "matrix/refuse-point-nan-matrix",
        "and so does the point query");

    /* The zeros a refusal writes: the buffer is poisoned with a previous answer first, so the check
       fails if a refused call leaves the OLD transform in place for a caller that ignored the code.
    */
    xud_check(xud_matrix_translate(xudTestMatrixA, (XudVector3){ 9.0f, 9.0f, 9.0f }) == XUD_OK, "matrix/zero-setup", "a real transform first");
    xud_check((xud_matrix_translate(xudTestMatrixA, (XudVector3){ nan, 0.0f, 0.0f }) == XUD_INVALID_ARG)
                  && (xud_test_matrix_is_zero(xudTestMatrixA) == 1u),
        "matrix/refuse-writes-zeros", "a refused translate overwrites the old matrix with zeros");
    xud_check(xud_matrix_translate(xudTestMatrixA, (XudVector3){ 9.0f, 9.0f, 9.0f }) == XUD_OK, "matrix/zero-setup-again", "and again");
    xud_check((xud_matrix_rotate(xudTestMatrixA, xudTestOrigin, 1.0f) == XUD_INVALID_ARG) && (xud_test_matrix_is_zero(xudTestMatrixA) == 1u),
        "matrix/refuse-writes-zeros-rotate", "a refused rotation does the same");
    xud_check(xud_matrix_translate(xudTestMatrixA, (XudVector3){ 9.0f, 9.0f, 9.0f }) == XUD_OK, "matrix/zero-setup-thrice", "once more");
    xud_check(xud_matrix_identity(xudTestMatrixB) == XUD_OK, "matrix/zero-poison-setup", "and a second factor, poisoned");
    xudTestMatrixB[3] = nan;
    xud_check((xud_matrix_multiply(xudTestMatrixA, xudTestMatrixA, xudTestMatrixB) == XUD_INVALID_ARG)
                  && (xud_test_matrix_is_zero(xudTestMatrixA) == 1u),
        "matrix/refuse-writes-zeros-multiply", "and so does a refused product, even though its out was a factor");

    xud_check((xud_matrix_transform_point(&xudTestPoint, xudTestMatrixA, (XudVector3){ nan, 0.0f, 0.0f }) == XUD_INVALID_ARG)
                  && ((xudTestPoint.x == 0.0f) && (xudTestPoint.y == 0.0f) && (xudTestPoint.z == 0.0f)),
        "matrix/refuse-point-zeros", "and the refused point answers the origin rather than a NaN");
}

void xud_test_matrix(void) {
    xud_test_matrix_placement();
    xud_test_matrix_rotate();
    xud_test_matrix_euler();
    xud_test_matrix_product();
    xud_test_matrix_compose();
    xud_test_matrix_refusals();
}

