/***********************************************************************************************
 *
 *   hello_ui.c - hello world, told through two sliders that turn a 3d cube
 *
 *   Owns : one font file read off disk, one context, one ui rung opened on that font, and a frame
 *          loop that feeds the context's input into the ui, draws a window with two sliders, turns
 *          the cube below by the angles they carry, and folds the frame into the batch. Nothing
 *          else: no atlas of its own (the rung bakes the font), no pipeline of its own (the fold
 *          draws through the existing TEXTURED row), no scissor of its own (the windows overlap by
 *          paint order).
 *
 *   Layer: examples. Public API only (xud.h) plus stdio for the lines it prints, stdlib for the
 *          one buffer the font file is read into, and math.h for the cube's two rotations. The
 *          caller names a font file and a window extent, and the widgets are written against the
 *          nuklear context xud_ui_get_context() hands back - the gui is a first-class rung, not an
 *          opaque handle, so this example includes the nuklear widget calls the way it includes
 *          the rest of the public API.
 *
 *   What this example draws: a titled window holding two sliders - a yaw and a pitch in degrees -
 *          and behind it, in the frame's one 3D camera, a cube whose eight corners the sliders'
 *          angles rotate and whose six faces the two triangle calls per face draw in six colours.
 *          The cube is built of triangles rather than drawn with xud_draw_cube() because that call
 *          is axis-aligned: the model matrix is the camera + transform step's, and that step has
 *          not landed, so a rotated box is the caller's own rotation of the shape's vertices.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include <math.h> /* Required for: sinf and cosf, the cube's two rotations */
#include <stdio.h> /* Required for: fopen, fseek, ftell, fread, fclose and printf, the file read and the report */
#include <stdlib.h> /* Required for: malloc and free, the one buffer the font file is read into */

#include "xud.h" /* Required for: the context pair, the camera pair, the triangle draw, the ui rung's five calls, and the nuklear widget API they frame */

/*-----------------------------------------------------------------------------------------------
 * The window, the font file and the run
 *-----------------------------------------------------------------------------------------------*/

#define XUD_HELLO_WINDOW_WIDTH 640u
#define XUD_HELLO_WINDOW_HEIGHT 360u

/* The font file is the repo's own test asset, read from the gitignored resources/ dir: the bake
   below is stated against one file the repo carries, so a run on any machine shows the same
   letterforms rather than whichever font collection that machine happens to ship. */
#define XUD_HELLO_FONT_PATH "resources/fonts/NotoSansTC-Regular.ttf"
#define XUD_HELLO_FONT_PIXEL_SIZE 16u

/* The run's length in wall seconds and its frame cap, the same safety net every example carries:
   the seconds are what end a normal run and the cap is the net for a clock that never answers. */
#define XUD_HELLO_RUN_SECONDS 4.0
#define XUD_HELLO_FRAME_CAP 60000u

/*-----------------------------------------------------------------------------------------------
 * The cube: its corners, its faces and the camera it is drawn in
 *-----------------------------------------------------------------------------------------------*/

#define XUD_HELLO_CUBE_HALF 0.5f
#define XUD_HELLO_DEGREES_PER_RADIANS 57.29577951308232f

/* The unit cube's eight corners, half an edge out from the origin. The rotation below is applied
   to these once per frame rather than baked, because the sliders carry the angles and a frame
   that turns is a frame whose corners moved. */
static const XudVector3 xud_hello_corners[8] = {
    { -XUD_HELLO_CUBE_HALF, -XUD_HELLO_CUBE_HALF, -XUD_HELLO_CUBE_HALF },
    { XUD_HELLO_CUBE_HALF, -XUD_HELLO_CUBE_HALF, -XUD_HELLO_CUBE_HALF },
    { XUD_HELLO_CUBE_HALF, XUD_HELLO_CUBE_HALF, -XUD_HELLO_CUBE_HALF },
    { -XUD_HELLO_CUBE_HALF, XUD_HELLO_CUBE_HALF, -XUD_HELLO_CUBE_HALF },
    { -XUD_HELLO_CUBE_HALF, -XUD_HELLO_CUBE_HALF, XUD_HELLO_CUBE_HALF },
    { XUD_HELLO_CUBE_HALF, -XUD_HELLO_CUBE_HALF, XUD_HELLO_CUBE_HALF },
    { XUD_HELLO_CUBE_HALF, XUD_HELLO_CUBE_HALF, XUD_HELLO_CUBE_HALF },
    { -XUD_HELLO_CUBE_HALF, XUD_HELLO_CUBE_HALF, XUD_HELLO_CUBE_HALF },
};

/* The cube's six faces, each as a quadrilateral walked counter-clockwise seen from outside and
   one colour: the two triangles a quad is cut into are (a, b, c) and (a, c, d) in that walk. The
   walk is what the pipeline's front face reads, the same winding xud_shapes.h states for its own
   boxes. */
typedef struct XudHelloFace {
    uint8_t i0;
    uint8_t i1;
    uint8_t i2;
    uint8_t i3;
    XudColor color;
} XudHelloFace;

static const XudHelloFace xud_hello_faces[6] = {
    { 0u, 3u, 2u, 1u, { 220u, 60u, 60u, 255u } },
    { 4u, 5u, 7u, 6u, { 90u, 200u, 90u, 255u } },
    { 1u, 2u, 6u, 5u, { 60u, 110u, 220u, 255u } },
    { 0u, 4u, 7u, 3u, { 240u, 160u, 40u, 255u } },
    { 3u, 7u, 6u, 2u, { 240u, 220u, 60u, 255u } },
    { 0u, 1u, 5u, 4u, { 180u, 90u, 220u, 255u } },
};

/* The frame's one camera: the eye stands on +z looking at the cube's centre. The cube's corner
   reaches 0.5 * sqrt(3) = 0.866 world units out at every rotation, and the eye standing 4.0 down
   -z puts that corner at clip x 2.414 * 0.866 / (4.0 - 0.866) = 0.67 at fovy 45 - inside the clip
   volume at every yaw and pitch the two sliders can reach, with the near plane (0.1) staying 3.1
   units clear of the nearest corner. */
static XudCamera3D xud_hello_camera(void) {
    return (XudCamera3D) {
        .position = (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 4.0f },
        .target = (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 0.0f },
        .up = (XudVector3) { .x = 0.0f, .y = 1.0f, .z = 0.0f },
        .fovy = 45.0f,
        .near = 0.1f,
        .far = 20.0f,
    };
}

/*-----------------------------------------------------------------------------------------------
 * The font file, read once
 *-----------------------------------------------------------------------------------------------*/

static uint8_t* xud_hello_ui_font_read(const char* path, uint32_t* outBytes) {
    FILE* file = NULL;
    long size = 0L;
    uint8_t* buffer = NULL;

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
    buffer = (uint8_t*)malloc((size_t)size);
    if (buffer == NULL) {
        (void)fclose(file);
        return NULL;
    }
    if (fread(buffer, 1u, (size_t)size, file) != (size_t)size) {
        free(buffer);
        (void)fclose(file);
        return NULL;
    }
    (void)fclose(file);
    *outBytes = (uint32_t)size;
    return buffer;
}

/*-----------------------------------------------------------------------------------------------
 * The cube, drawn from the sliders' two angles
 *-----------------------------------------------------------------------------------------------*/

/* The yaw is a turn about y and the pitch a turn about x, the pitch applied after the yaw so a
   pitch of 0 leaves the yaw's turn standing. Each is the one rotation the cube's frame carries,
   and their product is the matrix the eight corners are passed through: a corner is the column
   (x, y, z, 1) the matrix's rows are dotted against. */
static XudVector3 xud_hello_rotate(const XudVector3 point, float yawRadians, float pitchRadians) {
    const float cy = cosf(yawRadians);
    const float sy = sinf(yawRadians);
    const float cx = cosf(pitchRadians);
    const float sx = sinf(pitchRadians);

    return (XudVector3) {
        .x = (cy * point.x) + (sy * point.z),
        .y = (sx * sy * point.x) + (cx * point.y) - (sx * cy * point.z),
        .z = (-cx * sy * point.x) + (sx * point.y) + (cx * cy * point.z),
    };
}

static XudResult xud_hello_draw_cube(XudContext* context, float yawRadians, float pitchRadians) {
    XudVector3 corners[8];
    uint32_t face;

    for (uint32_t i = 0u; i < 8u; i++) {
        corners[i] = xud_hello_rotate(xud_hello_corners[i], yawRadians, pitchRadians);
    }

    for (face = 0u; face < 6u; face++) {
        const XudVector3 a = corners[xud_hello_faces[face].i0];
        const XudVector3 b = corners[xud_hello_faces[face].i1];
        const XudVector3 c = corners[xud_hello_faces[face].i2];
        const XudVector3 d = corners[xud_hello_faces[face].i3];
        XudResult step = XUD_OK;

        step = xud_draw_triangle_3d(context, a, b, c, xud_hello_faces[face].color);
        if (step == XUD_OK) {
            step = xud_draw_triangle_3d(context, a, c, d, xud_hello_faces[face].color);
        }
        if (step != XUD_OK) {
            return step;
        }
    }
    return XUD_OK;
}

int main(void) {
    uint32_t fontBytes = 0u;
    uint8_t* fileData = xud_hello_ui_font_read(XUD_HELLO_FONT_PATH, &fontBytes);

    if (fileData == NULL) {
        printf("hello_ui: no font file at %s, so there is nothing to bake\n", XUD_HELLO_FONT_PATH);
        return 1;
    }
    printf("hello_ui: read %u bytes of %s\n", fontBytes, XUD_HELLO_FONT_PATH);

    XudContext* context = NULL;
    XudResult step = xud_init_context(&context, XUD_HELLO_WINDOW_WIDTH, XUD_HELLO_WINDOW_HEIGHT);

    printf("hello_ui: xud_init_context(%u, %u) -> %s (xudlibc %s)\n", XUD_HELLO_WINDOW_WIDTH, XUD_HELLO_WINDOW_HEIGHT,
        xud_result_name(step), XUD_VERSION);
    if (step != XUD_OK) {
        free(fileData);
        return (step == XUD_UNSUPPORTED) ? 0 : 1;
    }

    /* The ui rung, opened on the font's bytes: the rung bakes the file into one atlas image, opens
       that image as a TEXTURE and names it on every glyph, so the caller never names an atlas or a
       sampler - the same "name a file, not a texture" shape the font pair carries. */
    const XudFontData fontData = (XudFontData) {
        .fileData = fileData, .fileBytes = fontBytes, .pixelSize = XUD_HELLO_FONT_PIXEL_SIZE, .codepoints = NULL, .codepointCount = 0u
    };

    step = xud_ui_init(context, &fontData);
    if (step != XUD_OK) {
        printf("hello_ui: xud_ui_init -> %s\n", xud_result_name(step));
        (void)xud_close_context(&context);
        free(fileData);
        return 1;
    }
    printf("hello_ui: xud_ui_init -> %s\n", xud_result_name(step));

    struct nk_context* nk = xud_ui_get_context(context);
    if (nk == NULL) {
        printf("hello_ui: no nuklear context behind the open rung\n");
        (void)xud_ui_close(context);
        (void)xud_close_context(&context);
        free(fileData);
        return 1;
    }

    /* The widget state the frame loop carries: the cube's two angles in degrees, one per slider.
       Both live in the example rather than the rung - the rung owns the context and the atlas,
       and the caller owns the widgets it draws against it. They start off-axis so the first
       frame already shows the cube turned. */
    float yawDegrees = 30.0f;
    float pitchDegrees = -20.0f;

    uint32_t frame = 0u;
    uint32_t shortFrames = 0u;
    double now = 0.0;

    for (frame = 0u; frame < XUD_HELLO_FRAME_CAP; frame++) {
        step = xud_begin_drawing(context);
        if (step != XUD_OK) {
            printf("hello_ui: frame %u xud_begin_drawing -> %s\n", frame, xud_result_name(step));
            break;
        }

        /* The input, fed from the context's own input rung into the nuklear context before the
           widgets read it: the keys, the mouse and the wheel the frame below's sliders answer are
           the state this call wrote. */
        step = xud_ui_begin(context);
        if (step != XUD_OK) {
            printf("hello_ui: frame %u xud_ui_begin -> %s\n", frame, xud_result_name(step));
            break;
        }

        /* The cube, drawn through the frame's one 3D camera before the ui: the camera is the eye
           above looking at the origin, and the two slider angles are what turn the cube inside it
           - the corners the rotate-and-draw below carry are the angles as they are this frame. */
        const float yawRadians = yawDegrees / XUD_HELLO_DEGREES_PER_RADIANS;
        const float pitchRadians = pitchDegrees / XUD_HELLO_DEGREES_PER_RADIANS;

        step = xud_begin_camera_3d(context, xud_hello_camera());
        if (step != XUD_OK) {
            printf("hello_ui: frame %u xud_begin_camera_3d -> %s\n", frame, xud_result_name(step));
            break;
        }
        step = xud_hello_draw_cube(context, yawRadians, pitchRadians);
        if (step == XUD_OK) {
            step = xud_end_camera_3d(context);
        }
        if (step != XUD_OK) {
            printf("hello_ui: frame %u cube -> %s\n", frame, xud_result_name(step));
            break;
        }

        /* The frame's widgets, written against the context the rung handed back: one window holding
           a line that reports the two angles and the two sliders that carry them - a yaw that
           turns the cube about its vertical axis and a pitch that tips it over its front face.
           nk_begin() opens the window and nk_end() closes it, and a slider answers the mouse the
           input rung fed before it. */
        if (nk_begin(nk, "hello_ui", nk_rect(12, 12, 200, 105), NK_WINDOW_TITLE | NK_WINDOW_BORDER)) {
            char label[48];
            snprintf(label, sizeof(label), "yaw %3d  pitch %3d", (int)yawDegrees, (int)pitchDegrees);
            nk_layout_row_dynamic(nk, 18, 1);
            nk_label(nk, label, NK_TEXT_LEFT);
            nk_layout_row_dynamic(nk, 25, 1);
            nk_slider_float(nk, 0.0f, &yawDegrees, 360.0f, 1.0f);
            nk_layout_row_dynamic(nk, 25, 1);
            nk_slider_float(nk, -90.0f, &pitchDegrees, 90.0f, 1.0f);
        }
        nk_end(nk);

        /* The fold: convert the frame into the staging and fold it into the batch at the context's
           own extent, through the atlas's one TEXTURE slot. A frame that does not fit the batch is
           deferred as one unit, so a short frame is not the run's end - the boundary above grows the
           batch for the next frame and the ui reappears, the same short-frame shape the other
           examples state. */
        step = xud_ui_draw(context);
        if (step == XUD_OUT_OF_MEMORY) {
            shortFrames++;
            step = XUD_OK;
        } else if (step != XUD_OK) {
            printf("hello_ui: frame %u xud_ui_draw -> %s\n", frame, xud_result_name(step));
            break;
        }

        step = xud_end_drawing(context);
        if (step != XUD_OK) {
            printf("hello_ui: frame %u xud_end_drawing -> %s\n", frame, xud_result_name(step));
            break;
        }

        if (xud_get_time(&now, context) != XUD_OK) {
            printf("hello_ui: the clock refused at frame %u, and the run stops here\n", frame);
            break;
        }
        if (now >= XUD_HELLO_RUN_SECONDS) {
            break;
        }
    }

    printf("hello_ui: %u frames, %u short frames while the batch settled, %.2f s, yaw %.0f pitch %.0f\n", frame, shortFrames, now,
        yawDegrees, pitchDegrees);

    /* The ui given back BEFORE the context: the atlas it owns is a TEXTURE slot of the context's
       handle table, and the table is the top rung xud_close_context() retires first - though
       xud_close_context() closes the ui first of all now, so a caller that closes the context
       without the ui still gets a clean teardown. */
    step = xud_ui_close(context);
    printf("hello_ui: xud_ui_close -> %s\n", xud_result_name(step));

    const XudResult closed = xud_close_context(&context);

    printf("hello_ui: xud_close_context -> %s\n", xud_result_name(closed));
    free(fileData);

    return ((step != XUD_OK) || (closed != XUD_OK)) ? 1 : 0;
}
