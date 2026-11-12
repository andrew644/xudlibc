/***********************************************************************************************
 *
 *   xud_matrix.c - the matrix rung: eight builders over sixteen floats the caller owns
 *
 *   Owns : the finiteness guards every builder runs before it writes, the normalisation
 *          xud_matrix_rotate() needs before Rodrigues has a direction to spin about, and the two
 *          closed forms - the axis-angle's and the XYZ euler's - the rest of the rung is built from.
 *          See xud_matrix.h for the layer, the layout, and the multiplication order.
 *   Layer: pure arithmetic. Sixteen floats in, sixteen floats out, nothing read from a context and
 *          nothing kept: a builder that refuses writes all sixteen zeros so a caller that ignored
 *          the code still cannot draw a half-built transform.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud_matrix.h" /* Required for: the axis floor, and the eight declarations this file answers */

#include <math.h> /* Required for: sinf/cosf/sqrtf/isfinite - the closed forms and the guards */
#include <stddef.h> /* Required for: NULL, the pointer every guard reads */
#include <stdint.h> /* Required for: uint32_t, the loop bounds and the guard counters */

/*-----------------------------------------------------------------------------------------------
 * The guards, internal to the rung
 *-----------------------------------------------------------------------------------------------*/

static uint32_t xud_matrix_vector_is_finite(XudVector3 vector) {
    return ((isfinite((double)vector.x) != 0) && (isfinite((double)vector.y) != 0) && (isfinite((double)vector.z) != 0)) ? 1u : 0u;
}

static uint32_t xud_matrix_is_finite(const float* matrix) {
    uint32_t axis = 0u;

    for (axis = 0u; axis < XUD_MATRIX_ELEMENT_COUNT; ++axis) {
        if (isfinite((double)matrix[axis]) == 0) {
            return 0u;
        }
    }

    return 1u;
}

/* The one answer every refusal gives: the zeros are not decoration, they are what makes a caller
   that printed the code and carried on draw nothing rather than draw whatever the buffer held.
*/
static void xud_matrix_fill_zero(float* outMatrix) {
    uint32_t element = 0u;

    for (element = 0u; element < XUD_MATRIX_ELEMENT_COUNT; ++element) {
        outMatrix[element] = 0.0f;
    }
}

static void xud_matrix_copy(float* outMatrix, const float* matrix) {
    uint32_t element = 0u;

    for (element = 0u; element < XUD_MATRIX_ELEMENT_COUNT; ++element) {
        outMatrix[element] = matrix[element];
    }
}

/*-----------------------------------------------------------------------------------------------
 * The two closed forms, internal to the rung
 *-----------------------------------------------------------------------------------------------*/

/* Rodrigues, written straight into the columns: the element at column c, row r is out[c*4 + r].
   The axis is a unit vector by the time this is reached - xud_matrix_rotate() normalises and
   refuses before it calls - so the three squares and three products below are direction cosines.
*/
static void xud_matrix_fill_rotation_axis(float* outMatrix, XudVector3 axis, float radians) {
    const float cosine = cosf(radians);
    const float sine = sinf(radians);
    const float tail = 1.0f - cosine;
    const float x = axis.x;
    const float y = axis.y;
    const float z = axis.z;
    const float xy = x * y * tail;
    const float xz = x * z * tail;
    const float yz = y * z * tail;
    const float xs = x * sine;
    const float ys = y * sine;
    const float zs = z * sine;

    outMatrix[0] = (x * x * tail) + cosine;
    outMatrix[1] = xy + zs;
    outMatrix[2] = xz - ys;
    outMatrix[3] = 0.0f;

    outMatrix[4] = xy - zs;
    outMatrix[5] = (y * y * tail) + cosine;
    outMatrix[6] = yz + xs;
    outMatrix[7] = 0.0f;

    outMatrix[8] = xz + ys;
    outMatrix[9] = yz - xs;
    outMatrix[10] = (z * z * tail) + cosine;
    outMatrix[11] = 0.0f;

    outMatrix[12] = 0.0f;
    outMatrix[13] = 0.0f;
    outMatrix[14] = 0.0f;
    outMatrix[15] = 1.0f;
}

/* Rx * Ry * Rz, the same rotation raymath's MatrixRotateXYZ (raymath.h 1897) builds for a row
   vector from Rz(-x) * Ry(-y) * Rx(-z): negating every angle and transposing the product is the
   same transform read as columns, so the two agree to the rounding and a caller may bring the
   angles straight over. Written into the rotation's own nine slots; the fourth column and row are
   left for the caller to fill, which is what lets xud_matrix_compose() scale the columns inside it.
*/
static void xud_matrix_fill_rotation_xyz(float* outMatrix, XudVector3 radians) {
    const float cosX = cosf(radians.x);
    const float sinX = sinf(radians.x);
    const float cosY = cosf(radians.y);
    const float sinY = sinf(radians.y);
    const float cosZ = cosf(radians.z);
    const float sinZ = sinf(radians.z);

    outMatrix[0] = cosY * cosZ;
    outMatrix[1] = (sinX * sinY * cosZ) + (cosX * sinZ);
    outMatrix[2] = (-cosX * sinY * cosZ) + (sinX * sinZ);

    outMatrix[4] = -cosY * sinZ;
    outMatrix[5] = (cosX * cosZ) - (sinX * sinY * sinZ);
    outMatrix[6] = (sinX * cosZ) + (cosX * sinY * sinZ);

    outMatrix[8] = sinY;
    outMatrix[9] = -sinX * cosY;
    outMatrix[10] = cosX * cosY;
}

/*-----------------------------------------------------------------------------------------------
 * The builders
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_matrix_identity(float* outMatrix) {
    if (outMatrix == NULL) {
        return XUD_INVALID_ARG;
    }

    xud_matrix_fill_zero(outMatrix);

    outMatrix[0] = 1.0f;
    outMatrix[5] = 1.0f;
    outMatrix[10] = 1.0f;
    outMatrix[15] = 1.0f;

    return XUD_OK;
}

XudResult xud_matrix_translate(float* outMatrix, XudVector3 translation) {
    if (outMatrix == NULL) {
        return XUD_INVALID_ARG;
    }

    if (xud_matrix_vector_is_finite(translation) == 0u) {
        xud_matrix_fill_zero(outMatrix);
        return XUD_INVALID_ARG;
    }

    xud_matrix_identity(outMatrix);

    outMatrix[12] = translation.x;
    outMatrix[13] = translation.y;
    outMatrix[14] = translation.z;

    return XUD_OK;
}

XudResult xud_matrix_scale(float* outMatrix, XudVector3 scale) {
    if (outMatrix == NULL) {
        return XUD_INVALID_ARG;
    }

    if (xud_matrix_vector_is_finite(scale) == 0u) {
        xud_matrix_fill_zero(outMatrix);
        return XUD_INVALID_ARG;
    }

    xud_matrix_fill_zero(outMatrix);

    outMatrix[0] = scale.x;
    outMatrix[5] = scale.y;
    outMatrix[10] = scale.z;
    outMatrix[15] = 1.0f;

    return XUD_OK;
}

XudResult xud_matrix_rotate(float* outMatrix, XudVector3 axis, float radians) {
    float length = 0.0f;

    if (outMatrix == NULL) {
        return XUD_INVALID_ARG;
    }

    if ((xud_matrix_vector_is_finite(axis) == 0u) || (isfinite((double)radians) == 0)) {
        xud_matrix_fill_zero(outMatrix);
        return XUD_INVALID_ARG;
    }

    length = sqrtf((axis.x * axis.x) + (axis.y * axis.y) + (axis.z * axis.z));

    if ((length * length) < XUD_MATRIX_AXIS_MIN_LENGTH_SQUARED) {
        xud_matrix_fill_zero(outMatrix);
        return XUD_INVALID_ARG;
    }

    /* The axis is scaled rather than divided component by component so that a caller who handed
       over (0, 0, 2) gets the rotation about +z they meant, the way raymath's MatrixRotate
       normalises internally (raymath.h 1792's Vector3Normalize) instead of refusing.
    */
    xud_matrix_fill_rotation_axis(outMatrix, (XudVector3){ axis.x / length, axis.y / length, axis.z / length }, radians);

    return XUD_OK;
}

XudResult xud_matrix_rotate_xyz(float* outMatrix, XudVector3 radians) {
    if (outMatrix == NULL) {
        return XUD_INVALID_ARG;
    }

    if (xud_matrix_vector_is_finite(radians) == 0u) {
        xud_matrix_fill_zero(outMatrix);
        return XUD_INVALID_ARG;
    }

    xud_matrix_fill_zero(outMatrix);
    xud_matrix_fill_rotation_xyz(outMatrix, radians);

    outMatrix[15] = 1.0f;

    return XUD_OK;
}


XudResult xud_matrix_multiply(float* outMatrix, const float* left, const float* right) {
    float product[XUD_MATRIX_ELEMENT_COUNT];
    uint32_t row = 0u;
    uint32_t column = 0u;

    if ((outMatrix == NULL) || (left == NULL) || (right == NULL)) {
        return XUD_INVALID_ARG;
    }

    if ((xud_matrix_is_finite(left) == 0u) || (xud_matrix_is_finite(right) == 0u)) {
        xud_matrix_fill_zero(outMatrix);
        return XUD_INVALID_ARG;
    }

    /* The product lands in a local and is copied out only once it is whole, which is what lets
       outMatrix be left or right: a caller composing a transform in place - the model.transform
       update raylib's own examples do - does not overwrite a factor mid-sum.
    */
    for (column = 0u; column < 4u; ++column) {
        for (row = 0u; row < 4u; ++row) {
            product[(column * 4u) + row] = (left[row] * right[(column * 4u)]) + (left[4u + row] * right[(column * 4u) + 1u])
                + (left[8u + row] * right[(column * 4u) + 2u]) + (left[12u + row] * right[(column * 4u) + 3u]);
        }
    }

    xud_matrix_copy(outMatrix, product);

    return XUD_OK;
}

XudResult xud_matrix_compose(float* outMatrix, XudVector3 translation, XudVector3 radians, XudVector3 scale) {
    float rotation[XUD_MATRIX_ELEMENT_COUNT];

    if (outMatrix == NULL) {
        return XUD_INVALID_ARG;
    }

    if ((xud_matrix_vector_is_finite(translation) == 0u) || (xud_matrix_vector_is_finite(radians) == 0u)
        || (xud_matrix_vector_is_finite(scale) == 0u)) {
        xud_matrix_fill_zero(outMatrix);
        return XUD_INVALID_ARG;
    }

    xud_matrix_fill_zero(rotation);
    xud_matrix_fill_rotation_xyz(rotation, radians);

    /* T * R * S by hand: a scale is a rotation's columns multiplied by its three numbers, and a
       translation is its fourth column, so the compose costs no third matrix and no multiply - and
       the order is the one the rung's note states, the scale meeting the vertex first.
    */
    outMatrix[0] = rotation[0] * scale.x;
    outMatrix[1] = rotation[1] * scale.x;
    outMatrix[2] = rotation[2] * scale.x;
    outMatrix[3] = 0.0f;

    outMatrix[4] = rotation[4] * scale.y;
    outMatrix[5] = rotation[5] * scale.y;
    outMatrix[6] = rotation[6] * scale.y;
    outMatrix[7] = 0.0f;

    outMatrix[8] = rotation[8] * scale.z;
    outMatrix[9] = rotation[9] * scale.z;
    outMatrix[10] = rotation[10] * scale.z;
    outMatrix[11] = 0.0f;

    outMatrix[12] = translation.x;
    outMatrix[13] = translation.y;
    outMatrix[14] = translation.z;
    outMatrix[15] = 1.0f;

    return XUD_OK;
}

XudResult xud_matrix_transform_point(XudVector3* outPoint, const float* matrix, XudVector3 point) {
    if ((outPoint == NULL) || (matrix == NULL)) {
        return XUD_INVALID_ARG;
    }

    if ((xud_matrix_is_finite(matrix) == 0u) || (xud_matrix_vector_is_finite(point) == 0u)) {
        *outPoint = (XudVector3){ 0.0f, 0.0f, 0.0f };
        return XUD_INVALID_ARG;
    }

    /* The affine half only: the fourth row is never read, which is raymath's Vector3Transform
       (raymath.h 899-901) too, and the reason this call is a placement and not a projection - a
       view-projection run through here answers the clip position without the divide, which is a
       number with no world meaning. xud_get_world_to_screen() is that other question.
    */
    *outPoint = (XudVector3){ (matrix[0] * point.x) + (matrix[4] * point.y) + (matrix[8] * point.z) + matrix[12],
        (matrix[1] * point.x) + (matrix[5] * point.y) + (matrix[9] * point.z) + matrix[13],
        (matrix[2] * point.x) + (matrix[6] * point.y) + (matrix[10] * point.z) + matrix[14] };

    return XUD_OK;
}

