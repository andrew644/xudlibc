/***********************************************************************************************
 *
 *   camera_free.c - raylib's core_3d_camera_free example on xudlibc
 *
 *   Owns : one context, one font, one camera record and one cube. The scene is the shapes rung's -
 *          a box, its wire frame and the ground grid - and the example is about the one mode of
 *          xud_update_camera() with no body walking it: XUD_CAMERA_MODE_FREE, the mode whose eye is
 *          free in every direction, so the arrow keys and the mouse turn it, the wheel pushes it
 *          toward or away from its target, and the wheel held down slides it across the plane the
 *          up vector spans. That is rcamera's CAMERA_FREE (reference/raylib/src/rcamera.h 479 to
 *          489) and it is the mode raylib's own example is a demonstration of: the camera is the
 *          subject, the cube is scenery. Layer: examples. Public API only (xud.h).
 *
 *   Missing : the two calls raylib's example needs and this library did not have when the first
 *          camera example was written - xud_update_camera() (rcamera's UpdateCamera, raylib.h 1213)
 *          and xud_hide_cursor()/xud_show_cursor() (DisableCursor/EnableCursor). Both are public
 *          now, so this file adds nothing to the library; it is the second consumer of the pair, and
 *          the first one that never walks.
 *
 *   Pairing : the camera pair does NOT nest inside the drawing pair the way raylib's
 *          BeginMode3D/EndMode3D do - the note examples/camera_first_person.c states applies here in
 *          full: this library holds ONE matrix per frame and writes it at the submission, so a camera
 *          ended before xud_end_drawing() is a frame drawn in clip space. The camera is begun at the
 *          top of the frame and taken back when the run is over.
 *
 *   Controls: the mouse turns the view, the wheel zooms toward and away from the target, the wheel
 *          held down pans, Z puts the target back at the origin - raylib's example's one key
 *          (its file, line 52) - and ESC quits. The cursor is hidden for the whole run, as raylib's
 *          example hides it at line 35: a free camera has no pixel to point at.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include <stdarg.h> /* Required for: va_start, va_end, the TextFormat half of the HUD below */
#include <stdio.h>  /* Required for: printf, snprintf, and the file's own report */
#include <stdlib.h> /* Required for: free, the font file the reader below hands up */

#include "xud.h" /* Required for: the camera record and the mode list, xud_update_camera, the shapes the scene is built from, the screen-fixed text pair, the cursor pair, the input and clock queries, and the context's own pair - the whole public API this example is written against */

/* The window: raylib's example names 800x450 and this is the same extent, so the HUD's box is the
   same box at the same pixels.
*/
#define XUD_CAMERA_FREE_WIDTH 800u
#define XUD_CAMERA_FREE_HEIGHT 450u

/* The font: the file's bytes (the public load takes bytes and not a path), baked at an em height the
   HUD's ten-pixel rows would only round up from, and the default codepoint set - a NULL array with a
   zero count is the printable ASCII run, which is every character the HUD below writes.
*/
#define XUD_CAMERA_FREE_FONT_PATH "resources/fonts/NotoSansTC-Regular.ttf"
#define XUD_CAMERA_FREE_FONT_PIXELS 16u
#define XUD_CAMERA_FREE_LINE_MAX 96

/* The cube raylib's example stands at the origin: two units each way, red with a maroon frame. */
#define XUD_CAMERA_FREE_CUBE_SIZE 2.0f

/* The HUD's box: the same 320x93 raylib's example draws at (10, 10) (its file, line 71), with the
   rows inside it at the same offsets.
*/
#define XUD_CAMERA_FREE_BOX_X 10
#define XUD_CAMERA_FREE_BOX_Y 10
#define XUD_CAMERA_FREE_BOX_WIDTH 320
#define XUD_CAMERA_FREE_BOX_HEIGHT 93

/* The palette, the raylib colours the same example names, as the bytes this library's colour is:
   four uint8_t in RGBA order, which is the batch's own vertex format rather than a converted one.
*/
static const XudColor xudCameraFreeRaywhite = { .r = 245u, .g = 245u, .b = 250u, .a = 255u };
static const XudColor xudCameraFreeLightGray = { .r = 200u, .g = 200u, .b = 200u, .a = 255u };
static const XudColor xudCameraFreeGray = { .r = 130u, .g = 130u, .b = 130u, .a = 255u };
static const XudColor xudCameraFreeDarkGray = { .r = 90u, .g = 90u, .b = 90u, .a = 255u };
static const XudColor xudCameraFreeSky = { .r = 130u, .g = 170u, .b = 205u, .a = 255u };
static const XudColor xudCameraFreeBlue = { .r = 0u, .g = 120u, .b = 240u, .a = 255u };
static const XudColor xudCameraFreeDarkBlue = { .r = 0u, .g = 60u, .b = 120u, .a = 255u };
static const XudColor xudCameraFreeRed = { .r = 220u, .g = 40u, .b = 40u, .a = 255u };
static const XudColor xudCameraFreeMaroon = { .r = 127u, .g = 0u, .b = 0u, .a = 255u };
static const XudColor xudCameraFreeBlack = { .r = 0u, .g = 0u, .b = 0u, .a = 255u };

/* The camera the run starts on: the ten-unit diagonal raylib's example opens with (its file, lines
   28 to 31), looking at the origin from a corner of the grid, which is the framing that shows the
   grid's two-tone centre lines cross under the cube.
*/
static const XudCamera3D xudCameraFreeStartCamera = { .position = { .x = 10.0f, .y = 10.0f, .z = 10.0f },
    .target = { .x = 0.0f, .y = 0.0f, .z = 0.0f },
    .up = { .x = 0.0f, .y = 1.0f, .z = 0.0f },
    .fovy = 45.0f,
    .near = 0.1f,
    .far = 200.0f };

static uint8_t* xud_camera_free_read(const char* path, uint32_t* outBytes);
static XudResult xud_camera_free_text(
    XudContext* context, XudHandle font, int32_t posX, int32_t posY, const char* format, XudColor color, ...);
static XudResult xud_camera_free_hud(XudContext* context, XudHandle font, const XudCamera3D* camera);

/*-----------------------------------------------------------------------------------------------
 * The two pieces of machinery the HUD needs: a file reader, and a text call that formats
 *-----------------------------------------------------------------------------------------------*/

/* The font file's bytes: xud_load_font() takes bytes rather than a path, so the public header
   cannot read the file for the caller and the caller reads it here. NULL when there is no file.
*/
static uint8_t* xud_camera_free_read(const char* path, uint32_t* outBytes) {
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
   keyed by codepoint and not by byte - and the row is drawn screen-fixed at the caller's pixel. A row
   that does not fit the buffer is refused rather than drawn truncated.
*/
static XudResult xud_camera_free_text(
    XudContext* context, XudHandle font, int32_t posX, int32_t posY, const char* format, XudColor color, ...) {
    char line[XUD_CAMERA_FREE_LINE_MAX];
    uint32_t codepoints[XUD_CAMERA_FREE_LINE_MAX];
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

    return xud_draw_text_screen(context, font, codepoints, (uint32_t)written, posX, posY, (float)XUD_CAMERA_FREE_FONT_PIXELS, 0.0f, color);
}

/*-----------------------------------------------------------------------------------------------
 * The HUD
 *-----------------------------------------------------------------------------------------------*/

/* The controls box raylib's example draws at the same pixels (its file, lines 71 to 76), plus the
   eye and target the run is moving - which is the part of the free camera a picture of it cannot
   show: the mode's whole state is the two points and the up between them, and Z is the one key that
   puts one of them back. The box is one opaque pale blue rather than raylib's half-alpha sky: the
   row these quads record into is the opaque one, so a faded colour here would be a full one with a
   misleading name.
*/
static XudResult xud_camera_free_hud(XudContext* context, XudHandle font, const XudCamera3D* camera) {
    XudResult result;

    result = xud_draw_rectangle(
        context, XUD_CAMERA_FREE_BOX_X, XUD_CAMERA_FREE_BOX_Y, XUD_CAMERA_FREE_BOX_WIDTH, XUD_CAMERA_FREE_BOX_HEIGHT, xudCameraFreeSky);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_draw_rectangle_lines(
        context, XUD_CAMERA_FREE_BOX_X, XUD_CAMERA_FREE_BOX_Y, XUD_CAMERA_FREE_BOX_WIDTH, XUD_CAMERA_FREE_BOX_HEIGHT, xudCameraFreeBlue);
    if (result != XUD_OK) {
        return result;
    }

    result = xud_camera_free_text(
        context, font, XUD_CAMERA_FREE_BOX_X + 10, XUD_CAMERA_FREE_BOX_Y + 8, "Free camera default controls:", xudCameraFreeBlack);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_camera_free_text(
        context, font, XUD_CAMERA_FREE_BOX_X + 30, XUD_CAMERA_FREE_BOX_Y + 26, "- Mouse Wheel to Zoom in-out", xudCameraFreeDarkGray);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_camera_free_text(
        context, font, XUD_CAMERA_FREE_BOX_X + 30, XUD_CAMERA_FREE_BOX_Y + 44, "- Mouse Wheel Pressed to Pan", xudCameraFreeDarkGray);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_camera_free_text(
        context, font, XUD_CAMERA_FREE_BOX_X + 30, XUD_CAMERA_FREE_BOX_Y + 62, "- Z to zoom to (0, 0, 0)", xudCameraFreeDarkGray);
    if (result != XUD_OK) {
        return result;
    }

    return xud_camera_free_text(context, font, (int32_t)XUD_CAMERA_FREE_WIDTH - 300, 10,
        "eye (%.2f, %.2f, %.2f)  target (%.2f, %.2f, %.2f)", xudCameraFreeDarkBlue, (double)camera->position.x, (double)camera->position.y,
        (double)camera->position.z, (double)camera->target.x, (double)camera->target.y, (double)camera->target.z);
}

/*-----------------------------------------------------------------------------------------------
 * The run
 *-----------------------------------------------------------------------------------------------*/

int main(void) {
    XudCamera3D camera = xudCameraFreeStartCamera;
    XudFontData fontData
        = { .fileData = NULL, .fileBytes = 0u, .pixelSize = XUD_CAMERA_FREE_FONT_PIXELS, .codepoints = NULL, .codepointCount = 0u };
    uint8_t* fontFile = NULL;
    uint32_t fontBytes = 0u;
    uint32_t frame = 0u;
    uint32_t shortFrames = 0u;
    XudContext* context = NULL;
    XudHandle font;
    XudResult step;

    fontFile = xud_camera_free_read(XUD_CAMERA_FREE_FONT_PATH, &fontBytes);
    if (fontFile == NULL) {
        printf("camera_free: no font at %s, so the HUD has nothing to draw with\n", XUD_CAMERA_FREE_FONT_PATH);
        return 1;
    }

    step = xud_init_context(&context, XUD_CAMERA_FREE_WIDTH, XUD_CAMERA_FREE_HEIGHT, "camera_free");
    printf("camera_free: xud_init_context(%u, %u, \"%s\") -> %s (xudlibc %s)\n", XUD_CAMERA_FREE_WIDTH, XUD_CAMERA_FREE_HEIGHT,
        "camera_free", xud_result_name(step), XUD_VERSION);
    if (step != XUD_OK) {
        free(fontFile);
        return (step == XUD_UNSUPPORTED) ? 0 : 1;
    }

    fontData.fileData = fontFile;
    fontData.fileBytes = fontBytes;
    step = xud_load_font(&font, context, &fontData);
    if (step != XUD_OK) {
        printf("camera_free: xud_load_font -> %s\n", xud_result_name(step));
        (void)xud_close_context(&context);
        free(fontFile);
        return 1;
    }

    /* The cursor, gone before the loop: the mode reads the pointer's DELTAS, and a pointer that
       reports an absolute position stops reporting them the moment it reaches the window's edge -
       which for a look-around is inside a second.
    */
    step = xud_hide_cursor(context);
    if (step != XUD_OK) {
        printf("camera_free: xud_hide_cursor -> %s, so the look drags an absolute cursor\n", xud_result_name(step));
    }
    if (xud_set_target_fps(context, 60u) != XUD_OK) {
        printf("camera_free: xud_set_target_fps -> refused, so the run free-runs\n");
    }

    for (;;) {
        bool shouldClose = false;
        bool pressed = false;

        step = xud_window_should_close(&shouldClose, context);
        if (step != XUD_OK) {
            printf("camera_free: frame %u xud_window_should_close -> %s\n", frame, xud_result_name(step));
            break;
        }
        if (shouldClose) {
            printf("camera_free: frame %u: the window was closed\n", frame);
            break;
        }
        if ((xud_is_key_pressed(&pressed, context, XUD_KEY_ESCAPE) == XUD_OK) && pressed) {
            printf("camera_free: frame %u: ESC\n", frame);
            break;
        }
        /* Z, raylib's example's one key: the target goes back to the origin and the eye stays where
           it is, which is the mode's own zoom-to-point rather than a reset of the camera.
        */
        if ((xud_is_key_pressed(&pressed, context, XUD_KEY_Z) == XUD_OK) && pressed) {
            camera.target = (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 0.0f };
        }

        step = xud_update_camera(context, &camera, XUD_CAMERA_MODE_FREE);
        if (step != XUD_OK) {
            printf("camera_free: frame %u xud_update_camera(FREE) -> %s\n", frame, xud_result_name(step));
            break;
        }

        step = xud_begin_drawing(context);
        if (step != XUD_OK) {
            printf("camera_free: frame %u xud_begin_drawing -> %s, and the run stops here\n", frame, xud_result_name(step));
            break;
        }
        (void)xud_clear_background(context, xudCameraFreeRaywhite);

        /* The camera, begun here and NOT ended inside the frame: see the note in the header and the
           longer one in examples/camera_first_person.c. Each frame's begin replaces the live record.
        */
        (void)xud_begin_camera_3d(context, camera);
        step = xud_draw_cube(context, (XudVector3) { .x = 0.0f, .y = XUD_CAMERA_FREE_CUBE_SIZE * 0.5f, .z = 0.0f },
            XUD_CAMERA_FREE_CUBE_SIZE, XUD_CAMERA_FREE_CUBE_SIZE, XUD_CAMERA_FREE_CUBE_SIZE, xudCameraFreeRed);
        if (step == XUD_OK) {
            step = xud_draw_cube_wires(context, (XudVector3) { .x = 0.0f, .y = XUD_CAMERA_FREE_CUBE_SIZE * 0.5f, .z = 0.0f },
                XUD_CAMERA_FREE_CUBE_SIZE, XUD_CAMERA_FREE_CUBE_SIZE, XUD_CAMERA_FREE_CUBE_SIZE, xudCameraFreeMaroon);
        }
        if (step == XUD_OK) {
            step = xud_draw_grid(
                context, (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 0.0f }, 10u, 1.0f, xudCameraFreeLightGray, xudCameraFreeGray);
        }
        if (step != XUD_OK) {
            /* The batch's capacity doubles at the start of a frame, so the first frames of a scene
               are drawn short until the capacity reaches what it asks for. Every frame redraws
               everything, so a short one costs only the shapes that did not fit.
            */
            if (shortFrames == 0u) {
                printf("camera_free: frame %u a draw refused -> %s; the scene is whole from the frame whose capacity fits it\n", frame,
                    xud_result_name(step));
            }
            shortFrames++;
        }

        step = xud_camera_free_hud(context, font, &camera);
        if (step != XUD_OK) {
            if (shortFrames == 0u) {
                printf("camera_free: frame %u a HUD draw refused -> %s\n", frame, xud_result_name(step));
            }
            shortFrames++;
        }
        step = xud_end_drawing(context);
        if (step != XUD_OK) {
            printf("camera_free: frame %u xud_end_drawing -> %s, and the run stops here\n", frame, xud_result_name(step));
            break;
        }
        frame++;
    }

    /* The camera goes away with the run rather than with the frame: the last frame it was live for
       has been submitted by now.
    */
    step = xud_end_camera_3d(context);
    if (step != XUD_OK) {
        printf("camera_free: xud_end_camera_3d -> %s\n", xud_result_name(step));
    }

    printf("camera_free: %u frame(s), %u of them short, eye (%.2f, %.2f, %.2f), target (%.2f, %.2f, %.2f)\n", frame, shortFrames,
        (double)camera.position.x, (double)camera.position.y, (double)camera.position.z, (double)camera.target.x, (double)camera.target.y,
        (double)camera.target.z);

    (void)xud_show_cursor(context);
    step = xud_close_font(context, font);
    if (step != XUD_OK) {
        printf("camera_free: xud_close_font -> %s\n", xud_result_name(step));
    }
    free(fontFile);
    step = xud_close_context(&context);
    printf("camera_free: xud_close_context -> %s\n", xud_result_name(step));

    return (step == XUD_OK) ? 0 : 1;
}
