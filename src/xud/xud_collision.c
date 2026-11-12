/***********************************************************************************************
 *
 *   xud_collision.c - the collision rung: the solid-vs-solid tests and the two ray casts
 *
 *   Owns : the arithmetic behind the five calls - the validity every one of them opens with, the three
 *          yes/no tests, the per-axis slab interval and the two casts that combine three of them.
 *   Layer: the rung xud_collision.h states. Every function here is host arithmetic on the caller's
 *          floats: no context, no driver, no Vulkan or GLFW symbol the linker could see, so the whole
 *          of the rung's correctness is asserted in the headless default test run.
 *
 *   Where this restates raylib and where it does not. The sphere test is raylib's sqrt-free squared
 *   comparison (rmodels.c 4175) and the box-sphere test its nearest-point clamp (4201). Three places
 *   differ on purpose: the box test is inclusive on all three axes because raylib is inclusive only on
 *   x (4186) and so answers "no collision" for two boxes resting on one another; the sphere cast solves
 *   the quadratic with the direction's own squared length as its leading term rather than assuming the
 *   direction is unit (4220), which is the same arithmetic for the normalised ray the fold answers and
 *   still correct for one the caller scaled; and the box cast takes the face whose interval the three
 *   share as the hit and reads its normal off that axis, instead of dividing the slab bounds by a
 *   direction component that may be zero (4278, where the division answers infinity and a zero numerator
 *   on the slab plane answers NaN) and then recovering the face by truncating a scaled vector to an
 *   integer (4305).
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include "xud_collision.h" /* Required for: the five declarations this file answers, and the parallel floor */
#include <stddef.h> /* Required for: NULL, the out-parameter every call refuses a NULL of */

#include <float.h> /* Required for: FLT_MAX, the end of a slab a ray runs parallel to */
#include <math.h> /* Required for: isfinite/sqrtf/fabsf - host arithmetic on the caller's floats */

/*-----------------------------------------------------------------------------------------------
 * The validity every one of the five opens with
 *-----------------------------------------------------------------------------------------------*/

/* Finite rather than "not a NaN": an infinity has no position either, and the squared forms below turn
   one into an infinity that compares as though it were a distance. The double cast is the batch rung's
   idiom for the same question (xud_batch.c 655), taken rather than a second one invented here.
*/
static uint32_t xud_collision_vector_is_finite(XudVector3 vector) {
    return ((isfinite((double)vector.x) != 0) && (isfinite((double)vector.y) != 0) && (isfinite((double)vector.z) != 0)) ? 1u : 0u;
}

/* A radius has to be something a surface can be at: zero is a sphere with no surface to hit, whose
   normal would be a zero vector divided by zero, and a negative one is a subtraction the caller got
   backwards. Both are refusals rather than a false, because a caller that asks about a sphere it has not
   finished building has not asked a question the rung can answer.
*/
static uint32_t xud_collision_radius_is_positive(float radius) { return (radius > 0.0f) ? 1u : 0u; }

/* A box is its two corners with the least of every axis on one end and the most on the other; a minimum
   past the maximum is a caller that subtracted the half-size where it should have added it. A zero width
   on one axis - a plate - IS allowed: the interval is a moment rather than a span, and a sphere or a ray
   can still meet it.
*/
static uint32_t xud_collision_box_is_well_formed(XudBoundingBox box) {
    if (xud_collision_vector_is_finite(box.min) == 0u) {
        return 0u;
    }
    if (xud_collision_vector_is_finite(box.max) == 0u) {
        return 0u;
    }

    return ((box.min.x <= box.max.x) && (box.min.y <= box.max.y) && (box.min.z <= box.max.z)) ? 1u : 0u;
}

/* A ray needs an origin somewhere and a direction with one component long enough to divide by - the
   squared floor being the component floor times itself. Its LENGTH is not required to be one: a hit's
   distance is a multiple of the direction as carried, so a caller that hands over a scaled direction
   gets a distance in the same units and a point in world ones.
*/
static uint32_t xud_collision_ray_is_well_formed(XudRay ray) {
    float floorSquared = XUD_COLLISION_PARALLEL_COMPONENT * XUD_COLLISION_PARALLEL_COMPONENT;
    float lengthSquared = (ray.direction.x * ray.direction.x) + (ray.direction.y * ray.direction.y) + (ray.direction.z * ray.direction.z);

    if (xud_collision_vector_is_finite(ray.position) == 0u) {
        return 0u;
    }
    if (xud_collision_vector_is_finite(ray.direction) == 0u) {
        return 0u;
    }

    return (lengthSquared > floorSquared) ? 1u : 0u;
}

/* The one refusal the casts share: a miss is the record written all-zero, which is raylib's zeroed
   RayCollision and the reason its picking example's "click again to deselect" is one assignment.
*/
static void xud_collision_write_miss(XudRayCollision* outCollision) {
    outCollision->hit = false;
    outCollision->distance = 0.0f;
    outCollision->point = (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 0.0f };
    outCollision->normal = (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 0.0f };
}

/*-----------------------------------------------------------------------------------------------
 * The three tests - solid against solid
 *-----------------------------------------------------------------------------------------------*/

/// See xud.h.
XudResult xud_check_collision_spheres(bool* outHit, XudVector3 center1, float radius1, XudVector3 center2, float radius2) {
    float apartX = center1.x - center2.x;
    float apartY = center1.y - center2.y;
    float apartZ = center1.z - center2.z;
    float radiiSum = radius1 + radius2;

    if (outHit == NULL) {
        return XUD_INVALID_ARG;
    }
    if ((xud_collision_vector_is_finite(center1) == 0u) || (xud_collision_vector_is_finite(center2) == 0u)) {
        return XUD_INVALID_ARG;
    }
    if ((xud_collision_radius_is_positive(radius1) == 0u) || (xud_collision_radius_is_positive(radius2) == 0u)) {
        return XUD_INVALID_ARG;
    }

    /* Squared against squared, raylib's choice for the same reason (rmodels.c 4173): the comparison is the
       whole question and a square root buys nothing but error.
    */
    *outHit = ((apartX * apartX) + (apartY * apartY) + (apartZ * apartZ)) <= (radiiSum * radiiSum);

    return XUD_OK;
}

/// See xud.h.
XudResult xud_check_collision_boxes(bool* outHit, XudBoundingBox box1, XudBoundingBox box2) {
    if (outHit == NULL) {
        return XUD_INVALID_ARG;
    }
    if ((xud_collision_box_is_well_formed(box1) == 0u) || (xud_collision_box_is_well_formed(box2) == 0u)) {
        return XUD_INVALID_ARG;
    }

    /* Overlap on every axis, each axis inclusive: the nearest of one box's corners is not past the farthest
       of the other's. Touching counts, on all three axes - see the banner for why that is one place this
       is deliberately not raylib.
    */
    *outHit = ((box1.min.x <= box2.max.x) && (box1.max.x >= box2.min.x) && (box1.min.y <= box2.max.y) && (box1.max.y >= box2.min.y)
        && (box1.min.z <= box2.max.z) && (box1.max.z >= box2.min.z));

    return XUD_OK;
}

/* The corner-to-corner clamp, written as two comparisons rather than as raylib's Clamp() helper: the
   value is already known finite, so the order of the two tests cannot be changed by a NaN.
*/
static float xud_collision_clamp(float value, float low, float high) {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }

    return value;
}

/// See xud.h.
XudResult xud_check_collision_box_sphere(bool* outHit, XudBoundingBox box, XudVector3 center, float radius) {
    float nearX = xud_collision_clamp(center.x, box.min.x, box.max.x);
    float nearY = xud_collision_clamp(center.y, box.min.y, box.max.y);
    float nearZ = xud_collision_clamp(center.z, box.min.z, box.max.z);
    float apartX = center.x - nearX;
    float apartY = center.y - nearY;
    float apartZ = center.z - nearZ;

    if (outHit == NULL) {
        return XUD_INVALID_ARG;
    }
    if (xud_collision_box_is_well_formed(box) == 0u) {
        return XUD_INVALID_ARG;
    }
    if (xud_collision_vector_is_finite(center) == 0u) {
        return XUD_INVALID_ARG;
    }
    if (xud_collision_radius_is_positive(radius) == 0u) {
        return XUD_INVALID_ARG;
    }

    /* The box's point nearest the centre is the one the sphere reaches first: when the centre is inside
       the box the three clamps answer the centre itself, the distance is zero, and the answer is a hit.
    */
    *outHit = ((apartX * apartX) + (apartY * apartY) + (apartZ * apartZ)) <= (radius * radius);

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The two casts - ray against solid
 *-----------------------------------------------------------------------------------------------*/

/* One component of the centre-to-hit vector, read as the axis the slab test landed on. */
static float xud_collision_axis_value(XudVector3 vector, uint32_t axis) {
    if (axis == 0u) {
        return vector.x;
    }
    if (axis == 1u) {
        return vector.y;
    }

    return vector.z;
}

/* One component written, the other two left at the value handed in: the normal of a box hit is exactly
   one axis long, so the cast builds it by asking which axis and writing that one.
*/
static XudVector3 xud_collision_axis_vector(uint32_t axis, float value) {
    XudVector3 out = { .x = 0.0f, .y = 0.0f, .z = 0.0f };

    if (axis == 0u) {
        out.x = value;
    } else if (axis == 1u) {
        out.y = value;
    } else {
        out.z = value;
    }

    return out;
}

/// See xud.h.
XudResult xud_get_ray_collision_sphere(XudRayCollision* outCollision, XudRay ray, XudVector3 center, float radius) {
    float toCenterX = center.x - ray.position.x;
    float toCenterY = center.y - ray.position.y;
    float toCenterZ = center.z - ray.position.z;
    float along = (toCenterX * ray.direction.x) + (toCenterY * ray.direction.y) + (toCenterZ * ray.direction.z);
    float centreDistanceSquared = (toCenterX * toCenterX) + (toCenterY * toCenterY) + (toCenterZ * toCenterZ);
    float radiusSquared = radius * radius;
    float directionSquared
        = (ray.direction.x * ray.direction.x) + (ray.direction.y * ray.direction.y) + (ray.direction.z * ray.direction.z);
    float discriminant = (along * along) - (directionSquared * (centreDistanceSquared - radiusSquared));
    float halfChord;
    float distance;
    uint32_t inside;

    if (outCollision == NULL) {
        return XUD_INVALID_ARG;
    }
    if (xud_collision_ray_is_well_formed(ray) == 0u) {
        return XUD_INVALID_ARG;
    }
    if (xud_collision_vector_is_finite(center) == 0u) {
        return XUD_INVALID_ARG;
    }
    if (xud_collision_radius_is_positive(radius) == 0u) {
        return XUD_INVALID_ARG;
    }

    /* The line misses the sphere when the squared half-chord goes negative: nothing is on the line at the
       sphere's surface, and the cast says so without a square root of a negative.
    */
    if (discriminant < 0.0f) {
        xud_collision_write_miss(outCollision);
        return XUD_OK;
    }

    /* Divided by the direction's squared length, which is the quadratic's leading term and one for the
       normalised ray the fold answers - raylib's form (rmodels.c 4232) drops that divisor and is right only
       while the direction is unit. The well-formedness test above is what keeps it off zero.
    */
    halfChord = sqrtf(discriminant) / directionSquared;
    inside = (centreDistanceSquared < radiusSquared) ? 1u : 0u;
    distance = (inside != 0u) ? ((along / directionSquared) + halfChord) : ((along / directionSquared) - halfChord);

    /* Both roots negative is a sphere behind the origin: the line meets it, the ray does not. */
    if (distance < 0.0f) {
        xud_collision_write_miss(outCollision);
        return XUD_OK;
    }

    outCollision->hit = true;
    outCollision->distance = distance;
    outCollision->point = (XudVector3) { .x = ray.position.x + (ray.direction.x * distance),
        .y = ray.position.y + (ray.direction.y * distance),
        .z = ray.position.z + (ray.direction.z * distance) };

    /* The surface's own direction, which is (point - centre) over the radius: the point is ON the sphere, so
       the quotient is unit length by construction and needs no normalising of its own. An origin inside
       the sphere answers the far surface, and the outward normal there points back the way the ray came.
    */
    outCollision->normal = (XudVector3) { .x = (outCollision->point.x - center.x) / radius,
        .y = (outCollision->point.y - center.y) / radius,
        .z = (outCollision->point.z - center.z) / radius };
    if (inside != 0u) {
        outCollision->normal.x = -outCollision->normal.x;
        outCollision->normal.y = -outCollision->normal.y;
        outCollision->normal.z = -outCollision->normal.z;
    }

    return XUD_OK;
}

/* Where the ray is inside the slab between the two planes, on one axis: the interval of t it spends there,
   or the whole line when it runs parallel to the planes and between them. A ray parallel to the planes and
   OUTSIDE them is the one answer an interval cannot carry, and it answers false - the axis never yields, so
   the cast reports a miss without combining the three.
*/
static uint32_t xud_collision_axis_interval(float origin, float direction, float slabMin, float slabMax, float* outEnter, float* outExit) {
    float enter;
    float exit;

    if (fabsf(direction) <= XUD_COLLISION_PARALLEL_COMPONENT) {
        if ((origin < slabMin) || (origin > slabMax)) {
            return 0u;
        }

        *outEnter = -FLT_MAX;
        *outExit = FLT_MAX;

        return 1u;
    }

    enter = (slabMin - origin) / direction;
    exit = (slabMax - origin) / direction;
    *outEnter = (enter < exit) ? enter : exit;
    *outExit = (enter < exit) ? exit : enter;

    return 1u;
}

/// See xud.h.
XudResult xud_get_ray_collision_box(XudRayCollision* outCollision, XudRay ray, XudBoundingBox box) {
    float enter[3];
    float exit[3];
    float latestEnter;
    float earliestExit;
    uint32_t axis;
    uint32_t entryAxis = 0u;
    uint32_t exitAxis = 0u;
    float distance;
    float faceSign;
    uint32_t inside;

    if (outCollision == NULL) {
        return XUD_INVALID_ARG;
    }
    if (xud_collision_ray_is_well_formed(ray) == 0u) {
        return XUD_INVALID_ARG;
    }
    if (xud_collision_box_is_well_formed(box) == 0u) {
        return XUD_INVALID_ARG;
    }

    if ((xud_collision_axis_interval(ray.position.x, ray.direction.x, box.min.x, box.max.x, &enter[0], &exit[0]) == 0u)
        || (xud_collision_axis_interval(ray.position.y, ray.direction.y, box.min.y, box.max.y, &enter[1], &exit[1]) == 0u)
        || (xud_collision_axis_interval(ray.position.z, ray.direction.z, box.min.z, box.max.z, &enter[2], &exit[2]) == 0u)) {
        xud_collision_write_miss(outCollision);
        return XUD_OK;
    }

    /* The three axes' intervals share the moment the ray is inside all three: the latest entry against the
       earliest exit. The first axis wins a tie, which is the same answer an edge or a corner hit has to
       settle for and the reason the normal is stated as an axis rather than as a blend of two.
    */
    latestEnter = enter[0];
    earliestExit = exit[0];
    for (axis = 1u; axis < 3u; ++axis) {
        if (enter[axis] > latestEnter) {
            latestEnter = enter[axis];
            entryAxis = axis;
        }
        if (exit[axis] < earliestExit) {
            earliestExit = exit[axis];
            exitAxis = axis;
        }
    }

    /* Everything behind the origin, or three intervals that share no moment: a miss either way. */
    if ((earliestExit < 0.0f) || (latestEnter > earliestExit)) {
        xud_collision_write_miss(outCollision);
        return XUD_OK;
    }

    /* Every axis entered before any exits and the exit still ahead of it: the origin is inside the box, so
       the surface the ray meets is the one it LEAVES by - the positive distance raylib arrives at by tracing
       a negated ray and negating the answer back (rmodels.c 4274, 4316).
    */
    inside = ((latestEnter < 0.0f) && (earliestExit > 0.0f)) ? 1u : 0u;
    distance = (inside != 0u) ? earliestExit : latestEnter;

    /* Travelling the positive sense of an axis means entering through its least face and leaving through
       its most, so the outward normal is the negative of the travel for an entry and the travel itself for
       an exit. The axis is never one the ray runs parallel to: a parallel axis carries minus and plus the
       whole line, so it can neither be the latest entry nor the earliest exit.
    */
    faceSign = (xud_collision_axis_value(ray.direction, (inside != 0u) ? exitAxis : entryAxis) > 0.0f) ? 1.0f : -1.0f;

    outCollision->hit = true;
    outCollision->distance = distance;
    outCollision->point = (XudVector3) { .x = ray.position.x + (ray.direction.x * distance),
        .y = ray.position.y + (ray.direction.y * distance),
        .z = ray.position.z + (ray.direction.z * distance) };
    outCollision->normal = xud_collision_axis_vector((inside != 0u) ? exitAxis : entryAxis, (inside != 0u) ? faceSign : -faceSign);

    return XUD_OK;
}
