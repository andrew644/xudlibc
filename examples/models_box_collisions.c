/***********************************************************************************************
 *
 *   models_box_collisions.c - raylib's models_box_collisions example on xudlibc
 *
 *   Owns : one context, one font, one fixed camera and a player box that walks the grid into an
 *          enemy box and an enemy sphere. The scene is the shapes rung's - two boxes, a sphere,
 *          their wires and the ground grid - and the example is about the player half of the
 *          collision rung: does the box the player stands in overlap the box the enemy stands in,
 *          does it meet the sphere the third shape is, either answer painting the player RED.
 *          Layer: examples. Public API only (xud.h).
 *
 *   Missing : the two tests this example is built on - xud_check_collision_boxes()
 *          (CheckCollisionBoxes, raylib.h 1669) and xud_check_collision_box_sphere()
 *          (CheckCollisionBoxSphere, raylib.h 1670). Both are host arithmetic on the caller's
 *          own floats and read no context; the answers arrive through out-parameters beside an
 *          XudResult rather than as the bare bool raylib can afford.
 *
 *   Refusal : the rung refuses what raylib answers silently - a NaN corner, a box whose minimum
 *          is past its maximum, a radius not greater than zero - and a refusal here is printed
 *          once per step and the collision keeps the value the frame gave it, the same thing the
 *          original's untouched `collision` variable does when neither test runs.
 *
 *   Pairing : the camera pair does NOT nest inside the drawing pair the way raylib's
 *          BeginMode3D/EndMode3D do - the same note examples/camera_first_person.c states - and
 *          this camera never moves at all: it is the record the run opens with, framed once.
 *
 *   Controls: the arrow keys move the player, one key per frame with the original's own
 *          precedence (right, else left, else down, else up). ESC quits.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include <stdarg.h> /* Required for: va_start, va_end, the TextFormat half of the HUD below */
#include <stdio.h>  /* Required for: printf, snprintf, and the file's own report */
#include <stdlib.h> /* Required for: free, the font file the reader below hands up */

#include "xud.h" /* Required for: the camera record, the collision rung's two tests, the shapes the scene is built from, the screen-fixed text pair, the input and clock queries and the context's own pair - the whole public API this example is written against */

/* The window. raylib's example names 800x450 and this is the same extent, so the HUD's pixel
   layout below is the same layout.
*/
#define XUD_BOX_COLLISIONS_WIDTH 800u
#define XUD_BOX_COLLISIONS_HEIGHT 450u

/* The player: raylib's 1x2x1 box at (0, 1, 2) (its file, line 33 to 34), walked 0.2 a frame. */
#define XUD_BOX_COLLISIONS_PLAYER_WIDTH 1.0f
#define XUD_BOX_COLLISIONS_PLAYER_HEIGHT 2.0f
#define XUD_BOX_COLLISIONS_MOVE_STEP 0.2f

/* The enemies: raylib's 2x2x2 box at (-4, 1, 0) and its radius-1.5 sphere at (4, 0, 0) (its
   file, line 37 to 41).
*/
#define XUD_BOX_COLLISIONS_ENEMY_BOX_SIZE 2.0f
#define XUD_BOX_COLLISIONS_ENEMY_SPHERE_RADIUS 1.5f

/* The font: the file's bytes (the public load takes bytes and not a path), baked at the em height
   raylib's example asks its DrawText for, and the default codepoint set - a NULL array with a
   zero count is the printable ASCII run, which is every character the HUD below writes.
*/
#define XUD_BOX_COLLISIONS_FONT_PATH "resources/fonts/NotoSansTC-Regular.ttf"
#define XUD_BOX_COLLISIONS_FONT_PIXELS 20u
#define XUD_BOX_COLLISIONS_LINE_MAX 96

/* The palette, the raylib colours the same example names, as the bytes this library's colour is:
   four uint8_t in RGBA order, which is the batch's own vertex format rather than a converted one.
*/
static const XudColor xudBoxCollisionsRaywhite = { .r = 245u, .g = 245u, .b = 250u, .a = 255u };
static const XudColor xudBoxCollisionsLightGray = { .r = 200u, .g = 200u, .b = 200u, .a = 255u };
static const XudColor xudBoxCollisionsGray = { .r = 130u, .g = 130u, .b = 130u, .a = 255u };
static const XudColor xudBoxCollisionsDarkGray = { .r = 80u, .g = 80u, .b = 80u, .a = 255u };
static const XudColor xudBoxCollisionsRed = { .r = 200u, .g = 0u, .b = 0u, .a = 255u };
static const XudColor xudBoxCollisionsGreen = { .r = 0u, .g = 228u, .b = 48u, .a = 255u };
static const XudColor xudBoxCollisionsLime = { .r = 0u, .g = 220u, .b = 80u, .a = 255u };
static const XudColor xudBoxCollisionsOrange = { .r = 255u, .g = 161u, .b = 0u, .a = 255u };

/* The camera the run opens on and never moves: raylib's (its file, line 31) - ten units up and
   ten back looking at the origin over up +Y, a 45 degree aperture, and raylib's own cull
   distances (RL_CULL_DISTANCE_NEAR, RL_CULL_DISTANCE_FAR in its config.h).
*/
static const XudCamera3D xudBoxCollisionsStartCamera = { .position = { .x = 0.0f, .y = 10.0f, .z = 10.0f },
    .target = { .x = 0.0f, .y = 0.0f, .z = 0.0f },
    .up = { .x = 0.0f, .y = 1.0f, .z = 0.0f },
    .fovy = 45.0f,
    .near = 0.01f,
    .far = 1000.0f };

static uint8_t* xud_box_collisions_read(const char* path, uint32_t* outBytes);
static XudResult xud_box_collisions_text(
    XudContext* context, XudHandle font, int32_t posX, int32_t posY, const char* format, XudColor color, ...);
static XudResult xud_box_collisions_scene(XudContext* context, XudVector3 playerPosition, XudColor playerColor);

/*-----------------------------------------------------------------------------------------------
 * The two pieces of machinery the HUD needs: a file reader, and a text call that formats
 *-----------------------------------------------------------------------------------------------*/

/* The font file's bytes: xud_load_font() takes bytes rather than a path, so the public header
   cannot read the file for the caller and the caller reads it here. NULL when there is no file.
*/
static uint8_t* xud_box_collisions_read(const char* path, uint32_t* outBytes) {
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

/* raylib's DrawText(TextFormat(...)) as one call: the format goes through vsnprintf into a row
   of bytes, the bytes become codepoints - the public text pair takes a uint32_t array because a
   font is keyed by codepoint and not by byte - and the row is drawn screen-fixed at the caller's
   pixel. A row that does not fit the buffer is refused rather than drawn truncated: half a label
   on the HUD is a lie about the number it was formatting.
*/
static XudResult xud_box_collisions_text(
    XudContext* context, XudHandle font, int32_t posX, int32_t posY, const char* format, XudColor color, ...) {
    char line[XUD_BOX_COLLISIONS_LINE_MAX];
    uint32_t codepoints[XUD_BOX_COLLISIONS_LINE_MAX];
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

    return xud_draw_text_screen(
        context, font, codepoints, (uint32_t)written, posX, posY, (float)XUD_BOX_COLLISIONS_FONT_PIXELS, 0.0f, color);
}

/*-----------------------------------------------------------------------------------------------
 * The scene
 *-----------------------------------------------------------------------------------------------*/

/* The two enemy places, as the bytes raylib names them at its setup (its file, line 37 and 40). */
static const XudVector3 xudBoxCollisionsEnemyBoxPosition = { .x = -4.0f, .y = 1.0f, .z = 0.0f };
static const XudVector3 xudBoxCollisionsEnemySpherePosition = { .x = 4.0f, .y = 0.0f, .z = 0.0f };

/* A centre and three full extents as the two corners the collision rung reads - the halving the
   original writes out inline at every one of its four BoundingBox constructions, named once.
*/
static XudBoundingBox xud_box_collisions_box(XudVector3 center, float width, float height, float length) {
    XudBoundingBox box;

    box.min = (XudVector3) { .x = center.x - (width * 0.5f), .y = center.y - (height * 0.5f), .z = center.z - (length * 0.5f) };
    box.max = (XudVector3) { .x = center.x + (width * 0.5f), .y = center.y + (height * 0.5f), .z = center.z + (length * 0.5f) };

    return box;
}

/* The scene, every frame: the enemy box and its wires, the enemy sphere and its globe grid, the
   player in the colour the tests left it, and the grid it all stands on - raylib's calls in
   raylib's order (its file, line 99 to 110). The library's sphere wires carry the header's own
   tessellation rather than raylib's caller-named rings and slices (see xud_draw_sphere_wires);
   the 16x16 the original asks for is the closest thing to it the public shape draws.
*/
static XudResult xud_box_collisions_scene(XudContext* context, XudVector3 playerPosition, XudColor playerColor) {
    XudResult result;

    result = xud_draw_cube(context, xudBoxCollisionsEnemyBoxPosition, XUD_BOX_COLLISIONS_ENEMY_BOX_SIZE, XUD_BOX_COLLISIONS_ENEMY_BOX_SIZE,
        XUD_BOX_COLLISIONS_ENEMY_BOX_SIZE, xudBoxCollisionsGray);
    if (result == XUD_OK) {
        result = xud_draw_cube_wires(context, xudBoxCollisionsEnemyBoxPosition, XUD_BOX_COLLISIONS_ENEMY_BOX_SIZE,
            XUD_BOX_COLLISIONS_ENEMY_BOX_SIZE, XUD_BOX_COLLISIONS_ENEMY_BOX_SIZE, xudBoxCollisionsDarkGray);
    }
    if (result == XUD_OK) {
        result
            = xud_draw_sphere(context, xudBoxCollisionsEnemySpherePosition, XUD_BOX_COLLISIONS_ENEMY_SPHERE_RADIUS, xudBoxCollisionsGray);
    }
    if (result == XUD_OK) {
        result = xud_draw_sphere_wires(
            context, xudBoxCollisionsEnemySpherePosition, XUD_BOX_COLLISIONS_ENEMY_SPHERE_RADIUS, xudBoxCollisionsDarkGray);
    }
    if (result == XUD_OK) {
        result = xud_draw_cube(context, playerPosition, XUD_BOX_COLLISIONS_PLAYER_WIDTH, XUD_BOX_COLLISIONS_PLAYER_HEIGHT,
            XUD_BOX_COLLISIONS_PLAYER_WIDTH, playerColor);
    }
    if (result != XUD_OK) {
        return result;
    }

    return xud_draw_grid(
        context, (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 0.0f }, 10u, 1.0f, xudBoxCollisionsLightGray, xudBoxCollisionsGray);
}

/* raylib's DrawFPS(10, 10) - it has no twin here, so the row is drawn the way every other HUD row
   of these examples is: the count read from xud_get_fps(), printed at the size DrawText itself
   asks for, in LIME, in ORANGE below thirty and in RED below fifteen (rtext.c 1199 to 1205). A
   refused read costs one printed line, not a row of the wrong number.
*/
static XudResult xud_box_collisions_fps(XudContext* context, XudHandle font, uint32_t* refusals) {
    XudColor color = xudBoxCollisionsLime;
    float frames = 0.0f;
    XudResult result;

    result = xud_get_fps(&frames, context);
    if (result != XUD_OK) {
        if (*refusals == 0u) {
            printf("models_box_collisions: xud_get_fps -> %s, so the FPS row stays off from here on\n", xud_result_name(result));
        }
        (*refusals)++;
        return XUD_OK;
    }
    if (((int)frames < 30) && ((int)frames >= 15)) {
        color = xudBoxCollisionsOrange;
    } else if ((int)frames < 15) {
        color = xudBoxCollisionsRed;
    }

    return xud_box_collisions_text(context, font, 10, 10, "%2d FPS", color, (int)frames);
}

/* One short frame said out loud, and only the first: the batch's capacity DOUBLES at the start of a
   frame, so a scene of two boxes, a sphere, their wires, a ten-division grid and the HUD's glyph
   quads is drawn short for the first frame or two and whole from the frame whose capacity fits it.
   Every frame redraws the whole scene, so the short ones cost nothing but the shapes that did not
   fit.
*/
static void xud_box_collisions_count_short_frame(uint32_t frame, XudResult step, uint32_t* shortFrames) {
    if (*shortFrames == 0u) {
        printf("models_box_collisions: frame %u a draw refused -> %s; the batch's capacity doubles at the start of the next frame, so "
               "this\n"
               "models_box_collisions:   scene is short for the first frame or two and whole from the one that fits\n",
            frame, xud_result_name(step));
    }
    (*shortFrames)++;
}

/*-----------------------------------------------------------------------------------------------
 * The run
 *-----------------------------------------------------------------------------------------------*/

int main(void) {
    XudCamera3D camera = xudBoxCollisionsStartCamera;
    XudFontData fontData
        = { .fileData = NULL, .fileBytes = 0u, .pixelSize = XUD_BOX_COLLISIONS_FONT_PIXELS, .codepoints = NULL, .codepointCount = 0u };
    XudVector3 playerPosition = { .x = 0.0f, .y = 1.0f, .z = 2.0f };
    XudColor playerColor = xudBoxCollisionsGreen;
    bool collision = false;
    uint8_t* fontFile = NULL;
    uint32_t fontBytes = 0u;
    uint32_t frame = 0u;
    uint32_t shortFrames = 0u;
    uint32_t fpsRefusals = 0u;
    uint32_t boxTestRefusals = 0u;
    uint32_t sphereTestRefusals = 0u;
    XudContext* context = NULL;
    XudHandle font;
    XudResult step;

    fontFile = xud_box_collisions_read(XUD_BOX_COLLISIONS_FONT_PATH, &fontBytes);
    if (fontFile == NULL) {
        printf("models_box_collisions: no font at %s, so the HUD has nothing to draw with\n", XUD_BOX_COLLISIONS_FONT_PATH);
        return 1;
    }

    step = xud_init_context(&context, XUD_BOX_COLLISIONS_WIDTH, XUD_BOX_COLLISIONS_HEIGHT);
    printf("models_box_collisions: xud_init_context(%u, %u) -> %s (xudlibc %s)\n", XUD_BOX_COLLISIONS_WIDTH, XUD_BOX_COLLISIONS_HEIGHT,
        xud_result_name(step), XUD_VERSION);
    if (step != XUD_OK) {
        free(fontFile);
        return (step == XUD_UNSUPPORTED) ? 0 : 1;
    }

    fontData.fileData = fontFile;
    fontData.fileBytes = fontBytes;
    step = xud_load_font(&font, context, &fontData);
    if (step != XUD_OK) {
        printf("models_box_collisions: xud_load_font -> %s\n", xud_result_name(step));
        (void)xud_close_context(&context);
        free(fontFile);
        return 1;
    }

    if (xud_set_target_fps(context, 60u) != XUD_OK) {
        printf("models_box_collisions: xud_set_target_fps -> refused, so the run free-runs\n");
    }

    for (;;) {
        bool shouldClose = false;
        bool pressed = false;
        bool held = false;
        bool hit = false;
        XudBoundingBox playerBox;
        XudBoundingBox enemyBox;

        step = xud_window_should_close(&shouldClose, context);
        if (step != XUD_OK) {
            printf("models_box_collisions: frame %u xud_window_should_close -> %s\n", frame, xud_result_name(step));
            break;
        }
        if (shouldClose) {
            printf("models_box_collisions: frame %u: the window was closed\n", frame);
            break;
        }
        if ((xud_is_key_pressed(&pressed, context, XUD_KEY_ESCAPE) == XUD_OK) && pressed) {
            printf("models_box_collisions: frame %u: ESC\n", frame);
            break;
        }

        /* Move player, and the original's precedence kept (its file, line 55 to 58): ONE key per
           frame, right before left before down before up - two arrows held at once move along one
           axis only, the axis the earlier test names.
        */
        if ((xud_is_key_down(&held, context, XUD_KEY_RIGHT) == XUD_OK) && held) {
            playerPosition.x += XUD_BOX_COLLISIONS_MOVE_STEP;
        } else if ((xud_is_key_down(&held, context, XUD_KEY_LEFT) == XUD_OK) && held) {
            playerPosition.x -= XUD_BOX_COLLISIONS_MOVE_STEP;
        } else if ((xud_is_key_down(&held, context, XUD_KEY_DOWN) == XUD_OK) && held) {
            playerPosition.z += XUD_BOX_COLLISIONS_MOVE_STEP;
        } else if ((xud_is_key_down(&held, context, XUD_KEY_UP) == XUD_OK) && held) {
            playerPosition.z -= XUD_BOX_COLLISIONS_MOVE_STEP;
        }

        collision = false;
        playerBox = xud_box_collisions_box(
            playerPosition, XUD_BOX_COLLISIONS_PLAYER_WIDTH, XUD_BOX_COLLISIONS_PLAYER_HEIGHT, XUD_BOX_COLLISIONS_PLAYER_WIDTH);
        enemyBox = xud_box_collisions_box(xudBoxCollisionsEnemyBoxPosition, XUD_BOX_COLLISIONS_ENEMY_BOX_SIZE,
            XUD_BOX_COLLISIONS_ENEMY_BOX_SIZE, XUD_BOX_COLLISIONS_ENEMY_BOX_SIZE);

        /* Check collisions player vs enemy-box (the original's line 63), and the same again
           against the sphere (its line 78): either hit paints the player, neither leaves it
           GREEN. A refused test is said once per rung and the frame's collision keeps the value
           the tests above gave it.
        */
        step = xud_check_collision_boxes(&hit, playerBox, enemyBox);
        if (step != XUD_OK) {
            if (boxTestRefusals == 0u) {
                printf("models_box_collisions: xud_check_collision_boxes -> %s, so the box test answers nothing\n", xud_result_name(step));
            }
            boxTestRefusals++;
        } else if (hit) {
            collision = true;
        }

        step = xud_check_collision_box_sphere(&hit, playerBox, xudBoxCollisionsEnemySpherePosition, XUD_BOX_COLLISIONS_ENEMY_SPHERE_RADIUS);
        if (step != XUD_OK) {
            if (sphereTestRefusals == 0u) {
                printf("models_box_collisions: xud_check_collision_box_sphere -> %s, so the sphere test answers nothing\n",
                    xud_result_name(step));
            }
            sphereTestRefusals++;
        } else if (hit) {
            collision = true;
        }

        playerColor = collision ? xudBoxCollisionsRed : xudBoxCollisionsGreen;

        step = xud_begin_drawing(context);
        if (step != XUD_OK) {
            printf("models_box_collisions: frame %u xud_begin_drawing -> %s, and the run stops here\n", frame, xud_result_name(step));
            break;
        }
        (void)xud_clear_background(context, xudBoxCollisionsRaywhite);

        /* The camera, begun here and NOT ended inside the frame - see examples/camera_first_person.c
           for what the wrong order draws. Each frame's begin replaces the live record.
        */
        (void)xud_begin_camera_3d(context, camera);
        step = xud_box_collisions_scene(context, playerPosition, playerColor);
        if (step != XUD_OK) {
            xud_box_collisions_count_short_frame(frame, step, &shortFrames);
        }

        step = xud_box_collisions_text(context, font, 220, 40, "Move player with arrow keys to collide", xudBoxCollisionsGray);
        if (step != XUD_OK) {
            xud_box_collisions_count_short_frame(frame, step, &shortFrames);
        }
        step = xud_box_collisions_fps(context, font, &fpsRefusals);
        if (step != XUD_OK) {
            xud_box_collisions_count_short_frame(frame, step, &shortFrames);
        }

        step = xud_end_drawing(context);
        if (step != XUD_OK) {
            printf("models_box_collisions: frame %u xud_end_drawing -> %s, and the run stops here\n", frame, xud_result_name(step));
            break;
        }
        frame++;
    }

    /* The camera goes away with the run rather than with the frame: the last frame it was live
       for has been submitted by now.
    */
    step = xud_end_camera_3d(context);
    if (step != XUD_OK) {
        printf("models_box_collisions: xud_end_camera_3d -> %s\n", xud_result_name(step));
    }

    printf("models_box_collisions: %u frame(s), %u of them short, player %s at (%.2f, %.2f, %.2f)\n", frame, shortFrames,
        collision ? "colliding" : "free", (double)playerPosition.x, (double)playerPosition.y, (double)playerPosition.z);

    step = xud_close_font(context, font);
    if (step != XUD_OK) {
        printf("models_box_collisions: xud_close_font -> %s\n", xud_result_name(step));
    }
    free(fontFile);
    step = xud_close_context(&context);
    printf("models_box_collisions: xud_close_context -> %s\n", xud_result_name(step));

    return (step == XUD_OK) ? 0 : 1;
}
