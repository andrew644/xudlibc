/***********************************************************************************************
 *
 *   models_waving_cubes.c - raylib's models_waving_cubes example on xudlibc
 *
 *   Owns : one context, one font for the one HUD line, and one camera record. The scene is the box
 *          call's own - a field of boxes whose height, spacing and colour are all a function of the
 *          cell they stand in and of the clock, redrawn from the eye that walks around it. Every
 *          number of the wave is raylib's at its file's own lines 53 to 102: the time scale, the
 *          scatter, the three spacings, the cube size and the hue. Layer: examples. Public API only.
 *
 *   Shape : raylib's eye orbits the field (its file, lines 58 to 61: the x and the z of it are the
 *          cosine and the sine of a third of the clock, at forty units, a radius this file shortens).
 *          That motion is kept, and it is the clock's motion and not an input's - no xud_update_camera()
 *          and no key read below, so the camera record itself never changes - only where the clock puts
 *          the eye.
 *
 *   Missing : four things. (1) The block count: raylib's field is 15 on a side, 3375 boxes (its
 *          file, line 43), and this library draws a frame out of ONE batch whose capacity is bounded
 *          at 4096 vertices (xud_batch.h: XUD_BATCH_MAX_VERTICES) while a box costs 24 of them, so
 *          the most any frame can hold is a hundred and seventy boxes and this field is 5 on a side,
 *          125 boxes. Every other number is raylib's - the same /30 block scale, the same scatter,
 *          the same three spacings and the same size - so a cell of this field and the cell of the
 *          same name there move together. The one number that is not raylib's own is the orbit's
 *          radius: a field a third of the side hangs at raylib's forty units in a third of the frame,
 *          so the two numbers beside the camera record name the nearer eye. (2) ColorFromHSV, whose
 *          one job is the rainbow (its file,
 *          line 96): this library has no colour conversion in the public header, so the six-sector
 *          conversion that call is made of is written below, at the same saturation and value. (3)
 *          DrawFPS, which in this revision is the fps, its three bands and one string (rtext.c 1197
 *          to 1205); xud_get_fps() through snprintf and the UTF-32 half of the text pair is the same
 *          line at the same pixel. (4) The grid's colour: raylib draws one grey, this library's grid
 *          takes the two-tone pair, so the centre lines are raylib's own 0.5 grey and the rest its
 *          own 0.75 (rmodels.c 1096 and 1100).
 *
 *   Pairing : the camera pair does NOT nest inside the drawing pair the way raylib's BeginMode3D/
 *          EndMode3D do. This library writes ONE matrix per frame, into set 0's block at the
 *          submission itself, so a camera ended inside the frame is a frame drawn in clip space -
 *          the field's forty units would be a scatter of pixels. The camera is therefore begun every
 *          frame, each begin replacing the record before it, and taken back when the run ends.
 *
 *   Controls: nothing but the window's close button and ESC, which is all raylib's example answers.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include <math.h>   /* Required for: sinf, cosf, sin, the wave and the eye's orbit */
#include <stdio.h>  /* Required for: fopen, fseek, ftell, fread, fclose, snprintf, printf */
#include <stdlib.h> /* Required for: free and malloc, the one buffer the font file is read into */
#include <string.h> /* Required for: strlen, the length of the row the fps line formats into */

#include "xud.h" /* Required for: the camera record, the frame pair, the box call and the ground grid, the font pair and the screen-fixed text, the clock and the context's own pair - the whole public API this example is written against */

#define LOGNAME "models_waving_cubes"

/* raylib's own extent (its file, lines 29 and 30). */
#define XUD_WAVING_WIDTH 800u
#define XUD_WAVING_HEIGHT 450u

/* The field: raylib's numBlocks (its file, line 43) at the largest side the batch's frame bound
   allows - see the Missing half of the banner. The cells are named x, y, z exactly as there.
*/
#define XUD_WAVING_BLOCKS 5u

/* The grid raylib's DrawGrid(10, 5.0f) is (its file, line 72): ten divisions five units apart. */
#define XUD_WAVING_GRID_DIVISIONS 10u
#define XUD_WAVING_GRID_SPACING 5.0f

/* The font: the repo's own asset, read as bytes because xud_load_font() takes bytes and not a path. */
#define XUD_WAVING_FONT_PATH "resources/fonts/NotoSansTC-Regular.ttf"
#define XUD_WAVING_FONT_PIXELS 16u

/* The fps line: raylib's DrawFPS at its own pixel and its own size (its file, line 109). */
#define XUD_WAVING_FPS_X 10
#define XUD_WAVING_FPS_Y 10
#define XUD_WAVING_FPS_PIXELS 20.0f
#define XUD_WAVING_LINE_MAX 32

/* The palette, raylib's own bytes (reference/raylib/src/raylib.h 174 to 200), and the grid's two
   greys, its 0.5 and its 0.75 (rmodels.c 1096 and 1100).
*/
static const XudColor xudWavingRaywhite = { .r = 245u, .g = 245u, .b = 245u, .a = 255u };
static const XudColor xudWavingLime = { .r = 0u, .g = 158u, .b = 47u, .a = 255u };
static const XudColor xudWavingOrange = { .r = 255u, .g = 161u, .b = 0u, .a = 255u };
static const XudColor xudWavingRed = { .r = 230u, .g = 41u, .b = 55u, .a = 255u };
static const XudColor xudWavingGridCenter = { .r = 128u, .g = 128u, .b = 128u, .a = 255u };
static const XudColor xudWavingGridLine = { .r = 191u, .g = 191u, .b = 191u, .a = 255u };

/* The eye raylib's example declares (its file, lines 36 to 39), at the two numbers that are this
   file's own. The orbit below overwrites the x and the z of it with the clock's own circle every
   frame, exactly as there, and only the height is the record's: raylib's eye is twenty units up on
   a forty-unit circle around a field that reaches forty-five units at the widest of its wave, and
   this field - a third of the side - reaches seventeen, so the circle is twenty and the height ten,
   which is the same eye above the same fraction of the same wave.
*/
#define XUD_WAVING_ORBIT_RADIUS 20.0f
#define XUD_WAVING_EYE_HEIGHT 10.0f

static const XudCamera3D xudWavingStartCamera = { .position = { .x = XUD_WAVING_ORBIT_RADIUS, .y = XUD_WAVING_EYE_HEIGHT, .z = 0.0f },
    .target = { .x = 0.0f, .y = 0.0f, .z = 0.0f },
    .up = { .x = 0.0f, .y = 1.0f, .z = 0.0f },
    .fovy = 70.0f,
    .near = 0.5f,
    .far = 100.0f };

static uint8_t* xud_waving_read(const char* path, uint32_t* outBytes);
static void xud_text_error(const char* stage, XudResult result);
static XudResult xud_waving_text(XudContext* context, XudHandle font, int32_t posX, int32_t posY, const char* line, XudColor color);
static XudColor xud_waving_hsv(float hue, float saturation, float value);
static XudResult xud_waving_field(XudContext* context, double seconds, float scale);
static int xud_run(XudContext* context);

/*-----------------------------------------------------------------------------------------------
 * The machinery the run needs: a file reader, the run's own report, a text call, a colour
 *-----------------------------------------------------------------------------------------------*/

/* The font file's bytes: xud_load_font() takes bytes rather than a path, so the public header
   cannot read the file for the caller and the caller reads it here. NULL when there is no file.
*/
static uint8_t* xud_waving_read(const char* path, uint32_t* outBytes) {
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

/* One refused call said out loud: the stage the run was in, and the rung's own answer. */
static void xud_text_error(const char* stage, XudResult result) {
    printf(LOGNAME ": %s -> refused (%s)\n", stage, xud_result_name(result));
}

/* raylib's DrawText at a screen pixel: the bytes become codepoints - the public text pair takes a
   uint32_t array because a font is keyed by codepoint and not by byte - and the row is drawn
   screen-fixed, so the orbiting eye moves the field around it rather than the string.
*/
static XudResult xud_waving_text(XudContext* context, XudHandle font, int32_t posX, int32_t posY, const char* line, XudColor color) {
    uint32_t codepoints[XUD_WAVING_LINE_MAX];
    const size_t bytes = strlen(line);
    uint32_t step = 0u;

    if ((bytes == 0u) || (bytes >= sizeof(codepoints))) {
        return XUD_INVALID_ARG;
    }
    for (step = 0u; step < (uint32_t)bytes; step++) {
        codepoints[step] = (uint32_t)(uint8_t)line[step];
    }

    return xud_draw_text_screen(context, font, codepoints, (uint32_t)bytes, posX, posY, XUD_WAVING_FPS_PIXELS, 0.0f, color);
}

/* raylib's ColorFromHSV (reference/raylib/src/raylib.h 1493: hue in degrees, saturation and value
   in [0,1]): the six sectors of the hue wheel, one of them named by the degrees divided by sixty,
   and the three shades the sector's own fraction of it mixes between. The public header has no
   colour conversion of any kind, so the conversion the rainbow below is made of is written here.
*/
static XudColor xud_waving_hsv(float hue, float saturation, float value) {
    float h = hue;
    int sector = 0;
    float fraction = 0.0f;
    float p = 0.0f;
    float q = 0.0f;
    float t = 0.0f;

    while (h < 0.0f) {
        h += 360.0f;
    }
    while (h >= 360.0f) {
        h -= 360.0f;
    }
    sector = (int)(h / 60.0f) % 6;
    fraction = (h / 60.0f) - (float)sector;
    p = value * (1.0f - saturation);
    q = value * (1.0f - fraction * saturation);
    t = value * (1.0f - (1.0f - fraction) * saturation);

    switch (sector) {
    case 0:
        return (XudColor) { .r = (uint8_t)(value * 255.0f), .g = (uint8_t)(t * 255.0f), .b = (uint8_t)(p * 255.0f), .a = 255u };
    case 1:
        return (XudColor) { .r = (uint8_t)(q * 255.0f), .g = (uint8_t)(value * 255.0f), .b = (uint8_t)(p * 255.0f), .a = 255u };
    case 2:
        return (XudColor) { .r = (uint8_t)(p * 255.0f), .g = (uint8_t)(value * 255.0f), .b = (uint8_t)(t * 255.0f), .a = 255u };
    case 3:
        return (XudColor) { .r = (uint8_t)(p * 255.0f), .g = (uint8_t)(q * 255.0f), .b = (uint8_t)(value * 255.0f), .a = 255u };
    case 4:
        return (XudColor) { .r = (uint8_t)(t * 255.0f), .g = (uint8_t)(p * 255.0f), .b = (uint8_t)(value * 255.0f), .a = 255u };
    default:
        return (XudColor) { .r = (uint8_t)(value * 255.0f), .g = (uint8_t)(p * 255.0f), .b = (uint8_t)(q * 255.0f), .a = 255u };
    }
}

/*-----------------------------------------------------------------------------------------------
 * The field
 *-----------------------------------------------------------------------------------------------*/

/* The 125 boxes of the field (raylib's 3375, at the same every-number - see the Missing half of
   the banner), and the grid they hang over. The clock the caller hands in is the run's own seconds
   and the scale the run's own time scale, both read once per frame there rather than once per cell.
*/
static XudResult xud_waving_field(XudContext* context, double seconds, float scale) {
    static const XudVector3 gridCenter = { .x = 0.0f, .y = 0.0f, .z = 0.0f };
    XudResult step;
    uint32_t x = 0u;
    uint32_t y = 0u;
    uint32_t z = 0u;

    step = xud_draw_grid(context, gridCenter, XUD_WAVING_GRID_DIVISIONS, XUD_WAVING_GRID_SPACING, xudWavingGridCenter, xudWavingGridLine);
    if (step != XUD_OK) {
        return step;
    }

    for (x = 0u; x < XUD_WAVING_BLOCKS; x++) {
        for (y = 0u; y < XUD_WAVING_BLOCKS; y++) {
            for (z = 0u; z < XUD_WAVING_BLOCKS; z++) {
                /* The cell's own three numbers, in raylib's order (its file, lines 81 to 102). */
                const float sum = (float)(x + y + z);
                const float blockScale = sum / 30.0f;
                const float scatter = sinf((blockScale * 20.0f) + ((float)seconds * 4.0f));
                const float half = (float)XUD_WAVING_BLOCKS / 2.0f;
                const XudVector3 position = { .x = ((float)x - half) * (scale * 3.0f) + scatter,
                    .y = ((float)y - half) * (scale * 2.0f) + scatter,
                    .z = ((float)z - half) * (scale * 3.0f) + scatter };
                const float size = (2.4f - scale) * blockScale;
                const XudColor color = xud_waving_hsv((float)(((int)sum * 18) % 360), 0.75f, 0.9f);

                step = xud_draw_cube(context, position, size, size, size, color);
                if (step != XUD_OK) {
                    return step;
                }
            }
        }
    }

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The run
 *-----------------------------------------------------------------------------------------------*/

/* One short frame said out loud, and only the first: the batch's capacity DOUBLES at the start of a
   frame (xud_begin_drawing's growth), so a field this size is drawn short for the first frames and
   whole from the frame whose capacity fits it. Every frame redraws the whole field, so a short one
   costs nothing but the boxes that did not fit - and the wave is the same wave either way, because
   it is a function of the clock and not of what got drawn.
*/
static void xud_waving_count_short_frame(uint32_t frame, XudResult step, uint32_t* shortFrames) {
    if (*shortFrames == 0u) {
        printf(LOGNAME ": frame %u draw %s; the batch's capacity doubles at the start of the next frame, so this\n" LOGNAME
                       ":   field is short for the first few frames and whole from the one whose capacity fits it\n",
            frame, xud_result_name(step));
    }
    (*shortFrames)++;
}

static int xud_run(XudContext* context) {
    XudCamera3D camera = xudWavingStartCamera;
    XudFontData fontData
        = { .fileData = NULL, .fileBytes = 0u, .pixelSize = XUD_WAVING_FONT_PIXELS, .codepoints = NULL, .codepointCount = 0u };
    uint8_t* fontFile = NULL;
    uint32_t fontBytes = 0u;
    uint32_t frame = 0u;
    uint32_t shortFrames = 0u;
    XudHandle font;
    XudResult step;

    fontFile = xud_waving_read(XUD_WAVING_FONT_PATH, &fontBytes);
    if (fontFile == NULL) {
        printf(LOGNAME ": no font at %s, so the fps line has nothing to draw with\n", XUD_WAVING_FONT_PATH);
        return 1;
    }
    fontData.fileData = fontFile;
    fontData.fileBytes = fontBytes;
    step = xud_load_font(&font, context, &fontData);
    if (step != XUD_OK) {
        xud_text_error("xud_load_font", step);
        free(fontFile);
        return 1;
    }
    if (xud_set_target_fps(context, 60u) != XUD_OK) {
        printf(LOGNAME ": xud_set_target_fps -> refused, so the run free-runs\n");
    }

    for (;;) {
        bool shouldClose = false;
        bool pressed = false;
        char line[XUD_WAVING_LINE_MAX];
        XudColor fpsColor = xudWavingLime;
        double seconds = 0.0;
        float fps = 0.0f;
        float scale = 0.0f;
        int written = 0;

        step = xud_window_should_close(&shouldClose, context);
        if (step != XUD_OK) {
            xud_text_error("frame xud_window_should_close", step);
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

        /* The clock, the time scale and the orbit: raylib's Update half, its file lines 53 to 61,
           read once a frame rather than once a cell.
        */
        if (xud_get_time(&seconds, context) != XUD_OK) {
            seconds = 0.0;
        }
        scale = (2.0f + (float)sin(seconds)) * 0.7f;
        camera.position.x = (float)cos(seconds * 0.3) * XUD_WAVING_ORBIT_RADIUS;
        camera.position.z = (float)sin(seconds * 0.3) * XUD_WAVING_ORBIT_RADIUS;

        step = xud_begin_drawing(context);
        if (step != XUD_OK) {
            xud_text_error("frame xud_begin_drawing", step);
            break;
        }
        step = xud_clear_background(context, xudWavingRaywhite);
        if (step != XUD_OK) {
            xud_text_error("frame xud_clear_background", step);
            (void)xud_end_drawing(context);
            break;
        }
        /* Begun here and NOT ended inside the frame: see the Pairing half of the banner. */
        step = xud_begin_camera_3d(context, camera);
        if (step != XUD_OK) {
            xud_text_error("frame xud_begin_camera_3d", step);
            (void)xud_end_drawing(context);
            break;
        }
        step = xud_waving_field(context, seconds, scale);
        if (step != XUD_OK) {
            xud_waving_count_short_frame(frame, step, &shortFrames);
        }

        /* raylib's DrawFPS(10, 10), its three bands and its one string (rtext.c 1199 to 1205). */
        if (xud_get_fps(&fps, context) != XUD_OK) {
            fps = 0.0f;
        }
        if (fps < 15.0f) {
            fpsColor = xudWavingRed;
        } else if (fps < 30.0f) {
            fpsColor = xudWavingOrange;
        }
        written = snprintf(line, sizeof(line), "%2d FPS", (int)fps);
        if ((written > 0) && ((size_t)written < sizeof(line))) {
            step = xud_waving_text(context, font, XUD_WAVING_FPS_X, XUD_WAVING_FPS_Y, line, fpsColor);
            if (step != XUD_OK) {
                xud_waving_count_short_frame(frame, step, &shortFrames);
            }
        }

        step = xud_end_drawing(context);
        if (step != XUD_OK) {
            xud_text_error("frame xud_end_drawing", step);
            break;
        }
        frame++;
    }

    /* The camera goes away with the run rather than with the frame: the last frame it was live for
       has been submitted by now.
    */
    step = xud_end_camera_3d(context);
    if (step != XUD_OK) {
        xud_text_error("xud_end_camera_3d", step);
    }
    step = xud_close_font(context, font);
    if (step != XUD_OK) {
        xud_text_error("xud_close_font", step);
    }
    free(fontFile);

    printf(LOGNAME ": %u frame(s), %u of them short\n", frame, shortFrames);
    return 0;
}

int main(void) {
    XudContext* context = NULL;
    XudResult step;
    int status = 0;

    step = xud_init_context(&context, XUD_WAVING_WIDTH, XUD_WAVING_HEIGHT);
    printf(
        LOGNAME ": xud_init_context(%u, %u) -> %s (xudlibc %s)\n", XUD_WAVING_WIDTH, XUD_WAVING_HEIGHT, xud_result_name(step), XUD_VERSION);
    if (step != XUD_OK) {
        return (step == XUD_UNSUPPORTED) ? 0 : 1;
    }

    status = xud_run(context);

    step = xud_close_context(&context);
    printf(LOGNAME ": xud_close_context -> %s\n", xud_result_name(step));

    return (status != 0) ? status : ((step == XUD_OK) ? 0 : 1);
}
