/***********************************************************************************************
 *
 *   core_3d_picking.c - raylib's core_3d_picking example on xudlibc
 *
 *   Owns : one context, one font, one camera record, one cube, and the ONE click that decides
 *          whether that cube is selected. The scene is the shapes rung's - a box, its wire frame
 *          and the ground grid - and the example is about the picking half of the collision rung:
 *          the pixel the pointer stands on, traced into the camera's world as a ray, against the
 *          box the cube is, which is what raylib's GetScreenToWorldRay and GetRayCollisionBox
 *          answer together. Layer: examples. Public API only (xud.h).
 *
 *   Missing : the two calls this example is built on, both now public - xud_get_screen_to_world_ray()
 *          (GetScreenToWorldRay, raylib.h 1083), which folds a pixel into a world ray through the
 *          swapchain's own extent, and xud_get_ray_collision_box() (GetRayCollisionBox, raylib.h
 *          1672), the slab cast that says where the ray meets the box. xud_update_camera()
 *          (rcamera's UpdateCamera) drives the first-person walk, but ONLY while the cursor is
 *          hidden - the same gate raylib's example opens its frame with (its file, line 52) - and
 *          xud_hide_cursor()/xud_show_cursor() (DisableCursor/EnableCursor) are what the right
 *          mouse button toggles.
 *
 *   Refusal : the collision rung refuses what raylib answers silently - a NaN ray, a box whose
 *          minimum is past its maximum - and a refusal here keeps the selection exactly where the
 *          last click left it, which is the same thing the click does when nothing was hit. The
 *          trace refuses a camera the view matrix would refuse, where raylib divides by a zero
 *          forward vector; the example says so and does not redden the box.
 *
 *   Pairing : the camera pair does NOT nest inside the drawing pair the way raylib's
 *          BeginMode3D/EndMode3D do - the same note examples/camera_first_person.c states: this
 *          library writes ONE matrix per frame, at the submission, so a camera ended inside the
 *          frame is a frame drawn in clip space.
 *
 *   Controls: the right mouse button takes or gives back the cursor; while it is gone the arrow
 *          keys and the mouse look and W A S D walk. The left button, on an unselected box, picks;
 *          on the selected one it clears. ESC quits.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include <stdarg.h> /* Required for: va_start, va_end, the TextFormat half of the HUD below */
#include <stdio.h>  /* Required for: printf, snprintf, and the file's own report */
#include <stdlib.h> /* Required for: free, the font file the reader below hands up */

#include "xud.h" /* Required for: the camera record and the mode list, xud_update_camera, the fold from a pixel to a world ray, the collision rung's ray-versus-box cast, the shapes the scene is built from, the screen-fixed text pair, the cursor pair, the input queries and the context's own pair - the whole public API this example is written against */

/* The window. raylib's example names 800x450 and this is the same extent, so the HUD's pixel
   layout below is the same layout.
*/
#define XUD_PICKING_WIDTH 800u
#define XUD_PICKING_HEIGHT 450u

/* The cube and its box: raylib's (2, 2, 2) at (0, 1, 0) (its file, line 38 to 39). The box the
   cast runs against is that centre and those extents halved into two corners, which is exactly
   the BoundingBox the original builds inline at the call.
*/
#define XUD_PICKING_CUBE_SIZE 2.0f
#define XUD_PICKING_GLOW_GROW 0.2f

/* The font: the file's bytes (the public load takes bytes and not a path), baked at the em height
   raylib's example asks its largest DrawText for, and the default codepoint set - a NULL array
   with a zero count is the printable ASCII run, which is every character the HUD below writes.
*/
#define XUD_PICKING_FONT_PATH "resources/fonts/NotoSansTC-Regular.ttf"
#define XUD_PICKING_FONT_PIXELS 20u
#define XUD_PICKING_LINE_MAX 96

/* The palette, the raylib colours the same example names, as the bytes this library's colour is:
   four uint8_t in RGBA order, which is the batch's own vertex format rather than a converted one.
*/
static const XudColor xudPickingRaywhite = { .r = 245u, .g = 245u, .b = 250u, .a = 255u };
static const XudColor xudPickingLightGray = { .r = 200u, .g = 200u, .b = 200u, .a = 255u };
static const XudColor xudPickingGray = { .r = 130u, .g = 130u, .b = 130u, .a = 255u };
static const XudColor xudPickingDarkGray = { .r = 80u, .g = 80u, .b = 80u, .a = 255u };
static const XudColor xudPickingRed = { .r = 200u, .g = 0u, .b = 0u, .a = 255u };
static const XudColor xudPickingMaroon = { .r = 127u, .g = 0u, .b = 0u, .a = 255u };
static const XudColor xudPickingGreen = { .r = 0u, .g = 228u, .b = 48u, .a = 255u };
static const XudColor xudPickingLime = { .r = 0u, .g = 220u, .b = 80u, .a = 255u };
static const XudColor xudPickingOrange = { .r = 255u, .g = 161u, .b = 0u, .a = 255u };

/* The camera the run opens on: raylib's (its file, line 32 to 35) - ten units out on all three
   axes looking at the origin over up +Y, a 45 degree aperture, and raylib's own cull distances
   (RL_CULL_DISTANCE_NEAR, RL_CULL_DISTANCE_FAR in its config.h) because the example leaves them
   at the library's defaults.
*/
static const XudCamera3D xudPickingStartCamera = { .position = { .x = 10.0f, .y = 10.0f, .z = 10.0f },
    .target = { .x = 0.0f, .y = 0.0f, .z = 0.0f },
    .up = { .x = 0.0f, .y = 1.0f, .z = 0.0f },
    .fovy = 45.0f,
    .near = 0.01f,
    .far = 1000.0f };

static uint8_t* xud_picking_read(const char* path, uint32_t* outBytes);
static XudResult xud_picking_text(
    XudContext* context, XudHandle font, int32_t posX, int32_t posY, float pixelSize, const char* format, XudColor color, ...);
static XudResult xud_picking_scene(XudContext* context, XudVector3 cubePosition, XudRay ray, bool selected);

/*-----------------------------------------------------------------------------------------------
 * The two pieces of machinery the HUD needs: a file reader, and a text call that formats
 *-----------------------------------------------------------------------------------------------*/

/* The font file's bytes: xud_load_font() takes bytes rather than a path, so the public header
   cannot read the file for the caller and the caller reads it here. NULL when there is no file.
*/
static uint8_t* xud_picking_read(const char* path, uint32_t* outBytes) {
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
   is keyed by codepoint and not by byte - and the row is drawn screen-fixed at the caller's pixel,
   at the caller's em height, because this HUD asks three sizes of one font (20, 30 and 10) where
   the siblings asked one. A row that does not fit the buffer is refused rather than drawn
   truncated: half a label on the HUD is a lie about the number it was formatting.
*/
static XudResult xud_picking_text(
    XudContext* context, XudHandle font, int32_t posX, int32_t posY, float pixelSize, const char* format, XudColor color, ...) {
    char line[XUD_PICKING_LINE_MAX];
    uint32_t codepoints[XUD_PICKING_LINE_MAX];
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

    return xud_draw_text_screen(context, font, codepoints, (uint32_t)written, posX, posY, pixelSize, 0.0f, color);
}

/*-----------------------------------------------------------------------------------------------
 * The scene
 *-----------------------------------------------------------------------------------------------*/

/* The scene, every frame: the box and its wire frame in the selected pair or the plain one, then
   the pick's ray, then the grid it all stands on - raylib's calls in raylib's order (its file,
   line 84 to 98). The selected form adds the second, 0.2-larger wire box: the outline a click
   leaves behind. The ray is the one the last successful pick left in the record - and the record
   starts zeroed, so before the first click xud_draw_ray() draws a zero-length line at the origin,
   which is what raylib's DrawRay(ray, MAROON) draws in the same situation and why the original
   can afford to draw it every frame.
*/
static XudResult xud_picking_scene(XudContext* context, XudVector3 cubePosition, XudRay ray, bool selected) {
    XudResult result;

    if (selected) {
        result = xud_draw_cube(context, cubePosition, XUD_PICKING_CUBE_SIZE, XUD_PICKING_CUBE_SIZE, XUD_PICKING_CUBE_SIZE, xudPickingRed);
        if (result == XUD_OK) {
            result = xud_draw_cube_wires(
                context, cubePosition, XUD_PICKING_CUBE_SIZE, XUD_PICKING_CUBE_SIZE, XUD_PICKING_CUBE_SIZE, xudPickingMaroon);
        }
        if (result == XUD_OK) {
            result = xud_draw_cube_wires(context, cubePosition, XUD_PICKING_CUBE_SIZE + XUD_PICKING_GLOW_GROW,
                XUD_PICKING_CUBE_SIZE + XUD_PICKING_GLOW_GROW, XUD_PICKING_CUBE_SIZE + XUD_PICKING_GLOW_GROW, xudPickingGreen);
        }
    } else {
        result = xud_draw_cube(context, cubePosition, XUD_PICKING_CUBE_SIZE, XUD_PICKING_CUBE_SIZE, XUD_PICKING_CUBE_SIZE, xudPickingGray);
        if (result == XUD_OK) {
            result = xud_draw_cube_wires(
                context, cubePosition, XUD_PICKING_CUBE_SIZE, XUD_PICKING_CUBE_SIZE, XUD_PICKING_CUBE_SIZE, xudPickingDarkGray);
        }
    }
    if (result != XUD_OK) {
        return result;
    }

    result = xud_draw_ray(context, ray, xudPickingMaroon);
    if (result != XUD_OK) {
        return result;
    }

    return xud_draw_grid(context, (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 0.0f }, 10u, 1.0f, xudPickingLightGray, xudPickingGray);
}

/* raylib's DrawFPS(10, 10) - it has no twin here, so the row is drawn the way every other HUD row
   of these examples is: the count read from xud_get_fps(), printed at the size DrawText itself
   asks for, in LIME, in ORANGE below thirty and in RED below fifteen (rtext.c 1199 to 1205). A
   refused read costs one printed line, not a row of the wrong number.
*/
static XudResult xud_picking_fps(XudContext* context, XudHandle font, uint32_t* refusals) {
    XudColor color = xudPickingLime;
    float frames = 0.0f;
    XudResult result;

    result = xud_get_fps(&frames, context);
    if (result != XUD_OK) {
        if (*refusals == 0u) {
            printf("core_3d_picking: xud_get_fps -> %s, so the FPS row stays off from here on\n", xud_result_name(result));
        }
        (*refusals)++;
        return XUD_OK;
    }
    if (((int)frames < 30) && ((int)frames >= 15)) {
        color = xudPickingOrange;
    } else if ((int)frames < 15) {
        color = xudPickingRed;
    }

    return xud_picking_text(context, font, 10, 10, (float)XUD_PICKING_FONT_PIXELS, "%2d FPS", color, (int)frames);
}

/* One short frame said out loud, and only the first: the batch's capacity DOUBLES at the start of a
   frame, so a scene of a box, its wires, a ten-division grid and the HUD's glyph quads is drawn short
   for the first frame or two and whole from the frame whose capacity fits it. Every frame redraws the
   whole scene, so the short ones cost nothing but the shapes that did not fit.
*/
static void xud_picking_count_short_frame(uint32_t frame, XudResult step, uint32_t* shortFrames) {
    if (*shortFrames == 0u) {
        printf("core_3d_picking: frame %u a draw refused -> %s; the batch's capacity doubles at the start of the next frame, so this\n"
               "core_3d_picking:   scene is short for the first frame or two and whole from the one that fits\n",
            frame, xud_result_name(step));
    }
    (*shortFrames)++;
}

/*-----------------------------------------------------------------------------------------------
 * The run
 *-----------------------------------------------------------------------------------------------*/

int main(void) {
    XudCamera3D camera = xudPickingStartCamera;
    XudFontData fontData
        = { .fileData = NULL, .fileBytes = 0u, .pixelSize = XUD_PICKING_FONT_PIXELS, .codepoints = NULL, .codepointCount = 0u };
    const XudVector3 cubePosition = { .x = 0.0f, .y = 1.0f, .z = 0.0f };
    XudBoundingBox cubeBox = { .min = { .x = 0.0f, .y = 0.0f, .z = 0.0f }, .max = { .x = 0.0f, .y = 0.0f, .z = 0.0f } };
    XudRay ray = { .position = { .x = 0.0f, .y = 0.0f, .z = 0.0f }, .direction = { .x = 0.0f, .y = 0.0f, .z = 0.0f } };
    XudRayCollision collision
        = { .hit = false, .distance = 0.0f, .point = { .x = 0.0f, .y = 0.0f, .z = 0.0f }, .normal = { .x = 0.0f, .y = 0.0f, .z = 0.0f } };
    uint8_t* fontFile = NULL;
    uint32_t fontBytes = 0u;
    uint32_t cursorHidden = 0u;
    uint32_t frame = 0u;
    uint32_t shortFrames = 0u;
    uint32_t fpsRefusals = 0u;
    uint32_t cursorRefusals = 0u;
    XudContext* context = NULL;
    XudHandle font;
    XudResult step;

    fontFile = xud_picking_read(XUD_PICKING_FONT_PATH, &fontBytes);
    if (fontFile == NULL) {
        printf("core_3d_picking: no font at %s, so the HUD has nothing to draw with\n", XUD_PICKING_FONT_PATH);
        return 1;
    }

    step = xud_init_context(&context, XUD_PICKING_WIDTH, XUD_PICKING_HEIGHT);
    printf("core_3d_picking: xud_init_context(%u, %u) -> %s (xudlibc %s)\n", XUD_PICKING_WIDTH, XUD_PICKING_HEIGHT, xud_result_name(step),
        XUD_VERSION);
    if (step != XUD_OK) {
        free(fontFile);
        return (step == XUD_UNSUPPORTED) ? 0 : 1;
    }

    fontData.fileData = fontFile;
    fontData.fileBytes = fontBytes;
    step = xud_load_font(&font, context, &fontData);
    if (step != XUD_OK) {
        printf("core_3d_picking: xud_load_font -> %s\n", xud_result_name(step));
        (void)xud_close_context(&context);
        free(fontFile);
        return 1;
    }

    /* The box the cast runs against, written out once from the centre and the halved extents -
       the same two corners the original builds inline at its GetRayCollisionBox call.
    */
    cubeBox.min = (XudVector3) { .x = cubePosition.x - (XUD_PICKING_CUBE_SIZE * 0.5f),
        .y = cubePosition.y - (XUD_PICKING_CUBE_SIZE * 0.5f),
        .z = cubePosition.z - (XUD_PICKING_CUBE_SIZE * 0.5f) };
    cubeBox.max = (XudVector3) { .x = cubePosition.x + (XUD_PICKING_CUBE_SIZE * 0.5f),
        .y = cubePosition.y + (XUD_PICKING_CUBE_SIZE * 0.5f),
        .z = cubePosition.z + (XUD_PICKING_CUBE_SIZE * 0.5f) };

    if (xud_set_target_fps(context, 60u) != XUD_OK) {
        printf("core_3d_picking: xud_set_target_fps -> refused, so the run free-runs\n");
    }

    for (;;) {
        bool shouldClose = false;
        bool pressed = false;
        bool hidden = false;

        step = xud_window_should_close(&shouldClose, context);
        if (step != XUD_OK) {
            printf("core_3d_picking: frame %u xud_window_should_close -> %s\n", frame, xud_result_name(step));
            break;
        }
        if (shouldClose) {
            printf("core_3d_picking: frame %u: the window was closed\n", frame);
            break;
        }
        if ((xud_is_key_pressed(&pressed, context, XUD_KEY_ESCAPE) == XUD_OK) && pressed) {
            printf("core_3d_picking: frame %u: ESC\n", frame);
            break;
        }

        /* The gate raylib's example opens its Update half with (its file, line 52): the camera
           walks only while the cursor is hidden, because the look reads pointer deltas that an
           absolute cursor cannot give. The query is the library's own answer rather than the
           toggle below's memory, so a cursor the window system gave back behind the run's back
           stops the look on the frame it happened.
        */
        step = xud_is_cursor_hidden(&hidden, context);
        if (step != XUD_OK) {
            if (cursorRefusals == 0u) {
                printf("core_3d_picking: xud_is_cursor_hidden -> %s, so the gate keeps the state the last toggle left\n",
                    xud_result_name(step));
            }
            cursorRefusals++;
        } else {
            cursorHidden = hidden ? 1u : 0u;
        }

        if (cursorHidden != 0u) {
            step = xud_update_camera(context, &camera, XUD_CAMERA_MODE_FIRST_PERSON);
            if (step != XUD_OK) {
                printf("core_3d_picking: frame %u xud_update_camera -> %s, and the run stops here\n", frame, xud_result_name(step));
                break;
            }
        }

        /* The toggle of the original's line 55: the right button takes the cursor if it is shown
           and gives it back if it is hidden.
        */
        if ((xud_is_mouse_button_pressed(&pressed, context, XUD_MOUSE_BUTTON_RIGHT) == XUD_OK) && pressed) {
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

        /* The pick, and its twin (the original's line 61 to 73): the left button on an
           unselected box traces this frame's pixel into a world ray and casts it against the
           cube's box, and the left button on the selected one clears the hit and nothing else -
           the same click that selected the box is the one that lets it go.
        */
        if ((xud_is_mouse_button_pressed(&pressed, context, XUD_MOUSE_BUTTON_LEFT) == XUD_OK) && pressed) {
            if (!collision.hit) {
                XudVector2 mouse = { .x = 0.0f, .y = 0.0f };

                step = xud_get_mouse_position(&mouse, context);
                if (step != XUD_OK) {
                    printf("core_3d_picking: frame %u xud_get_mouse_position -> %s, so this click has no pixel to trace\n", frame,
                        xud_result_name(step));
                } else {
                    step = xud_get_screen_to_world_ray(&ray, context, mouse, &camera);
                    if (step != XUD_OK) {
                        printf("core_3d_picking: frame %u xud_get_screen_to_world_ray -> %s, so the box stays as the last click left it\n",
                            frame, xud_result_name(step));
                    } else {
                        step = xud_get_ray_collision_box(&collision, ray, cubeBox);
                        if (step != XUD_OK) {
                            printf("core_3d_picking: frame %u xud_get_ray_collision_box -> %s, so the box stays as the last cast left it\n",
                                frame, xud_result_name(step));
                        }
                    }
                }
            } else {
                collision.hit = false;
            }
        }

        step = xud_begin_drawing(context);
        if (step != XUD_OK) {
            printf("core_3d_picking: frame %u xud_begin_drawing -> %s, and the run stops here\n", frame, xud_result_name(step));
            break;
        }
        (void)xud_clear_background(context, xudPickingRaywhite);

        /* The camera, begun here and NOT ended inside the frame - see examples/camera_first_person.c
           for what the wrong order draws. Each frame's begin replaces the live record.
        */
        (void)xud_begin_camera_3d(context, camera);
        step = xud_picking_scene(context, cubePosition, ray, collision.hit);
        if (step != XUD_OK) {
            xud_picking_count_short_frame(frame, step, &shortFrames);
        }

        step = xud_picking_text(
            context, font, 240, 10, (float)XUD_PICKING_FONT_PIXELS, "Try clicking on the box with your mouse!", xudPickingDarkGray);
        if (step != XUD_OK) {
            xud_picking_count_short_frame(frame, step, &shortFrames);
        }

        /* The selected label, centred the way the original centres it (its file, line 104): the
           row measured at its own thirty-pixel em, the window's width minus that halved, hung
           one tenth of the way down. Both screen-fixed draws, like every text row.
        */
        if (collision.hit) {
            static const uint32_t xudPickingSelectedLabel[] = { 'B', 'O', 'X', ' ', 'S', 'E', 'L', 'E', 'C', 'T', 'E', 'D' };
            float labelWidth = 0.0f;

            step = xud_measure_text(&labelWidth, context, font, xudPickingSelectedLabel,
                (uint32_t)(sizeof(xudPickingSelectedLabel) / sizeof(xudPickingSelectedLabel[0])), 30.0f, 0.0f);
            if (step == XUD_OK) {
                step = xud_picking_text(context, font, (int32_t)(((float)XUD_PICKING_WIDTH - labelWidth) * 0.5f),
                    (int32_t)((float)XUD_PICKING_HEIGHT * 0.1f), 30.0f, "BOX SELECTED", xudPickingGreen);
            }
            if (step != XUD_OK) {
                xud_picking_count_short_frame(frame, step, &shortFrames);
            }
        }

        step = xud_picking_text(context, font, 10, 430, 10.0f, "Right click mouse to toggle camera controls", xudPickingGray);
        if (step != XUD_OK) {
            xud_picking_count_short_frame(frame, step, &shortFrames);
        }
        step = xud_picking_fps(context, font, &fpsRefusals);
        if (step != XUD_OK) {
            xud_picking_count_short_frame(frame, step, &shortFrames);
        }

        step = xud_end_drawing(context);
        if (step != XUD_OK) {
            printf("core_3d_picking: frame %u xud_end_drawing -> %s, and the run stops here\n", frame, xud_result_name(step));
            break;
        }
        frame++;
    }

    /* The camera goes away with the run rather than with the frame: the last frame it was live
       for has been submitted by now.
    */
    step = xud_end_camera_3d(context);
    if (step != XUD_OK) {
        printf("core_3d_picking: xud_end_camera_3d -> %s\n", xud_result_name(step));
    }

    printf("core_3d_picking: %u frame(s), %u of them short, box %s, eye (%.2f, %.2f, %.2f)\n", frame, shortFrames,
        collision.hit ? "selected" : "free", (double)camera.position.x, (double)camera.position.y, (double)camera.position.z);

    (void)xud_show_cursor(context);
    step = xud_close_font(context, font);
    if (step != XUD_OK) {
        printf("core_3d_picking: xud_close_font -> %s\n", xud_result_name(step));
    }
    free(fontFile);
    step = xud_close_context(&context);
    printf("core_3d_picking: xud_close_context -> %s\n", xud_result_name(step));

    return (step == XUD_OK) ? 0 : 1;
}
