/***********************************************************************************************
 *
 *   models_yaw_pitch_roll.c - raylib's models_yaw_pitch_roll example on xudlibc
 *
 *   Owns : one context, one font, one loaded model, one still camera. The model is the repo's own
 *          glTF under the gitignored resources/ dir, drawn every frame through a matrix the run
 *          builds from three angles the keys turn. Layer: examples. Public API only (xud.h).
 *
 *   Shape : raylib's example writes model.transform = MatrixRotateXYZ(euler) and hands DrawModel a
 *          position the .glb happened to be authored around, so its rotation happens to be about the
 *          model's own centre. Here the centre is read (xud_get_model_bounding_box) and spent:
 *          translate-to-origin, rotate, translate-back, three calls' worth of the matrix rung
 *          (xud_matrix_translate, xud_matrix_rotate_xyz, xud_matrix_multiply) and one composite the
 *          _ex draw reads. Any asset therefore turns about itself.
 *
 *   Added : the three axes, through the rotation's centre. raylib's example has nothing to compare
 *          the angles against except its own text, and a textured solid whose silhouette changes
 *          little at small angles is a poor read of nine floats. The three segments are the rotation
 *          spent on three points (xud_matrix_transform_point), so what is drawn is the same matrix
 *          the model was drawn through and not a second copy of the angles.
 *
 *   Controls: LEFT/RIGHT turn yaw, UP/DOWN pitch, A/S roll, at raylib's own one radian per second;
 *          ESC quits. The camera is still, as it is there - the model is the only thing that moves.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include <math.h>   /* Required for: sinf, tanf, atanf, sqrtf, and the bounding-sphere fit below */
#include <stdarg.h> /* Required for: va_start, va_end, the TextFormat half of the HUD below */
#include <stdio.h>  /* Required for: printf, snprintf, and the file's own report */
#include <stdlib.h> /* Required for: malloc, free, the font's bytes */

#include "xud.h" /* Required for: xud_load_model and the model _ex draw, the two model queries, the matrix rung, the camera pair, the screen-fixed text pair, the input queries and the clock - the whole public API this example is written against */

#define XUD_LOG_NAME "models_yaw_pitch_roll"

#define XUD_YPR_WIDTH 800u /* raylib's example's own extent (its file, lines 29 and 30) */
#define XUD_YPR_HEIGHT 450u

#define XUD_YPR_MODEL_PATH "resources/model/gltf/raylib_logo_3d.glb"
#define XUD_YPR_FONT_PATH "resources/fonts/NotoSansTC-Regular.ttf"
#define XUD_YPR_FONT_PIXELS 16u
#define XUD_YPR_LINE_MAX 96

/* The turn rate, raylib's `float rotationSpeed = 1.0f*GetFrameTime()`: one radian per second of
   each held key, so an angle that is a whole number of seconds is a whole number of radians. */
#define XUD_YPR_RADIANS_PER_SECOND 1.0f

#define XUD_YPR_DEGREES_PER_RADIANS 57.2957795130832f

/* The aperture, and the eye's two swings off the front. raylib's example states its eye as the
   literal (-30, 30, 30); the fit below reaches the same kind of three-quarter view from a distance
   that is a function of the model instead of a number that holds only for one container-sized asset. */
#define XUD_YPR_FOVY_DEG 45.0f
#define XUD_YPR_YAW_DEG 45.0f
#define XUD_YPR_PITCH_DEG 30.0f
#define XUD_YPR_FIT_MARGIN 1.08f
#define XUD_YPR_PLANE_SLACK 1.0f
#define XUD_YPR_NEAR_MIN 0.01f /* the floor the camera rung refuses at, read as a floor rather than a plane */

/* The axes, against the sphere that holds the box at every angle: long enough to end outside the
   solid, short enough that the fit above, which is stated against that same sphere, still frames them. */
#define XUD_YPR_AXIS_REACH 1.15f

static const XudColor xudYprRaywhite = { .r = 245u, .g = 245u, .b = 250u, .a = 255u };
static const XudColor xudYprMaroon = { .r = 127u, .g = 0u, .b = 0u, .a = 255u };
static const XudColor xudYprWhite = { .r = 255u, .g = 255u, .b = 255u, .a = 255u };
static const XudColor xudYprRed = { .r = 220u, .g = 40u, .b = 40u, .a = 255u };
static const XudColor xudYprGreen = { .r = 0u, .g = 180u, .b = 60u, .a = 255u };
static const XudColor xudYprBlue = { .r = 0u, .g = 120u, .b = 240u, .a = 255u };

static uint8_t* xud_ypr_read(const char* path, uint32_t* outBytes);
static XudResult xud_ypr_text(XudContext* context, XudHandle font, int32_t posX, int32_t posY, const char* format, XudColor color, ...);
static XudCamera3D xud_ypr_camera(XudVector3 centre, float radius);

/*-----------------------------------------------------------------------------------------------
 * The machinery the run needs: a file reader, a text call that formats, and the still eye
 *-----------------------------------------------------------------------------------------------*/

/* The font file's bytes: xud_load_font() takes bytes rather than a path, so the caller reads the
   file. NULL when there is no file.
*/
static uint8_t* xud_ypr_read(const char* path, uint32_t* outBytes) {
    FILE* file = NULL;
    long size = 0L;
    uint8_t* buffer = NULL;
    size_t read = 0u;

    *outBytes = 0u;
    file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }
    if (fseek(file, 0L, SEEK_END) != 0) {
        (void)fclose(file);
        return NULL;
    }
    size = ftell(file);
    if (size <= 0L) {
        (void)fclose(file);
        return NULL;
    }
    rewind(file);
    buffer = malloc((size_t)size);
    if (buffer == NULL) {
        (void)fclose(file);
        return NULL;
    }
    read = fread(buffer, 1u, (size_t)size, file);
    (void)fclose(file);
    if (read != (size_t)size) {
        free(buffer);
        return NULL;
    }
    *outBytes = (uint32_t)size;
    return buffer;
}

/* raylib's DrawText(TextFormat(...)) as one call: the format goes through snprintf into a row of
   bytes, the bytes become codepoints - the public text pair takes a uint32_t array because a font is
   keyed by codepoint and not by byte - and the row is drawn screen-fixed at the caller's pixel.
*/
static XudResult xud_ypr_text(XudContext* context, XudHandle font, int32_t posX, int32_t posY, const char* format, XudColor color, ...) {
    char line[XUD_YPR_LINE_MAX];
    uint32_t codepoints[XUD_YPR_LINE_MAX];
    uint32_t step = 0u;
    va_list arguments;
    int written = 0;

    va_start(arguments, color);
    written = vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);

    if ((written < 0) || ((size_t)written >= sizeof(line))) {
        return XUD_INVALID_ARG;
    }
    for (step = 0u; step < (uint32_t)written; step++) {
        codepoints[step] = (uint32_t)(uint8_t)line[step];
    }

    return xud_draw_text_screen(context, font, codepoints, (uint32_t)written, posX, posY, (float)XUD_YPR_FONT_PIXELS, 0.0f, color);
}

/* The still eye, at a distance the box answers rather than one that holds for one asset only. The
   extent is the box's SPHERE and not the box, because the box turns: the solid swept over the run is
   every rotation of the box, and the smallest bound that holds for all of them is the sphere around
   it. A per-axis fit would frame the model at yaw 0 and clip a corner as soon as a turn brought the
   box's diagonal to the frustum's edge - a bug that would appear only mid-run. The limiting
   half-angle is the smaller of the two, and at 16:9 that is the vertical one, the horizontal derived
   from it by the aspect as the camera rung's own projection derives it.
*/
static XudCamera3D xud_ypr_camera(XudVector3 centre, float radius) {
    const float halfVertical = (XUD_YPR_FOVY_DEG / 2.0f) / XUD_YPR_DEGREES_PER_RADIANS;
    const float halfHorizontal = atanf(tanf(halfVertical) * ((float)XUD_YPR_WIDTH / (float)XUD_YPR_HEIGHT));
    const float halfLimit = (halfHorizontal < halfVertical) ? halfHorizontal : halfVertical;
    const float yaw = XUD_YPR_YAW_DEG / XUD_YPR_DEGREES_PER_RADIANS;
    const float pitch = XUD_YPR_PITCH_DEG / XUD_YPR_DEGREES_PER_RADIANS;
    const float distance = (radius > 0.0f) ? (radius / sinf(halfLimit)) * XUD_YPR_FIT_MARGIN : XUD_YPR_NEAR_MIN;
    const float near = distance - radius - XUD_YPR_PLANE_SLACK;

    return (XudCamera3D) { .position = (XudVector3) { .x = centre.x + (distance * sinf(yaw) * cosf(pitch)),
                               .y = centre.y + (distance * sinf(pitch)),
                               .z = centre.z + (distance * cosf(yaw) * cosf(pitch)) },
        .target = (XudVector3) { .x = centre.x, .y = centre.y, .z = centre.z },
        .up = (XudVector3) { .x = 0.0f, .y = 1.0f, .z = 0.0f },
        .fovy = XUD_YPR_FOVY_DEG,
        .near = (near > XUD_YPR_NEAR_MIN) ? near : XUD_YPR_NEAR_MIN,
        .far = distance + radius + XUD_YPR_PLANE_SLACK };
}

/*-----------------------------------------------------------------------------------------------
 * The placement, and the axes that show where it points
 *-----------------------------------------------------------------------------------------------*/

/* translate-to-centre * rotate * translate-to-origin: the composite xud_draw_model_ex() reads, with
   the model's own centre as the pivot. The two centring matrices are the identity with a point in
   their fourth column, so the chain is the same three factors raylib's example gets by having its
   asset authored about its middle; multiplication spends the RIGHT factor first, which is why the
   centring is on the right of the product that carries the rotation. The composite goes through
   xud_matrix_compose's two-multiplies shape rather than being assembled float by float.
*/
static XudResult xud_ypr_orient(float* outPlacement, XudVector3 centre, XudVector3 euler) {
    float toOrigin[16];
    float toCentre[16];
    float rotation[16];
    float spun[16];
    XudResult result;

    result = xud_matrix_translate(toOrigin, (XudVector3) { .x = -centre.x, .y = -centre.y, .z = -centre.z });
    if (result == XUD_OK) {
        result = xud_matrix_rotate_xyz(rotation, euler);
    }
    if (result == XUD_OK) {
        result = xud_matrix_multiply(spun, rotation, toOrigin);
    }
    if (result == XUD_OK) {
        result = xud_matrix_translate(toCentre, centre);
    }
    if (result == XUD_OK) {
        result = xud_matrix_multiply(outPlacement, toCentre, spun);
    }

    return result;
}

/* The three axes, through the pivot: the placement spent on four POINTS, which is what the rung
   offers, so the segments drawn are the rotation's own answer rather than a recomputation of it from
   the angles. Each runs from the pivot outwards, the way raymath's Vector3Transform would carry the
   origin and three unit points.
*/
static XudResult xud_ypr_axes(XudContext* context, const float* placement, XudVector3 centre, float reach) {
    XudVector3 pivot;
    XudVector3 end;
    XudResult result;

    result = xud_matrix_transform_point(&pivot, placement, centre);
    if (result == XUD_OK) {
        result = xud_matrix_transform_point(&end, placement, (XudVector3) { .x = centre.x + reach, .y = centre.y, .z = centre.z });
    }
    if (result == XUD_OK) {
        result = xud_draw_line_3d(context, pivot, end, xudYprRed);
    }
    if (result == XUD_OK) {
        result = xud_matrix_transform_point(&end, placement, (XudVector3) { .x = centre.x, .y = centre.y + reach, .z = centre.z });
    }
    if (result == XUD_OK) {
        result = xud_draw_line_3d(context, pivot, end, xudYprGreen);
    }
    if (result == XUD_OK) {
        result = xud_matrix_transform_point(&end, placement, (XudVector3) { .x = centre.x, .y = centre.y, .z = centre.z + reach });
    }
    if (result == XUD_OK) {
        result = xud_draw_line_3d(context, pivot, end, xudYprBlue);
    }

    return result;
}

/*-----------------------------------------------------------------------------------------------
 * The HUD
 *-----------------------------------------------------------------------------------------------*/

/* raylib's four DrawText lines (its file, lines 105 to 108): the sentence, then the three angles in
   degrees. Its font size of 10 is this library's own 16-pixel face, so the sentence is broken over
   two rows rather than run off the window's edge, and the block sits above the bottom of a 450-row
   window instead of at the row 400 that only clears a 10-pixel line.
*/
static XudResult xud_ypr_hud(XudContext* context, XudHandle font, XudVector3 euler) {
    XudResult result;

    result = xud_ypr_text(context, font, 30, 296, "ARROWS: YAW AND PITCH, A AND S: ROLL", xudYprMaroon);
    if (result == XUD_OK) {
        result = xud_ypr_text(context, font, 30, 322, "This is a test scene to understand when", xudYprMaroon);
    }
    if (result == XUD_OK) {
        result = xud_ypr_text(context, font, 30, 344, "GLTF 2.0 support is implemented", xudYprMaroon);
    }
    if (result == XUD_OK) {
        result = xud_ypr_text(context, font, 30, 374, "Yaw: %.03f", xudYprMaroon, euler.y * XUD_YPR_DEGREES_PER_RADIANS);
    }
    if (result == XUD_OK) {
        result = xud_ypr_text(context, font, 30, 396, "Pitch: %.03f", xudYprMaroon, euler.x * XUD_YPR_DEGREES_PER_RADIANS);
    }
    if (result == XUD_OK) {
        result = xud_ypr_text(context, font, 30, 418, "Roll: %.03f", xudYprMaroon, euler.z * XUD_YPR_DEGREES_PER_RADIANS);
    }

    return result;
}

/*-----------------------------------------------------------------------------------------------
 * The run
 *-----------------------------------------------------------------------------------------------*/

int main(void) {
    XudCamera3D camera;
    XudVector3 centre = { .x = 0.0f, .y = 0.0f, .z = 0.0f };
    XudVector3 euler = { .x = 0.0f, .y = 0.0f, .z = 0.0f };
    XudFontData fontData
        = { .fileData = NULL, .fileBytes = 0u, .pixelSize = XUD_YPR_FONT_PIXELS, .codepoints = NULL, .codepointCount = 0u };
    float placement[16];
    uint8_t* fontFile = NULL;
    uint32_t fontBytes = 0u;
    uint32_t primitives = 0u;
    uint32_t frame = 0u;
    uint32_t shortFrames = 0u;
    float radius = 1.0f;
    XudContext* context = NULL;
    XudHandle font;
    XudHandle model;
    XudResult result;

    fontFile = xud_ypr_read(XUD_YPR_FONT_PATH, &fontBytes);
    if (fontFile == NULL) {
        printf("%s: no font at %s, so the HUD has nothing to draw with\n", XUD_LOG_NAME, XUD_YPR_FONT_PATH);
        return 1;
    }

    result = xud_init_context(&context, XUD_YPR_WIDTH, XUD_YPR_HEIGHT);
    printf("%s: xud_init_context(%u, %u) -> %s (xudlibc %s)\n", XUD_LOG_NAME, XUD_YPR_WIDTH, XUD_YPR_HEIGHT, xud_result_name(result),
        XUD_VERSION);
    if (result != XUD_OK) {
        free(fontFile);
        return (result == XUD_UNSUPPORTED) ? 0 : 1;
    }

    fontData.fileData = fontFile;
    fontData.fileBytes = fontBytes;
    result = xud_load_font(&font, context, &fontData);
    if (result != XUD_OK) {
        printf("%s: xud_load_font -> %s\n", XUD_LOG_NAME, xud_result_name(result));
        (void)xud_close_context(&context);
        free(fontFile);
        return 1;
    }

    result = xud_load_model(&model, context, XUD_YPR_MODEL_PATH);
    printf("%s: xud_load_model(%s) -> %s\n", XUD_LOG_NAME, XUD_YPR_MODEL_PATH, xud_result_name(result));
    if (result != XUD_OK) {
        (void)xud_close_font(context, font);
        (void)xud_close_context(&context);
        free(fontFile);
        return 1;
    }

    /* The two numbers the load folded, read back through the public queries: the box is the pivot the
       rotation spends and the sphere the fit is stated against, and the count is how many rows the
       draw records - one per glTF PRIMITIVE.
    */
    {
        XudVector3 min;
        XudVector3 max;

        result = xud_get_model_mesh_count(&primitives, context, model);
        if (result == XUD_OK) {
            result = xud_get_model_bounding_box(&min, &max, context, model);
        }
        if (result != XUD_OK) {
            printf("%s: the model's own two queries -> %s\n", XUD_LOG_NAME, xud_result_name(result));
            (void)xud_close_model(context, model);
            (void)xud_close_font(context, font);
            (void)xud_close_context(&context);
            free(fontFile);
            return 1;
        }
        centre.x = (min.x + max.x) / 2.0f;
        centre.y = (min.y + max.y) / 2.0f;
        centre.z = (min.z + max.z) / 2.0f;
        radius
            = sqrtf(((max.x - min.x) * (max.x - min.x)) + ((max.y - min.y) * (max.y - min.y)) + ((max.z - min.z) * (max.z - min.z))) / 2.0f;

        printf("%s: %u primitive row(s), box (%.3f, %.3f, %.3f) to (%.3f, %.3f, %.3f), pivot (%.3f, %.3f, %.3f), radius %.3f\n",
            XUD_LOG_NAME, (unsigned int)primitives, min.x, min.y, min.z, max.x, max.y, max.z, centre.x, centre.y, centre.z, radius);
    }

    camera = xud_ypr_camera(centre, radius);
    if (xud_set_target_fps(context, 60u) != XUD_OK) {
        printf("%s: xud_set_target_fps -> refused, so the run free-runs\n", XUD_LOG_NAME);
    }

    for (;;) {
        bool shouldClose = false;
        bool pressed = false;
        bool down = false;
        float seconds = 0.0f;
        float turn = 0.0f;

        result = xud_window_should_close(&shouldClose, context);
        if (result != XUD_OK) {
            printf("%s: frame %u xud_window_should_close -> %s\n", XUD_LOG_NAME, frame, xud_result_name(result));
            break;
        }
        if (shouldClose) {
            printf("%s: frame %u: the window was closed\n", XUD_LOG_NAME, frame);
            break;
        }
        if ((xud_is_key_pressed(&pressed, context, XUD_KEY_ESCAPE) == XUD_OK) && pressed) {
            printf("%s: frame %u: ESC\n", XUD_LOG_NAME, frame);
            break;
        }

        /* The turn of one frame: raylib's rotationSpeed, one radian to the second, spent on whichever
           keys are held. A frame whose clock refuses turns nothing rather than turning by a number it
           did not read.
        */
        if (xud_get_frame_time(&seconds, context) != XUD_OK) {
            seconds = 0.0f;
        }
        turn = XUD_YPR_RADIANS_PER_SECOND * seconds;
        if ((xud_is_key_down(&down, context, XUD_KEY_LEFT) == XUD_OK) && down) {
            euler.y -= turn;
        }
        if ((xud_is_key_down(&down, context, XUD_KEY_RIGHT) == XUD_OK) && down) {
            euler.y += turn;
        }
        if ((xud_is_key_down(&down, context, XUD_KEY_UP) == XUD_OK) && down) {
            euler.x -= turn;
        }
        if ((xud_is_key_down(&down, context, XUD_KEY_DOWN) == XUD_OK) && down) {
            euler.x += turn;
        }
        if ((xud_is_key_down(&down, context, XUD_KEY_A) == XUD_OK) && down) {
            euler.z -= turn;
        }
        if ((xud_is_key_down(&down, context, XUD_KEY_S) == XUD_OK) && down) {
            euler.z += turn;
        }

        /* The frame's one matrix, built before the frame opens: it is a number, not drawing state, and
           a refusal in it is a bug in this file rather than a short batch.
        */
        result = xud_ypr_orient(placement, centre, euler);
        if (result != XUD_OK) {
            printf("%s: frame %u xud_ypr_orient -> %s, and the run stops here\n", XUD_LOG_NAME, frame, xud_result_name(result));
            break;
        }

        result = xud_begin_drawing(context);
        if (result != XUD_OK) {
            printf("%s: frame %u xud_begin_drawing -> %s, and the run stops here\n", XUD_LOG_NAME, frame, xud_result_name(result));
            break;
        }
        (void)xud_clear_background(context, xudYprRaywhite);

        /* The camera is begun here and NOT ended inside the frame: the end is a per-frame teardown on
           the renderer and a per-run teardown at the bottom of main. See examples/camera_first_person.c.
        */
        (void)xud_begin_camera_3d(context, camera);
        result = xud_draw_model_ex(context, model, placement, xudYprWhite);
        if (result == XUD_OK) {
            result = xud_ypr_axes(context, placement, centre, radius * XUD_YPR_AXIS_REACH);
        }
        if (result != XUD_OK) {
            /* The batch's capacity doubles at the start of a frame, so a scene is drawn short in its
               first frames until the capacity reaches what it asks for.
            */
            if (shortFrames == 0u) {
                printf("%s: frame %u a draw refused -> %s; the scene is whole from the frame whose capacity fits it\n", XUD_LOG_NAME, frame,
                    xud_result_name(result));
            }
            shortFrames++;
        }

        result = xud_ypr_hud(context, font, euler);
        if (result != XUD_OK) {
            if (shortFrames == 0u) {
                printf("%s: frame %u a HUD draw refused -> %s\n", XUD_LOG_NAME, frame, xud_result_name(result));
            }
            shortFrames++;
        }
        result = xud_end_drawing(context);
        if (result != XUD_OK) {
            printf("%s: frame %u xud_end_drawing -> %s, and the run stops here\n", XUD_LOG_NAME, frame, xud_result_name(result));
            break;
        }
        frame++;
    }

    /* The objects go in the reverse order they came: the camera, then the model, then the font, then
       the context. The font outlives the loop because the HUD draws through it.
    */
    (void)xud_end_camera_3d(context);
    result = xud_close_model(context, model);
    if (result != XUD_OK) {
        printf("%s: xud_close_model -> %s\n", XUD_LOG_NAME, xud_result_name(result));
    }
    result = xud_close_font(context, font);
    if (result != XUD_OK) {
        printf("%s: xud_close_font -> %s\n", XUD_LOG_NAME, xud_result_name(result));
    }
    free(fontFile);
    printf("%s: %u frame(s), %u of them short, at yaw %.03f pitch %.03f roll %.03f degrees\n", XUD_LOG_NAME, frame, shortFrames,
        euler.y * XUD_YPR_DEGREES_PER_RADIANS, euler.x * XUD_YPR_DEGREES_PER_RADIANS, euler.z * XUD_YPR_DEGREES_PER_RADIANS);
    result = xud_close_context(&context);
    printf("%s: xud_close_context -> %s\n", XUD_LOG_NAME, xud_result_name(result));

    return (result == XUD_OK) ? 0 : 1;
}
