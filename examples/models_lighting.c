/***********************************************************************************************
 *
 *   models_lighting.c - the three kinds of light a frame is shaded by, in one scene
 *
 *   Owns : one context, one camera record, and the frame's light table. The scene is a row of
 *          spheres and their pedestals under three lights - a SUN raking from above-left, a
 *          POINT circling the middle at walking height, and a SPOT throwing a cone down onto
 *          the centre sphere from a corner - and the whole point of the file is that the point
 *          and the spot are VISIBLE: their gizmos are small bright cubes riding the light's own
 *          position, the point's pool travels across the pedestals as it orbits, and the spot's
 *          cone edge crosses the floor. Layer: examples. Public API only.
 *
 *   Shape : this is the shape of raylib's models_lighting showcase - one scene, a few lights,
 *          keys that move their numbers - with the three kinds this library's table carries
 *          instead of raylib's single point light: every kind of xud_set_lights() has a seat.
 *          Nothing here reads the keys every frame into new geometry; a key nudges ONE number
 *          of the table and the next xud_set_lights() republishes the whole table with it. The
 *          orbit is the clock's, not an input's - the same call reads the clock and moves the
 *          point, so the run is a function of wall time alone.
 *
 *   Missing : the house model: raylib's showcase puts its lights on a loaded glTF house; this
 *          scene is spheres, boxes and the grid, which are the same shapes the other examples
 *          draw and need no resources on disk.
 *
 *          The shadow map is not missing: the sun in the table casts one (todo.md S2.5), so the
 *          floor under each sphere and pedestal is darkened by its own occluder, and the far
 *          side of a sphere goes dark twice over - Lambert and the sun's own map.
 *
 *   Controls: ESC quits. UP and DOWN lengthen and shorten the POINT's range - the distance its
 *          pool stops lighting. LEFT and RIGHT open and close the SPOT's outer cone. A and Z
 *          raise and lower the ambient floor; at Z's floor the sun is the only thing lighting
 *          the room, and the point's pool glows hardest.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include <math.h>  /* Required for: sinf, cosf, sqrtf - the orbit, and the cone axis pointing from the lamp to the centre */
#include <stdio.h> /* Required for: printf */

#include "xud.h" /* Required for: the light table and its pair, the camera, the frame pair, the sphere and box and grid calls, the clock, the key queries and the context pair */

#define LOGNAME "models_lighting"

#define XUD_LIGHTING_WIDTH 900u
#define XUD_LIGHTING_HEIGHT 500u

/* The orbit: the point light walks this circle at this height, one lap every fourteen seconds. */
#define XUD_LIGHTING_ORBIT_RADIUS 3.5f
#define XUD_LIGHTING_ORBIT_HEIGHT 1.6f
#define XUD_LIGHTING_ORBIT_SECONDS 14.0f

/* The spot's lamp: one corner, high, aimed at the centre of the row. The direction is written
   per frame as (0, 0, 0) minus this position - the axis points FROM the lamp.
*/
#define XUD_LIGHTING_SPOT_X 6.0f
#define XUD_LIGHTING_SPOT_Y 6.5f
#define XUD_LIGHTING_SPOT_Z 5.0f

static void xud_lighting_error(const char* what, XudResult result) { printf(LOGNAME ": %s -> %d\n", what, (int)result); }

/* The scene: the grid, the row of pedestals and spheres, and the two gizmos. Every append is
   refused wholesale when the batch's frame capacity is short (xud_batch.h: the capacity GROWS,
   one frame at a time, from a small initial capacity - xud_draw_cube returns XUD_OUT_OF_MEMORY
   and the next xud_begin_drawing() takes the hint), so the caller below counts such frames
   rather than quitting the run over one.
*/
static XudResult xud_lighting_scene(XudContext* context, XudVector3 pointPosition, XudVector3 spotPosition) {
    static const float xudLightingRowX[5] = { -6.0f, -3.0f, 0.0f, 3.0f, 6.0f };
    static const XudColor xudLightingRowColor[5] = { { 200u, 90u, 90u, 255u }, { 90u, 160u, 200u, 255u }, { 230u, 210u, 110u, 255u },
        { 140u, 200u, 130u, 255u }, { 180u, 130u, 200u, 255u } };
    XudResult step = XUD_OK;

    step = xud_draw_grid(
        context, (XudVector3) { 0.0f, 0.0f, 0.0f }, 20u, 1.0f, (XudColor) { 128u, 128u, 128u, 255u }, (XudColor) { 64u, 64u, 64u, 255u });

    /* The row: five pedestals, one sphere on each, the middle sphere the biggest - the spot's
       target. The colours are the meshes' own; what each one looks like this frame is the
       table's job.
    */
    for (uint32_t index = 0u; (index < 5u) && (step == XUD_OK); index++) {
        const float pedestalHeight = 0.5f + (0.5f * (float)(index % 3u));
        step = xud_draw_cube(context, (XudVector3) { xudLightingRowX[index], pedestalHeight * 0.5f, 0.0f }, 1.2f, pedestalHeight, 1.2f,
            (XudColor) { 150u, 150u, 155u, 255u });
        if (step == XUD_OK) {
            step = xud_draw_sphere(context, (XudVector3) { xudLightingRowX[index], pedestalHeight + ((index == 2u) ? 0.9f : 0.55f), 0.0f },
                (index == 2u) ? 0.9f : 0.55f, xudLightingRowColor[index]);
        }
    }

    /* The gizmos: a cube at each of the two positional lights' own positions, in that light's
       colour at full intensity - the point's own cube is the brightest green thing on screen
       because its light is one radius away from it.
    */
    if (step == XUD_OK) {
        step = xud_draw_cube(context, (XudVector3) { pointPosition.x, pointPosition.y + 0.7f, pointPosition.z }, 0.35f, 0.35f, 0.35f,
            (XudColor) { 180u, 255u, 190u, 255u });
    }
    if (step == XUD_OK) {
        step = xud_draw_cube(context, spotPosition, 0.45f, 0.45f, 0.45f, (XudColor) { 255u, 226u, 170u, 255u });
    }

    return step;
}

int main(void) {
    XudContext* context = NULL;
    XudLight lights[3];
    XudCamera3D camera;
    XudVector3 pointPosition;
    double seconds = 0.0;
    float pointRange = 8.0f;
    float spotOuter = 24.0f;
    float ambientIntensity = 0.35f;
    bool down = false;
    XudResult step = XUD_OK;
    XudResult scene = XUD_OK;
    uint32_t frame = 0u;
    uint32_t shortFrames = 0u;

    step = xud_init_context(&context, XUD_LIGHTING_WIDTH, XUD_LIGHTING_HEIGHT, LOGNAME);
    if (step != XUD_OK) {
        xud_lighting_error("xud_init_context", step);
        return 1;
    }

    /* The table, in the order the fragment stage will sum it: a raking sun, the circling point,
       the corner spot. xud_set_lights replaces the table whole, so these three are exactly the
       frame's lights - the default sun the context was seeded with is gone from the first frame.
    */
    lights[0u].position = (XudVector3) { 0.0f, 0.0f, 0.0f };
    lights[0u].direction = (XudVector3) { -1.6f, 0.4f, -0.3f };
    lights[0u].color = (XudColor) { 220u, 226u, 255u, 255u };
    lights[0u].intensity = 0.55f;
    lights[0u].range = 0.0f;
    lights[0u].innerConeDegrees = 0.0f;
    lights[0u].outerConeDegrees = 0.0f;
    lights[0u].kind = XUD_LIGHT_KIND_SUN;

    lights[1u].position = (XudVector3) { XUD_LIGHTING_ORBIT_RADIUS, XUD_LIGHTING_ORBIT_HEIGHT, 0.0f };
    lights[1u].direction = (XudVector3) { 0.0f, 1.0f, 0.0f };
    lights[1u].color = (XudColor) { 80u, 255u, 120u, 255u };
    lights[1u].intensity = 2.0f;
    lights[1u].range = pointRange;
    lights[1u].innerConeDegrees = 0.0f;
    lights[1u].outerConeDegrees = 0.0f;
    lights[1u].kind = XUD_LIGHT_KIND_POINT;

    lights[2u].position = (XudVector3) { XUD_LIGHTING_SPOT_X, XUD_LIGHTING_SPOT_Y, XUD_LIGHTING_SPOT_Z };
    lights[2u].direction = (XudVector3) { -XUD_LIGHTING_SPOT_X, -XUD_LIGHTING_SPOT_Y, -XUD_LIGHTING_SPOT_Z };
    lights[2u].color = (XudColor) { 255u, 200u, 120u, 255u };
    lights[2u].intensity = 3.0f;
    lights[2u].range = 24.0f;
    lights[2u].innerConeDegrees = 10.0f;
    lights[2u].outerConeDegrees = spotOuter;
    lights[2u].kind = XUD_LIGHT_KIND_SPOT;

    step = xud_set_lights(context, lights, 3u);
    if (step != XUD_OK) {
        xud_lighting_error("xud_set_lights", step);
        (void)xud_close_context(&context);
        return 1;
    }

    step = xud_set_ambient_light(context, (XudColor) { 40u, 46u, 70u, 255u }, ambientIntensity);
    if (step != XUD_OK) {
        xud_lighting_error("xud_set_ambient_light", step);
        (void)xud_close_context(&context);
        return 1;
    }

    printf(
        LOGNAME ": UP/DOWN point range %.1f - LEFT/RIGHT spot cone %.0f deg - A/Z ambient %.2f\n", pointRange, spotOuter, ambientIntensity);

    camera = (XudCamera3D) { .position = { 8.5f, 6.0f, 8.5f },
        .target = { 0.0f, 0.8f, 0.0f },
        .up = { 0.0f, 1.0f, 0.0f },
        .fovy = 55.0f,
        .near = 0.25f,
        .far = 120.0f };

    while (1) {
        step = xud_is_key_down(&down, context, XUD_KEY_ESCAPE);
        if (step != XUD_OK) {
            xud_lighting_error("frame xud_is_key_down", step);
            break;
        }
        if (down) {
            break;
        }

        step = xud_get_time(&seconds, context);
        if (step != XUD_OK) {
            xud_lighting_error("frame xud_get_time", step);
            break;
        }

        /* The keys move ONE number of the table each, and the table is republished below with
           the frame: a refusal here is a refused NUDGE (out-of-range), not a lost scene, and the
           table the last accepted call left is what keeps drawing.
        */
        step = xud_is_key_down(&down, context, XUD_KEY_UP);
        if (step == XUD_OK) {
            if (down && (pointRange < 20.0f)) {
                pointRange += 0.05f;
            }
        }
        step = xud_is_key_down(&down, context, XUD_KEY_DOWN);
        if (step == XUD_OK) {
            if (down && (pointRange > 1.0f)) {
                pointRange -= 0.05f;
            }
        }
        step = xud_is_key_down(&down, context, XUD_KEY_RIGHT);
        if (step == XUD_OK) {
            if (down && (spotOuter < 70.0f)) {
                spotOuter += 0.2f;
            }
        }
        step = xud_is_key_down(&down, context, XUD_KEY_LEFT);
        if (step == XUD_OK) {
            if (down && (spotOuter > 2.0f)) {
                spotOuter -= 0.2f;
            }
        }
        step = xud_is_key_down(&down, context, XUD_KEY_A);
        if (step == XUD_OK) {
            if (down && (ambientIntensity < 1.0f)) {
                ambientIntensity += 0.005f;
            }
        }
        step = xud_is_key_down(&down, context, XUD_KEY_Z);
        if (step == XUD_OK) {
            if (down && (ambientIntensity > 0.0f)) {
                ambientIntensity -= 0.005f;
            }
        }

        /* One lap every XUD_LIGHTING_ORBIT_SECONDS, and the axis of the spot recomputed from the
           lamp to the centre: direction is (0,0,0) minus the position, which the table's setter
           normalizes on the way in.
        */
        pointPosition.x = XUD_LIGHTING_ORBIT_RADIUS * cosf(seconds * (6.2831853f / XUD_LIGHTING_ORBIT_SECONDS));
        pointPosition.y = XUD_LIGHTING_ORBIT_HEIGHT;
        pointPosition.z = XUD_LIGHTING_ORBIT_RADIUS * sinf(seconds * (6.2831853f / XUD_LIGHTING_ORBIT_SECONDS));

        lights[1u].position = pointPosition;
        lights[1u].range = pointRange;
        lights[2u].outerConeDegrees = spotOuter;

        step = xud_begin_drawing(context);
        if (step != XUD_OK) {
            xud_lighting_error("frame xud_begin_drawing", step);
            break;
        }

        step = xud_set_lights(context, lights, 3u);
        if (step != XUD_OK) {
            xud_lighting_error("frame xud_set_lights", step);
        }
        step = xud_set_ambient_light(context, (XudColor) { 40u, 46u, 70u, 255u }, ambientIntensity);
        if (step != XUD_OK) {
            xud_lighting_error("frame xud_set_ambient_light", step);
        }

        step = xud_clear_background(context, (XudColor) { 10u, 10u, 16u, 255u });
        if (step != XUD_OK) {
            xud_lighting_error("frame xud_clear_background", step);
            (void)xud_end_drawing(context);
            break;
        }
        step = xud_begin_camera_3d(context, camera);
        if (step != XUD_OK) {
            xud_lighting_error("frame xud_begin_camera_3d", step);
            (void)xud_end_drawing(context);
            break;
        }

        scene = xud_lighting_scene(context, pointPosition, lights[2u].position);
        if (scene != XUD_OK) {
            shortFrames++;
            if ((shortFrames == 1u) || ((shortFrames % 60u) == 0u)) {
                printf(LOGNAME ": frame %u drew short (%d) - the batch grows one frame at a time\n", frame, (int)scene);
            }
        }

        step = xud_end_camera_3d(context);
        if (step != XUD_OK) {
            xud_lighting_error("frame xud_end_camera_3d", step);
            (void)xud_end_drawing(context);
            break;
        }

        (void)xud_end_drawing(context);

        if (frame == 90u) {
            printf(LOGNAME ": ninety frames in, the point at (%.2f, %.2f, %.2f), range %.1f, cone %.0f, ambient %.2f\n", pointPosition.x,
                pointPosition.y, pointPosition.z, pointRange, spotOuter, ambientIntensity);
        }
        frame++;
    }

    step = xud_close_context(&context);
    if (step != XUD_OK) {
        xud_lighting_error("xud_close_context", step);
        return 1;
    }

    return 0;
}
