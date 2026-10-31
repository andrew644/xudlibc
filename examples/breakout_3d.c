/***********************************************************************************************
 *
 *   breakout_3d.c - a 3D breakout: a ball, a paddle, a field of bricks in three depth layers
 *
 *   Owns : one game - the ball's motion and its bounces, the brick field and the one brick a
 *          contact removes, the paddle's chase rule, the example's own projection that places
 *          every quad in clip space, and the frame loop that draws one frame per step of that
 *          simulation. Nothing else: no input, no clock, no camera, no batch, no buffer.
 *   Layer: examples. Public API only (xud.h) plus stdio and math.h's fabsf. One context, one
 *          frame loop, and the public calls the game needs - xud_init_context(),
 *          xud_begin_drawing(), xud_draw_triangle_3d() for every quad, xud_end_drawing() and
 *          xud_close_context() - and nothing below xud.h.
 *
 *   The game is 3D in the one way this library can make it 3D today: every object has a z, the
 *   ball travels in x, y and z, the bricks sit at three different depths, and the depth buffer
 *   the render pass owns resolves which of them is nearer. What makes that depth VISIBLE is the
 *   projection below, and the projection is the example's own - which is the one thing a reader
 *   should understand before changing anything here.
 *
 *   Three rungs of the library are todo.md's Later items rather than open bugs, and this
 *   example stands in for each of them in the smallest way that keeps the example honest about
 *   which half is whose:
 *
 *     - No camera + transform step. The frame's viewProj and every draw's model matrix are
 *       identity, so a shape's coordinates are clip-space coordinates - the same fact
 *       hello_triangle's note states, at a whole scene's scale. xud_breakout_project() is the
 *       example's own pinhole: it does the divide by the distance from the eye HERE rather than
 *       in the shader, because the shader's w is 1 and the values a draw is handed are already
 *       NDC. That divide is exact for every quad this example draws because every one of them
 *       lies in a plane of constant z - the four corners of a quad share a depth, so they share
 *       the projection's scale and the quad's own depth is constant across it, which is what
 *       the rasteriser's interpolation needs. A quad that spanned two depths would want
 *       perspective-correct interpolation, and that is the camera + transform step's to own.
 *
 *     - No input wrapper, so nothing moves the paddle: the game drives it with its own rule
 *       (the paddle chases the ball at a bounded speed, a bound slower than the ball's fastest
 *       x, so the game sometimes misses). This example is a game that plays itself, and the
 *       half that is not the library's is the hand on the paddle rather than the game.
 *
 *     - No clock, so the simulation steps a fixed amount per frame rather than per second. The
 *       game is deterministic: no clock, no randomness, no filesystem, one fixed starting
 *       layout, so two runs of this example are the same run.
 *
 *   One public shape call is used here and only one: xud_draw_triangle_3d(). The reason is the
 *   projection above. xud_draw_cube() places its 24 corners from a centre and three extents IN
 *   THE SPACE IT IS HANDED, and a projected box's corners are not centre +/- half in that space,
 *   so a box this example projected itself would be 12 hand-placed triangles - 36 vertices of
 *   the batch's 64 - and the budget below says no to that. The cube and the sphere calls are for
 *   a scene the camera + transform step places, not for one this example projects by hand.
 *
 *   The batch's capacity is the one budget this example obeys, and the one number it must not
 *   reach for: xud_batch.h's initial capacity is internal and user code includes xud.h only. So
 *   the budget below is the example's own count of its own shapes, asserted against the two
 *   numbers the library caps it at today (64 vertices, 96 indices) restated here. A frame that
 *   wanted more would be REFUSED by the batch rather than truncated, and every draw below
 *   checks its answer for exactly that reason.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include <math.h> /* Required for: fabsf, the interval overlap every collision below reads */
#include <stdio.h>

#include "xud.h"

/*-----------------------------------------------------------------------------------------------
 * The window, the example's own camera and the playfield box
 *-----------------------------------------------------------------------------------------------*/

/* 16:9 rather than square, because clip space is -1..1 on BOTH axes while a window is not, and
   the aspect below is what turns the game's square world units back into clip x. A square
   window would make the aspect 1 and hide the one place that conversion belongs. */
#define XUD_BREAKOUT_WINDOW_WIDTH 640u
#define XUD_BREAKOUT_WINDOW_HEIGHT 360u
#define XUD_BREAKOUT_ASPECT ((float)XUD_BREAKOUT_WINDOW_WIDTH / (float)XUD_BREAKOUT_WINDOW_HEIGHT)

/* Every distance below is stated in INTEGER milli-world-units and the float macro is derived
   from it, because the fit checks this example wants are static_asserts and a static_assert
   owes an INTEGER constant expression under -Wpedantic - a float one is an error, not a
   warning. A world unit is XUD_BREAKOUT_UNIT of these, and the world unit is the focus plane's
   unit: one world unit there is half a window height in y. */
#define XUD_BREAKOUT_UNIT 1000
#define XUD_BREAKOUT_MM(value) ((float)(value) / (float)XUD_BREAKOUT_UNIT)

/* The example's own pinhole (see the note above): the eye sits on +Z looking down -Z, FOCUS_D
   is the eye-to-focus DISTANCE and the plane at that distance is the one a world unit maps 1:1
   in y, so a world unit is half a window height there and half a window width divided by the
   aspect in x. NEAR_D and FAR_D are the two distances a vertex's z is mapped from, and the map
   is linear ON PURPOSE: every quad this example draws has one depth, so a linear map is exact
   and a perspective-correct one would be a second thing for this example to invent. */
#define XUD_BREAKOUT_EYE_Z_MM 2400
#define XUD_BREAKOUT_FOCUS_D_MM 2400
#define XUD_BREAKOUT_NEAR_D_MM 1500
#define XUD_BREAKOUT_FAR_D_MM 3500

/* The playfield box, in world units: the ball bounces inside it, the brick field sits inside it
   and its far wall is the back plane the frame loop draws. */
#define XUD_BREAKOUT_HALF_X_MM 1000
#define XUD_BREAKOUT_HALF_Y_MM 620
#define XUD_BREAKOUT_HALF_Z_MM 500

/* The world half-extent VISIBLE at a distance from the eye, in milli-units: the pinhole's scale
   there is FOCUS_D/distance and the focus plane's visible half-height is one world unit, so the
   visible half-height is FOCUS_D/distance world units. The integer division rounds DOWN, which
   is the safe direction for an assert that a shape FITS inside what is visible. */
#define XUD_BREAKOUT_VISIBLE_HALF_Y_MM(distance) ((XUD_BREAKOUT_FOCUS_D_MM * XUD_BREAKOUT_UNIT) / (distance))
#define XUD_BREAKOUT_VISIBLE_HALF_X_MM(distance)                                                                                           \
    ((XUD_BREAKOUT_VISIBLE_HALF_Y_MM(distance) * (int)XUD_BREAKOUT_WINDOW_WIDTH) / (int)XUD_BREAKOUT_WINDOW_HEIGHT)

/* The frustum fit: the box's NEAREST depth is EYE_Z - HALF_Z, and if the box fits the visible
   extent there it fits it everywhere, because the visible extent only grows with distance from
   the eye. These four asserts are the reason the game is on screen at all rather than a claim
   that it is; the two distance ones are the other half of the same fit, that no vertex of the
   scene is ever behind the eye or past the far distance. */
static_assert((XUD_BREAKOUT_EYE_Z_MM - XUD_BREAKOUT_HALF_Z_MM) >= XUD_BREAKOUT_NEAR_D_MM,
    "breakout_3d.c: the nearest point of the box is in front of the near distance, so no vertex of the scene is ever behind the eye");
static_assert((XUD_BREAKOUT_EYE_Z_MM + XUD_BREAKOUT_HALF_Z_MM) <= XUD_BREAKOUT_FAR_D_MM,
    "breakout_3d.c: and the farthest point of it is inside the far distance, so the whole box is inside the mapped z range");
static_assert(XUD_BREAKOUT_HALF_Y_MM <= XUD_BREAKOUT_VISIBLE_HALF_Y_MM(XUD_BREAKOUT_EYE_Z_MM - XUD_BREAKOUT_HALF_Z_MM),
    "breakout_3d.c: the playfield's half-height fits the frustum at the box's nearest depth, so nothing in the game is clipped out");
static_assert(XUD_BREAKOUT_HALF_X_MM <= XUD_BREAKOUT_VISIBLE_HALF_X_MM(XUD_BREAKOUT_EYE_Z_MM - XUD_BREAKOUT_HALF_Z_MM),
    "breakout_3d.c: and so is its half-width, which is the same fit read across the aspect");

/* The ball is a BOX rather than a sphere for two reasons that are the same reason: the sphere's
   tessellation is the library's (xud_shapes.h's claim about the batch's capacity) and its
   vertices cannot be projected by this example for the reason the note above states, and its
   90 indices would spend the batch's whole 96 of their own. A box's own half-extent is what
   makes every collision below a box-overlap test rather than a swept-volume one. */
#define XUD_BREAKOUT_BALL_HALF_MM 50
#define XUD_BREAKOUT_BALL_SPEED_X_MM 14
#define XUD_BREAKOUT_BALL_SPEED_Y_MM 11
#define XUD_BREAKOUT_BALL_SPEED_Z_MM 8
#define XUD_BREAKOUT_BALL_SPEED_X_MIN_MM 6
#define XUD_BREAKOUT_BALL_SPEED_X_MAX_MM 18

/* The paddle is a slab that spans the playfield's WHOLE depth, and that is a game decision
   rather than a shape one: a paddle with a shallow depth would only ever catch a ball that
   happened to be at its own depth, which is a coin flip rather than a game. Its x speed is
   BELOW the ball's fastest x, so the chase below sometimes loses and the bottom of the box is
   what a loss is for. The paddle's own half-depth is the box's half-depth by definition. */
#define XUD_BREAKOUT_PADDLE_HALF_X_MM 220
#define XUD_BREAKOUT_PADDLE_HALF_Y_MM 45
#define XUD_BREAKOUT_PADDLE_HALF_Z_MM XUD_BREAKOUT_HALF_Z_MM
#define XUD_BREAKOUT_PADDLE_SPEED_MM 11
#define XUD_BREAKOUT_PADDLE_Y_MM (-550)

static_assert(XUD_BREAKOUT_PADDLE_HALF_X_MM <= XUD_BREAKOUT_HALF_X_MM,
    "breakout_3d.c: a paddle wider than the box would be pushed out of it by its own clamp");
static_assert((XUD_BREAKOUT_PADDLE_Y_MM - XUD_BREAKOUT_PADDLE_HALF_Y_MM) >= -XUD_BREAKOUT_HALF_Y_MM,
    "breakout_3d.c: the paddle sits inside the box, so the ball can always reach the plane it bounces on");

/* The brick field: three columns by two rows, staggered across three depth layers so the field
   is a 3D lattice rather than a wall - a wall at one depth would be a breakout whose third
   axis is decoration. The spans are wider than the bricks, so the field is a set of DISTINCT
   bricks rather than one block, and the assert below is what says so. The field is 6 bricks
   because the budget below is the batch's, not the game's. */
#define XUD_BREAKOUT_BRICK_COLS 3
#define XUD_BREAKOUT_BRICK_ROWS 2
#define XUD_BREAKOUT_BRICK_LAYERS 3
#define XUD_BREAKOUT_BRICK_COUNT (XUD_BREAKOUT_BRICK_COLS * XUD_BREAKOUT_BRICK_ROWS)
#define XUD_BREAKOUT_BRICK_HALF_X_MM 110
#define XUD_BREAKOUT_BRICK_HALF_Y_MM 80
#define XUD_BREAKOUT_BRICK_HALF_Z_MM 90
#define XUD_BREAKOUT_BRICK_SPAN_X_MM 260
#define XUD_BREAKOUT_BRICK_SPAN_Y_MM 200
#define XUD_BREAKOUT_BRICK_Z_STEP_MM 300
#define XUD_BREAKOUT_BRICK_ROW_LIFT_MM 150 /* the field's own lift off the box's centre, in milli-units */

static_assert(XUD_BREAKOUT_BRICK_SPAN_X_MM > (2 * XUD_BREAKOUT_BRICK_HALF_X_MM),
    "breakout_3d.c: a brick is narrower than its column span, so the field is distinct bricks rather than one block");
static_assert(XUD_BREAKOUT_BRICK_SPAN_Y_MM > (2 * XUD_BREAKOUT_BRICK_HALF_Y_MM),
    "breakout_3d.c: and shorter than its row span, which is the same claim read up the field");
static_assert(
    ((((XUD_BREAKOUT_BRICK_LAYERS - 1) / 2) * XUD_BREAKOUT_BRICK_Z_STEP_MM) + XUD_BREAKOUT_BRICK_HALF_Z_MM) <= XUD_BREAKOUT_HALF_Z_MM,
    "breakout_3d.c: the deepest layer and a brick's own half-extent fit inside the box, so no brick is ever outside the playfield");
static_assert(
    ((((XUD_BREAKOUT_BRICK_COLS - 1) / 2) * XUD_BREAKOUT_BRICK_SPAN_X_MM) + XUD_BREAKOUT_BRICK_HALF_X_MM) <= XUD_BREAKOUT_HALF_X_MM,
    "breakout_3d.c: the field's widest brick fits the box in x");
static_assert(
    ((((XUD_BREAKOUT_BRICK_ROWS - 1) / 2) * XUD_BREAKOUT_BRICK_SPAN_Y_MM) + XUD_BREAKOUT_BRICK_ROW_LIFT_MM + XUD_BREAKOUT_BRICK_HALF_Y_MM)
        <= XUD_BREAKOUT_HALF_Y_MM,
    "breakout_3d.c: and its highest brick fits the box in y, above the field's own lift");
static_assert(XUD_BREAKOUT_BRICK_Z_STEP_MM > (XUD_BREAKOUT_BRICK_HALF_Z_MM + XUD_BREAKOUT_BALL_HALF_MM),
    "breakout_3d.c: two bricks the ball could overlap in x and y are further apart in depth than a ball and a brick's own half-extents sum "
    "to, so the ball is never inside two at once and one brick per step is the whole test");

/* The float form of the distances above, which is what the game and the projection actually use. */
#define XUD_BREAKOUT_EYE_Z XUD_BREAKOUT_MM(XUD_BREAKOUT_EYE_Z_MM)
#define XUD_BREAKOUT_FOCUS_D XUD_BREAKOUT_MM(XUD_BREAKOUT_FOCUS_D_MM)
#define XUD_BREAKOUT_NEAR_D XUD_BREAKOUT_MM(XUD_BREAKOUT_NEAR_D_MM)
#define XUD_BREAKOUT_FAR_D XUD_BREAKOUT_MM(XUD_BREAKOUT_FAR_D_MM)
#define XUD_BREAKOUT_HALF_X XUD_BREAKOUT_MM(XUD_BREAKOUT_HALF_X_MM)
#define XUD_BREAKOUT_HALF_Y XUD_BREAKOUT_MM(XUD_BREAKOUT_HALF_Y_MM)
#define XUD_BREAKOUT_HALF_Z XUD_BREAKOUT_MM(XUD_BREAKOUT_HALF_Z_MM)
#define XUD_BREAKOUT_BALL_HALF XUD_BREAKOUT_MM(XUD_BREAKOUT_BALL_HALF_MM)
#define XUD_BREAKOUT_BALL_SPEED_X XUD_BREAKOUT_MM(XUD_BREAKOUT_BALL_SPEED_X_MM)
#define XUD_BREAKOUT_BALL_SPEED_Y XUD_BREAKOUT_MM(XUD_BREAKOUT_BALL_SPEED_Y_MM)
#define XUD_BREAKOUT_BALL_SPEED_Z XUD_BREAKOUT_MM(XUD_BREAKOUT_BALL_SPEED_Z_MM)
#define XUD_BREAKOUT_BALL_SPEED_X_MIN XUD_BREAKOUT_MM(XUD_BREAKOUT_BALL_SPEED_X_MIN_MM)
#define XUD_BREAKOUT_BALL_SPEED_X_MAX XUD_BREAKOUT_MM(XUD_BREAKOUT_BALL_SPEED_X_MAX_MM)
#define XUD_BREAKOUT_PADDLE_HALF_X XUD_BREAKOUT_MM(XUD_BREAKOUT_PADDLE_HALF_X_MM)
#define XUD_BREAKOUT_PADDLE_HALF_Y XUD_BREAKOUT_MM(XUD_BREAKOUT_PADDLE_HALF_Y_MM)
#define XUD_BREAKOUT_PADDLE_HALF_Z XUD_BREAKOUT_MM(XUD_BREAKOUT_PADDLE_HALF_Z_MM)
#define XUD_BREAKOUT_PADDLE_SPEED XUD_BREAKOUT_MM(XUD_BREAKOUT_PADDLE_SPEED_MM)
#define XUD_BREAKOUT_PADDLE_Y XUD_BREAKOUT_MM(XUD_BREAKOUT_PADDLE_Y_MM)
#define XUD_BREAKOUT_BRICK_HALF_X XUD_BREAKOUT_MM(XUD_BREAKOUT_BRICK_HALF_X_MM)
#define XUD_BREAKOUT_BRICK_HALF_Y XUD_BREAKOUT_MM(XUD_BREAKOUT_BRICK_HALF_Y_MM)
#define XUD_BREAKOUT_BRICK_HALF_Z XUD_BREAKOUT_MM(XUD_BREAKOUT_BRICK_HALF_Z_MM)
#define XUD_BREAKOUT_BRICK_SPAN_X XUD_BREAKOUT_MM(XUD_BREAKOUT_BRICK_SPAN_X_MM)
#define XUD_BREAKOUT_BRICK_SPAN_Y XUD_BREAKOUT_MM(XUD_BREAKOUT_BRICK_SPAN_Y_MM)
#define XUD_BREAKOUT_BRICK_Z_STEP XUD_BREAKOUT_MM(XUD_BREAKOUT_BRICK_Z_STEP_MM)
#define XUD_BREAKOUT_BRICK_ROW_LIFT_F XUD_BREAKOUT_MM(XUD_BREAKOUT_BRICK_ROW_LIFT_MM)

/*-----------------------------------------------------------------------------------------------
 * The frame's budget, counted from the example's own shapes
 *-----------------------------------------------------------------------------------------------*/

/* The costs below are the public calls' own documented costs, restated here rather than read:
   a triangle is three vertices and three indices, and every quad this example draws is two
   triangles. The two numbers the asserts are written against are the batch's capacity today -
   64 vertices and 96 indices, xud_batch.h's initial capacity, which is internal and which user
   code does not reach. A frame that wanted more would be refused by the batch rather than
   truncated, and the draw loop below checks every answer for exactly that reason. */
#define XUD_BREAKOUT_TRIANGLE_VERTEX_COUNT 3u
#define XUD_BREAKOUT_TRIANGLE_INDEX_COUNT 3u
#define XUD_BREAKOUT_QUAD_TRIANGLE_COUNT 2u
#define XUD_BREAKOUT_FRAME_QUAD_COUNT (XUD_BREAKOUT_BRICK_COUNT + 3) /* the field, the paddle, the ball, the back plane */
#define XUD_BREAKOUT_FRAME_VERTEX_COUNT                                                                                                    \
    (XUD_BREAKOUT_FRAME_QUAD_COUNT * XUD_BREAKOUT_QUAD_TRIANGLE_COUNT * XUD_BREAKOUT_TRIANGLE_VERTEX_COUNT)
#define XUD_BREAKOUT_FRAME_INDEX_COUNT                                                                                                     \
    (XUD_BREAKOUT_FRAME_QUAD_COUNT * XUD_BREAKOUT_QUAD_TRIANGLE_COUNT * XUD_BREAKOUT_TRIANGLE_INDEX_COUNT)

static_assert(XUD_BREAKOUT_FRAME_VERTEX_COUNT <= 64u,
    "breakout_3d.c: a frame's whole field of quads fits the batch's capacity today, so no draw in the loop is ever refused for being at a "
    "bound");
static_assert(XUD_BREAKOUT_FRAME_INDEX_COUNT <= 96u, "breakout_3d.c: and so are its indices, which is the other half of the same budget");

/* The window's stay-open frame count, and the interval the game's own state is printed at -
   enough frames that the ball crosses the field several times and the depth is legible, and no
   feature of the library's own. There is no clock, so this is a frame count rather than a
   duration: see the note above. */
#define XUD_BREAKOUT_FRAMES 900u
#define XUD_BREAKOUT_REPORT_EVERY 60u

/*-----------------------------------------------------------------------------------------------
 * The game's own state
 *-----------------------------------------------------------------------------------------------*/

typedef struct XudBreakoutBall {
    XudVector3 position;
    XudVector3 velocity;
    XudVector3 half; /* the ball's own half-extents, all three the same: the ball is a box */
} XudBreakoutBall;

typedef struct XudBreakoutPaddle {
    XudVector3 centre;
    XudVector3 half;
    float speed;
} XudBreakoutPaddle;

typedef struct XudBreakoutBrick {
    XudVector3 centre;
    XudVector3 half;
    XudColor colour;
    uint32_t alive;
} XudBreakoutBrick;

#define XUD_BREAKOUT_NO_BRICK 0xFFFFFFFFu /* "this step hit no brick", the same sentinel shape the rest of this tree uses */

/* One component of a vector by axis, so the collision below can state "the axis whose overlap
   is shallowest" once rather than three times as three named pairs of cases. */
static float* xud_breakout_axis(XudVector3* vector, uint32_t axis) {
    if (axis == 0u) {
        return &vector->x;
    }
    if (axis == 1u) {
        return &vector->y;
    }

    return &vector->z;
}

/*-----------------------------------------------------------------------------------------------
 * The example's own projection - see the note at the top of the file
 *-----------------------------------------------------------------------------------------------*/

static XudVector3 xud_breakout_project(XudVector3 world) {
    const float distance = XUD_BREAKOUT_EYE_Z - world.z;
    const float scale = XUD_BREAKOUT_FOCUS_D / distance;
    XudVector3 clip;

    clip.x = (world.x * scale) / XUD_BREAKOUT_ASPECT;
    clip.y = world.y * scale;
    clip.z = ((2.0f * (distance - XUD_BREAKOUT_NEAR_D)) / (XUD_BREAKOUT_FAR_D - XUD_BREAKOUT_NEAR_D)) - 1.0f;

    return clip;
}

/* One quad, drawn as the two triangles a quad is. The four corners are the caller's centre plus
   or minus the caller's half-extents in the WORLD space above the projection, and each corner
   is projected on its own - which is the condition the note at the top of the file states: all
   four share the caller's z, so all four share the projection's scale and the quad's depth is
   constant across it. The two triangles are (0,1,2) and (0,2,3) over corners in counter-clockwise
   order seen from +Z, the same winding raylib's DrawTriangle3D documents and the pipeline's
   front face names; the pipeline culls nothing, so the winding is the shape's own consistency
   rather than a gate the draw obeys. */
static XudResult xud_breakout_draw_quad(XudContext* context, XudVector3 centre, float halfX, float halfY, XudColor colour) {
    const XudVector3 c0 = xud_breakout_project((XudVector3) { .x = centre.x - halfX, .y = centre.y - halfY, .z = centre.z });
    const XudVector3 c1 = xud_breakout_project((XudVector3) { .x = centre.x + halfX, .y = centre.y - halfY, .z = centre.z });
    const XudVector3 c2 = xud_breakout_project((XudVector3) { .x = centre.x + halfX, .y = centre.y + halfY, .z = centre.z });
    const XudVector3 c3 = xud_breakout_project((XudVector3) { .x = centre.x - halfX, .y = centre.y + halfY, .z = centre.z });

    XudResult result = xud_draw_triangle_3d(context, c0, c1, c2, colour);
    if (result != XUD_OK) {
        return result;
    }

    return xud_draw_triangle_3d(context, c0, c2, c3, colour);
}

/*-----------------------------------------------------------------------------------------------
 * The game's rules
 *-----------------------------------------------------------------------------------------------*/

/* The ball starts where it starts, below the field and above the paddle, with a direction that
   depends only on the frame count so a long run is not one identical loop: the x sign
   alternates and the z sign alternates at a different rate. No randomness, so the run is
   reproducible, and the start is inside the box by the asserts above rather than by luck. */
static void xud_breakout_reset_ball(XudBreakoutBall* ball, uint32_t frame) {
    ball->position = (XudVector3) { .x = 0.0f, .y = XUD_BREAKOUT_MM(-300), .z = 0.0f };
    ball->velocity = (XudVector3) { .x = ((frame % 2u) == 0u) ? XUD_BREAKOUT_BALL_SPEED_X : -XUD_BREAKOUT_BALL_SPEED_X,
        .y = XUD_BREAKOUT_BALL_SPEED_Y,
        .z = ((frame % 3u) == 0u) ? -XUD_BREAKOUT_BALL_SPEED_Z : XUD_BREAKOUT_BALL_SPEED_Z };
    ball->half = (XudVector3) { .x = XUD_BREAKOUT_BALL_HALF, .y = XUD_BREAKOUT_BALL_HALF, .z = XUD_BREAKOUT_BALL_HALF };
}

/* A wall bounce: the ball is a box and the playfield is a box, so an axis's own test is exact
   and the ball is put back flush against the wall rather than left inside it. The clamp is what
   keeps a step that overshot from carrying the ball further out on the next step. */
static void xud_breakout_bounce_wall(float* position, float* velocity, float half, float limit) {
    if ((*position + half) > limit) {
        *position = limit - half;
        *velocity = -*velocity;
    } else if ((*position - half) < -limit) {
        *position = -limit + half;
        *velocity = -*velocity;
    }
}

/* The english the paddle adds: the contact point off the paddle's centre pushes the ball's x
   velocity, which is breakout's own rule rather than an invention here. It is clamped at both
   ends because both ends are stalls - a ball whose x velocity is 0 bounces up and down in one
   column forever, and a ball whose x velocity is huge never reaches the brick field at all. */
static void xud_breakout_clamp_x_velocity(XudBreakoutBall* ball) {
    const float magnitude = fabsf(ball->velocity.x);
    const float sign = (ball->velocity.x < 0.0f) ? -1.0f : 1.0f;
    float clamped = magnitude;

    if (clamped < XUD_BREAKOUT_BALL_SPEED_X_MIN) {
        clamped = XUD_BREAKOUT_BALL_SPEED_X_MIN;
    }
    if (clamped > XUD_BREAKOUT_BALL_SPEED_X_MAX) {
        clamped = XUD_BREAKOUT_BALL_SPEED_X_MAX;
    }

    ball->velocity.x = sign * clamped;
}

/* The brick the ball is inside, if any. The ball and a brick are both boxes, so the test is
   three interval overlaps, and the axis the ball is pushed back along is the one whose overlap
   is SHALLOWEST - which is the face the ball crossed. One brick per step, in field order: two
   bricks in one step would want a swept test, which is the mesh / camera rung's rather than one
   invented here, and the field's LAYERS are staggered so two bricks the ball could overlap in x
   and y are 300 apart in z, which is more than a ball and a brick's own half-extents sum to -
   the assert above is that fact, and so the ball is never inside two at once. */
static uint32_t xud_breakout_hit_brick(XudBreakoutBall* ball, XudBreakoutBrick* bricks) {
    uint32_t i = 0u;

    for (i = 0u; i < (uint32_t)XUD_BREAKOUT_BRICK_COUNT; i++) {
        const float overlapX = (ball->half.x + bricks[i].half.x) - fabsf(ball->position.x - bricks[i].centre.x);
        const float overlapY = (ball->half.y + bricks[i].half.y) - fabsf(ball->position.y - bricks[i].centre.y);
        const float overlapZ = (ball->half.z + bricks[i].half.z) - fabsf(ball->position.z - bricks[i].centre.z);
        uint32_t axis = 0u;
        float least = 0.0f;

        if (bricks[i].alive == 0u) {
            continue;
        }
        if ((overlapX <= 0.0f) || (overlapY <= 0.0f) || (overlapZ <= 0.0f)) {
            continue;
        }

        least = overlapX;
        if (overlapY < least) {
            axis = 1u;
            least = overlapY;
        }
        if (overlapZ < least) {
            axis = 2u;
            least = overlapZ;
        }

        const float ballAt = *xud_breakout_axis(&ball->position, axis);
        const float brickAt = *xud_breakout_axis(&bricks[i].centre, axis);
        const float halfAlong = *xud_breakout_axis(&ball->half, axis) + *xud_breakout_axis(&bricks[i].half, axis);
        const float side = (ballAt >= brickAt) ? 1.0f : -1.0f;

        *xud_breakout_axis(&ball->position, axis) = brickAt + (side * halfAlong);
        *xud_breakout_axis(&ball->velocity, axis) = -(*xud_breakout_axis(&ball->velocity, axis));
        bricks[i].alive = 0u;

        return i;
    }

    return XUD_BREAKOUT_NO_BRICK;
}

/* The paddle's chase: the paddle moves toward the ball's x at its own bounded speed, and the
   bound is BELOW the ball's fastest x so the chase sometimes loses. The paddle is kept inside
   the box so it cannot be pushed out of the playfield by a ball that is at a wall. */
static void xud_breakout_step_paddle(XudBreakoutPaddle* paddle, XudBreakoutBall* ball) {
    const float delta = ball->position.x - paddle->centre.x;
    const float step = (delta > paddle->speed) ? paddle->speed : ((delta < -paddle->speed) ? -paddle->speed : delta);
    const float limit = XUD_BREAKOUT_HALF_X - paddle->half.x;

    paddle->centre.x += step;
    if (paddle->centre.x > limit) {
        paddle->centre.x = limit;
    } else if (paddle->centre.x < -limit) {
        paddle->centre.x = -limit;
    }
}

/* The catch: the ball is a box and the paddle is a slab, so the catch is the y-plane crossing
   plus an x and z overlap. The crossing is read against the ball's position BEFORE the step as
   well as after it, which is what makes a step that jumped past the paddle's top still a catch
   rather than a ball that fell through it. */
static void xud_breakout_catch(XudBreakoutBall* ball, XudBreakoutPaddle* paddle) {
    const float plane = paddle->centre.y + paddle->half.y;
    const float bottom = ball->position.y - ball->half.y;
    const float previous = bottom - ball->velocity.y;

    if ((ball->velocity.y >= 0.0f) || (bottom > plane) || (previous <= plane)) {
        return;
    }
    if ((fabsf(ball->position.x - paddle->centre.x) > (ball->half.x + paddle->half.x))
        || (fabsf(ball->position.z - paddle->centre.z) > (ball->half.z + paddle->half.z))) {
        return;
    }

    ball->position.y = plane + ball->half.y;
    ball->velocity.y = -ball->velocity.y;
    ball->velocity.x += ((ball->position.x - paddle->centre.x) / paddle->half.x) * XUD_BREAKOUT_BALL_SPEED_X_MIN;
    xud_breakout_clamp_x_velocity(ball);
}

/* One step of the game, in the order the step is honest in: the paddle moves first so it is
   where it is when the ball arrives, the ball moves one axis at a time (the example's own
   simplification of a swept test - see the note above), the catch is read after the move so a
   ball that crossed the paddle's plane this step is caught, and a brick is read last so a ball
   that both caught and hit is caught first. The brick index returned is what the frame loop
   prints; XUD_BREAKOUT_NO_BRICK means nothing was removed this step. */
static uint32_t xud_breakout_step(XudBreakoutBall* ball, XudBreakoutPaddle* paddle, XudBreakoutBrick* bricks) {
    xud_breakout_step_paddle(paddle, ball);

    ball->position.x += ball->velocity.x;
    xud_breakout_bounce_wall(&ball->position.x, &ball->velocity.x, ball->half.x, XUD_BREAKOUT_HALF_X);

    ball->position.y += ball->velocity.y;
    if ((ball->position.y + ball->half.y) > XUD_BREAKOUT_HALF_Y) {
        ball->position.y = XUD_BREAKOUT_HALF_Y - ball->half.y;
        ball->velocity.y = -ball->velocity.y;
    }

    ball->position.z += ball->velocity.z;
    xud_breakout_bounce_wall(&ball->position.z, &ball->velocity.z, ball->half.z, XUD_BREAKOUT_HALF_Z);

    xud_breakout_catch(ball, paddle);

    return xud_breakout_hit_brick(ball, bricks);
}

/* The bottom of the box is OPEN, and that is what makes this a game rather than a demo: a ball
   that crossed the paddle's plane outside the paddle's x extent keeps falling, and reaching the
   bottom of the box is a miss. A miss puts the ball back where it started rather than ending the
   run, because ending it would make the example a run that stops after the first miss and the
   interesting half of the run is the half after it. */
static uint32_t xud_breakout_missed(XudBreakoutBall* ball) { return ((ball->position.y - ball->half.y) < -XUD_BREAKOUT_HALF_Y) ? 1u : 0u; }

/*-----------------------------------------------------------------------------------------------
 * The frame loop
 *-----------------------------------------------------------------------------------------------*/

int main(void) {
    XudContext* context = NULL;
    XudBreakoutBall ball;
    XudBreakoutPaddle paddle;
    XudBreakoutBrick bricks[XUD_BREAKOUT_BRICK_COUNT];
    uint32_t frame = 0u;
    uint32_t misses = 0u;
    uint32_t cleared = 0u;
    XudResult result = xud_init_context(&context, XUD_BREAKOUT_WINDOW_WIDTH, XUD_BREAKOUT_WINDOW_HEIGHT);

    printf("breakout_3d: xud_init_context(%u, %u) -> %s (xudlibc %s)\n", XUD_BREAKOUT_WINDOW_WIDTH, XUD_BREAKOUT_WINDOW_HEIGHT,
        xud_result_name(result), XUD_VERSION);
    if (result != XUD_OK) {
        return (result == XUD_UNSUPPORTED) ? 0 : 1;
    }

    /* The field, laid out once: the column and the row place a brick in x and y, and the layer
       is the diagonal of that pair rather than a third loop - which is what staggers the field
       across depth instead of stacking three walls one behind the other, where the nearest wall
       would hide the two behind it and the depth would be decoration again. */
    for (uint32_t index = 0u; index < (uint32_t)XUD_BREAKOUT_BRICK_COUNT; index++) {
        const uint32_t col = index % (uint32_t)XUD_BREAKOUT_BRICK_COLS;
        const uint32_t row = index / (uint32_t)XUD_BREAKOUT_BRICK_COLS;
        const uint32_t layer = (col + row) % (uint32_t)XUD_BREAKOUT_BRICK_LAYERS;
        const float centreX = ((float)col - (((float)XUD_BREAKOUT_BRICK_COLS - 1.0f) * 0.5f)) * XUD_BREAKOUT_BRICK_SPAN_X;
        const float centreY = ((((float)row - (((float)XUD_BREAKOUT_BRICK_ROWS - 1.0f) * 0.5f)) * XUD_BREAKOUT_BRICK_SPAN_Y)
            + XUD_BREAKOUT_BRICK_ROW_LIFT_F);
        const float centreZ = (((float)layer - (((float)XUD_BREAKOUT_BRICK_LAYERS - 1.0f) * 0.5f)) * XUD_BREAKOUT_BRICK_Z_STEP);

        bricks[index] = (XudBreakoutBrick) { .centre = (XudVector3) { .x = centreX, .y = centreY, .z = centreZ },
            .half = (XudVector3) { .x = XUD_BREAKOUT_BRICK_HALF_X, .y = XUD_BREAKOUT_BRICK_HALF_Y, .z = XUD_BREAKOUT_BRICK_HALF_Z },
            .colour = (XudColor) { .r = (uint8_t)(60u + (index * 30u)),
                .g = (uint8_t)(180u + (index * 18u)),
                .b = (uint8_t)(220u - (index * 24u)),
                .a = 255u },
            .alive = 1u };
    }

    paddle = (XudBreakoutPaddle) { .centre = (XudVector3) { .x = 0.0f, .y = XUD_BREAKOUT_PADDLE_Y, .z = 0.0f },
        .half = (XudVector3) { .x = XUD_BREAKOUT_PADDLE_HALF_X, .y = XUD_BREAKOUT_PADDLE_HALF_Y, .z = XUD_BREAKOUT_PADDLE_HALF_Z },
        .speed = XUD_BREAKOUT_PADDLE_SPEED };

    xud_breakout_reset_ball(&ball, 0u);

    for (frame = 0u; frame < XUD_BREAKOUT_FRAMES; frame++) {
        const uint32_t hit = xud_breakout_step(&ball, &paddle, bricks);
        uint32_t alive = 0u;

        if (hit != XUD_BREAKOUT_NO_BRICK) {
            cleared++;
            printf("frame %u: the ball cleared brick %u\n", frame, hit);
        }
        if (xud_breakout_missed(&ball) != 0u) {
            misses++;
            printf("frame %u: the ball fell out at the bottom, miss %u\n", frame, misses);
            xud_breakout_reset_ball(&ball, frame);
        }

        result = xud_begin_drawing(context);
        if (result != XUD_OK) {
            printf("frame %u: xud_begin_drawing -> %s\n", frame, xud_result_name(result));
            break;
        }

        /* The back plane first, and first on purpose: it is the farthest thing in the scene and
           the one quad that makes the depth legible at all, because the playfield's front
           cross-section projects LARGER than it does - the back plane is at the far distance and
           the ball at the front is at the near one, so a ball at the front is drawn outside the
           back plane's own edges and over it. The library never sorts and the opaque variant
           needs no order, so this is the honest habit rather than a requirement. */
        result = xud_breakout_draw_quad(context, (XudVector3) { .x = 0.0f, .y = 0.0f, .z = -XUD_BREAKOUT_HALF_Z }, XUD_BREAKOUT_HALF_X,
            XUD_BREAKOUT_HALF_Y, (XudColor) { .r = 18u, .g = 22u, .b = 40u, .a = 255u });
        if (result != XUD_OK) {
            printf("frame %u: the back plane -> %s\n", frame, xud_result_name(result));
            break;
        }

        for (uint32_t index = 0u; index < (uint32_t)XUD_BREAKOUT_BRICK_COUNT; index++) {
            if (bricks[index].alive == 0u) {
                continue;
            }
            result
                = xud_breakout_draw_quad(context, bricks[index].centre, bricks[index].half.x, bricks[index].half.y, bricks[index].colour);
            if (result != XUD_OK) {
                printf("frame %u: brick %u -> %s\n", frame, index, xud_result_name(result));
                break;
            }
        }
        if (result != XUD_OK) {
            break;
        }

        /* The paddle's front face, at the front of the slab rather than at its centre: the slab
           spans the whole depth, so the face is the nearest thing the paddle has and the face
           that is drawn is the face the player would be watching. */
        result = xud_breakout_draw_quad(context,
            (XudVector3) { .x = paddle.centre.x, .y = paddle.centre.y, .z = paddle.centre.z + paddle.half.z }, paddle.half.x, paddle.half.y,
            (XudColor) { .r = 240u, .g = 240u, .b = 240u, .a = 255u });
        if (result != XUD_OK) {
            printf("frame %u: the paddle -> %s\n", frame, xud_result_name(result));
            break;
        }

        result = xud_breakout_draw_quad(
            context, ball.position, ball.half.x, ball.half.y, (XudColor) { .r = 255u, .g = 60u, .b = 60u, .a = 255u });
        if (result != XUD_OK) {
            printf("frame %u: the ball -> %s\n", frame, xud_result_name(result));
            break;
        }

        result = xud_end_drawing(context);
        if (result != XUD_OK) {
            printf("frame %u: xud_end_drawing -> %s\n", frame, xud_result_name(result));
            break;
        }

        for (uint32_t index = 0u; index < (uint32_t)XUD_BREAKOUT_BRICK_COUNT; index++) {
            if (bricks[index].alive != 0u) {
                alive++;
            }
        }
        if ((frame % XUD_BREAKOUT_REPORT_EVERY) == 0u) {
            printf("frame %u: bricks %u/%u, misses %u, ball x %+.3f y %+.3f z %+.3f, paddle x %+.3f\n", frame, alive,
                (uint32_t)XUD_BREAKOUT_BRICK_COUNT, misses, ball.position.x, ball.position.y, ball.position.z, paddle.centre.x);
        }

        if (alive == 0u) {
            printf("frame %u: the field is cleared, and the game has nothing left to clear\n", frame);
            break;
        }
    }

    printf("breakout_3d: %u frames, %u/%u bricks cleared, %u misses\n", frame, cleared, (uint32_t)XUD_BREAKOUT_BRICK_COUNT, misses);

    const XudResult closed = xud_close_context(&context);
    printf("xud_close_context -> %s\n", xud_result_name(closed));

    return (closed != XUD_OK) ? 1 : 0;
}
