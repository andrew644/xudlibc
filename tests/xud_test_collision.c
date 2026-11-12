/***********************************************************************************************
 *
 *   xud_test_collision.c - the collision rung's five calls
 *
 *   Owns : the tests of xud_check_collision_spheres(), xud_check_collision_boxes(),
 *          xud_check_collision_box_sphere(), xud_get_ray_collision_sphere() and
 *          xud_get_ray_collision_box(), and nothing else: the ray the two casts are handed is the
 *          camera area's to test, and where a hit lands on the frame is the fold's.
 *   Layer: tests, above the camera area and below the core, which is the layer order: the casts read
 *          the same XudRay the camera's fold fills, so the area runs after the one that produces it.
 *          There is no GPU half, which is the point of the rung rather than an omission: none of the
 *          five reads a context, so every fact about them - which pairs touch, how far along a ray the
 *          first surface is, which way that surface faces - is answerable with no driver present.
 *
 *   What the expected values are. Every one is a number the area states in the check's own words -
 *   a sphere of radius 1 at the origin seen from five units away is hit at four, and the surface it
 *   was hit at is (-1, 0, 0) with the normal pointing back down the same axis - never a value read
 *   back out of the implementation. The boundary cases are where the rung's promises live, so most
 *   of the checks sit on one: surfaces exactly touching, a ray exactly tangent, an origin exactly on
 *   a face, the two faces of a box at one and the same moment. The comparisons are exact wherever
 *   nothing but additions and one division are crossed, and against the area's tolerance wherever a
 *   square root is.
 *
 *   Deterministic: fixed solids, fixed rays, no time, no randomness, no filesystem.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include <math.h> /* Required for: NAN, INFINITY, the area's own sqrt for a diagonal's unit direction */
#include <stddef.h> /* Required for: NULL, the out-parameter the refusal checks hand over */

#include "xud.h" /* Required for: XudResult, XudVector3, XudRay, XudBoundingBox, XudRayCollision, the five calls */
#include "xud_collision.h" /* Required for: XUD_COLLISION_PARALLEL_COMPONENT, the floor a near-parallel ray is built under */
#include "xud_test_harness.h"

#define XUD_TEST_COLLISION_TOLERANCE                                                                                                       \
    1e-4f /* The reach of the square roots: every exact-looking value that crosses one is read against this */

static int xudTestNear(float actual, float expected) {
    float gap = actual - expected;

    if (gap < 0.0f) {
        gap = -gap;
    }

    return (gap <= XUD_TEST_COLLISION_TOLERANCE) ? 1 : 0;
}

static int xudTestVectorNear(XudVector3 actual, float x, float y, float z) {
    return (xudTestNear(actual.x, x) && xudTestNear(actual.y, y) && xudTestNear(actual.z, z)) ? 1 : 0;
}

/* A box from the centre and the full size every cube-shaped draw call takes, because that is how the
   two examples this rung is built for write one - and because a test that spelled the corners out by
   hand would be one subtraction error away from testing a different box than it names.
*/
static XudBoundingBox xudTestBox(XudVector3 center, float width, float height, float length) {
    XudBoundingBox box;

    box.min = (XudVector3) { .x = center.x - (width * 0.5f), .y = center.y - (height * 0.5f), .z = center.z - (length * 0.5f) };
    box.max = (XudVector3) { .x = center.x + (width * 0.5f), .y = center.y + (height * 0.5f), .z = center.z + (length * 0.5f) };

    return box;
}

static XudRay xudTestRay(XudVector3 position, XudVector3 direction) {
    XudRay ray;

    ray.position = position;
    ray.direction = direction;

    return ray;
}

/* Every refusal leaves the answer alone: a caller that reads its own record after a refused call has to
   find what it left there, not a half-written hit. The pre-set values are ones no honest answer writes.
*/
static void xud_test_collision_refusals(void) {
    bool hit = true;
    XudRayCollision record;
    XudVector3 center = { .x = 0.0f, .y = 0.0f, .z = 0.0f };
    XudVector3 apart = { .x = 9.0f, .y = 0.0f, .z = 0.0f };
    XudVector3 swappedMin = { .x = 1.0f, .y = 1.0f, .z = 1.0f };
    XudVector3 swappedMax = { .x = -1.0f, .y = -1.0f, .z = -1.0f };
    XudBoundingBox unit = xudTestBox(center, 2.0f, 2.0f, 2.0f);
    XudBoundingBox swapped = { .min = swappedMin, .max = swappedMax };
    XudVector3 nan = { .x = NAN, .y = 0.0f, .z = 0.0f };
    XudVector3 infinite = { .x = 0.0f, .y = INFINITY, .z = 0.0f };
    XudVector3 tiny = { .x = XUD_COLLISION_PARALLEL_COMPONENT, .y = 0.0f, .z = 0.0f };
    XudVector3 zero = { .x = 0.0f, .y = 0.0f, .z = 0.0f };
    XudRay unitRay = xudTestRay(center, (XudVector3) { .x = 0.0f, .y = 0.0f, .z = -1.0f });
    XudRay badOrigin = xudTestRay(nan, (XudVector3) { .x = 0.0f, .y = 0.0f, .z = -1.0f });
    XudRay zeroDirection = xudTestRay(center, zero);
    XudRay tinyDirection = xudTestRay(center, tiny);
    XudRay nanDirection = xudTestRay(center, nan);

    xud_check(xud_check_collision_spheres(NULL, center, 1.0f, apart, 1.0f) == XUD_INVALID_ARG, "collision/null-hit-spheres",
        "the sphere test refuses a NULL answer rather than reporting a collision nobody can read");
    xud_check(
        xud_check_collision_boxes(NULL, unit, unit) == XUD_INVALID_ARG, "collision/null-hit-boxes", "the box test refuses a NULL answer");
    xud_check(xud_check_collision_box_sphere(NULL, unit, center, 1.0f) == XUD_INVALID_ARG, "collision/null-hit-box-sphere",
        "the box-sphere test refuses a NULL answer");
    xud_check(xud_get_ray_collision_sphere(NULL, unitRay, center, 1.0f) == XUD_INVALID_ARG, "collision/null-hit-ray-sphere",
        "the sphere cast refuses a NULL record");
    xud_check(xud_get_ray_collision_box(NULL, unitRay, unit) == XUD_INVALID_ARG, "collision/null-hit-ray-box",
        "the box cast refuses a NULL record");

    hit = true;
    xud_check(xud_check_collision_spheres(&hit, nan, 1.0f, apart, 1.0f) == XUD_INVALID_ARG, "collision/nan-centre",
        "a centre that is not a place is refused rather than answered false");
    xud_check(hit == true, "collision/refusal-leaves-answer-spheres", "a refused sphere test leaves the caller's answer alone");
    xud_check(xud_check_collision_spheres(&hit, center, 1.0f, infinite, 1.0f) == XUD_INVALID_ARG, "collision/infinite-centre",
        "an infinity is not a distance, and the squared comparison would treat it as one");
    xud_check(xud_check_collision_spheres(&hit, center, 0.0f, apart, 1.0f) == XUD_INVALID_ARG, "collision/zero-radius",
        "a sphere with no radius has no surface to be at");
    xud_check(xud_check_collision_spheres(&hit, center, -1.0f, apart, 1.0f) == XUD_INVALID_ARG, "collision/negative-radius",
        "a negative radius is a subtraction the caller got backwards");
    xud_check(xud_check_collision_spheres(&hit, center, NAN, apart, 1.0f) == XUD_INVALID_ARG, "collision/nan-radius",
        "a NaN radius fails the same positive test rather than answering false");

    hit = false;
    xud_check(xud_check_collision_boxes(&hit, swapped, unit) == XUD_INVALID_ARG, "collision/swapped-box",
        "a box whose minimum is past its maximum is refused, not read as an empty interval");
    xud_check(hit == false, "collision/refusal-leaves-answer-boxes", "a refused box test leaves the caller's answer alone");
    xud_check(xud_check_collision_box_sphere(&hit, unit, center, 0.0f) == XUD_INVALID_ARG, "collision/box-sphere-zero-radius",
        "the box-sphere test refuses a radius with no surface");
    xud_check(xud_check_collision_box_sphere(&hit, swapped, center, 1.0f) == XUD_INVALID_ARG, "collision/box-sphere-swapped-box",
        "and a box that is not a box");

    record.hit = true;
    record.distance = 1234.5f;
    record.point = (XudVector3) { .x = 7.0f, .y = 7.0f, .z = 7.0f };
    record.normal = (XudVector3) { .x = -3.0f, .y = -3.0f, .z = -3.0f };
    xud_check(xud_get_ray_collision_sphere(&record, zeroDirection, center, 1.0f) == XUD_INVALID_ARG, "collision/ray-zero-direction",
        "a ray with no direction has nothing to trace along");
    xud_check(xud_get_ray_collision_sphere(&record, tinyDirection, center, 1.0f) == XUD_INVALID_ARG, "collision/ray-tiny-direction",
        "a direction whose every component is at the parallel floor is the same nothing");
    xud_check(xud_get_ray_collision_sphere(&record, nanDirection, center, 1.0f) == XUD_INVALID_ARG, "collision/ray-nan-direction",
        "and one that is not a direction at all");
    xud_check(xud_get_ray_collision_sphere(&record, badOrigin, center, 1.0f) == XUD_INVALID_ARG, "collision/ray-nan-origin",
        "an origin that is not a place is refused");
    xud_check((record.hit == true) && (record.distance == 1234.5f) && xudTestVectorNear(record.point, 7.0f, 7.0f, 7.0f)
            && xudTestVectorNear(record.normal, -3.0f, -3.0f, -3.0f),
        "collision/refusal-leaves-record", "a refused cast leaves the record the caller filled untouched");
    xud_check(xud_get_ray_collision_box(&record, unitRay, swapped) == XUD_INVALID_ARG, "collision/ray-box-swapped",
        "the box cast refuses the same swapped box");
    xud_check(xud_get_ray_collision_box(&record, tinyDirection, unit) == XUD_INVALID_ARG, "collision/ray-box-tiny-direction",
        "and the same direction with nothing in it");
}

/* The three yes/no tests, and the boundaries between them: touching is the interesting case, because a
   rung that measures with squares has to decide what "exactly far enough" means, and because two boxes
   resting on one another is the shape a game is built out of.
*/
static void xud_test_collision_solids(void) {
    bool hit = false;
    XudVector3 centreA = { .x = 0.0f, .y = 0.0f, .z = 0.0f };
    XudVector3 centreB = { .x = 5.0f, .y = 0.0f, .z = 0.0f };
    XudVector3 diagonal = { .x = 3.0f, .y = 4.0f, .z = 0.0f };
    XudVector3 near = { .x = 0.0f, .y = 0.25f, .z = 0.0f };
    XudVector3 side = { .x = 5.0f, .y = 0.0f, .z = 0.0f };
    XudVector3 face = { .x = 2.0f, .y = 0.0f, .z = 0.0f };
    XudVector3 corner = { .x = 1.6f, .y = 1.6f, .z = 0.0f };
    XudVector3 above = { .x = 0.0f, .y = 0.5f, .z = 0.0f };
    XudVector3 higher = { .x = 0.0f, .y = 0.6f, .z = 0.0f };
    XudBoundingBox unit = xudTestBox(centreA, 2.0f, 2.0f, 2.0f);
    XudBoundingBox beside = xudTestBox(side, 2.0f, 2.0f, 2.0f);
    XudBoundingBox overlapping = xudTestBox((XudVector3) { .x = 1.0f, .y = 0.0f, .z = 0.0f }, 2.0f, 2.0f, 2.0f);
    XudBoundingBox touching = xudTestBox((XudVector3) { .x = 2.0f, .y = 0.0f, .z = 0.0f }, 2.0f, 2.0f, 2.0f);
    XudBoundingBox stacked = xudTestBox((XudVector3) { .x = 0.0f, .y = 2.0f, .z = 0.0f }, 2.0f, 2.0f, 2.0f);
    XudBoundingBox inside = xudTestBox(centreA, 1.0f, 1.0f, 1.0f);
    XudBoundingBox apart = xudTestBox((XudVector3) { .x = 0.0f, .y = 0.0f, .z = 9.0f }, 2.0f, 2.0f, 2.0f);
    XudBoundingBox plate = xudTestBox(centreA, 2.0f, 0.0f, 2.0f);
    XudBoundingBox point = xudTestBox(centreA, 0.0f, 0.0f, 0.0f);

    /* Two spheres. */
    xud_check(xud_check_collision_spheres(&hit, centreA, 1.0f, centreB, 2.0f) == XUD_OK, "collision/spheres-call",
        "the sphere test answers OK for a well-formed pair");
    xud_check(hit == false, "collision/spheres-apart", "centres five apart with radii one and two leave a gap of two");
    xud_check((xud_check_collision_spheres(&hit, centreA, 2.0f, (XudVector3) { .x = 4.0f, .y = 0.0f, .z = 0.0f }, 3.0f) == XUD_OK)
            && (hit == true),
        "collision/spheres-overlap", "centres four apart with radii two and three overlap by one");
    xud_check((xud_check_collision_spheres(&hit, centreA, 2.0f, centreB, 3.0f) == XUD_OK) && (hit == true), "collision/spheres-touching",
        "centres five apart with radii two and three touch, and touching is a collision");
    xud_check((xud_check_collision_spheres(&hit, centreA, 2.0f, (XudVector3) { .x = 5.001f, .y = 0.0f, .z = 0.0f }, 3.0f) == XUD_OK)
            && (hit == false),
        "collision/spheres-just-past-touch", "one thousandth further apart and the same pair is not touching");
    xud_check((xud_check_collision_spheres(&hit, centreA, 5.0f, near, 1.0f) == XUD_OK) && (hit == true), "collision/spheres-contained",
        "a sphere wholly inside another is a collision: containment is overlap");
    xud_check((xud_check_collision_spheres(&hit, centreA, 1.0f, centreA, 1.0f) == XUD_OK) && (hit == true), "collision/spheres-same-sphere",
        "a sphere against itself collides");
    xud_check((xud_check_collision_spheres(&hit, centreA, 2.5f, diagonal, 2.5f) == XUD_OK) && (hit == true),
        "collision/spheres-diagonal-touch",
        "a three-four-zero separation is five units, so radii summing to five touch on a diagonal too - the squared length is the same "
        "number on every axis");
    xud_check((xud_check_collision_spheres(&hit, diagonal, 2.5f, centreA, 2.5f) == XUD_OK) && (hit == true), "collision/spheres-order-free",
        "and the test does not care which sphere is asked about first");

    /* Two boxes. */
    xud_check((xud_check_collision_boxes(&hit, unit, overlapping) == XUD_OK) && (hit == true), "collision/boxes-overlap",
        "boxes whose centres are one apart with width two each share a slab of one unit on x");
    xud_check((xud_check_collision_boxes(&hit, unit, apart) == XUD_OK) && (hit == false), "collision/boxes-apart",
        "the same pair apart on z alone does not overlap: every axis has to");
    xud_check((xud_check_collision_boxes(&hit, unit, touching) == XUD_OK) && (hit == true), "collision/boxes-shared-face-x",
        "faces at the same x share a plane, which is a collision");
    xud_check((xud_check_collision_boxes(&hit, unit, stacked) == XUD_OK) && (hit == true), "collision/boxes-shared-face-y",
        "and the same is true of a box resting on top: raylib's y and z tests are strict where its x test is not, so two crates on the "
        "ground would answer false there");
    xud_check((xud_check_collision_boxes(&hit, inside, unit) == XUD_OK) && (hit == true), "collision/boxes-contained",
        "a box wholly inside another overlaps");
    xud_check((xud_check_collision_boxes(&hit, inside, inside) == XUD_OK) && (hit == true), "collision/boxes-same-box",
        "a box against itself overlaps");
    xud_check((xud_check_collision_boxes(&hit, unit, plate) == XUD_OK) && (hit == true), "collision/boxes-plate",
        "a box with no height still occupies its plane, and the cube straddling that plane overlaps it");
    xud_check((xud_check_collision_boxes(&hit, point, unit) == XUD_OK) && (hit == true), "collision/boxes-degenerate-inside",
        "a box with no size anywhere is a point, and a point inside the unit box is inside");
    xud_check((xud_check_collision_boxes(&hit, point, beside) == XUD_OK) && (hit == false), "collision/boxes-degenerate-outside",
        "the same point outside is not");

    /* A box and a sphere: the box's nearest point, whatever that point is - a face, an edge, a corner. */
    xud_check((xud_check_collision_box_sphere(&hit, unit, side, 1.0f) == XUD_OK) && (hit == false), "collision/box-sphere-away",
        "a sphere of radius one four units from the nearest face is not touching");
    xud_check((xud_check_collision_box_sphere(&hit, unit, face, 1.0f) == XUD_OK) && (hit == true), "collision/box-sphere-face-touch",
        "moved to two on x the sphere's surface meets the box's face exactly, and meeting is contact");
    xud_check((xud_check_collision_box_sphere(&hit, unit, centreA, 0.1f) == XUD_OK) && (hit == true), "collision/box-sphere-inside",
        "a sphere whose centre is inside the box is in contact with it: the nearest point is the centre itself");
    xud_check((xud_check_collision_box_sphere(&hit, unit, centreA, 10.0f) == XUD_OK) && (hit == true), "collision/box-sphere-enclosing",
        "and a sphere big enough to swallow the box is too");
    xud_check((xud_check_collision_box_sphere(&hit, unit, corner, 0.84f) == XUD_OK) && (hit == false), "collision/box-sphere-corner-miss",
        "a sphere at (1.6, 1.6, 0) is 0.8485 from the box's corner, so radius 0.84 misses - the nearest point is the corner, not a face");
    xud_check((xud_check_collision_box_sphere(&hit, unit, corner, 0.85f) == XUD_OK) && (hit == true), "collision/box-sphere-corner-hit",
        "one hundredth of radius more reaches that same corner");
    xud_check((xud_check_collision_box_sphere(&hit, plate, above, 0.5f) == XUD_OK) && (hit == true), "collision/box-sphere-plate-touch",
        "a half-unit sphere centred half a unit above the ground plate touches it");
    xud_check((xud_check_collision_box_sphere(&hit, plate, higher, 0.5f) == XUD_OK) && (hit == false), "collision/box-sphere-plate-miss",
        "a tenth higher and it is resting on nothing");
}

/* The sphere cast: where along the ray the surface is, and which way it faces there. The distance is a
   multiple of the direction as carried, so the checks that hand over a unit direction read world units and
   the one that hands over a direction of length two reads halves of them - which is where this arithmetic
   is deliberately wider than raylib's.
*/
static void xud_test_collision_cast_sphere(void) {
    XudRayCollision record;
    XudVector3 origin = { .x = 0.0f, .y = 0.0f, .z = 0.0f };
    XudVector3 ahead = { .x = -5.0f, .y = 0.0f, .z = 0.0f };
    XudVector3 tangent = { .x = -5.0f, .y = 1.0f, .z = 0.0f };
    XudVector3 awayFrom = { .x = 5.0f, .y = 0.0f, .z = 0.0f };
    XudVector3 diagonal = { .x = -3.0f, .y = -3.0f, .z = 0.0f };
    float half = sqrtf(0.5f);
    XudRay towards = xudTestRay(ahead, (XudVector3) { .x = 1.0f, .y = 0.0f, .z = 0.0f });
    XudRay sideways = xudTestRay(ahead, (XudVector3) { .x = -1.0f, .y = 0.0f, .z = 0.0f });
    XudRay high = xudTestRay((XudVector3) { .x = -5.0f, .y = 2.0f, .z = 0.0f }, (XudVector3) { .x = 1.0f, .y = 0.0f, .z = 0.0f });
    XudRay grazing = xudTestRay(tangent, (XudVector3) { .x = 1.0f, .y = 0.0f, .z = 0.0f });
    XudRay away = xudTestRay(awayFrom, (XudVector3) { .x = 1.0f, .y = 0.0f, .z = 0.0f });
    XudRay upFromCentre = xudTestRay(origin, (XudVector3) { .x = 0.0f, .y = 1.0f, .z = 0.0f });
    XudRay longUp = xudTestRay(origin, (XudVector3) { .x = 0.0f, .y = 2.0f, .z = 0.0f });
    XudRay corner = xudTestRay(diagonal, (XudVector3) { .x = half, .y = half, .z = 0.0f });

    xud_get_ray_collision_sphere(&record, towards, origin, 1.0f);
    xud_check(record.hit == true, "collision/sphere-hit-head-on", "a ray aimed at a sphere hits it");
    xud_check(xudTestNear(record.distance, 4.0f), "collision/sphere-distance-head-on", "five out, radius one: the surface is four along");
    xud_check(
        xudTestVectorNear(record.point, -1.0f, 0.0f, 0.0f), "collision/sphere-point-head-on", "the hit is the near side of the sphere");
    xud_check(xudTestVectorNear(record.normal, -1.0f, 0.0f, 0.0f), "collision/sphere-normal-head-on",
        "and the surface there faces back down the ray");

    xud_get_ray_collision_sphere(&record, high, origin, 1.0f);
    xud_check(record.hit == false, "collision/sphere-miss", "a ray two units above the centre passes a unit sphere");
    xud_check((record.distance == 0.0f) && xudTestVectorNear(record.point, 0.0f, 0.0f, 0.0f)
            && xudTestVectorNear(record.normal, 0.0f, 0.0f, 0.0f),
        "collision/sphere-miss-is-zeroed", "a miss writes the whole record zero, so a caller's stale hit cannot survive one");

    xud_get_ray_collision_sphere(&record, sideways, origin, 1.0f);
    xud_check(record.hit == false, "collision/sphere-behind",
        "the same sphere five behind a ray travelling the other way is not hit: a line meets it, a ray does not");
    xud_get_ray_collision_sphere(&record, away, origin, 1.0f);
    xud_check(record.hit == false, "collision/sphere-behind-other-way", "and a sphere behind the origin is not hit from that side either");

    xud_get_ray_collision_sphere(&record, grazing, origin, 1.0f);
    xud_check(record.hit == true, "collision/sphere-tangent", "a ray one unit off the centre of a unit sphere grazes it");
    xud_check(xudTestNear(record.distance, 5.0f) && xudTestVectorNear(record.point, 0.0f, 1.0f, 0.0f), "collision/sphere-tangent-place",
        "the graze is directly above the centre, five units along");
    xud_check(xudTestVectorNear(record.normal, 0.0f, 1.0f, 0.0f), "collision/sphere-tangent-normal", "and faces straight up there");

    xud_get_ray_collision_sphere(&record, upFromCentre, origin, 1.0f);
    xud_check((record.hit == true) && xudTestNear(record.distance, 1.0f) && xudTestVectorNear(record.point, 0.0f, 1.0f, 0.0f),
        "collision/sphere-inside", "a ray that starts at the centre leaves through the surface one unit up");
    xud_check(xudTestVectorNear(record.normal, 0.0f, -1.0f, 0.0f), "collision/sphere-inside-normal-backwards",
        "and the surface it left through faces back in, which is the outward face of the far side");

    xud_get_ray_collision_sphere(&record, longUp, origin, 1.0f);
    xud_check((record.hit == true) && xudTestNear(record.distance, 0.5f) && xudTestVectorNear(record.point, 0.0f, 1.0f, 0.0f),
        "collision/sphere-scaled-direction",
        "the same cast with a direction of length two hits the same place at half the parameter: distance counts direction lengths, not "
        "world units, unless the direction is unit");

    xud_get_ray_collision_sphere(&record, corner, origin, 1.0f);
    xud_check(record.hit == true, "collision/sphere-diagonal-hit", "a ray at forty-five degrees through a sphere hits it");
    xud_check(xudTestNear(record.distance, (3.0f * sqrtf(2.0f)) - 1.0f), "collision/sphere-diagonal-distance",
        "four and a quarter units from the centre less the radius");
    xud_check(xudTestVectorNear(record.point, -half, -half, 0.0f) && xudTestVectorNear(record.normal, -half, -half, 0.0f),
        "collision/sphere-diagonal-place", "the hit and the normal are the same point on the unit sphere, both at forty-five degrees");
    xud_check(
        xudTestNear(
            sqrtf((record.normal.x * record.normal.x) + (record.normal.y * record.normal.y) + (record.normal.z * record.normal.z)), 1.0f),
        "collision/sphere-normal-is-unit", "and the normal a cast answers is a direction, not a distance");
}

/* The box cast: which face, and from outside or in. Every normal here is exactly one axis long, which is
   the difference this rung states - raylib recovers the face by truncating a scaled vector, which lands on
   the same answer in the middle of a face and on a fuzzier one along an edge.
*/
static void xud_test_collision_cast_box(void) {
    XudRayCollision record;
    XudVector3 origin = { .x = 0.0f, .y = 0.0f, .z = 0.0f };
    XudBoundingBox unit = xudTestBox(origin, 2.0f, 2.0f, 2.0f);
    XudBoundingBox plate = xudTestBox(origin, 2.0f, 0.0f, 2.0f);
    float third = 1.0f / sqrtf(3.0f);
    float half = sqrtf(0.5f);
    XudRay alongX = xudTestRay((XudVector3) { .x = -5.0f, .y = 0.0f, .z = 0.0f }, (XudVector3) { .x = 1.0f, .y = 0.0f, .z = 0.0f });
    XudRay backX = xudTestRay((XudVector3) { .x = -5.0f, .y = 0.0f, .z = 0.0f }, (XudVector3) { .x = -1.0f, .y = 0.0f, .z = 0.0f });
    XudRay alongY = xudTestRay((XudVector3) { .x = 0.0f, .y = 5.0f, .z = 0.0f }, (XudVector3) { .x = 0.0f, .y = -1.0f, .z = 0.0f });
    XudRay highOver = xudTestRay((XudVector3) { .x = 0.0f, .y = 5.0f, .z = 0.0f }, (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 1.0f });
    XudRay throughSlab = xudTestRay((XudVector3) { .x = 0.0f, .y = 0.5f, .z = -5.0f }, (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 1.0f });
    XudRay corner = xudTestRay((XudVector3) { .x = -3.0f, .y = -3.0f, .z = -3.0f }, (XudVector3) { .x = third, .y = third, .z = third });
    XudRay fromInside = xudTestRay(origin, (XudVector3) { .x = 1.0f, .y = 0.0f, .z = 0.0f });
    XudRay insideDiagonal = xudTestRay((XudVector3) { .x = 0.5f, .y = 0.25f, .z = 0.0f }, (XudVector3) { .x = half, .y = half, .z = 0.0f });
    XudRay onTheFace = xudTestRay((XudVector3) { .x = -1.0f, .y = 0.0f, .z = 0.0f }, (XudVector3) { .x = 1.0f, .y = 0.0f, .z = 0.0f });
    XudRay longDown = xudTestRay((XudVector3) { .x = 0.0f, .y = 0.0f, .z = 5.0f }, (XudVector3) { .x = 0.0f, .y = 0.0f, .z = -2.0f });
    XudRay offCentreDown = xudTestRay((XudVector3) { .x = 0.5f, .y = -5.0f, .z = 0.25f }, (XudVector3) { .x = 0.0f, .y = 1.0f, .z = 0.0f });
    XudRay ontoPlate = xudTestRay((XudVector3) { .x = 0.0f, .y = -5.0f, .z = 0.0f }, (XudVector3) { .x = 0.0f, .y = 1.0f, .z = 0.0f });

    xud_get_ray_collision_box(&record, alongX, unit);
    xud_check((record.hit == true) && xudTestNear(record.distance, 4.0f), "collision/box-hit-head-on",
        "a ray five out along x meets the unit box four along");
    xud_check(xudTestVectorNear(record.point, -1.0f, 0.0f, 0.0f) && xudTestVectorNear(record.normal, -1.0f, 0.0f, 0.0f),
        "collision/box-face-head-on", "the face met is the x-minus one, and it faces the way the ray came");

    xud_get_ray_collision_box(&record, alongY, unit);
    xud_check((record.hit == true) && xudTestNear(record.distance, 4.0f) && xudTestVectorNear(record.point, 0.0f, 1.0f, 0.0f)
            && xudTestVectorNear(record.normal, 0.0f, 1.0f, 0.0f),
        "collision/box-face-from-above", "the same box met from above is met on its top, and the normal is the top's");

    xud_get_ray_collision_box(&record, offCentreDown, unit);
    xud_check((record.hit == true) && xudTestNear(record.distance, 4.0f) && xudTestVectorNear(record.point, 0.5f, -1.0f, 0.25f)
            && xudTestVectorNear(record.normal, 0.0f, -1.0f, 0.0f),
        "collision/box-point-carries-other-axes",
        "a ray that misses the centre still hits where it crosses: the hit carries the ray's own x and z, and the face met from below is "
        "the "
        "bottom one");

    xud_get_ray_collision_box(&record, highOver, unit);
    xud_check(record.hit == false, "collision/box-parallel-outside",
        "a ray five units above the box and travelling along z never enters the y slab it is outside");

    xud_get_ray_collision_box(&record, throughSlab, unit);
    xud_check((record.hit == true) && xudTestNear(record.distance, 4.0f) && xudTestVectorNear(record.point, 0.0f, 0.5f, -1.0f)
            && xudTestVectorNear(record.normal, 0.0f, 0.0f, -1.0f),
        "collision/box-parallel-inside-two",
        "a ray inside the x and y slabs and travelling along the third enters through the z face it is heading for: five out, the near "
        "face "
        "one unit short of the box");

    xud_get_ray_collision_box(&record, backX, unit);
    xud_check(record.hit == false, "collision/box-behind", "the box behind a ray travelling away from it is not hit");

    xud_get_ray_collision_box(&record, corner, unit);
    xud_check((record.hit == true) && xudTestNear(record.distance, 2.0f / third), "collision/box-corner-distance",
        "a ray into the corner of a box from (-3, -3, -3) meets it where all three slabs are entered at once");
    xud_check(xudTestVectorNear(record.point, -1.0f, -1.0f, -1.0f), "collision/box-corner-point", "and that place is the corner");
    xud_check(xudTestVectorNear(record.normal, -1.0f, 0.0f, 0.0f), "collision/box-corner-normal-first-axis",
        "three faces meet at once, so the normal is the first axis's - the tie-break the rung states rather than a blend of the three");

    xud_get_ray_collision_box(&record, fromInside, unit);
    xud_check((record.hit == true) && xudTestNear(record.distance, 1.0f) && xudTestVectorNear(record.point, 1.0f, 0.0f, 0.0f)
            && xudTestVectorNear(record.normal, 1.0f, 0.0f, 0.0f),
        "collision/box-from-inside",
        "a ray that starts inside answers the face it leaves by, at a positive distance, facing out through it");

    xud_get_ray_collision_box(&record, insideDiagonal, unit);
    xud_check((record.hit == true) && xudTestNear(record.distance, half) && xudTestVectorNear(record.point, 1.0f, 0.75f, 0.0f)
            && xudTestVectorNear(record.normal, 1.0f, 0.0f, 0.0f),
        "collision/box-from-inside-earliest-exit",
        "inside and travelling diagonally, the face met is the one reached FIRST - x at half a unit, not y at one and a half");

    xud_get_ray_collision_box(&record, onTheFace, unit);
    xud_check((record.hit == true) && (record.distance == 0.0f) && xudTestVectorNear(record.normal, -1.0f, 0.0f, 0.0f),
        "collision/box-origin-on-face", "a ray that starts on a surface is on it: a hit at zero, the face's own normal");

    xud_get_ray_collision_box(&record, longDown, unit);
    xud_check((record.hit == true) && xudTestNear(record.distance, 2.0f) && xudTestVectorNear(record.point, 0.0f, 0.0f, 1.0f)
            && xudTestVectorNear(record.normal, 0.0f, 0.0f, 1.0f),
        "collision/box-scaled-direction",
        "a direction of length two covers the last face in two halves: the hit is the same place, the parameter is half");

    xud_get_ray_collision_box(&record, ontoPlate, plate);
    xud_check((record.hit == true) && xudTestNear(record.distance, 5.0f) && xudTestVectorNear(record.point, 0.0f, 0.0f, 0.0f)
            && xudTestVectorNear(record.normal, 0.0f, -1.0f, 0.0f),
        "collision/box-plate", "a box with no thickness is still a surface: the ray from below meets it at the plate's own plane");
}

/* The rung read with the camera's fold, which is the pair the picking example is: the fold answers the
   pixel a solid's centre lands at, the trace answers the ray through that pixel, the cast answers where
   along it the solid is, and the fold again answers where that hit lands. The four together are what makes
   "click the box" mean the box under the cursor, so the round trip is stated here even though every part
   of it is tested in its own area too.
*/
static void xud_test_collision_and_the_fold(void) {
    XudCamera3D camera = { .position = (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 10.0f },
        .target = (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 0.0f },
        .up = (XudVector3) { .x = 0.0f, .y = 1.0f, .z = 0.0f },
        .fovy = 45.0f,
        .near = 0.1f,
        .far = 100.0f };
    XudVector3 boxCentre = { .x = 0.0f, .y = 0.0f, .z = 0.0f };
    XudBoundingBox unit = xudTestBox(boxCentre, 2.0f, 2.0f, 2.0f);
    XudVector2 pixel;
    XudVector2 hitPixel;
    XudVector2 beside;
    XudRay ray;
    XudRayCollision record;
    XudResult result;

    result = xud_get_world_to_screen_ex(&pixel, boxCentre, &camera, 800.0f, 450.0f);
    xud_check(result == XUD_OK, "collision/fold-centre", "the box's centre is in front of the camera, so it has a pixel");

    result = xud_get_screen_to_world_ray_ex(&ray, pixel, &camera, 800.0f, 450.0f);
    xud_check(
        (result == XUD_OK) && xudTestVectorNear(ray.position, 0.0f, 0.0f, 10.0f) && xudTestVectorNear(ray.direction, 0.0f, 0.0f, -1.0f),
        "collision/fold-trace", "tracing back through that pixel gives the eye and the axis it is looking down");

    result = xud_get_ray_collision_box(&record, ray, unit);
    xud_check((result == XUD_OK) && (record.hit == true) && xudTestNear(record.distance, 9.0f), "collision/fold-cast-distance",
        "and the near face of the unit box is nine of the way along it");

    result = xud_get_world_to_screen_ex(&hitPixel, record.point, &camera, 800.0f, 450.0f);
    xud_check((result == XUD_OK) && xudTestNear(hitPixel.x, pixel.x) && xudTestNear(hitPixel.y, pixel.y), "collision/fold-round-trip",
        "the hit the cast found folds back to the pixel the cursor was on, which is the click landing where it was aimed");

    beside = (XudVector2) { .x = pixel.x + 200.0f, .y = pixel.y };
    result = xud_get_screen_to_world_ray_ex(&ray, beside, &camera, 800.0f, 450.0f);
    xud_get_ray_collision_box(&record, ray, unit);
    xud_check((result == XUD_OK) && (record.hit == false), "collision/fold-cast-miss",
        "two hundred pixels to the side is off the box, and the click that makes answers no hit");

    result = xud_get_screen_to_world_ray_ex(&ray, pixel, &camera, 800.0f, 450.0f);
    xud_get_ray_collision_sphere(&record, ray, boxCentre, 1.0f);
    xud_check((result == XUD_OK) && (record.hit == true) && xudTestNear(record.distance, 9.0f)
            && xudTestVectorNear(record.point, 0.0f, 0.0f, 1.0f) && xudTestVectorNear(record.normal, 0.0f, 0.0f, 1.0f),
        "collision/fold-cast-sphere", "the same ray against a unit sphere at the same place meets it nine along, facing the eye");
}

void xud_test_collision(void) {
    xud_test_collision_refusals();
    xud_test_collision_solids();
    xud_test_collision_cast_sphere();
    xud_test_collision_cast_box();
    xud_test_collision_and_the_fold();
}
