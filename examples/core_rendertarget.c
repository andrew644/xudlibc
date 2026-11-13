/***********************************************************************************************
 *
 * core_rendertarget.c - the off-screen render target, both halves of its life in one run: the
 * waving field of cubes is drawn into a 256x256 target between xud_begin_texture_mode() and
 * xud_end_texture_mode(), and the finished image comes back onto the window as an ordinary
 * texture - xud_draw_texture() sampling the target's own handle - in the SAME frame the field
 * was drawn into it.
 *
 * raylib has no example that is only this pair; its BeginTextureMode users (textures_bagels.c,
 * shaders_erect_shadow.c) each fold another lesson in beside it. The window's 800x450 and the
 * field's numbers are models_waving_cubes.c's, reduced to one plane of cubes: what the example
 * is about is where a cube's pixels LAND, not how many of them wave.
 *
 * The camera's aspect is the WINDOW's, because the camera block is one per frame and reads the
 * swapchain's extent - so the square target shows the wide frustum squeezed into a square, and
 * the cubes of the field come out a little narrow. That is the frame's one honest distortion;
 * the sampled quad shows exactly what the target holds.
 *
 ***********************************************************************************************/

#include <math.h>  /* Required for: sinf, cosf - the wave and the eye's orbit */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h> /* Required for: printf, the run's own report */

#include "xud.h" /* Required for: the target quartet, the frame pair, the camera pair, the grid,
                    the box call and the sampled quad - the whole public API this example is
                    written against */

#define LOGNAME "core_rendertarget"

#define XUD_RT_WINDOW_WIDTH 800u
#define XUD_RT_WINDOW_HEIGHT 450u

/* The target: raylib's example-sized scratch image, square to keep the squeeze it takes visible
   as one even narrowing rather than a diagonal skew. */
#define XUD_RT_TARGET_SIDE 256u

/* The field: one plane of cubes - the target's batch holds it, not the screen's. */
#define XUD_RT_CELLS 5u
#define XUD_RT_CELL_SPACING 3.0f
#define XUD_RT_CUBE_SIZE 1.4f

/* The sampled quad's seat on the window and the frame drawn around it. */
#define XUD_RT_QUAD_X 20
#define XUD_RT_QUAD_Y 60
#define XUD_RT_FRAME_THICK 2u

/* The eye: the same orbit models_waving_cubes.c flies, at its own radius and height. */
#define XUD_RT_ORBIT_RADIUS 16.0f
#define XUD_RT_ORBIT_SPEED 0.4f
#define XUD_RT_EYE_HEIGHT 8.0f
#define XUD_RT_TARGET_FPS 60u
#define XUD_RT_WAVE_SPEED 4.0f

static const XudCamera3D xudRtStartCamera = { .position = { .x = XUD_RT_ORBIT_RADIUS, .y = XUD_RT_EYE_HEIGHT, .z = 0.0f },
    .target = { .x = 0.0f, .y = 0.0f, .z = 0.0f },
    .up = { .x = 0.0f, .y = 1.0f, .z = 0.0f },
    .fovy = 60.0f,
    .near = 0.5f,
    .far = 100.0f };
static const XudColor xudRtRaywhite = { .r = 245u, .g = 245u, .b = 245u, .a = 255u };
static const XudColor xudRtGridCenter = { .r = 128u, .g = 128u, .b = 128u, .a = 255u };
static const XudColor xudRtGridLine = { .r = 191u, .g = 191u, .b = 191u, .a = 255u };
static const XudColor xudRtMaroon = { .r = 190u, .g = 33u, .b = 55u, .a = 255u };
static const XudColor xudRtWhite = { .r = 255u, .g = 255u, .b = 255u, .a = 255u };

static void xud_rt_error(const char* stage, XudResult result);
static XudColor xud_rt_hue(int32_t degrees, float saturation, float value);
static XudResult xud_rt_field(XudContext* context, double seconds);
static int xud_run(XudContext* context);

/*-----------------------------------------------------------------------------------------------
 * The machinery the run needs: the report, a colour, the field
 *-----------------------------------------------------------------------------------------------*/

/* One refused call said out loud: the stage the run was in, and the rung's own answer. */
static void xud_rt_error(const char* stage, XudResult result) { printf(LOGNAME ": %s -> refused (%s)\n", stage, xud_result_name(result)); }

/* One hue of the wheel at full saturation's neighbourhood: the field's colours, the same rainbow
   models_waving_cubes.c hangs on the cell sums, at the six sectors instead of a table. */
static XudColor xud_rt_hue(int32_t degrees, float saturation, float value) {
    const float sector = (float)(degrees / 60);
    const float fraction = ((float)degrees / 60.0f) - sector;
    const float p = value * (1.0f - saturation);
    const float q = value * (1.0f - (saturation * fraction));
    const float t = value * (1.0f - (saturation * (1.0f - fraction)));
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;

    switch (degrees / 60) {
    case 0:
        red = value;
        green = t;
        blue = p;
        break;
    case 1:
        red = q;
        green = value;
        blue = p;
        break;
    case 2:
        red = p;
        green = value;
        blue = t;
        break;
    case 3:
        red = p;
        green = q;
        blue = value;
        break;
    case 4:
        red = t;
        green = p;
        blue = value;
        break;
    default:
        red = value;
        green = p;
        blue = q;
        break;
    }

    return (XudColor) { .r = (uint8_t)(red * 255.0f), .g = (uint8_t)(green * 255.0f), .b = (uint8_t)(blue * 255.0f), .a = 255u };
}

/* The field: the grid and the plane of cubes, every one of them appended to whichever batch the
   open scope names - the target's while the scope is open. The wave is the clock's, the colours
   the cell sums', both read the same whether the frame draws to the target or to the window. */
static XudResult xud_rt_field(XudContext* context, double seconds) {
    static const XudVector3 gridCenter = { .x = 0.0f, .y = 0.0f, .z = 0.0f };
    const float half = (float)XUD_RT_CELLS / 2.0f;
    XudResult step;
    uint32_t x = 0u;
    uint32_t z = 0u;

    step = xud_draw_grid(context, gridCenter, XUD_RT_CELLS, XUD_RT_CELL_SPACING, xudRtGridCenter, xudRtGridLine);
    if (step != XUD_OK) {
        return step;
    }

    for (x = 0u; x < XUD_RT_CELLS; x++) {
        for (z = 0u; z < XUD_RT_CELLS; z++) {
            const float sum = (float)(x + z);
            const float scatter = sinf((sum * 2.0f) + ((float)seconds * XUD_RT_WAVE_SPEED));
            const XudVector3 position
                = { .x = ((float)x - half) * XUD_RT_CELL_SPACING, .y = scatter + 1.0f, .z = ((float)z - half) * XUD_RT_CELL_SPACING };

            step = xud_draw_cube(context, position, XUD_RT_CUBE_SIZE, XUD_RT_CUBE_SIZE, XUD_RT_CUBE_SIZE,
                xud_rt_hue((int32_t)(((int32_t)sum * 36) % 360), 0.75f, 0.9f));
            if (step != XUD_OK) {
                return step;
            }
        }
    }

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The run
 *-----------------------------------------------------------------------------------------------*/

static int xud_run(XudContext* context) {
    XudCamera3D camera = xudRtStartCamera;
    XudHandle target = XUD_HANDLE_NONE;
    uint32_t frame = 0u;
    uint32_t shortFrames = 0u;
    XudResult step;

    step = xud_create_render_texture(&target, context, XUD_RT_TARGET_SIDE, XUD_RT_TARGET_SIDE);
    if (step != XUD_OK) {
        xud_rt_error("xud_create_render_texture", step);
        return 1;
    }
    if (xud_set_target_fps(context, XUD_RT_TARGET_FPS) != XUD_OK) {
        printf(LOGNAME ": xud_set_target_fps -> refused, so the run free-runs\n");
    }

    for (;;) {
        bool shouldClose = false;
        bool pressed = false;
        double seconds = 0.0;

        step = xud_window_should_close(&shouldClose, context);
        if (step != XUD_OK) {
            xud_rt_error("frame xud_window_should_close", step);
            break;
        }
        if (shouldClose) {
            printf(LOGNAME ": frame %u: the window was closed\n", frame);
            break;
        }
        if ((xud_is_key_pressed(&pressed, context, XUD_KEY_ESCAPE) == XUD_OK) && pressed) {
            printf(LOGNAME ": frame %u: ESC\n", frame);
            break;
        }

        if (xud_get_time(&seconds, context) != XUD_OK) {
            seconds = 0.0;
        }
        camera.position.x = (float)cos(seconds * XUD_RT_ORBIT_SPEED) * XUD_RT_ORBIT_RADIUS;
        camera.position.z = (float)sin(seconds * XUD_RT_ORBIT_SPEED) * XUD_RT_ORBIT_RADIUS;

        step = xud_begin_drawing(context);
        if (step != XUD_OK) {
            xud_rt_error("frame xud_begin_drawing", step);
            break;
        }
        /* The background is the ONE clear colour of the frame: it clears the target's pass when
           the scope opens AND the screen's pass, so the sampled quad sits on the same RAYWHITE
           its own field was drawn over. */
        step = xud_clear_background(
            context, (XudColor) { .r = (((frame / 30u) % 2u) == 0u) ? 255u : 0u, .g = 0u, .b = 0u, .a = xudRtRaywhite.a });
        if (step != XUD_OK) {
            xud_rt_error("frame xud_clear_background", step);
            (void)xud_end_drawing(context);
            break;
        }

        /* The scope: every row appended from here to end_texture_mode lands in the TARGET's
           batch, and the frame records the target's pass before the screen's - which is what
           lets the quad below sample an image the SAME frame wrote. */
        step = xud_begin_texture_mode(context, target);
        if (step != XUD_OK) {
            xud_rt_error("frame xud_begin_texture_mode", step);
            (void)xud_end_drawing(context);
            break;
        }
        step = xud_begin_camera_3d(context, camera);
        if (step != XUD_OK) {
            xud_rt_error("frame xud_begin_camera_3d", step);
            (void)xud_end_texture_mode(context);
            (void)xud_end_drawing(context);
            break;
        }
        step = xud_rt_field(context, seconds);
        if (step != XUD_OK) {
            if (shortFrames == 0u) {
                printf(LOGNAME ": frame %u field draw refused (%s); the batch doubles its capacity at the start of\n" LOGNAME
                               ":   the next frame, so the field is short for the first few and whole from the one that fits\n",
                    frame, xud_result_name(step));
            }
            shortFrames++;
        }
        step = xud_end_camera_3d(context);
        if (step != XUD_OK) {
            xud_rt_error("frame xud_end_camera_3d", step);
        }
        step = xud_end_texture_mode(context);
        if (step != XUD_OK) {
            xud_rt_error("frame xud_end_texture_mode", step);
            (void)xud_end_drawing(context);
            break;
        }

        /* Back on the screen: the target is now an ordinary texture - its pass's finalLayout put
           it in SHADER_READ_ONLY - and the quad below is the first reader of it. */
        step = xud_draw_texture(context, target, XUD_RT_QUAD_X, XUD_RT_QUAD_Y, xudRtWhite);
        if (step != XUD_OK) {
            xud_rt_error("frame xud_draw_texture", step);
        }
        step = xud_draw_rectangle_lines(context, XUD_RT_QUAD_X - (int32_t)XUD_RT_FRAME_THICK, XUD_RT_QUAD_Y - (int32_t)XUD_RT_FRAME_THICK,
            (int32_t)XUD_RT_TARGET_SIDE + (int32_t)(2u * XUD_RT_FRAME_THICK),
            (int32_t)XUD_RT_TARGET_SIDE + (int32_t)(2u * XUD_RT_FRAME_THICK), xudRtMaroon);
        if (step != XUD_OK) {
            xud_rt_error("frame xud_draw_rectangle_lines", step);
        }

        if ((frame % 60u) == 0u) {
            XudBatchStats probe;
            if (xud_get_batch_stats(&probe, context) == XUD_OK) {
                printf(LOGNAME ": frame %u screen vertices=%u draws=%u\n", frame, probe.vertexCount, probe.drawCount);
            }
        }
        step = xud_end_drawing(context);
        if (step != XUD_OK) {
            xud_rt_error("frame xud_end_drawing", step);
            break;
        }
        frame++;
    }

    step = xud_close_render_texture(context, target);
    if (step != XUD_OK) {
        xud_rt_error("xud_close_render_texture", step);
    }

    printf(LOGNAME ": %u frame(s), %u of them short\n", frame, shortFrames);
    return 0;
}

int main(void) {
    XudContext* context = NULL;
    XudResult step;
    int status = 0;

    step = xud_init_context(&context, XUD_RT_WINDOW_WIDTH, XUD_RT_WINDOW_HEIGHT, LOGNAME);
    printf(LOGNAME ": xud_init_context(%u, %u, \"%s\") -> %s (xudlibc %s)\n", XUD_RT_WINDOW_WIDTH, XUD_RT_WINDOW_HEIGHT, LOGNAME,
        xud_result_name(step), XUD_VERSION);
    if (step != XUD_OK) {
        return (step == XUD_UNSUPPORTED) ? 0 : 1;
    }

    status = xud_run(context);

    step = xud_close_context(&context);
    printf(LOGNAME ": xud_close_context -> %s\n", xud_result_name(step));

    return (status != 0) ? status : ((step == XUD_OK) ? 0 : 1);
}
