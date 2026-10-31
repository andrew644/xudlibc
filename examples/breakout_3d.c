/***********************************************************************************************
 *
 *   breakout_3d.c - a 3D breakout: a ball, a paddle, a field of bricks in three depth layers
 *
 *   Owns : one game - the ball's motion and its bounces, the brick field and the one brick a
 *          contact removes, the paddle and the hand that moves it, the Escape that ends the run,
 *          the one camera the scene is drawn with, and the frame loop that draws the scene every
 *          frame and steps the simulation at a fixed rate in wall time. Nothing else: no batch,
 *          no buffer, no projection of its own.
 *   Layer: examples. Public API (xud.h) plus stdio, math.h's fabsf and GLFW's key state - and
 *          ONE internal header, xud_core.h, for the one field the input needs: the GLFWwindow*
 *          the context owns. Everything else the game calls is public - xud_init_context(),
 *          xud_begin_camera_3d(), xud_begin_drawing(), xud_draw_triangle_3d() for every quad,
 *          xud_end_drawing(), xud_end_camera_3d() and
 *          xud_close_context() - and the reason that one exception is here, and why it is the
 *          narrowest form of it, is the input note below.
 *
 *   The game is 3D in the one way this library can make it 3D today: every object has a z, the
 *   ball travels in x, y and z, the bricks sit at three different depths, and the depth buffer
 *   the render pass owns resolves which of them is nearer. What makes that depth VISIBLE is the
 *   camera below, and the camera is the library's - the example states the eye, the aperture and
 *   the two planes, and the camera rung turns them into the one matrix a frame is drawn with,
 *   which is the one thing a reader should understand before changing anything here.
 *
 *   The camera + transform rung is OPEN now, and this example is its first GPU consumer: the pair
 *   xud_begin_camera_3d()/xud_end_camera_3d() puts the scene's one camera in and takes it out, and
 *   the library's camera rung (src/xud/xud_camera.h/.c, landed 2026-10-31 as S28) builds the
 *   camera's basis, the symmetric frustum at the extent a frame renders at and the product of the
 *   two into set 0's viewProj every frame. A shape's coordinates are therefore WORLD coordinates
 *   and the divide happens in the shader, which is what makes the depth buffer's answer the honest
 *   one: the back plane at the far depth and the ball at the near one are resolved by the
 *   pipeline's LESS compare rather than by the order the example draws them in. The quad's depth
 *   is still constant across it, because every quad below lies in a plane of constant z - a quad
 *   that spanned two depths would want the rasteriser's perspective-correct interpolation, which
 *   is a property of the shapes the cube and the sphere calls place rather than of these.
 *
 *   Two rungs of the library are todo.md's Later items rather than open bugs, and this
 *   example stands in for each of them in the smallest way that keeps the example honest about
 *   which half is whose:
 *
 *     - No input wrapper in xud (todo.md's Input wrapper item is a Later rung, not an open bug),
 *       so the example reads GLFW directly: xud_breakout_read_input() polls and queries the key
 *       state of the window the CONTEXT already owns. It is the temporary half of the input rung
 *       and it is deliberately the narrowest form of it - the example creates no window of its
 *       own (a second GLFW window would be a second surface competing for the same session, and
 *       the game would be played on a window nothing draws to), and it reaches the context's own
 *       window for one type, the GLFWwindow*, and nothing else. The paddle is the player's while
 *       a key is held and the game's own autopilot while none is, which is what keeps the example
 *       a game that plays itself when nobody is at a keyboard AND a game the player can play.
 *       glfwPollEvents() is not decoration: GLFW's Wayland backend receives key events through an
 *       epoll loop that only glfwPollEvents() and its wait forms drain (wl_window.c 2938 ->
 *       handleEvents), so a frame that never polls is a frame whose key state can never change,
 *       and nothing else in this tree polls GLFW.
 *
 *     - No clock in xud (todo.md's Clock item is a Later rung, not an open bug), so the example
 *       borrows GLFW's timer: glfwGetTime() is the same temporary rung as the key state, and it is
 *       what paces the game to a fixed step rate in wall time rather than one step per frame. It
 *       has to, because GLFW's window runs as fast as it can - measured here at about 3200 frames
 *       a second - and a game that stepped once per frame would be a game whose ball crosses the
 *       field fifty times a second, which is a blur a hand cannot play and cannot time an Escape
 *       against. The game's own rules stay deterministic: no randomness, no filesystem, one fixed
 *       starting layout, so two runs with nobody at the keyboard play the same game - though not
 *       for the same number of steps, which is now wall time's.
 *
 *   One public shape call is used here and only one: xud_draw_triangle_3d(). The reason is the
 *   budget below rather than a projection above - with the camera in the frame a cube COULD be
 *   placed for the ball, but the frame's eight quads are 48 vertices and 48 indices of the batch's
 *   64 and 96 and the cube's own 24 and 36 do not fit beside them, so the ball stays a quad and
 *   the sphere's 90 indices stay a claim rather than a spend. A frame that wanted more would be
 *   REFUSED by the batch rather than truncated, and every draw below checks its answer for exactly
 *   that reason.
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

#include "xud.h" /* Required for: the whole public API the game is written against */
#include "xud_core.h" /* Required for: struct XudContext's fields, and specifically the ONE field
                        below it that the input needs - context->window, whose GLFWwindow* is what
                        glfwGetKey is called on. This is the example's single exception to "user
                        code includes xud.h only", and it is the temporary input rung's own:
                        xud_core.h is the whole ladder, so it brings xud_window.h with it, and
                        xud_window.h brings <GLFW/glfw3.h> AFTER <vulkan/vulkan.h>, which is the
                        order GLFW's own header warns about (glfw3.h's Vulkan declarations sit
                        inside #if defined(VK_VERSION_1_0), so including <GLFW/glfw3.h> here
                        directly, first, would drop them and name GLFW's include dir ourselves -
                        the vendored tree's include dir is already on the path the xud target
                        exports PUBLIC). Nothing below xud.h is CALLED from here: GLFW's objects
                        are already in the xud archive (CMakeLists.txt, glfw_vendor as an OBJECT
                        target), so the only new link fact is glfwPollEvents and glfwGetKey. */

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

/* The example's camera, in the library's own terms: the eye sits on +Z looking down -Z at the
   field's centre, FOCUS_D is the eye-to-focus DISTANCE and the plane at that distance is the one
   a world unit maps 1:1 in y, so the aperture that buys that framing is the one whose focal
   length (the cotangent of its half, which is what the camera rung computes) IS FOCUS_D - stated
   below as a derived macro rather than as a chosen number, because the fit checks below are
   stated against FOCUS_D in integers and an aperture stated in degrees would be a float claim
   they could not make. NEAR_D and FAR_D are the camera's two planes, and the depth map is the
   library's hyperbolic one onto [0,1] rather than a linear map of the example's: every quad
   below still has one depth, but nothing has to be exact about the map any more, because the
   pipeline's LESS compare and the clear value of 1.0 are what decide which of two surfaces is
   seen. */
#define XUD_BREAKOUT_EYE_Z_MM 2400
#define XUD_BREAKOUT_FOCUS_D_MM 2400
#define XUD_BREAKOUT_NEAR_D_MM 1500
#define XUD_BREAKOUT_FAR_D_MM 3500

/* The playfield box, in world units: the ball bounces inside it, the brick field sits inside it
   and its far wall is the back plane the frame loop draws. */
#define XUD_BREAKOUT_HALF_X_MM 1000
#define XUD_BREAKOUT_HALF_Y_MM 620
#define XUD_BREAKOUT_HALF_Z_MM 500

/* The world half-extent VISIBLE at a distance from the eye, in milli-units: the pinhole's focal
   length is FOCUS_D world units, so the visible half-height at a distance d is d/FOCUS_D world
   units - which GROWS with the distance, the direction a nearer part of the scene has to be
   checked against. The integer division rounds DOWN, which is the safe direction for an assert
   that a shape FITS inside what is visible. */
#define XUD_BREAKOUT_VISIBLE_HALF_Y_MM(distance) ((distance) * XUD_BREAKOUT_UNIT / (XUD_BREAKOUT_FOCUS_D_MM))
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

/* The aperture the fit asserts above are stated against, DERIVED from FOCUS_D rather than chosen:
   the camera rung takes a vertical aperture in DEGREES and its focal length is the cotangent of
   the half of it, so the aperture whose focal length IS FOCUS_D world units - the framing a world
   unit maps 1:1 in y at the focus plane - is 2*atan(1/FOCUS_D) degrees. Runtime arithmetic rather
   than an assert, because a float static_assert is a -Wpedantic error here and the fit checks
   above are the integer form of the same fact, stated against the focal length rather than against
   an aperture. */
#define XUD_BREAKOUT_DEGREES_PER_RADIANS (180.0f / 3.1415926535897932f)
#define XUD_BREAKOUT_FOVY_DEG ((2.0f * atanf(1.0f / XUD_BREAKOUT_FOCUS_D)) * XUD_BREAKOUT_DEGREES_PER_RADIANS)
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

/* The step rate the game plays at, in wall time, and the reason it is in wall time at all: see
   the note above. One step per frame, SKIPPED until the accumulator crosses this interval, is what
   makes the game playable at the same speed on any machine - and its honest limit is that a machine
   slower than this rate runs the game slower rather than catching steps up. */
#define XUD_BREAKOUT_STEP_SECONDS (1.0 / 60.0)

/* The run's fallback cap, in seconds: the run's real end is the player holding Escape (see the
   input note above), and this is the half that keeps a run with nobody at a keyboard - a CI run,
   a headless one, a terminal whose window is in front of nobody - from hanging forever. glfwGet
   Time() measures from the window's creation, which xud_init_context() does, so this is the run's
   own time rather than the process's. Overridable at build time (-DXUD_BREAKOUT_MAX_SECONDS=N).
   Sixty seconds is enough for a hand to reach Escape and enough for the game to clear the field
   several times, which is what the round counter below is for. */
#ifndef XUD_BREAKOUT_MAX_SECONDS
#define XUD_BREAKOUT_MAX_SECONDS 60.0
#endif
#define XUD_BREAKOUT_REPORT_EVERY 60u

/*-----------------------------------------------------------------------------------------------
 * The game's own state
 *-----------------------------------------------------------------------------------------------*/

/* What one frame of the player's hand asks for: whether to end the run, and which way the paddle
   should go. A direction of 0 means no key was held this frame, which is the case the autopilot
   below answers rather than a third rule. */
typedef struct XudBreakoutInput {
    uint32_t escapeWanted;
    int32_t paddleDirection; /* -1 left, 0 none, +1 right */
} XudBreakoutInput;

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
 * The quad - the one shape this example draws, in world space
 *-----------------------------------------------------------------------------------------------*/

/* One quad, drawn as the two triangles a quad is. The four corners are the caller's centre plus
   or minus the caller's half-extents in WORLD space, and nothing here projects them: the camera
   the frame loop put in set 0's block is what divides them by the distance from the eye, in the
   shader, once per vertex rather than once per corner here. All four still share the caller's z,
   which is the condition the note at the top of the file states for the reason it now states it:
   a quad of constant depth is a quad whose depth the rasteriser can interpolate exactly, and the
   two triangles are (0,1,2) and (0,2,3) over corners in counter-clockwise order seen from +Z -
   the camera above is on +Z looking down -Z, so that is the order seen FROM THE CAMERA, which is
   the winding the pipeline's front face names; the pipeline culls nothing, so the winding is the
   shape's own consistency rather than a gate the draw obeys. */
static XudResult xud_breakout_draw_quad(XudContext* context, XudVector3 centre, float halfX, float halfY, XudColor colour) {
    const XudVector3 c0 = (XudVector3) { .x = centre.x - halfX, .y = centre.y - halfY, .z = centre.z };
    const XudVector3 c1 = (XudVector3) { .x = centre.x + halfX, .y = centre.y - halfY, .z = centre.z };
    const XudVector3 c2 = (XudVector3) { .x = centre.x + halfX, .y = centre.y + halfY, .z = centre.z };
    const XudVector3 c3 = (XudVector3) { .x = centre.x - halfX, .y = centre.y + halfY, .z = centre.z };

    XudResult result = xud_draw_triangle_3d(context, c0, c1, c2, colour);
    if (result != XUD_OK) {
        return result;
    }

    return xud_draw_triangle_3d(context, c0, c2, c3, colour);
}

/* How many bricks are still in the field - the frame loop's own count, read from the bricks' own
   alive flags rather than carried beside them, so the count cannot drift from what the field is. */
static uint32_t xud_breakout_count_alive(XudBreakoutBrick* bricks) {
    uint32_t alive = 0u;
    uint32_t index = 0u;

    for (index = 0u; index < (uint32_t)XUD_BREAKOUT_BRICK_COUNT; index++) {
        if (bricks[index].alive != 0u) {
            alive++;
        }
    }

    return alive;
}

/* A new round: the same field, the same six bricks, re-armed. The reason the game does this
   rather than ending is that the run's end is the player's Escape now, not the field's being
   empty - an empty field is a win, and a win that ends the run would make the longest part of the
   example, the part after the win, unreachable. The ball is reset beside it so a new round starts
   from the layout rather than from wherever the last one left it. */
static void xud_breakout_rearm_field(XudBreakoutBrick* bricks) {
    uint32_t index = 0u;

    for (index = 0u; index < (uint32_t)XUD_BREAKOUT_BRICK_COUNT; index++) {
        bricks[index].alive = 1u;
    }
}

/*-----------------------------------------------------------------------------------------------
 * The input rung - GLFW read directly, see the note at the top of the file
 *-----------------------------------------------------------------------------------------------*/

/* The window the CONTEXT owns, reached once rather than per frame, and guarded rather than assumed
   non-NULL: xud_init_context() answers XUD_OK only for a context that owns a window (the surface
   is a rung every XUD_OK context owns), so NULL here is not an expected state - and answering it
   rather than assuming it is what keeps a NULL window from being a glfwGetKey call on NULL. */
static GLFWwindow* xud_breakout_glfw_window(XudContext* context) {
    if ((context == NULL) || (context->window == NULL)) {
        return NULL;
    }

    return context->window->glfwWindow;
}

/* One frame's read of the player's hand. glfwPollEvents() runs first and every frame: GLFW's Wayland
   backend receives key events through an epoll loop that only glfwPollEvents() and its wait forms
   drain (wl_window.c 2938 -> handleEvents), so polling is what makes glfwGetKey able to answer
   GLFW_PRESS at all, and nothing else in this tree polls GLFW. The escape test is "not RELEASE"
   rather than "== PRESS" because glfwGetKey answers GLFW_REPEAT for a key held past the frame it
   was first read on - a test on PRESS alone would let a held Escape keep the run going on the
   frames between two PRESS reads, and the run would end on a frame chosen by the key repeat rate
   rather than on the frame the player pressed it. */
static XudBreakoutInput xud_breakout_read_input(GLFWwindow* glfwWindow) {
    XudBreakoutInput input = (XudBreakoutInput) { .escapeWanted = 0u, .paddleDirection = 0 };

    if (glfwWindow == NULL) {
        return input;
    }

    glfwPollEvents();
    input.escapeWanted = (glfwGetKey(glfwWindow, GLFW_KEY_ESCAPE) != GLFW_RELEASE) ? 1u : 0u;
    {
        const uint32_t left
            = (glfwGetKey(glfwWindow, GLFW_KEY_LEFT) != GLFW_RELEASE) || (glfwGetKey(glfwWindow, GLFW_KEY_A) != GLFW_RELEASE);
        const uint32_t right
            = (glfwGetKey(glfwWindow, GLFW_KEY_RIGHT) != GLFW_RELEASE) || (glfwGetKey(glfwWindow, GLFW_KEY_D) != GLFW_RELEASE);

        input.paddleDirection = (left != 0u) ? -1 : ((right != 0u) ? 1 : 0);
    }

    return input;
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
/* The paddle's two hands, and the reason both exist: the player's while a key is held (the
   paddle's own speed, the same bound the autopilot uses, so a key held the wrong way cannot make
   the paddle faster than the game thought it was) and the game's own chase while none is, which
   is what keeps an unattended run a game rather than a ball that always falls out. The clamp is
   the same either way, so neither hand can push the paddle out of the box. */
static void xud_breakout_step_paddle(XudBreakoutPaddle* paddle, XudBreakoutBall* ball, int32_t direction) {
    const float delta = ball->position.x - paddle->centre.x;
    const float chase = (delta > paddle->speed) ? paddle->speed : ((delta < -paddle->speed) ? -paddle->speed : delta);
    const float step = (direction != 0) ? ((direction < 0) ? -paddle->speed : paddle->speed) : chase;
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
static uint32_t xud_breakout_step(XudBreakoutBall* ball, XudBreakoutPaddle* paddle, XudBreakoutBrick* bricks, int32_t paddleDirection) {
    xud_breakout_step_paddle(paddle, ball, paddleDirection);

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
    uint32_t rounds = 1u;
    uint32_t escapeWanted = 0u;
    uint32_t timedOut = 0u;
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

    /* The window the CONTEXT owns, reached once rather than per frame: the context owns exactly
       one and its own note in xud_core.h says a window is a rung every XUD_OK context owns, so
       NULL here is not expected - and it is answered rather than assumed, because a NULL window
       would be a glfwGetKey call on NULL. */
    GLFWwindow* glfwWindow = xud_breakout_glfw_window(context);
    if (glfwWindow == NULL) {
        printf("breakout_3d: the context owns no window to read input from, so the run is the autopilot's alone\n");
    }

    /* The run's clock and its step accumulator. glfwGetTime() is GLFW's own timer (clock_gettime
       behind it, and the window's creation is where it starts), and it is the same temporary rung
       as the key state - see the note above for why the game needs a rate at all. */
    double last = glfwGetTime();
    double accumulator = 0.0;
    uint32_t steps = 0u;

    /* The scene's one camera, put in for the whole run rather than per frame: the pair is raylib's
       BeginCamera3D()/EndCamera3D() shape, the record is the caller's and the library copies it
       into the renderer that owns the frame loop, so the frames drawn between the two calls are
       the frames drawn with it. The eye is on +Z looking down -Z at the field's centre (the same
       eye the fit asserts above are stated against), the aperture is the derived one whose focal
       length IS FOCUS_D, and the two planes are the game's own depth budget - the asserts above
       are what say every vertex of the scene sits between them, which is what says the whole depth
       range lands across the whole scene rather than across a slice of it. */
    const XudCamera3D camera = (XudCamera3D) { .position = (XudVector3) { .x = 0.0f, .y = 0.0f, .z = XUD_BREAKOUT_EYE_Z },
        .target = (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 0.0f },
        .up = (XudVector3) { .x = 0.0f, .y = 1.0f, .z = 0.0f },
        .fovy = XUD_BREAKOUT_FOVY_DEG,
        .near = XUD_BREAKOUT_NEAR_D,
        .far = XUD_BREAKOUT_FAR_D };

    result = xud_begin_camera_3d(context, camera);
    printf("breakout_3d: xud_begin_camera_3d -> %s\n", xud_result_name(result));
    if (result != XUD_OK) {
        const XudResult closed = xud_close_context(&context);

        printf("xud_close_context -> %s\n", xud_result_name(closed));
        return 1;
    }

    for (frame = 0u;; frame++) {
        const XudBreakoutInput input = xud_breakout_read_input(glfwWindow);
        uint32_t alive = 0u;
        uint32_t hit = XUD_BREAKOUT_NO_BRICK;
        uint32_t stepped = 0u;

        if (input.escapeWanted != 0u) {
            printf("frame %u: escape is held, and the run stops here\n", frame);
            escapeWanted = 1u;
            break;
        }
        if (glfwGetTime() >= XUD_BREAKOUT_MAX_SECONDS) {
            printf("frame %u: the fallback time cap is reached, and the run stops here\n", frame);
            timedOut = 1u;
            break;
        }

        /* The accumulator GLFW's own timer feeds: one interval of wall time is one step of the
           game, and the frame that arrives before the interval is up simply does not step. */
        {
            const double now = glfwGetTime();

            accumulator += (now - last);
            last = now;
        }

        if (accumulator >= XUD_BREAKOUT_STEP_SECONDS) {
            accumulator -= XUD_BREAKOUT_STEP_SECONDS;
            stepped = 1u;
        }

        if (stepped != 0u) {
            steps++;
            hit = xud_breakout_step(&ball, &paddle, bricks, input.paddleDirection);

            if (hit != XUD_BREAKOUT_NO_BRICK) {
                cleared++;
                printf("step %u: the ball cleared brick %u\n", steps, hit);
            }
            if (xud_breakout_missed(&ball) != 0u) {
                misses++;
                printf("step %u: the ball fell out at the bottom, miss %u\n", steps, misses);
                xud_breakout_reset_ball(&ball, steps);
            }

            alive = xud_breakout_count_alive(bricks);
            if (alive == 0u) {
                rounds++;
                printf("step %u: the field is cleared, round %u re-arms it\n", steps, rounds);
                xud_breakout_rearm_field(bricks);
                xud_breakout_reset_ball(&ball, steps);
                alive = (uint32_t)XUD_BREAKOUT_BRICK_COUNT;
            }
            if ((steps % XUD_BREAKOUT_REPORT_EVERY) == 0u) {
                printf("step %u: bricks %u/%u, misses %u, ball x %+.3f y %+.3f z %+.3f, paddle x %+.3f\n", steps, alive,
                    (uint32_t)XUD_BREAKOUT_BRICK_COUNT, misses, ball.position.x, ball.position.y, ball.position.z, paddle.centre.x);
            }
        }

        result = xud_begin_drawing(context);
        if (result != XUD_OK) {
            printf("frame %u: xud_begin_drawing -> %s\n", frame, xud_result_name(result));
            break;
        }

        /* The back plane first, and first on purpose for a reason that is no longer the reason it
           used to be: it is the farthest thing in the scene and the one quad that makes the depth
           legible at all, and the playfield's front cross-section does project LARGER than it,
           because the back plane is at the far distance and the ball at the near one, so a ball
           at the front is drawn outside the back plane's own edges. But the ORDER is no longer
           what resolves which of the two is seen - the camera rung's matrix maps the near plane
           to 0 and the far plane to 1, and the pipeline's LESS compare against the clear value of
           1.0 decides, so a frame that drew the ball before the back plane would show the same
           game. The library never sorts and the opaque variant needs no order, so this stays the
           honest habit rather than a requirement. */
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
    }

    /* The camera taken away after the run rather than left live: the pair is a scope, and the
       frames drawn after here are drawn with no camera at all, which is the identity matrix the
       descriptor rung opens set 0's block with - nothing of this run's camera state reaches past
       here, which is the half of the pair's contract the example can show. */
    result = xud_end_camera_3d(context);
    if (result != XUD_OK) {
        printf("breakout_3d: xud_end_camera_3d -> %s\n", xud_result_name(result));
    }

    printf("breakout_3d: %u frames, %u steps, %u rounds, %u bricks cleared, %u misses, ended by %s\n", frame, steps, rounds, cleared,
        misses, (escapeWanted != 0u) ? "escape" : ((timedOut != 0u) ? "the fallback time cap" : "a draw call refusing"));

    const XudResult closed = xud_close_context(&context);
    printf("xud_close_context -> %s\n", xud_result_name(closed));

    return (closed != XUD_OK) ? 1 : 0;
}
