/***********************************************************************************************
 *
 *   models_geometric_shapes.c - raylib's models_geometric_shapes example on xudlibc
 *
 *   Owns : one context, one font for the one HUD line, and one camera record. The scene is the
 *          shapes rung's own - two boxes and three wire boxes, a sphere and a wire sphere, a
 *          frustum, a cone and three more cylinders' worth of wire, a capsule and its wires, and
 *          the ground grid - drawn once per frame from a fixed eye, with no input beyond the close
 *          and ESC. The solids and the eye are raylib's, at its file's own lines 32 to 74: the
 *          positions, the sizes and the colours are copied row for row, so a picture of this run
 *          and a picture of that one are the same picture. Layer: examples. Public API only.
 *
 *   Missing : three of raylib's calls have no counterpart here, and two of them change an argument.
 *          DrawFPS (its file, line 78) is a text call in this revision (rtext.c 1197: the bands are
 *          LIME above 30, ORANGE to 15, RED below, at 20 pixels), and this library has no default
 *          font to draw it with - so the fps line is xud_get_fps() through snprintf and the
 *          UTF-32 half of the text pair, at the same pixel and the same three bands. DrawCylinder
 *          and DrawCylinderWires there take a side count (4, 6 and 8 at its file's lines 64 to 69);
 *          xud_draw_cylinder() takes none, because a tessellation there is a claim about the batch's
 *          capacity rather than about the cylinder, and the header owns it (XUD_SHAPES_CYLINDER_
 *          SIDE_COUNT). DrawSphereWires' and DrawCapsuleWires' tessellations are the header's for
 *          the same reason. DrawGrid there is one grey; this library's grid takes both colours, so
 *          the two centre lines are raylib's own 0.5 grey and the rest its own 0.75 (rmodels.c 1096
 *          and 1100).
 *
 *   Pairing : the camera pair does NOT nest inside the drawing pair the way raylib's BeginMode3D/
 *          EndMode3D do. This library writes ONE matrix per frame, into set 0's block at the
 *          submission itself, so a camera ended inside the frame is a frame drawn in clip space:
 *          the grid's 10 units would be a line of pixels and the boxes off the screen. The camera is
 *          therefore begun every frame - each begin replacing the record before it - and taken back
 *          when the run ends, which is the one shape of raylib's example not copied.
 *
 *   Controls: nothing but the window's close button and ESC, which is all raylib's example answers.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include <stdio.h>  /* Required for: fopen, fseek, ftell, fread, fclose, snprintf, printf */
#include <stdlib.h> /* Required for: free and malloc, the one buffer the font file is read into */

#include "xud.h" /* Required for: the camera record, the frame pair, the eight shape calls, the ground grid, the font pair and the screen-fixed text, the clock's fps half and the context's own pair - the whole public API this example is written against */
#include <string.h> /* Required for: strlen, the length of the row the fps line formats into */

#define LOGNAME "models_geometric_shapes"

/* raylib's own extent (its file, lines 25 and 26), so the HUD's pixel is the same pixel. */
#define XUD_SHAPES_WIDTH 800u
#define XUD_SHAPES_HEIGHT 450u

/* The font: the repo's own asset, read as bytes because xud_load_font() takes bytes and not a path,
   baked at an em height the 20-pixel fps line only scales down from.
*/
#define XUD_SHAPES_FONT_PATH "resources/fonts/NotoSansTC-Regular.ttf"
#define XUD_SHAPES_FONT_PIXELS 16u

/* The fps line: raylib's DrawFPS at its own pixel (its file, line 78) and its own size. */
#define XUD_SHAPES_FPS_X 10
#define XUD_SHAPES_FPS_Y 10
#define XUD_SHAPES_FPS_PIXELS 20.0f
#define XUD_SHAPES_LINE_MAX 32

/* The palette, raylib's own bytes (reference/raylib/src/raylib.h 174 to 200) as this library's
   colour: four uint8_t in the order the batch's COLOR stream holds them.
*/
static const XudColor xudShapesRaywhite = { .r = 245u, .g = 245u, .b = 245u, .a = 255u };
static const XudColor xudShapesRed = { .r = 230u, .g = 41u, .b = 55u, .a = 255u };
static const XudColor xudShapesGold = { .r = 255u, .g = 203u, .b = 0u, .a = 255u };
static const XudColor xudShapesMaroon = { .r = 190u, .g = 33u, .b = 55u, .a = 255u };
static const XudColor xudShapesGreen = { .r = 0u, .g = 228u, .b = 48u, .a = 255u };
static const XudColor xudShapesLime = { .r = 0u, .g = 158u, .b = 47u, .a = 255u };
static const XudColor xudShapesSky = { .r = 102u, .g = 191u, .b = 255u, .a = 255u };
static const XudColor xudShapesDarkBlue = { .r = 0u, .g = 82u, .b = 172u, .a = 255u };
static const XudColor xudShapesBrown = { .r = 127u, .g = 106u, .b = 79u, .a = 255u };
static const XudColor xudShapesPink = { .r = 255u, .g = 109u, .b = 194u, .a = 255u };
static const XudColor xudShapesViolet = { .r = 135u, .g = 60u, .b = 190u, .a = 255u };
static const XudColor xudShapesPurple = { .r = 200u, .g = 122u, .b = 255u, .a = 255u };
static const XudColor xudShapesOrange = { .r = 255u, .g = 161u, .b = 0u, .a = 255u };

/* The grid's two greys, raylib's 0.5 and 0.75 (rmodels.c 1096 and 1100). */
static const XudColor xudShapesGridCenter = { .r = 128u, .g = 128u, .b = 128u, .a = 255u };
static const XudColor xudShapesGridLine = { .r = 191u, .g = 191u, .b = 191u, .a = 255u };

/* The eye raylib's example opens on (its file, lines 32 to 35), plus the two planes the camera
   record carries here and raylib's aggregate does not: the scene is twelve units across, so half a
   unit in front and two hundred behind is the whole depth budget of it.
*/
static const XudCamera3D xudShapesStartCamera = { .position = { .x = 0.0f, .y = 10.0f, .z = 10.0f },
    .target = { .x = 0.0f, .y = 0.0f, .z = 0.0f },
    .up = { .x = 0.0f, .y = 1.0f, .z = 0.0f },
    .fovy = 45.0f,
    .near = 0.5f,
    .far = 200.0f };

static uint8_t* xud_shapes_read(const char* path, uint32_t* outBytes);
static void xud_text_error(const char* stage, XudResult result);
static XudResult xud_shapes_text(XudContext* context, XudHandle font, int32_t posX, int32_t posY, const char* line, XudColor color);
static XudResult xud_shapes_scene(XudContext* context);
static int xud_run(XudContext* context);

/*-----------------------------------------------------------------------------------------------
 * The machinery the run needs: a file reader, the run's own report, and a text call
 *-----------------------------------------------------------------------------------------------*/

/* The font file's bytes: xud_load_font() takes bytes rather than a path, so the public header
   cannot read the file for the caller and the caller reads it here. NULL when there is no file.
*/
static uint8_t* xud_shapes_read(const char* path, uint32_t* outBytes) {
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
   screen-fixed, so the camera moves the scene around it rather than the string.
*/
static XudResult xud_shapes_text(XudContext* context, XudHandle font, int32_t posX, int32_t posY, const char* line, XudColor color) {
    uint32_t codepoints[XUD_SHAPES_LINE_MAX];
    const size_t bytes = strlen(line);
    uint32_t step = 0u;

    if ((bytes == 0u) || (bytes >= sizeof(codepoints))) {
        return XUD_INVALID_ARG;
    }
    for (step = 0u; step < (uint32_t)bytes; step++) {
        codepoints[step] = (uint32_t)(uint8_t)line[step];
    }

    return xud_draw_text_screen(context, font, codepoints, (uint32_t)bytes, posX, posY, XUD_SHAPES_FPS_PIXELS, 0.0f, color);
}

/*-----------------------------------------------------------------------------------------------
 * The scene
 *-----------------------------------------------------------------------------------------------*/

/* The twelve calls raylib's example makes (its file, lines 57 to 74), in its order, at its
   positions, at its sizes, in its colours. The grid is the same 10 divisions of 1.0 unit that its
   DrawGrid(10, 1.0f) is - this library's grid is centred on a point the caller names, and the point
   is the origin here as it is there.
*/
static XudResult xud_shapes_scene(XudContext* context) {
    static const XudVector3 box = { .x = -4.0f, .y = 0.0f, .z = 2.0f };
    static const XudVector3 wireBoxFar = { .x = -4.0f, .y = 0.0f, .z = -2.0f };
    static const XudVector3 sphere = { .x = -1.0f, .y = 0.0f, .z = -2.0f };
    static const XudVector3 sphereWires = { .x = 1.0f, .y = 0.0f, .z = 2.0f };
    static const XudVector3 frustum = { .x = 4.0f, .y = 0.0f, .z = -2.0f };
    static const XudVector3 shortCylinder = { .x = 4.5f, .y = -1.0f, .z = 2.0f };
    static const XudVector3 cone = { .x = 1.0f, .y = 0.0f, .z = -4.0f };
    static const XudVector3 capsuleStart = { .x = -3.0f, .y = 1.5f, .z = -4.0f };
    static const XudVector3 capsuleEnd = { .x = -4.0f, .y = -1.0f, .z = -4.0f };
    static const XudVector3 gridCenter = { .x = 0.0f, .y = 0.0f, .z = 0.0f };
    XudResult step;

    step = xud_draw_cube(context, box, 2.0f, 5.0f, 2.0f, xudShapesRed);
    if (step != XUD_OK) {
        return step;
    }
    step = xud_draw_cube_wires(context, box, 2.0f, 5.0f, 2.0f, xudShapesGold);
    if (step != XUD_OK) {
        return step;
    }
    step = xud_draw_cube_wires(context, wireBoxFar, 3.0f, 6.0f, 2.0f, xudShapesMaroon);
    if (step != XUD_OK) {
        return step;
    }
    step = xud_draw_sphere(context, sphere, 1.0f, xudShapesGreen);
    if (step != XUD_OK) {
        return step;
    }
    step = xud_draw_sphere_wires(context, sphereWires, 2.0f, xudShapesLime);
    if (step != XUD_OK) {
        return step;
    }
    step = xud_draw_cylinder(context, frustum, 1.0f, 2.0f, 3.0f, xudShapesSky);
    if (step != XUD_OK) {
        return step;
    }
    step = xud_draw_cylinder_wires(context, frustum, 1.0f, 2.0f, 3.0f, xudShapesDarkBlue);
    if (step != XUD_OK) {
        return step;
    }
    step = xud_draw_cylinder_wires(context, shortCylinder, 1.0f, 1.0f, 2.0f, xudShapesBrown);
    if (step != XUD_OK) {
        return step;
    }
    /* The cone is the cylinder with nothing on top, the same pair raylib draws at its 68 and 69. */
    step = xud_draw_cylinder(context, cone, 0.0f, 1.5f, 3.0f, xudShapesGold);
    if (step != XUD_OK) {
        return step;
    }
    step = xud_draw_cylinder_wires(context, cone, 0.0f, 1.5f, 3.0f, xudShapesPink);
    if (step != XUD_OK) {
        return step;
    }
    step = xud_draw_capsule(context, capsuleStart, capsuleEnd, 1.2f, xudShapesViolet);
    if (step != XUD_OK) {
        return step;
    }
    step = xud_draw_capsule_wires(context, capsuleStart, capsuleEnd, 1.2f, xudShapesPurple);
    if (step != XUD_OK) {
        return step;
    }

    return xud_draw_grid(context, gridCenter, 10u, 1.0f, xudShapesGridCenter, xudShapesGridLine);
}

/*-----------------------------------------------------------------------------------------------
 * The run
 *-----------------------------------------------------------------------------------------------*/

/* One short frame said out loud, and only the first: the batch's capacity DOUBLES at the start of a
   frame (xud_begin_drawing's growth), so a scene of twelve solids and their wires is drawn short for
   the first few frames and whole from the frame whose capacity fits it. Every frame redraws the whole
   scene, so a short one costs nothing but the shapes that did not fit.
*/
static void xud_shapes_count_short_frame(uint32_t frame, XudResult step, uint32_t* shortFrames) {
    if (*shortFrames == 0u) {
        printf(LOGNAME ": frame %u draw %s; the batch's capacity doubles at the start of the next frame, so this\n" LOGNAME
                       ":   scene is short for the first few frames and whole from the one whose capacity fits it\n",
            frame, xud_result_name(step));
    }
    (*shortFrames)++;
}

static int xud_run(XudContext* context) {
    XudCamera3D camera = xudShapesStartCamera;
    XudFontData fontData
        = { .fileData = NULL, .fileBytes = 0u, .pixelSize = XUD_SHAPES_FONT_PIXELS, .codepoints = NULL, .codepointCount = 0u };
    uint8_t* fontFile = NULL;
    uint32_t fontBytes = 0u;
    uint32_t frame = 0u;
    uint32_t shortFrames = 0u;
    XudHandle font;
    XudResult step;

    fontFile = xud_shapes_read(XUD_SHAPES_FONT_PATH, &fontBytes);
    if (fontFile == NULL) {
        printf(LOGNAME ": no font at %s, so the fps line has nothing to draw with\n", XUD_SHAPES_FONT_PATH);
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
        char line[XUD_SHAPES_LINE_MAX];
        XudColor fpsColor = xudShapesLime;
        float fps = 0.0f;
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

        step = xud_begin_drawing(context);
        if (step != XUD_OK) {
            xud_text_error("frame xud_begin_drawing", step);
            break;
        }
        step = xud_clear_background(context, xudShapesRaywhite);
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
        step = xud_shapes_scene(context);
        if (step != XUD_OK) {
            xud_shapes_count_short_frame(frame, step, &shortFrames);
        }

        /* raylib's DrawFPS(10, 10), which in this revision is the fps, its three bands and its one
           string (rtext.c 1199 to 1205) - the bands and the pixel are its own, the clock is this
           library's.
        */
        if (xud_get_fps(&fps, context) != XUD_OK) {
            fps = 0.0f;
        }
        if (fps < 15.0f) {
            fpsColor = xudShapesRed;
        } else if (fps < 30.0f) {
            fpsColor = xudShapesOrange;
        }
        written = snprintf(line, sizeof(line), "%2d FPS", (int)fps);
        if ((written > 0) && ((size_t)written < sizeof(line))) {
            step = xud_shapes_text(context, font, XUD_SHAPES_FPS_X, XUD_SHAPES_FPS_Y, line, fpsColor);
            if (step != XUD_OK) {
                xud_shapes_count_short_frame(frame, step, &shortFrames);
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

    step = xud_init_context(&context, XUD_SHAPES_WIDTH, XUD_SHAPES_HEIGHT);
    printf(
        LOGNAME ": xud_init_context(%u, %u) -> %s (xudlibc %s)\n", XUD_SHAPES_WIDTH, XUD_SHAPES_HEIGHT, xud_result_name(step), XUD_VERSION);
    if (step != XUD_OK) {
        return (step == XUD_UNSUPPORTED) ? 0 : 1;
    }

    status = xud_run(context);

    step = xud_close_context(&context);
    printf(LOGNAME ": xud_close_context -> %s\n", xud_result_name(step));

    return (status != 0) ? status : ((step == XUD_OK) ? 0 : 1);
}
