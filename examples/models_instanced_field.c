/***********************************************************************************************
 *
 *   models_instanced_field.c - a thousand cubes, ONE draw call: the S3.2 instanced row seen whole
 *
 *   Owns : one context, one cube mesh and one camera record. The field is a 32 by 32 grid of
 *          cubes - 1024 copies - and the whole of it goes out as ONE xud_draw_mesh_instanced
 *          call per frame: the cube's five buffers are uploaded once, and the per-copy height,
 *          spin and colour are DATA in the instance streams rather than 1024 pushes and 1024
 *          draws. The wave is the waving-cubes wave (the clock's, not an input's): each cell's
 *          height and tint are a function of the cell and of the frame's seconds, recomputed
 *          into the caller's own matrix array every frame. Layer: examples. Public API only.
 *
 *   Shape : the eye orbits the field at raylib's own fraction of the clock (a third of a turn
 *          per thirty-odd seconds), the same walking-around-the-field as models_waving_cubes.
 *          No key is read and no camera function is called: the record is constant but for the
 *          orbit's two numbers.
 *
 *   Why  : xud_draw_mesh() places one copy per call, and a thousand copies is a thousand draw
 *          rows - the CPU-bound field this row exists to replace. Here the count moves into the
 *          draw command's instanceCount and the transforms into two vertex bindings, so the
 *          frame's whole geometry is one vkCmdDrawIndexed and the matrices are a buffer write.
 *          A batch of this size is exactly where the difference is visible: same picture, one
 *          draw row.
 *
 *   Controls: nothing but the window's close button and ESC.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include <math.h>   /* Required for: sinf, cosf, the wave and the eye's orbit */
#include <stdio.h>  /* Required for: printf */
#include <stdlib.h> /* Required for: free and malloc, the cube's arrays and the instance matrices */

#include "xud.h" /* Required for: the mesh pair, the instanced draw, the camera pair, the frame pair, the matrix rung and the context's own pair - the whole public API this example is written against */

#define LOGNAME "models_instanced_field"

#define XUD_FIELD_WIDTH 960u
#define XUD_FIELD_HEIGHT 540u

/* The field: 32 on a side, 1024 copies, five units apart and one unit wide - a grid you can see
   the floor of. The instance region of the default batch holds thirty-two thousand copies, so a
   frame of this field is never the short kind. */
#define XUD_FIELD_SIDE 32u
#define XUD_FIELD_COUNT (XUD_FIELD_SIDE * XUD_FIELD_SIDE)
#define XUD_FIELD_SPACING 5.0f

/* The eye: the same height-over-radius proportion the waving field's eye hangs at. */
#define XUD_FIELD_ORBIT_RADIUS 70.0f
#define XUD_FIELD_EYE_HEIGHT 35.0f

static const XudCamera3D xudFieldStartCamera = { .position = { .x = XUD_FIELD_ORBIT_RADIUS, .y = XUD_FIELD_EYE_HEIGHT, .z = 0.0f },
    .target = { .x = 0.0f, .y = 0.0f, .z = 0.0f },
    .up = { .x = 0.0f, .y = 1.0f, .z = 0.0f },
    .fovy = 70.0f,
    .near = 0.5f,
    .far = 300.0f };

static void xud_text_error(const char* stage, XudResult result);

/* The six-sector HSV the wave colours with, the same conversion models_waving_cubes writes: this
   library has no colour conversion in the public header. */
static XudColor xud_field_hsv(float hue, float saturation, float value) {
    float chroma = value * saturation;
    float huePrime = hue / 60.0f;
    float match = chroma * (1.0f - fabsf(fmodf(huePrime, 2.0f) - 1.0f));
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
    XudColor color = { 0u, 0u, 0u, 255u };

    if (huePrime < 1.0f) {
        red = chroma;
        green = match;
    } else if (huePrime < 2.0f) {
        red = match;
        green = chroma;
    } else if (huePrime < 3.0f) {
        green = chroma;
        blue = match;
    } else if (huePrime < 4.0f) {
        green = match;
        blue = chroma;
    } else if (huePrime < 5.0f) {
        red = match;
        blue = chroma;
    } else {
        red = chroma;
        blue = match;
    }
    color.r = (uint8_t)((red + (value - chroma)) * 255.0f);
    color.g = (uint8_t)((green + (value - chroma)) * 255.0f);
    color.b = (uint8_t)((blue + (value - chroma)) * 255.0f);
    return color;
}

/* Fill one frame's worth of copies: the cell's distance from the field's centre sets the wave's
   phase, so the wave runs out of the middle of the grid, and the same number sets the hue. The
   matrices are the twelve numbers of xud_matrix_compose (a translation, a yaw, a scale) written
   straight into the caller's array at the copy's row, which is the whole per-copy channel of the
   instanced row. */
static void xud_field_fill(float* matrices, XudColor* tints, double seconds) {
    uint32_t cell = 0u;
    float half = (float)(XUD_FIELD_SIDE - 1u) * 0.5f * XUD_FIELD_SPACING;

    for (uint32_t row = 0u; row < XUD_FIELD_SIDE; row++) {
        for (uint32_t col = 0u; col < XUD_FIELD_SIDE; col++) {
            float x = (float)col * XUD_FIELD_SPACING - half;
            float z = (float)row * XUD_FIELD_SPACING - half;
            float distance = sqrtf(x * x + z * z);
            float wave = sinf((float)seconds * 2.0f - distance * 0.25f);
            float height = 1.0f + wave * 0.75f;
            XudVector3 radians = { .x = 0.0f, .y = wave * 0.5f, .z = 0.0f };
            XudVector3 scale = { .x = 1.0f, .y = height, .z = 1.0f };
            XudVector3 translation = { .x = x, .y = height * 0.5f, .z = z };

            (void)xud_matrix_compose(&matrices[(size_t)cell * 16u], translation, radians, scale);
            tints[cell] = xud_field_hsv((distance * 6.0f) + (float)seconds * 10.0f, 0.75f, 0.9f);
            cell++;
        }
    }
}

static int xud_run(XudContext* context) {
    XudResult step;
    XudHandle cube = { 0u, 0u, 0u };
    XudCamera3D camera = xudFieldStartCamera;
    float* matrices = NULL;
    XudColor* tints = NULL;
    float* positions = NULL;
    float* normals = NULL;
    uint32_t* indices = NULL;
    uint32_t frame = 0u;
    uint32_t shortFrames = 0u;
    int status = 1;

    /* One cube, cut and uploaded once for the whole run: every copy of the field shares its
       buffers, which is the point of the row. */
    XudMeshShape shape = { .kind = XUD_MESH_KIND_CUBE, .width = 1.0f, .height = 1.0f, .length = 1.0f };
    XudMeshCounts counts = { 0u, 0u };
    XudMeshBuffers buffers = { NULL, NULL, NULL, NULL };
    XudMeshData meshData = { 0u, 0u, NULL, NULL, NULL, NULL, NULL };

    step = xud_gen_mesh_count(&counts, &shape);
    if (step != XUD_OK) {
        xud_text_error("xud_gen_mesh_count", step);
        return 1;
    }
    positions = malloc((size_t)counts.vertexCount * 3u * sizeof(float));
    normals = malloc((size_t)counts.vertexCount * 3u * sizeof(float));
    indices = malloc((size_t)counts.indexCount * sizeof(uint32_t));
    matrices = malloc((size_t)XUD_FIELD_COUNT * 16u * sizeof(float));
    tints = malloc((size_t)XUD_FIELD_COUNT * sizeof(XudColor));
    if ((positions == NULL) || (normals == NULL) || (indices == NULL) || (matrices == NULL) || (tints == NULL)) {
        printf(LOGNAME ": out of memory for the cube and the field\n");
        goto cleanup;
    }
    buffers.pos = positions;
    buffers.normal = normals;
    buffers.index = indices;
    step = xud_gen_mesh(&meshData, &shape, &buffers);
    if (step != XUD_OK) {
        xud_text_error("xud_gen_mesh", step);
        goto cleanup;
    }
    step = xud_upload_mesh(&cube, context, &meshData);
    if (step != XUD_OK) {
        xud_text_error("xud_upload_mesh", step);
        goto cleanup;
    }

    printf(LOGNAME ": %u copies, one instanced draw per frame\n", XUD_FIELD_COUNT);

    for (;;) {
        bool shouldClose = false;
        bool pressed = false;
        double seconds = 0.0;

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

        if (xud_get_time(&seconds, context) != XUD_OK) {
            seconds = 0.0;
        }
        camera.position.x = (float)cos(seconds * 0.3) * XUD_FIELD_ORBIT_RADIUS;
        camera.position.z = (float)sin(seconds * 0.3) * XUD_FIELD_ORBIT_RADIUS;
        xud_field_fill(matrices, tints, seconds);

        step = xud_begin_drawing(context);
        if (step != XUD_OK) {
            xud_text_error("frame xud_begin_drawing", step);
            break;
        }
        step = xud_clear_background(context, (XudColor) { 24u, 24u, 32u, 255u });
        if (step != XUD_OK) {
            xud_text_error("frame xud_clear_background", step);
            (void)xud_end_drawing(context);
            break;
        }
        /* Begun here and NOT ended inside the frame: the camera pair does not nest inside the
           drawing pair (see models_waving_cubes' Pairing half - one matrix per frame at the
           submission itself). */
        step = xud_begin_camera_3d(context, camera);
        if (step != XUD_OK) {
            xud_text_error("frame xud_begin_camera_3d", step);
            (void)xud_end_drawing(context);
            break;
        }
        step = xud_draw_mesh_instanced(context, cube, matrices, tints, XUD_FIELD_COUNT);
        if (step == XUD_OUT_OF_MEMORY) {
            /* The frame's short kind, counted and drawn round at the NEXT boundary: the batch
               doubles the instance region for the ask it just refused, so a field this size is
               short for the first frame and whole from the second (xud_batch.h's growth). */
            shortFrames++;
        } else if (step != XUD_OK) {
            xud_text_error("xud_draw_mesh_instanced", step);
            (void)xud_end_drawing(context);
            break;
        }
        step = xud_end_drawing(context);
        if (step != XUD_OK) {
            xud_text_error("frame xud_end_drawing", step);
            break;
        }
        frame++;
    }

    step = xud_end_camera_3d(context);
    if (step != XUD_OK) {
        xud_text_error("xud_end_camera_3d", step);
    }
    step = xud_close_mesh(context, cube);
    if (step != XUD_OK) {
        xud_text_error("xud_close_mesh", step);
    }
    printf(LOGNAME ": %u frame(s), %u of them short\n", frame, shortFrames);
    status = 0;

cleanup:
    free(positions);
    free(normals);
    free(indices);
    free(matrices);
    free(tints);
    return status;
}

static void xud_text_error(const char* stage, XudResult result) { printf(LOGNAME ": %s -> %s\n", stage, xud_result_name(result)); }

int main(void) {
    XudContext* context = NULL;
    XudResult step;
    int status = 0;

    step = xud_init_context(&context, XUD_FIELD_WIDTH, XUD_FIELD_HEIGHT, LOGNAME);
    printf(LOGNAME ": xud_init_context(%u, %u, \"%s\") -> %s (xudlibc %s)\n", XUD_FIELD_WIDTH, XUD_FIELD_HEIGHT, LOGNAME,
        xud_result_name(step), XUD_VERSION);
    if (step != XUD_OK) {
        return (step == XUD_UNSUPPORTED) ? 0 : 1;
    }

    status = xud_run(context);

    step = xud_close_context(&context);
    printf(LOGNAME ": xud_close_context -> %s\n", xud_result_name(step));

    return (status != 0) ? status : ((step == XUD_OK) ? 0 : 1);
}
