/***********************************************************************************************
 *
 *   hello_textured_model.c - a .glb file with a baseColorTexture, loaded and drawn
 *
 *   Owns : one context, one .glb path handed to one load, the two numbers that load answers
 *          (its primitive count and its bounding box), the camera that box's own extent fits,
 *          the model drawn every frame through that camera, the same box drawn beside it as
 *          wires, and the teardown that gives the model back before it gives back the context.
 *          Nothing else: no mesh of its own, no material of its own, no texture of its own, no
 *          buffer, no batch, no matrix of its own.
 *
 *   Layer: examples. Public API only (xud.h) plus stdio for the lines it prints and math.h for
 *          the orbit and the fit - and nothing else. The split with hello_model.c is the point
 *          here: that file's model carries no images, so its primitives draw through the
 *          UNTEXTURED row of the pipeline list and its texture slots would be spent on nothing,
 *          while this file's model carries a baseColorTexture on its one material, so the load
 *          spends a TEXTURE slot for the document's image and names it as the material's
 *          diffuseMap - and xud_draw_model() records each primitive under that material, which
 *          is what moves its rows onto the textured row of the pipeline list and binds set 1 for
 *          them. Nothing below this title names a sampler, a VkImage or a descriptor: the whole
 *          texture path is inside the load and the draw, and the difference between this run's
 *          image and hello_model's is the file, not a call.
 *
 *   What the frame does, in the order the calls state it: xud_init_context() brings the whole
 *   ladder up; xud_load_model() parses the container, loads the document's buffers against the
 *   path's own directory prefix, decodes the document's image into a texture, spends one MESH
 *   slot per glTF primitive and one MATERIAL slot per document material plus the default, and
 *   wires the material's diffuseMap to that texture; xud_get_model_mesh_count() and
 *   xud_get_model_bounding_box() read the two numbers that load folded; xud_begin_camera_3d()
 *   puts the frame's own orbit in; xud_draw_model() records one row per primitive, each under
 *   that primitive's own material, the textured rows replaying through the row of the pipeline
 *   list whose fragment stage samples set 1; xud_draw_cube_wires() appends the box the query
 *   answered as the batch's own 8 vertices and 24 indices; and xud_end_drawing() closes the
 *   boundary, which is what uploads, binds both buffer sets, draws both rows through the two
 *   pipeline rows and presents.
 *
 *   What a run shows, and why it is the check this file exists for: the model's own surface,
 *   not a flat white approximation of it. hello_model.c states its texture is read by the
 *   document and then dropped because the pipeline list's untextured fragment stage is the only
 *   one its file's primitives resolve to; this file's primitives resolve to the textured row,
 *   and the run's image is the document's base color sampled at each fragment's uv and tinted
 *   by the Lambert term the same stage computes. Run both files on the same machine and the
 *   difference between the two windows is the texture, which is the whole of the statement.
 *
 *   The camera is derived from the model's own bounding box for the reason hello_model.c states
 *   it: the box the query answers is the extent the frustum is fitted to, and it is drawn beside
 *   the geometry as wires so a reader can see the fit and the box are one statement rather than
 *   two hopes.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include <math.h> /* Required for: sinf, cosf, atanf, tanf, sqrtf, fabsf - the orbit and the frustum fit */
#include <stdio.h> /* Required for: printf, the load's answers and the run's report */

#include "xud.h" /* Required for: xud_load_model/xud_close_model, xud_draw_model, xud_get_model_mesh_count, xud_get_model_bounding_box, XudCamera3D and the camera pair - the whole public model API, and no internal header */

/*-----------------------------------------------------------------------------------------------
 * The window, the file, the fit and the run
 *-----------------------------------------------------------------------------------------------*/

/* 16:9 rather than square, for the reason hello_model.c states it: the fit divides the box's
   extent by an aperture that carries the aspect, and a square window would hide the half of the
   fit the aspect is doing. */
#define XUD_HELLO_WINDOW_WIDTH 960u
#define XUD_HELLO_WINDOW_HEIGHT 540u
#define XUD_HELLO_ASPECT ((float)XUD_HELLO_WINDOW_WIDTH / (float)XUD_HELLO_WINDOW_HEIGHT)

/* The repo's own test asset under the gitignored resources/ dir rather than a generated file:
   this document's one material names a baseColorTexture, which is the one fact about the file
   that moves its primitives onto the textured row of the pipeline list. It is the shield of the
   repo's greenman set rather than greenman.glb itself because the body's primitives carry
   JOINTS_0 and WEIGHTS_0 and the gate refuses a skinned document, and the shield's single
   primitive is the textured, unskinned half of the same set. */
#define XUD_HELLO_MODEL_PATH "resources/model/gltf/greenman_shield.glb"

/* The aperture the fit is stated against, in degrees - the same constant as hello_model.c's, so
   the two files' orbits are the same motion and the texture is the only difference between the
   two runs. */
#define XUD_HELLO_FOVY_DEG 45.0f

#define XUD_HELLO_FIT_MARGIN 1.06f
#define XUD_HELLO_PLANE_SLACK 1.0f
#define XUD_HELLO_NEAR_MIN 0.01f
#define XUD_HELLO_YAW_MAX_DEG 32.0f
#define XUD_HELLO_YAW_PERIOD_SECONDS 9.0
#define XUD_HELLO_PITCH_MAX_DEG 10.0f
#define XUD_HELLO_PITCH_PERIOD_SECONDS 13.0
#define XUD_HELLO_RADIANS_PER_TURN 6.2831853071795864f
#define XUD_HELLO_DEGREES_PER_RADIANS 57.2957795130832f
#define XUD_HELLO_BOX_INFLATE 1.03f
#define XUD_HELLO_RUN_SECONDS 6.0
#define XUD_HELLO_FRAME_CAP 90000u

/*-----------------------------------------------------------------------------------------------
 * The fit: the box the load folded, turned into a distance
 *-----------------------------------------------------------------------------------------------*/

/* The bounding-sphere fit, stated for the reason hello_model.c states it: the orbit swings the
   eye, and the only extent bound that fits at every angle of the swing is the sphere around the
   box. */
static float xud_hello_fit_distance(float radius) {
    const float halfVertical = (XUD_HELLO_FOVY_DEG / 2.0f) / XUD_HELLO_DEGREES_PER_RADIANS;
    const float halfHorizontal = atanf(tanf(halfVertical) * XUD_HELLO_ASPECT);
    const float halfLimit = (halfHorizontal < halfVertical) ? halfHorizontal : halfVertical;

    if (!((halfLimit > 0.0f) && (radius > 0.0f))) {
        return XUD_HELLO_NEAR_MIN;
    }

    return (radius / sinf(halfLimit)) * XUD_HELLO_FIT_MARGIN;
}

/*-----------------------------------------------------------------------------------------------
 * The frame's own camera, built from the box rather than from a number
 *-----------------------------------------------------------------------------------------------*/

static XudCamera3D xud_hello_camera_at(XudVector3 centre, float radius, double seconds) {
    const float distance = xud_hello_fit_distance(radius);
    const float yawDegrees = XUD_HELLO_YAW_MAX_DEG * sinf((float)(seconds / XUD_HELLO_YAW_PERIOD_SECONDS) * XUD_HELLO_RADIANS_PER_TURN);
    const float pitchDegrees
        = XUD_HELLO_PITCH_MAX_DEG * sinf((float)(seconds / XUD_HELLO_PITCH_PERIOD_SECONDS) * XUD_HELLO_RADIANS_PER_TURN);
    const float yawRadians = yawDegrees / XUD_HELLO_DEGREES_PER_RADIANS;
    const float pitchRadians = pitchDegrees / XUD_HELLO_DEGREES_PER_RADIANS;
    const float near = distance - radius - XUD_HELLO_PLANE_SLACK;

    return (XudCamera3D) { .position = (XudVector3) { .x = centre.x + (distance * sinf(yawRadians) * cosf(pitchRadians)),
                               .y = centre.y + (distance * sinf(pitchRadians)),
                               .z = centre.z + (distance * cosf(yawRadians) * cosf(pitchRadians)) },
        .target = (XudVector3) { .x = centre.x, .y = centre.y, .z = centre.z },
        .up = (XudVector3) { .x = 0.0f, .y = 1.0f, .z = 0.0f },
        .fovy = XUD_HELLO_FOVY_DEG,
        .near = (near > XUD_HELLO_NEAR_MIN) ? near : XUD_HELLO_NEAR_MIN,
        .far = distance + radius + XUD_HELLO_PLANE_SLACK };
}

/*-----------------------------------------------------------------------------------------------
 * The report: the two numbers the load folded
 *-----------------------------------------------------------------------------------------------*/

static void xud_hello_model_report(XudContext* context, XudHandle model, XudVector3* outCentre, XudVector3* outHalf, float* outRadius) {
    uint32_t meshes = 0u;
    XudVector3 min;
    XudVector3 max;

    if (xud_get_model_mesh_count(&meshes, context, model) != XUD_OK) {
        printf("hello_textured_model: xud_get_model_mesh_count refused, so the run cannot say what was loaded\n");
        return;
    }
    if (xud_get_model_bounding_box(&min, &max, context, model) != XUD_OK) {
        printf("hello_textured_model: xud_get_model_bounding_box refused, so the camera cannot be fitted\n");
        return;
    }

    outCentre->x = (min.x + max.x) / 2.0f;
    outCentre->y = (min.y + max.y) / 2.0f;
    outCentre->z = (min.z + max.z) / 2.0f;
    outHalf->x = fabsf(max.x - min.x) / 2.0f;
    outHalf->y = fabsf(max.y - min.y) / 2.0f;
    outHalf->z = fabsf(max.z - min.z) / 2.0f;
    *outRadius = sqrtf((outHalf->x * outHalf->x) + (outHalf->y * outHalf->y) + (outHalf->z * outHalf->z));

    printf("hello_textured_model: %s -> %u primitive row(s), box (%.3f, %.3f, %.3f) to (%.3f, %.3f, %.3f)\n", XUD_HELLO_MODEL_PATH, meshes,
        min.x, min.y, min.z, max.x, max.y, max.z);
    printf("hello_textured_model: centre (%.3f, %.3f, %.3f), half-diagonal %.3f world units, fit distance %.3f\n", outCentre->x,
        outCentre->y, outCentre->z, *outRadius, xud_hello_fit_distance(*outRadius));
}

/*-----------------------------------------------------------------------------------------------
 * The frame's content: the model's rows, then the box's wires
 *-----------------------------------------------------------------------------------------------*/

static XudResult xud_hello_model_frame(XudContext* context, XudHandle model, XudVector3 centre, XudVector3 half) {
    XudResult result;

    result = xud_draw_model(context, model);
    if (result != XUD_OK) {
        return result;
    }

    result = xud_draw_cube_wires(context, centre, (half.x * 2.0f) * XUD_HELLO_BOX_INFLATE, (half.y * 2.0f) * XUD_HELLO_BOX_INFLATE,
        (half.z * 2.0f) * XUD_HELLO_BOX_INFLATE, (XudColor) { .r = 40u, .g = 220u, .b = 90u, .a = 255u });
    if (result != XUD_OK) {
        return result;
    }

    return XUD_OK;
}

int main(void) {
    XudContext* context = NULL;
    XudHandle model;
    XudVector3 centre = (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 0.0f };
    XudVector3 half = (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 0.0f };
    float radius = 0.0f;
    uint32_t frame = 0u;
    uint32_t refused = 0u;
    double now = 0.0;
    XudResult step = xud_init_context(&context, XUD_HELLO_WINDOW_WIDTH, XUD_HELLO_WINDOW_HEIGHT);

    printf("hello_textured_model: xud_init_context(%u, %u) -> %s (xudlibc %s)\n", XUD_HELLO_WINDOW_WIDTH, XUD_HELLO_WINDOW_HEIGHT,
        xud_result_name(step), XUD_VERSION);
    if (step != XUD_OK) {
        return (step == XUD_UNSUPPORTED) ? 0 : 1;
    }

    /* The load, in one call and one path: the container is autodetected rather than trusted from
       the extension, the buffers are loaded against the path's own directory prefix, the image
       is decoded into a texture, and the material the document names gets that texture as its
       diffuseMap. Nothing here names a mesh, a material or a texture: the load spends the slots
       and what a caller holds is one MODEL handle. */
    step = xud_load_model(&model, context, XUD_HELLO_MODEL_PATH);
    printf("hello_textured_model: xud_load_model(%s) -> %s\n", XUD_HELLO_MODEL_PATH, xud_result_name(step));
    if (step != XUD_OK) {
        printf("hello_textured_model: no model at %s, so there is nothing to draw\n", XUD_HELLO_MODEL_PATH);
        (void)xud_close_context(&context);
        return (step == XUD_NOT_FOUND) ? 0 : 1;
    }

    xud_hello_model_report(context, model, &centre, &half, &radius);
    if (!((radius > 0.0f))) {
        printf("hello_textured_model: the box is empty, so there is no extent to fit the camera to\n");
        (void)xud_close_model(context, model);
        (void)xud_close_context(&context);
        return 1;
    }

    /* The run, for the reason hello_model.c states its own: the camera is put in EVERY frame
       rather than once for the run, and taken away after the loop rather than inside one frame. */
    for (frame = 0u; frame < XUD_HELLO_FRAME_CAP; frame++) {
        step = xud_begin_drawing(context);
        if (step != XUD_OK) {
            printf("hello_textured_model: frame %u xud_begin_drawing -> %s\n", frame, xud_result_name(step));
            break;
        }

        step = xud_begin_camera_3d(context, xud_hello_camera_at(centre, radius, now));
        if (step != XUD_OK) {
            printf("hello_textured_model: frame %u xud_begin_camera_3d -> %s\n", frame, xud_result_name(step));
            refused++;
            break;
        }

        step = xud_hello_model_frame(context, model, centre, half);
        if (step != XUD_OK) {
            printf("hello_textured_model: frame %u the scene -> %s\n", frame, xud_result_name(step));
            refused++;
            break;
        }

        step = xud_end_drawing(context);
        if (step != XUD_OK) {
            printf("hello_textured_model: frame %u xud_end_drawing -> %s\n", frame, xud_result_name(step));
            break;
        }

        if (xud_get_time(&now, context) != XUD_OK) {
            printf("hello_textured_model: the clock refused at frame %u, and the run stops here\n", frame);
            break;
        }
        if (now >= XUD_HELLO_RUN_SECONDS) {
            break;
        }
    }

    printf("hello_textured_model: %u frames, %u refused, %.2f s\n", frame, refused, now);

    step = xud_end_camera_3d(context);
    if (step != XUD_OK) {
        printf("hello_textured_model: xud_end_camera_3d -> %s\n", xud_result_name(step));
    }

    /* The model given back BEFORE the context, for the reason hello_model.c states it: a MODEL
       slot is a list of MESH, MATERIAL and TEXTURE slots of the same table, and the context
       retires that table as its top rung. */
    step = xud_close_model(context, model);
    printf("hello_textured_model: xud_close_model -> %s\n", xud_result_name(step));

    const XudResult closed = xud_close_context(&context);

    printf("hello_textured_model: xud_close_context -> %s\n", xud_result_name(closed));

    return ((step != XUD_OK) || (closed != XUD_OK)) ? 1 : 0;
}
