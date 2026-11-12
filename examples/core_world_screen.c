/***********************************************************************************************
 *
 *   core_world_screen.c - raylib's core_world_screen example on xudlibc
 *
 *   Owns : one context, one font, one camera record and one cube. The scene is the shapes rung's -
 *          a box, its wire frame and the ground grid - and the example is about the ONE number the
 *          3D world hands back to the 2D surface: where the top of that box lands on the screen,
 *          which is what raylib's GetWorldToScreen answers and what a health bar, a name plate or a
 *          selection outline is placed by. Layer: examples. Public API only (xud.h).
 *
 *   Missing : one thing this example reads that raylib has and xudlibc did not - xud_get_world_to_screen()
 *          (GetWorldToScreen, raylib.h 1083) and its inverse xud_get_screen_to_world_ray()
 *          (GetScreenToWorldRayEx), which are the fold between a frame's pixels and the camera's
 *          world. The extent the fold divides by is this library's rather than raylib's: it reads
 *          xud_get_screen_width()/xud_get_screen_height(), the swapchain's own extent, because a
 *          compositor answers a different size than the one it was asked for more often than a
 *          desktop GL window does. xud_update_camera() (rcamera's UpdateCamera) drives the third
 *          person walk, and xud_hide_cursor()/xud_show_cursor() (DisableCursor/EnableCursor) give
 *          the pointer the look reads deltas from.
 *
 *   Refusal : the fold refuses a point at or behind the eye, where raylib divides by zero and
 *          answers a mirrored pixel. Walk the camera past the box and the label disappears instead
 *          of jumping to the other side of the frame; the HUD says so while it is gone. That is the
 *          example's whole subject, so it is drawn rather than mentioned.
 *
 *   Pairing : the camera pair does NOT nest inside the drawing pair the way raylib's
 *          BeginMode3D/EndMode3D do - the same note examples/camera_first_person.c states: this
 *          library writes ONE matrix per frame, at the submission, so a camera ended inside the
 *          frame is a frame drawn in clip space. The fold the label is placed by needs no live
 *          camera at all: it is arithmetic on the record and the extent, which is why it runs in the
 *          Update half of the loop, before the draws, exactly where raylib calls GetWorldToScreen.
 *
 *   Controls: the arrow keys and the mouse look around the box, W A S D walk, the scroll wheel and
 *          the keypad's + - zoom the third-person distance, TAB hides or shows the cursor, ESC quits.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include <stdarg.h> /* Required for: va_start, va_end, the TextFormat half of the HUD below */
#include <stdio.h>  /* Required for: printf, snprintf, and the file's own report */
#include <stdlib.h> /* Required for: free, the font file the reader below hands up */

#include "xud.h" /* Required for: the camera record, xud_update_camera, the fold between the world and the screen, the shapes the scene is built from, the screen-fixed text pair, the cursor pair, the input queries and the context's own pair - the whole public API this example is written against */

/* The window. raylib's example names 800x450 and this is the same extent, so the label's centring
   and the HUD's pixel layout below are the same layout.
*/
#define XUD_WORLD_SCREEN_WIDTH 800u
#define XUD_WORLD_SCREEN_HEIGHT 450u

/* The cube: raylib's (2, 2, 2) at the origin, and the lift its label hangs at - "with a little
   offset to be in top" (its file, line 53), which is the top face of a 2-high box and a half unit
   past it.
*/
#define XUD_WORLD_SCREEN_CUBE_SIZE 2.0f
#define XUD_WORLD_SCREEN_LABEL_LIFT 2.5f

/* The font: the file's bytes (the public load takes bytes and not a path), baked at the em height
   raylib's example asks its DrawText for, and the default codepoint set - a NULL array with a zero
   count is the printable ASCII run, which is every character the HUD below writes.
*/
#define XUD_WORLD_SCREEN_FONT_PATH "resources/fonts/NotoSansTC-Regular.ttf"
#define XUD_WORLD_SCREEN_FONT_PIXELS 20u
#define XUD_WORLD_SCREEN_LINE_MAX 96

/* The palette, the raylib colours the same example names, as the bytes this library's colour is:
   four uint8_t in RGBA order, which is the batch's own vertex format rather than a converted one.
*/
static const XudColor xudWorldScreenRaywhite = { .r = 245u, .g = 245u, .b = 250u, .a = 255u };
static const XudColor xudWorldScreenLightGray = { .r = 200u, .g = 200u, .b = 200u, .a = 255u };
static const XudColor xudWorldScreenGray = { .r = 130u, .g = 130u, .b = 130u, .a = 255u };
static const XudColor xudWorldScreenRed = { .r = 200u, .g = 0u, .b = 0u, .a = 255u };
static const XudColor xudWorldScreenMaroon = { .r = 127u, .g = 0u, .b = 0u, .a = 255u };
static const XudColor xudWorldScreenLime = { .r = 0u, .g = 220u, .b = 80u, .a = 255u };
static const XudColor xudWorldScreenBlack = { .r = 0u, .g = 0u, .b = 0u, .a = 255u };

/* The camera the run opens on: raylib's (its file, line 32 to 35) - ten units out on all three axes
   looking at the origin over up +Y, a 45 degree aperture, and raylib's own cull distances
   (RL_CULL_DISTANCE_NEAR, RL_CULL_DISTANCE_FAR in its config.h) because the example leaves them at
   the library's defaults. The two planes are depth-buffer questions only: the fold this example is
   about reads the aperture and the basis and never the planes.
*/
static const XudCamera3D xudWorldScreenStartCamera = { .position = { .x = 10.0f, .y = 10.0f, .z = 10.0f },
    .target = { .x = 0.0f, .y = 0.0f, .z = 0.0f },
    .up = { .x = 0.0f, .y = 1.0f, .z = 0.0f },
    .fovy = 45.0f,
    .near = 0.01f,
    .far = 1000.0f };

static uint8_t* xud_world_screen_read(const char* path, uint32_t* outBytes);
static XudResult xud_world_screen_text(
    XudContext* context, XudHandle font, int32_t posX, int32_t posY, const char* format, XudColor color, ...);
static XudResult xud_world_screen_scene(XudContext* context);

/*-----------------------------------------------------------------------------------------------
 * The two pieces of machinery the HUD needs: a file reader, and a text call that formats
 *-----------------------------------------------------------------------------------------------*/

/* The font file's bytes: xud_load_font() takes bytes rather than a path, so the public header
   cannot read the file for the caller and the caller reads it here. NULL when there is no file.
*/
static uint8_t* xud_world_screen_read(const char* path, uint32_t* outBytes) {
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

/* raylib's DrawText(TextFormat(...)) as one call: the format goes through vsnprintf into a row of
   bytes, the bytes become codepoints - the public text pair takes a uint32_t array because a font
   is keyed by codepoint and not by byte - and the row is drawn screen-fixed at the caller's pixel.
   A row that does not fit the buffer is refused rather than drawn truncated: half a label on the
   HUD is a lie about the number it was formatting.
*/
static XudResult xud_world_screen_text(
    XudContext* context, XudHandle font, int32_t posX, int32_t posY, const char* format, XudColor color, ...) {
    char line[XUD_WORLD_SCREEN_LINE_MAX];
    uint32_t codepoints[XUD_WORLD_SCREEN_LINE_MAX];
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

    return xud_draw_text_screen(context, font, codepoints, (uint32_t)written, posX, posY, (float)XUD_WORLD_SCREEN_FONT_PIXELS, 0.0f, color);
}

/*-----------------------------------------------------------------------------------------------
 * The scene
 *-----------------------------------------------------------------------------------------------*/

/* The scene, every frame: the box, the wire frame around that same box, and the grid the box stands
   on - raylib's three calls in raylib's order (its file, line 65 to 68), including the grid's two
   colours, which are the lines through the centre in one and the rest in the other. The grid IS the
   ground rather than a pattern laid over a plane: two surfaces at one height are a z-fight the depth
   buffer settles differently every frame.

   The box is centred ON the ground - 2 high at y zero, so the plane runs through its middle - and
   the eye is above that plane, which is why a screenshot of this scene shows a few grid lines laid
   over the box's lower half: along those rays the ground is reached before the box's lower faces are,
   so the depth buffer is right and the box simply straddles the floor it is drawn on. Move the eye
   below y zero - W A S D and the mouse - and the same lines go behind the box instead.
*/
static XudResult xud_world_screen_scene(XudContext* context) {
    XudResult result;

    result = xud_draw_cube(context, (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 0.0f }, XUD_WORLD_SCREEN_CUBE_SIZE,
        XUD_WORLD_SCREEN_CUBE_SIZE, XUD_WORLD_SCREEN_CUBE_SIZE, xudWorldScreenRed);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_draw_cube_wires(context, (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 0.0f }, XUD_WORLD_SCREEN_CUBE_SIZE,
        XUD_WORLD_SCREEN_CUBE_SIZE, XUD_WORLD_SCREEN_CUBE_SIZE, xudWorldScreenMaroon);
    if (result != XUD_OK) {
        return result;
    }

    return xud_draw_grid(context, (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 0.0f }, 10u, 1.0f, xudWorldScreenLightGray, xudWorldScreenGray);
}

/* One short frame said out loud, and only the first: the batch's capacity DOUBLES at the start of a
   frame, so a scene of a box, its wires, a ten-division grid and the HUD's glyph quads is drawn short
   for the first frame or two and whole from the frame whose capacity fits it. Every frame redraws the
   whole scene, so the short ones cost nothing but the shapes that did not fit.
*/
static void xud_world_screen_count_short_frame(uint32_t frame, XudResult step, uint32_t* shortFrames) {
    if (*shortFrames == 0u) {
        printf("core_world_screen: frame %u a draw refused -> %s; the batch's capacity doubles at the start of the next frame, so this\n"
               "core_world_screen:   scene is short for the first frame or two and whole from the one that fits\n",
            frame, xud_result_name(step));
    }
    (*shortFrames)++;
}

/*-----------------------------------------------------------------------------------------------
 * The run
 *-----------------------------------------------------------------------------------------------*/

int main(void) {
    XudCamera3D camera = xudWorldScreenStartCamera;
    XudFontData fontData
        = { .fileData = NULL, .fileBytes = 0u, .pixelSize = XUD_WORLD_SCREEN_FONT_PIXELS, .codepoints = NULL, .codepointCount = 0u };
    XudVector3 labelWorld = { .x = 0.0f, .y = XUD_WORLD_SCREEN_LABEL_LIFT, .z = 0.0f };
    XudVector2 labelScreen = { .x = 0.0f, .y = 0.0f };
    uint8_t* fontFile = NULL;
    uint32_t fontBytes = 0u;
    uint32_t labelVisible = 0u;
    uint32_t labelHidden = 0u;
    uint32_t cursorHidden = 1u;
    uint32_t frame = 0u;
    uint32_t shortFrames = 0u;
    float labelWidth = 0.0f;
    XudContext* context = NULL;
    XudHandle font;
    XudResult step;

    fontFile = xud_world_screen_read(XUD_WORLD_SCREEN_FONT_PATH, &fontBytes);
    if (fontFile == NULL) {
        printf("core_world_screen: no font at %s, so the label has nothing to draw with\n", XUD_WORLD_SCREEN_FONT_PATH);
        return 1;
    }

    step = xud_init_context(&context, XUD_WORLD_SCREEN_WIDTH, XUD_WORLD_SCREEN_HEIGHT);
    printf("core_world_screen: xud_init_context(%u, %u) -> %s (xudlibc %s)\n", XUD_WORLD_SCREEN_WIDTH, XUD_WORLD_SCREEN_HEIGHT,
        xud_result_name(step), XUD_VERSION);
    if (step != XUD_OK) {
        free(fontFile);
        return (step == XUD_UNSUPPORTED) ? 0 : 1;
    }

    fontData.fileData = fontFile;
    fontData.fileBytes = fontBytes;
    step = xud_load_font(&font, context, &fontData);
    if (step != XUD_OK) {
        printf("core_world_screen: xud_load_font -> %s\n", xud_result_name(step));
        (void)xud_close_context(&context);
        free(fontFile);
        return 1;
    }

    /* raylib's DisableCursor, before the loop rather than inside it: the pointer stops being drawn
       and stops reporting an absolute position, and every frame's look is then the delta since the
       frame before - which is what rcamera's third-person look reads.
    */
    step = xud_hide_cursor(context);
    if (step != XUD_OK) {
        printf("core_world_screen: xud_hide_cursor -> %s, so the look drags an absolute cursor\n", xud_result_name(step));
        cursorHidden = 0u;
    }
    if (xud_set_target_fps(context, 60u) != XUD_OK) {
        printf("core_world_screen: xud_set_target_fps -> refused, so the run free-runs\n");
    }

    /* The fold, asked once before the loop: the extent it divides by is the swapchain's own, and a
       context that never completed bring-up has none. Everything the label needs is then in hand.
    */
    {
        uint32_t extentWidth = 0u;
        uint32_t extentHeight = 0u;

        step = xud_get_screen_width(&extentWidth, context);
        if (step != XUD_OK) {
            printf("core_world_screen: xud_get_screen_width -> %s, so the fold has no extent to divide by\n", xud_result_name(step));
            (void)xud_close_font(context, font);
            (void)xud_close_context(&context);
            free(fontFile);
            return 1;
        }
        (void)xud_get_screen_height(&extentHeight, context);
        printf("core_world_screen: a frame renders at %u x %u, which is the extent the fold divides by\n", extentWidth, extentHeight);
    }

    for (;;) {
        bool shouldClose = false;
        bool pressed = false;

        step = xud_window_should_close(&shouldClose, context);
        if (step != XUD_OK) {
            printf("core_world_screen: frame %u xud_window_should_close -> %s\n", frame, xud_result_name(step));
            break;
        }
        if (shouldClose) {
            printf("core_world_screen: frame %u: the window was closed\n", frame);
            break;
        }
        if ((xud_is_key_pressed(&pressed, context, XUD_KEY_ESCAPE) == XUD_OK) && pressed) {
            printf("core_world_screen: frame %u: ESC\n", frame);
            break;
        }
        if ((xud_is_key_pressed(&pressed, context, XUD_KEY_TAB) == XUD_OK) && pressed) {
            if (cursorHidden != 0u) {
                if (xud_show_cursor(context) == XUD_OK) {
                    cursorHidden = 0u;
                }
            } else {
                if (xud_hide_cursor(context) == XUD_OK) {
                    cursorHidden = 1u;
                }
            }
        }

        step = xud_update_camera(context, &camera, XUD_CAMERA_MODE_THIRD_PERSON);
        if (step != XUD_OK) {
            printf("core_world_screen: frame %u xud_update_camera -> %s\n", frame, xud_result_name(step));
            break;
        }

        /* The Update half, and the one number the example is about: the top of the box, folded from
           the camera's world into the frame's pixels. It runs HERE, before any draw and before the
           camera is begun, because the fold is arithmetic on the record and the extent rather than a
           question about a live camera - the same place raylib calls GetWorldToScreen (its file,
           line 54). A point at or behind the eye is refused, and the label keeps the last place it
           was visible rather than being drawn at a mirrored pixel, which is the one thing this
           example does differently from the file it copies and why.
        */
        step = xud_get_world_to_screen(&labelScreen, context, labelWorld, &camera);
        if (step == XUD_OK) {
            labelVisible = 1u;
        } else {
            if (labelVisible != 0u) {
                labelHidden++;
            }
            labelVisible = 0u;
        }

        step = xud_begin_drawing(context);
        if (step != XUD_OK) {
            printf("core_world_screen: frame %u xud_begin_drawing -> %s, and the run stops here\n", frame, xud_result_name(step));
            break;
        }
        (void)xud_clear_background(context, xudWorldScreenRaywhite);

        /* The camera, begun here and NOT ended inside the frame - see examples/camera_first_person.c
           for what the wrong order draws. Each frame's begin replaces the live record.
        */
        (void)xud_begin_camera_3d(context, camera);
        step = xud_world_screen_scene(context);
        if (step != XUD_OK) {
            xud_world_screen_count_short_frame(frame, step, &shortFrames);
        }

        /* The label, screen-fixed and depth-free like every text draw: the row it records into is the
           UI column's, which neither tests nor writes the depth, so the claim raylib's last HUD line
           makes - "Text 2d should be always on top of the cube" - is a property of the row here
           rather than of the order the draws happen to come in.
        */
        if (labelVisible != 0u) {
            XudVector3 pen = { .x = 0.0f, .y = 0.0f, .z = 0.0f };
            uint32_t labelCodepoints[] = { 'E', 'n', 'e', 'm', 'y', ':', ' ', '1', '0', '0', '/', '1', '0', '0' };

            step = xud_measure_text(&labelWidth, context, font, labelCodepoints,
                (uint32_t)(sizeof(labelCodepoints) / sizeof(labelCodepoints[0])), (float)XUD_WORLD_SCREEN_FONT_PIXELS, 0.0f);
            if (step != XUD_OK) {
                xud_world_screen_count_short_frame(frame, step, &shortFrames);
            }
            pen.x = (float)((int32_t)labelScreen.x) - (labelWidth * 0.5f);
            step = xud_draw_text_screen(context, font, labelCodepoints, (uint32_t)(sizeof(labelCodepoints) / sizeof(labelCodepoints[0])),
                (int32_t)pen.x, (int32_t)labelScreen.y, (float)XUD_WORLD_SCREEN_FONT_PIXELS, 0.0f, xudWorldScreenBlack);
            if (step != XUD_OK) {
                xud_world_screen_count_short_frame(frame, step, &shortFrames);
            }
        }

        step = xud_world_screen_text(context, font, 10, 10, "Cube position in screen space coordinates: [%i, %i]", xudWorldScreenLime,
            (labelVisible != 0u) ? (int)labelScreen.x : 0, (labelVisible != 0u) ? (int)labelScreen.y : 0);
        if (step != XUD_OK) {
            xud_world_screen_count_short_frame(frame, step, &shortFrames);
        }
        step = xud_world_screen_text(context, font, 10, 40, "Text 2d should be always on top of the cube", xudWorldScreenGray);
        if (step != XUD_OK) {
            xud_world_screen_count_short_frame(frame, step, &shortFrames);
        }
        if (labelVisible == 0u) {
            step = xud_world_screen_text(context, font, 10, 70,
                "The box top is behind the eye: the fold refuses it, where raylib divides by zero and draws the mirror image",
                xudWorldScreenMaroon);
            if (step != XUD_OK) {
                xud_world_screen_count_short_frame(frame, step, &shortFrames);
            }
        }

        step = xud_end_drawing(context);
        if (step != XUD_OK) {
            printf("core_world_screen: frame %u xud_end_drawing -> %s, and the run stops here\n", frame, xud_result_name(step));
            break;
        }
        frame++;
    }

    /* The camera goes away with the run rather than with the frame: the last frame it was live for
       has been submitted by now.
    */
    step = xud_end_camera_3d(context);
    if (step != XUD_OK) {
        printf("core_world_screen: xud_end_camera_3d -> %s\n", xud_result_name(step));
    }

    printf("core_world_screen: %u frame(s), %u of them short, label hidden for %u of them, eye (%.2f, %.2f, %.2f)\n", frame, shortFrames,
        labelHidden, (double)camera.position.x, (double)camera.position.y, (double)camera.position.z);

    (void)xud_show_cursor(context);
    step = xud_close_font(context, font);
    if (step != XUD_OK) {
        printf("core_world_screen: xud_close_font -> %s\n", xud_result_name(step));
    }
    free(fontFile);
    step = xud_close_context(&context);
    printf("core_world_screen: xud_close_context -> %s\n", xud_result_name(step));

    return (step == XUD_OK) ? 0 : 1;
}
