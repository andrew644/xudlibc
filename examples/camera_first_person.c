/***********************************************************************************************
 *
 *   camera_first_person.c - raylib's core_3d_camera_first_person example on xudlibc
 *
 *   Owns : one context, one font, one camera record, and the twenty columns the scene is seeded
 *          from. The scene is the shapes rung's - a ground grid, three walls, the round shapes and
 *          their wire frames, twenty boxes with their wire frames, and the player's own box while the
 *          camera is behind it - and the
 *          controls are the camera rung's: xud_update_camera() is this library's copy of rcamera's
 *          UpdateCamera (reference/raylib/src/rcamera.h 440), so the walk, the look, the pan and
 *          the zoom are the library's hardcoded keyboard and mouse reading rather than a reading
 *          this example writes. raylib's example does the same (its file, line 121). Layer:
 *          examples. Public API only (xud.h).
 *
 *   Missing : four things this example reads that raylib has and xudlibc did not, now part of the
 *          public API - xud_update_camera() with the XUD_CAMERA_MODE_* list (rcamera's UpdateCamera
 *          and CameraMode), xud_hide_cursor()/xud_show_cursor() (DisableCursor/EnableCursor: the
 *          relative pointer a first-person look needs, because a locked pointer reports deltas and
 *          no absolute position), the ground grid and the round shapes (DrawGrid, DrawCylinder,
 *          DrawCylinderWires, DrawCapsule and DrawCapsuleWires: the arena's floor, cone and capsules),
 *          and xud_get_random_value()/xud_set_random_seed() (GetRandomValue/SetRandomSeed: the columns).
 *          raylib's example also toggles an orthographic projection with P; this library's one
 *          camera is perspective and xud.h says so beside the mode list, so there is no projection
 *          to toggle and no key for it here.
 *
 *   Pairing : the camera pair does NOT nest inside the drawing pair the way raylib's
 *          BeginMode3D/EndMode3D do. This library writes ONE matrix per frame, into set 0's block at
 *          the submission itself, so a camera ended inside the frame is a frame drawn in clip space;
 *          the camera here is begun every frame - each begin replacing the record before it - and
 *          taken back when the run ends. The note in the loop is the arithmetic of what the wrong
 *          order draws, and it is the one shape raylib's example this file copies that is not copied.
 *
 *   Controls: W A S D walk, the arrow keys and the mouse look, 1 2 3 4 pick the mode (free, first
 *          person, third person, orbital), TAB hides or shows the cursor, the scroll wheel and the
 *          keypad's + - zoom the modes that hold their target at a distance, and ESC quits.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include <stdarg.h> /* Required for: va_start, va_end, the TextFormat half of the HUD below */
#include <stdio.h>  /* Required for: printf, snprintf, and the file's own report */
#include <stdlib.h> /* Required for: free, the font file the reader below hands up */

#include "xud.h" /* Required for: the camera record and the mode list, xud_update_camera, the shapes the scene is built from, the screen-fixed text pair, the cursor pair, the random pair, the input and clock queries, and the context's own pair - the whole public API this example is written against */

/* The window. raylib's example names 800x450 and this is the same extent, so the HUD's pixel
   layout below is the same layout.
*/
#define XUD_FIRST_PERSON_WIDTH 800u
#define XUD_FIRST_PERSON_HEIGHT 450u

/* The columns: raylib's MAX_COLUMNS (its file, line 19), and the same 1..12 heights, the same
   +-15 extent, the same 2-wide box and the same red-heavy colour.
*/
#define XUD_FIRST_PERSON_MAX_COLUMNS 20u
#define XUD_FIRST_PERSON_HEIGHT_MIN 1
#define XUD_FIRST_PERSON_HEIGHT_MAX 12
#define XUD_FIRST_PERSON_EXTENT 15

/* The seed is named rather than left to the clock: the twenty columns a screenshot of this scene
   shows are then the twenty the next run shows too, which is what makes a picture of the scene
   evidence about a change rather than about the machine's entropy at the moment it was taken.
*/
#define XUD_FIRST_PERSON_SEED 20261111u

/* The font: the file's bytes (the public load takes bytes and not a path), baked at an em height
   the HUD's ten-pixel rows would only round up from, and the default codepoint set - a NULL array
   with a zero count is the printable ASCII run, which is every character the HUD below writes.
*/
#define XUD_FIRST_PERSON_FONT_PATH "resources/fonts/NotoSansTC-Regular.ttf"
#define XUD_FIRST_PERSON_FONT_PIXELS 16u
#define XUD_FIRST_PERSON_LINE_MAX 96

/* The eye's height: a walker's, so the ground plane is under the eye and the walls rise past it. */
#define XUD_FIRST_PERSON_EYE_HEIGHT 1.6f

/* The HUD's rows: the pitch between two lines and the y the first one hangs from, in framebuffer
   pixels with y down - the same hand xud_draw_text_screen() states for its own position.
*/
#define XUD_FIRST_PERSON_ROW_PITCH 22

/* The palette, the raylib colours the same example names, as the bytes this library's colour is:
   four uint8_t in RGBA order, which is the batch's own vertex format rather than a converted one.
*/
static const XudColor xudFirstPersonRaywhite = { .r = 245u, .g = 245u, .b = 250u, .a = 255u };
static const XudColor xudFirstPersonLightGray = { .r = 200u, .g = 200u, .b = 200u, .a = 255u };
static const XudColor xudFirstPersonGray = { .r = 130u, .g = 130u, .b = 130u, .a = 255u };
static const XudColor xudFirstPersonBlue = { .r = 0u, .g = 120u, .b = 240u, .a = 255u };
static const XudColor xudFirstPersonLime = { .r = 0u, .g = 220u, .b = 80u, .a = 255u };
static const XudColor xudFirstPersonGold = { .r = 255u, .g = 180u, .b = 0u, .a = 255u };
static const XudColor xudFirstPersonMaroon = { .r = 127u, .g = 0u, .b = 0u, .a = 255u };
static const XudColor xudFirstPersonSky = { .r = 168u, .g = 208u, .b = 248u, .a = 255u };
static const XudColor xudFirstPersonPurple = { .r = 120u, .g = 0u, .b = 160u, .a = 255u };
static const XudColor xudFirstPersonDarkPurple = { .r = 60u, .g = 0u, .b = 90u, .a = 255u };
static const XudColor xudFirstPersonBlack = { .r = 0u, .g = 0u, .b = 0u, .a = 255u };

/* The camera the run starts on, and the mode it starts in: the eye at a walker's height behind the
   origin looking at it, which is the framing raylib's example opens on.
*/
static const XudCamera3D xudFirstPersonStartCamera = { .position = { .x = 0.0f, .y = XUD_FIRST_PERSON_EYE_HEIGHT, .z = 10.0f },
    .target = { .x = 0.0f, .y = XUD_FIRST_PERSON_EYE_HEIGHT, .z = 0.0f },
    .up = { .x = 0.0f, .y = 1.0f, .z = 0.0f },
    .fovy = 60.0f,
    .near = 0.1f,
    .far = 200.0f };

/* One column of the scene: where it stands, how tall it is, and what it is painted with. */
typedef struct XudFirstPersonColumn {
    XudVector3 position;
    float height;
    XudColor color;
} XudFirstPersonColumn;

static uint8_t* xud_first_person_read(const char* path, uint32_t* outBytes);
static XudResult xud_first_person_text(
    XudContext* context, XudHandle font, int32_t posX, int32_t posY, const char* format, XudColor color, ...);
static XudResult xud_first_person_scene(XudContext* context, const XudFirstPersonColumn* columns, uint32_t columnCount);
static XudResult xud_first_person_hud(XudContext* context, XudHandle font, const XudCamera3D* camera, uint32_t mode, uint32_t cursorHidden);

/*-----------------------------------------------------------------------------------------------
 * The two pieces of machinery the HUD needs: a file reader, and a text call that formats
 *-----------------------------------------------------------------------------------------------*/

/* The font file's bytes: xud_load_font() takes bytes rather than a path, so the public header
   cannot read the file for the caller and the caller reads it here. NULL when there is no file.
*/
static uint8_t* xud_first_person_read(const char* path, uint32_t* outBytes) {
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
   bytes, the bytes become codepoints - the public text pair takes a uint32_t array because a font
   is keyed by codepoint and not by byte - and the row is drawn screen-fixed at the caller's pixel.
   A row that does not fit the buffer is refused rather than drawn truncated: half a label on the
   HUD is a lie about the number it was formatting.
*/
static XudResult xud_first_person_text(
    XudContext* context, XudHandle font, int32_t posX, int32_t posY, const char* format, XudColor color, ...) {
    char line[XUD_FIRST_PERSON_LINE_MAX];
    uint32_t codepoints[XUD_FIRST_PERSON_LINE_MAX];
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

    return xud_draw_text_screen(context, font, codepoints, (uint32_t)written, posX, posY, (float)XUD_FIRST_PERSON_FONT_PIXELS, 0.0f, color);
}

/*-----------------------------------------------------------------------------------------------
 * The scene and the HUD
 *-----------------------------------------------------------------------------------------------*/

/* The scene, every frame: the grid the walker is on, the three walls the arena is closed by, the row
   of round shapes that show what a cylinder, a cone, a frustum and a capsule look like when the eye
   walks around them, and the twenty columns - each one a box and the wire frame around that same box,
   the pair raylib's example draws (its file, line 159 and 160) and the reason the columns read as
   columns rather than as a field of blobs once the eye is inside them. The grid IS the ground rather
   than a pattern laid over a plane: two surfaces at one height are a z-fight the depth buffer settles
   differently every frame.
*/
static XudResult xud_first_person_scene(XudContext* context, const XudFirstPersonColumn* columns, uint32_t columnCount) {
    uint32_t column = 0u;
    XudResult result;

    result = xud_draw_cube(context, (XudVector3) { .x = -16.0f, .y = 2.5f, .z = 0.0f }, 1.0f, 5.0f, 32.0f, xudFirstPersonBlue);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_draw_cube(context, (XudVector3) { .x = 16.0f, .y = 2.5f, .z = 0.0f }, 1.0f, 5.0f, 32.0f, xudFirstPersonLime);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_draw_cube(context, (XudVector3) { .x = 0.0f, .y = 2.5f, .z = 16.0f }, 32.0f, 5.0f, 1.0f, xudFirstPersonGold);
    if (result != XUD_OK) {
        return result;
    }

    result
        = xud_draw_grid(context, (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 0.0f }, 32u, 1.0f, xudFirstPersonLightGray, xudFirstPersonGray);
    if (result != XUD_OK) {
        return result;
    }

    /* The round shapes, in a row the walker meets on the way to the far wall: a cylinder, a cone (the
       cylinder's zero top radius), a frustum, a capsule stood up and a capsule laid down - each with
       its wires, which is the pair raylib's geometric-shapes example draws for the same reason.
    */
    result = xud_draw_cylinder(context, (XudVector3) { .x = -8.0f, .y = 0.0f, .z = -8.0f }, 1.0f, 1.0f, 3.0f, xudFirstPersonPurple);
    if (result != XUD_OK) {
        return result;
    }
    result
        = xud_draw_cylinder_wires(context, (XudVector3) { .x = -8.0f, .y = 0.0f, .z = -8.0f }, 1.0f, 1.0f, 3.0f, xudFirstPersonDarkPurple);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_draw_cylinder(context, (XudVector3) { .x = -4.0f, .y = 0.0f, .z = -8.0f }, 0.0f, 1.5f, 3.0f, xudFirstPersonGold);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_draw_cylinder_wires(context, (XudVector3) { .x = -4.0f, .y = 0.0f, .z = -8.0f }, 0.0f, 1.5f, 3.0f, xudFirstPersonMaroon);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_draw_cylinder(context, (XudVector3) { .x = 0.0f, .y = 0.0f, .z = -8.0f }, 0.5f, 1.5f, 2.5f, xudFirstPersonBlue);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_draw_cylinder_wires(context, (XudVector3) { .x = 0.0f, .y = 0.0f, .z = -8.0f }, 0.5f, 1.5f, 2.5f, xudFirstPersonBlack);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_draw_capsule(context, (XudVector3) { .x = 4.0f, .y = 1.0f, .z = -8.0f }, (XudVector3) { .x = 4.0f, .y = 3.0f, .z = -8.0f },
        0.8f, xudFirstPersonLime);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_draw_capsule_wires(context, (XudVector3) { .x = 4.0f, .y = 1.0f, .z = -8.0f },
        (XudVector3) { .x = 4.0f, .y = 3.0f, .z = -8.0f }, 0.8f, xudFirstPersonMaroon);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_draw_capsule(context, (XudVector3) { .x = -6.0f, .y = 0.8f, .z = -3.0f },
        (XudVector3) { .x = 6.0f, .y = 0.8f, .z = -3.0f }, 0.8f, xudFirstPersonPurple);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_draw_capsule_wires(context, (XudVector3) { .x = -6.0f, .y = 0.8f, .z = -3.0f },
        (XudVector3) { .x = 6.0f, .y = 0.8f, .z = -3.0f }, 0.8f, xudFirstPersonDarkPurple);
    if (result != XUD_OK) {
        return result;
    }

    for (column = 0u; column < columnCount; column++) {
        result = xud_draw_cube(context, columns[column].position, 2.0f, columns[column].height, 2.0f, columns[column].color);
        if (result != XUD_OK) {
            return result;
        }
        result = xud_draw_cube_wires(context, columns[column].position, 2.0f, columns[column].height, 2.0f, xudFirstPersonMaroon);
        if (result != XUD_OK) {
            return result;
        }
    }

    return XUD_OK;
}

/* The mode's own name, the way raylib's example names it on the same HUD (its file, line 187). */
static const char* xud_first_person_mode_name(uint32_t mode) {
    const char* name = "CUSTOM";

    if (mode == XUD_CAMERA_MODE_FREE) {
        name = "FREE";
    } else if (mode == XUD_CAMERA_MODE_FIRST_PERSON) {
        name = "FIRST_PERSON";
    } else if (mode == XUD_CAMERA_MODE_THIRD_PERSON) {
        name = "THIRD_PERSON";
    } else if (mode == XUD_CAMERA_MODE_ORBITAL) {
        name = "ORBITAL";
    }

    return name;
}

/* The HUD: two boxes of the same kind raylib's example draws (its file, line 173 to 195) - the
   controls on the left, the camera record on the right. Both are screen-fixed and depth-free, so the
   camera whose numbers they print moves the scene behind them and not the text. The boxes are one
   opaque pale blue rather than a half-alpha sky blue: the row these quads record into is the opaque
   one, so a faded colour here would be a full one with a misleading name.
*/
static XudResult xud_first_person_hud(
    XudContext* context, XudHandle font, const XudCamera3D* camera, uint32_t mode, uint32_t cursorHidden) {
    const int32_t left = 5;
    const int32_t right = (int32_t)XUD_FIRST_PERSON_WIDTH - 385;
    const int32_t boxWidth = 375;
    const int32_t boxHeight = 152;
    int32_t row = 15;
    XudResult result;

    result = xud_draw_rectangle(context, left, 5, boxWidth, boxHeight, xudFirstPersonSky);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_draw_rectangle_lines(context, left, 5, boxWidth, boxHeight, xudFirstPersonBlue);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_draw_rectangle(context, right, 5, boxWidth, boxHeight, xudFirstPersonSky);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_draw_rectangle_lines(context, right, 5, boxWidth, boxHeight, xudFirstPersonBlue);
    if (result != XUD_OK) {
        return result;
    }

    row = 15;
    result = xud_first_person_text(context, font, left + 10, row, "Camera controls:", xudFirstPersonBlack);
    row += XUD_FIRST_PERSON_ROW_PITCH;
    if (result == XUD_OK) {
        result = xud_first_person_text(context, font, left + 10, row, "- Walk: W, A, S, D", xudFirstPersonBlack);
        row += XUD_FIRST_PERSON_ROW_PITCH;
    }
    if (result == XUD_OK) {
        result = xud_first_person_text(context, font, left + 10, row, "- Look: arrow keys or mouse", xudFirstPersonBlack);
        row += XUD_FIRST_PERSON_ROW_PITCH;
    }
    if (result == XUD_OK) {
        result = xud_first_person_text(context, font, left + 10, row, "- Modes: 1 free, 2 first, 3 third, 4 orbit", xudFirstPersonBlack);
        row += XUD_FIRST_PERSON_ROW_PITCH;
    }
    if (result == XUD_OK) {
        result = xud_first_person_text(
            context, font, left + 10, row, "- Zoom: wheel or keypad + -  (orbit, third, free)", xudFirstPersonBlack);
        row += XUD_FIRST_PERSON_ROW_PITCH;
    }
    if (result == XUD_OK) {
        result = xud_first_person_text(
            context, font, left + 10, row, (cursorHidden != 0u) ? "- Cursor: TAB shows it" : "- Cursor: TAB hides it", xudFirstPersonBlack);
        row += XUD_FIRST_PERSON_ROW_PITCH;
    }
    if (result == XUD_OK) {
        result = xud_first_person_text(context, font, left + 10, row, "- Quit: ESC", xudFirstPersonBlack);
    }
    if (result != XUD_OK) {
        return result;
    }

    row = 15;
    result = xud_first_person_text(context, font, right + 10, row, "Camera status:", xudFirstPersonBlack);
    row += XUD_FIRST_PERSON_ROW_PITCH;
    if (result == XUD_OK) {
        result = xud_first_person_text(context, font, right + 10, row, "- Mode: %s", xudFirstPersonBlack, xud_first_person_mode_name(mode));
        row += XUD_FIRST_PERSON_ROW_PITCH;
    }
    if (result == XUD_OK) {
        result = xud_first_person_text(context, font, right + 10, row, "- Position: (%06.2f, %06.2f, %06.2f)", xudFirstPersonBlack,
            (double)camera->position.x, (double)camera->position.y, (double)camera->position.z);
        row += XUD_FIRST_PERSON_ROW_PITCH;
    }
    if (result == XUD_OK) {
        result = xud_first_person_text(context, font, right + 10, row, "- Target: (%06.2f, %06.2f, %06.2f)", xudFirstPersonBlack,
            (double)camera->target.x, (double)camera->target.y, (double)camera->target.z);
        row += XUD_FIRST_PERSON_ROW_PITCH;
    }
    if (result == XUD_OK) {
        result = xud_first_person_text(context, font, right + 10, row, "- Up: (%06.2f, %06.2f, %06.2f)", xudFirstPersonBlack,
            (double)camera->up.x, (double)camera->up.y, (double)camera->up.z);
    }

    return result;
}

/* One short frame said out loud, and only the first: the batch's capacity DOUBLES at the start of a frame
   (xud_begin_drawing's growth, which xud_get_batch_stats()'s growthAsked field is what says is coming),
   so a scene this big - ground, three walls, twenty columns and their wires, and the HUD's glyph quads -
   is drawn short for the first few frames and whole from the frame whose capacity fits it. Every frame
   redraws the whole scene, so the short ones cost nothing but the shapes that did not fit; saying so
   once is the difference between a scene that looks broken and one that says what it is doing.
*/
static void xud_first_person_count_short_frame(uint32_t frame, XudResult step, uint32_t* shortFrames) {
    if (*shortFrames == 0u) {
        printf("camera_first_person: frame %u a draw refused -> %s; the batch's capacity doubles at the start of the next\n"
               "camera_first_person:   frame, so this scene is short for the first few frames and whole from the one that fits\n",
            frame, xud_result_name(step));
    }
    (*shortFrames)++;
}

/*-----------------------------------------------------------------------------------------------
 * The run
 *-----------------------------------------------------------------------------------------------*/

int main(void) {
    static const int xudFirstPersonModeKeys[4] = { XUD_KEY_ONE, XUD_KEY_TWO, XUD_KEY_THREE, XUD_KEY_FOUR };
    static const uint32_t xudFirstPersonModes[4]
        = { XUD_CAMERA_MODE_FREE, XUD_CAMERA_MODE_FIRST_PERSON, XUD_CAMERA_MODE_THIRD_PERSON, XUD_CAMERA_MODE_ORBITAL };
    XudFirstPersonColumn columns[XUD_FIRST_PERSON_MAX_COLUMNS];
    XudCamera3D camera = xudFirstPersonStartCamera;
    XudFontData fontData
        = { .fileData = NULL, .fileBytes = 0u, .pixelSize = XUD_FIRST_PERSON_FONT_PIXELS, .codepoints = NULL, .codepointCount = 0u };
    uint8_t* fontFile = NULL;
    uint32_t fontBytes = 0u;
    uint32_t cursorHidden = 1u;
    uint32_t mode = XUD_CAMERA_MODE_FIRST_PERSON;
    uint32_t column = 0u;
    uint32_t frame = 0u;
    uint32_t shortFrames = 0u;
    XudContext* context = NULL;
    XudHandle font;
    XudResult step;

    fontFile = xud_first_person_read(XUD_FIRST_PERSON_FONT_PATH, &fontBytes);
    if (fontFile == NULL) {
        printf("camera_first_person: no font at %s, so the HUD has nothing to draw with\n", XUD_FIRST_PERSON_FONT_PATH);
        return 1;
    }

    step = xud_init_context(&context, XUD_FIRST_PERSON_WIDTH, XUD_FIRST_PERSON_HEIGHT);
    printf("camera_first_person: xud_init_context(%u, %u) -> %s (xudlibc %s)\n", XUD_FIRST_PERSON_WIDTH, XUD_FIRST_PERSON_HEIGHT,
        xud_result_name(step), XUD_VERSION);
    if (step != XUD_OK) {
        free(fontFile);
        return (step == XUD_UNSUPPORTED) ? 0 : 1;
    }

    fontData.fileData = fontFile;
    fontData.fileBytes = fontBytes;
    step = xud_load_font(&font, context, &fontData);
    if (step != XUD_OK) {
        printf("camera_first_person: xud_load_font -> %s\n", xud_result_name(step));
        (void)xud_close_context(&context);
        free(fontFile);
        return 1;
    }

    /* The seed, named once: the columns below are then the same twenty every run draws. */
    step = xud_set_random_seed(context, XUD_FIRST_PERSON_SEED);
    if (step != XUD_OK) {
        printf("camera_first_person: xud_set_random_seed -> %s, so the scene is the machine's own\n", xud_result_name(step));
    }
    for (column = 0u; column < XUD_FIRST_PERSON_MAX_COLUMNS; column++) {
        int value = 0;

        step = xud_get_random_value(&value, context, XUD_FIRST_PERSON_HEIGHT_MIN, XUD_FIRST_PERSON_HEIGHT_MAX);
        if (step != XUD_OK) {
            printf("camera_first_person: xud_get_random_value -> %s, so there is no scene to walk\n", xud_result_name(step));
            (void)xud_close_font(context, font);
            (void)xud_close_context(&context);
            free(fontFile);
            return 1;
        }
        columns[column].height = (float)value;
        columns[column].position.y = columns[column].height * 0.5f;
        (void)xud_get_random_value(&value, context, -XUD_FIRST_PERSON_EXTENT, XUD_FIRST_PERSON_EXTENT);
        columns[column].position.x = (float)value;
        (void)xud_get_random_value(&value, context, -XUD_FIRST_PERSON_EXTENT, XUD_FIRST_PERSON_EXTENT);
        columns[column].position.z = (float)value;
        (void)xud_get_random_value(&value, context, 20, 255);
        columns[column].color.r = (uint8_t)value;
        (void)xud_get_random_value(&value, context, 10, 55);
        columns[column].color.g = (uint8_t)value;
        columns[column].color.b = 30u;
        columns[column].color.a = 255u;
    }

    /* raylib's DisableCursor, before the loop rather than inside it: the pointer stops being drawn
       and stops reporting an absolute position, and every frame's look is then the delta since the
       frame before - which is the one thing a look-around needs and the one thing an absolute cursor
       cannot give, because the hand reaches the edge of the window inside a second.
    */
    step = xud_hide_cursor(context);
    if (step != XUD_OK) {
        printf("camera_first_person: xud_hide_cursor -> %s, so the look drags an absolute cursor\n", xud_result_name(step));
        cursorHidden = 0u;
    }
    if (xud_set_target_fps(context, 60u) != XUD_OK) {
        printf("camera_first_person: xud_set_target_fps -> refused, so the run free-runs\n");
    }

    /* The frame loop. The key reads sit before xud_end_drawing() and so read the state the frame
       BEFORE made current - xud_end_drawing() is the poll GLFW's events arrive inside - which is the
       one-frame lag every app of this shape has, and no key here depends on which side of it it is.
    */
    for (;;) {
        bool shouldClose = false;
        bool pressed = false;
        uint32_t pick = 0u;

        step = xud_window_should_close(&shouldClose, context);
        if (step != XUD_OK) {
            printf("camera_first_person: frame %u xud_window_should_close -> %s\n", frame, xud_result_name(step));
            break;
        }
        if (shouldClose) {
            printf("camera_first_person: frame %u: the window was closed\n", frame);
            break;
        }
        if ((xud_is_key_pressed(&pressed, context, XUD_KEY_ESCAPE) == XUD_OK) && pressed) {
            printf("camera_first_person: frame %u: ESC\n", frame);
            break;
        }
        if ((xud_is_key_pressed(&pressed, context, XUD_KEY_TAB) == XUD_OK) && pressed) {
            if (cursorHidden != 0u) {
                (void)xud_show_cursor(context);
                cursorHidden = 0u;
            } else {
                (void)xud_hide_cursor(context);
                cursorHidden = 1u;
            }
        }
        for (pick = 0u; pick < 4u; pick++) {
            if ((xud_is_key_pressed(&pressed, context, xudFirstPersonModeKeys[pick]) == XUD_OK) && pressed) {
                mode = xudFirstPersonModes[pick];
                camera.up = (XudVector3) { .x = 0.0f, .y = 1.0f, .z = 0.0f };
            }
        }

        step = xud_update_camera(context, &camera, mode);
        if (step != XUD_OK) {
            printf("camera_first_person: frame %u xud_update_camera(%s) -> %s\n", frame, xud_first_person_mode_name(mode),
                xud_result_name(step));
            break;
        }

        /* The frame's draws. A refused append is not the run's end: the batch grows at the START of a
           frame (xud_get_batch_stats()'s growthAsked field is that policy said back), so the first
           frames of a scene this big are drawn short - ground, walls, some columns - until the capacity
           reaches what the whole scene asks for. Every frame redraws the whole scene, so the short ones
           are counted here and the run keeps walking.
        */
        step = xud_begin_drawing(context);
        if (step != XUD_OK) {
            printf("camera_first_person: frame %u xud_begin_drawing -> %s, and the run stops here\n", frame, xud_result_name(step));
            break;
        }
        (void)xud_clear_background(context, xudFirstPersonRaywhite);

        /* The camera, begun here and NOT ended inside the frame - the one place this example does not
           copy raylib's shape. raylib's BeginMode3D/EndMode3D nest inside BeginDrawing/EndDrawing
           because raylib bakes the view into every 3D vertex as it is appended; this library holds ONE
           matrix per frame and writes it into set 0's block at the submission itself (xud_renderer.c's
           note beside xud_camera_apply: the block's matrix is a function of THIS frame's camera state,
           and no camera live is the identity). A camera ended before the submission is therefore a
           frame drawn in clip space, where a ground plane 32 units wide is a line one pixel tall and a
           wall 32 units long is off the screen - which is what the pair's doc means by "the frames
           drawn after here are drawn with no camera at all". The pair is taken back below, once the
           run is over, and each frame's begin REPLACES the live record rather than stacking on it.
        */
        (void)xud_begin_camera_3d(context, camera);
        step = xud_first_person_scene(context, columns, XUD_FIRST_PERSON_MAX_COLUMNS);
        if ((step == XUD_OK) && (mode == XUD_CAMERA_MODE_THIRD_PERSON)) {
            /* The player's own box, at the target the camera is behind: raylib's example draws it in
               the third-person mode alone (its file, line 166), because it is the only mode whose eye
               is far enough from the walker to see it.
            */
            step = xud_draw_cube(context, camera.target, 0.5f, 0.5f, 0.5f, xudFirstPersonPurple);
            if (step == XUD_OK) {
                step = xud_draw_cube_wires(context, camera.target, 0.5f, 0.5f, 0.5f, xudFirstPersonDarkPurple);
            }
        }
        if (step != XUD_OK) {
            xud_first_person_count_short_frame(frame, step, &shortFrames);
        }

        step = xud_first_person_hud(context, font, &camera, mode, cursorHidden);
        if (step != XUD_OK) {
            xud_first_person_count_short_frame(frame, step, &shortFrames);
        }
        step = xud_end_drawing(context);
        if (step != XUD_OK) {
            printf("camera_first_person: frame %u xud_end_drawing -> %s, and the run stops here\n", frame, xud_result_name(step));
            break;
        }
        frame++;
    }

    /* The camera goes away with the run rather than with the frame: the last frame it was live for has
       been submitted by now.
    */
    step = xud_end_camera_3d(context);
    if (step != XUD_OK) {
        printf("camera_first_person: xud_end_camera_3d -> %s\n", xud_result_name(step));
    }

    printf("camera_first_person: %u frame(s), %u of them short, mode %s, eye (%.2f, %.2f, %.2f), cursor %s\n", frame, shortFrames,
        xud_first_person_mode_name(mode), (double)camera.position.x, (double)camera.position.y, (double)camera.position.z,
        (cursorHidden != 0u) ? "hidden" : "shown");

    (void)xud_show_cursor(context);
    step = xud_close_font(context, font);
    if (step != XUD_OK) {
        printf("camera_first_person: xud_close_font -> %s\n", xud_result_name(step));
    }
    free(fontFile);
    step = xud_close_context(&context);
    printf("camera_first_person: xud_close_context -> %s\n", xud_result_name(step));

    return (step == XUD_OK) ? 0 : 1;
}
