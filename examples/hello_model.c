/***********************************************************************************************
 *
 *   hello_model.c - a .glb file, loaded and drawn
 *
 *   Owns : one context, one .glb path handed to one load, the two numbers that load answers
 *          (its primitive count and its bounding box), the camera that box's own extent fits,
 *          the model drawn every frame through that camera, the same box drawn beside it as
 *          wires, one skinned .glb the gate is expected to refuse, and the teardown that gives
 *          the model back before it gives back the context. Nothing else: no mesh of its own,
 *          no material of its own, no buffer, no batch, no matrix of its own.
 *
 *   Layer: examples. Public API only (xud.h) plus stdio for the lines it prints and math.h for
 *          the orbit and the fit - and nothing else. That is the whole point of the public
 *          header here: user code names a FILE and a handle, and never a cgltf call, a VkBuffer,
 *          a staging block, a pipeline row or a descriptor. The one path this example does NOT
 *          read is the one hello_font.c does read: xud_load_model takes a path rather than bytes,
 *          because the loader rung is the file-system read's owner (see the note beside XudFontData
 *          in xud.h, which states the other half of that split), so there is no fopen, no fseek and
 *          no buffer anywhere below the title of this file.
 *
 *   What the frame does, in the order the calls state it: xud_init_context() brings the whole
 *   ladder up; xud_load_model() parses the container, loads the document's buffers against the
 *   path's own directory prefix, validates the accessor ranges against the bytes that load
 *   actually brought, and then spends one MESH slot per glTF primitive and one MATERIAL slot per
 *   document material plus the default - so a model is a list of the objects the rungs below own,
 *   and the document is gone by the time that call returns; xud_get_model_mesh_count() and
 *   xud_get_model_bounding_box() read the two numbers that load folded; xud_begin_camera_3d()
 *   puts the frame's own orbit in; xud_draw_model() records one row per primitive, each replaying
 *   through that primitive's own five buffers rather than through the batch's arena; xud_draw_cube_
 *   wires() appends the box the query answered as the batch's own 8 vertices and 24 indices; and
 *   xud_end_drawing() closes the boundary, which is what uploads, binds BOTH buffer sets, draws
 *   both rows through the two pipeline rows and presents.
 *
 *   The one thing this example is built around, and the reason it is not a shape list: a model
 *   brings its own placement with it and this library has no model matrix yet (the camera +
 *   transform step above the shapes is open, and xud_draw_model() states that its model matrix is
 *   the identity today). A model is therefore placed by the CAMERA alone, and the camera below is
 *   derived from the model's own bounding box rather than from a number copied from a modelling
 *   tool: the box the query answers is the extent the frustum is fitted to, and it is drawn beside
 *   the geometry as wires so a reader can see the fit and the box are one statement rather than
 *   two hopes. A model whose box is elsewhere is a camera that is elsewhere, and a model whose box
 *   is bigger is a camera that is further away - which is the property worth understanding before
 *   changing anything here, and the reason the run works on a file the example has never seen.
 *
 *   What the two rows of the frame are, and why there are two of them: the model's primitives are
 *   TRIANGLES and the box's wires are LINES, and topology is pipeline state rather than draw state
 *   (it lives in VkPipelineInputAssemblyStateCreateInfo, not in a call), so one frame records two
 *   rows through two rows of the pipeline list. That is also why the model's rows and the box's
 *   rows coexist at all: a mesh appends nothing to the batch's arena, it names its own five
 *   buffers, so the frame's upload and the frame's binds are two different sets of bytes and the
 *   batch's geometry list is what keeps them apart.
 *
 *   What a run shows, and why the load rather than the texture is the point here: the document's
 *   material names a baseColorTexture, so the load spends a TEXTURE slot for the image and names it
 *   as the material's diffuseMap, which moves the primitive's rows onto the textured row of the
 *   pipeline list and binds set 1 for them - the run's image is the document's base colour sampled
 *   at each fragment's uv and tinted by the Lambert term, exactly what hello_textured_model.c
 *   shows on the repo's greenman shield. The file this example reads is a MODEL rather than a
 *   texture because the point here is the LOAD: one path in, one MODEL handle out, the meshes,
 *   materials and texture behind it, and the gate's answer to the document it cannot read.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include <math.h> /* Required for: sinf, cosf, atanf, tanf, sqrtf, fabsf - the orbit and the frustum fit */
#include <stdio.h> /* Required for: printf, the load's answers and the run's report */

#include "xud.h" /* Required for: xud_load_model/xud_close_model, xud_draw_model, xud_get_model_mesh_count, xud_get_model_bounding_box, XudCamera3D and the camera pair - the whole public model API, and no internal header */

/*-----------------------------------------------------------------------------------------------
 * The window, the files, the fit and the run
 *-----------------------------------------------------------------------------------------------*/

/* 16:9 rather than square, because the fit below divides the box's extent by an aperture that
   carries the aspect: a square window would make the horizontal half-angle and the vertical one
   the same number and hide the half of the fit that the aspect is doing. */
#define XUD_HELLO_WINDOW_WIDTH 960u
#define XUD_HELLO_WINDOW_HEIGHT 540u
#define XUD_HELLO_ASPECT ((float)XUD_HELLO_WINDOW_WIDTH / (float)XUD_HELLO_WINDOW_HEIGHT)

/* The model is the repo's own test asset under the gitignored resources/ dir rather than a
   generated file: the run prints the box the LOAD folded out of this file's bytes, so a run on
   any machine prints the same numbers rather than whichever model collection that machine
   happens to ship. The one thing about this file that matters to the gate is stated below. */
#define XUD_HELLO_MODEL_PATH "resources/model/gltf/raylib_logo_3d.glb"

/* The second file is the gate's answer rather than a second scene: it carries JOINTS_0 and
   WEIGHTS_0, and this tree has no bone matrices, no second pipeline to read them through and
   no skinning, so the load is expected to answer XUD_UNSUPPORTED before a slot is spent. The
   run prints that answer rather than assuming it, which is what makes the boundary readable
   at a machine rather than only in the header's prose. */
#define XUD_HELLO_SKINNED_PATH "resources/model/gltf/robot.glb"

/* The aperture the fit below is stated against, in degrees - raylib's fovy. It is a constant
   rather than a per-frame swing so the orbit's swing is the ONLY motion in the run: a swinging
   aperture and a swinging eye would be two things to read instead of one. */
#define XUD_HELLO_FOVY_DEG 45.0f

/* The fit's slack. The bound below is a bounding-SPHERE fit, which is already slack for a flat
   wide model (the sphere's radius is the box's half-diagonal, past its half-width on every axis),
   so the margin stays small: at 1.06 the model fills about two thirds of the window's height
   rather than sitting in the middle of a sphere-sized frame. */
#define XUD_HELLO_FIT_MARGIN 1.06f

/* The near/far slack, in world units either side of the sphere the fit was computed against.
   The two planes are the caller's because the scene's depth budget is the caller's, and the
   camera rung maps [near,far] onto the whole of the depth buffer - so the budget below is as
   tight as the orbit allows and the depth buffer's precision is spent across the model rather
   than across empty space behind it. */
#define XUD_HELLO_PLANE_SLACK 1.0f
#define XUD_HELLO_NEAR_MIN 0.01f /* the floor the camera rung refuses at, read as a floor rather than as a plane */

/* The orbit's two swings, at DIFFERENT periods so the pair's combined direction repeats only
   at their sum's - the same shape breakout_3d's camera states, and the reason one period would
   make the run a diagonal sweep rather than an orbit. */
#define XUD_HELLO_YAW_MAX_DEG 32.0f
#define XUD_HELLO_YAW_PERIOD_SECONDS 9.0
#define XUD_HELLO_PITCH_MAX_DEG 10.0f
#define XUD_HELLO_PITCH_PERIOD_SECONDS 13.0
#define XUD_HELLO_RADIANS_PER_TURN 6.2831853071795864f
#define XUD_HELLO_DEGREES_PER_RADIANS 57.2957795130832f

/* The wire box's inflation, against the model's own box. The wires are the SAME box the query
   answered, and the model's extremes touch that box's faces, so an uninflated box would be a
   box whose edges coincide with geometry at the same depth - a z-fight rather than an outline.
   Three percent of the extent is enough to clear the depth buffer's resolution at the distance
   the fit lands at, and small enough that the outline still reads as the model's own box. */
#define XUD_HELLO_BOX_INFLATE 1.03f

/* The run's length in wall seconds and its frame cap. The clock is the library's own (xud_get_time,
   raylib's GetTime) and the cap is the safety net for a run whose clock never answers, set above
   what this machine reaches in the seconds below so the SECONDS are what end a normal run. */
#define XUD_HELLO_RUN_SECONDS 6.0
#define XUD_HELLO_FRAME_CAP 90000u

/*-----------------------------------------------------------------------------------------------
 * The fit: the box the load folded, turned into a distance
 *-----------------------------------------------------------------------------------------------*/

/* The bounding-sphere fit, and the reason it is a sphere rather than a box-against-frustum test:
   the orbit below swings the eye, so the extent that must fit is the one that fits at EVERY angle
   of that swing, and the only bound that is angle-independent is the sphere around the box. A
   per-axis fit would be tighter at yaw 0 and would clip a corner of the box as soon as the swing
   turned it toward the frustum's edge, which is a bug that would only appear mid-run. The limiting
   half-angle is the SMALLER of the two, and on a 16:9 window that is the vertical one: the
   horizontal half-angle is derived from the vertical one by the aspect, exactly as the camera
   rung's own projection builds it (xud_camera.h: the symmetric frustum at the caller's aspect),
   so the fit and the projection cannot disagree about which axis is which. */
static float xud_hello_fit_distance(float radius) {
    const float halfVertical = (XUD_HELLO_FOVY_DEG / 2.0f) / XUD_HELLO_DEGREES_PER_RADIANS;
    const float halfHorizontal = atanf(tanf(halfVertical) * XUD_HELLO_ASPECT);
    const float halfLimit = (halfHorizontal < halfVertical) ? halfHorizontal : halfVertical;

    if (!((halfLimit > 0.0f) && (radius > 0.0f))) {
        return XUD_HELLO_NEAR_MIN; /* a degenerate box or a degenerate aperture: the floor, not a division */
    }

    return (radius / sinf(halfLimit)) * XUD_HELLO_FIT_MARGIN;
}

/*-----------------------------------------------------------------------------------------------
 * The frame's own camera, built from the box rather than from a number
 *-----------------------------------------------------------------------------------------------*/

/* The eye orbits the box's CENTRE rather than the origin, and that is the whole reason the box's
   centre is read out of the query rather than assumed: a model whose geometry sits at (30, -12,
   7) is a model whose centre is (30, -12, 7), and an orbit about the origin would swing around a
   point outside the model rather than around the model. The two planes are stated against the
   sphere the fit used, which is what keeps the model inside [near,far] at every angle of the
   swing rather than only at the one the run starts at. */
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

/* The count and the box, read through the two public queries rather than reached through a record.
   The count is the number of rows xud_draw_model() will record - one per glTF PRIMITIVE rather than
   one per glTF MESH, which is the reason a model is a list of slots rather than one row - and the
   box is what the fit above and the wire box below are both stated against, so the printed box,
   the camera's distance and the drawn outline are one number read three ways. */
static void xud_hello_model_report(XudContext* context, XudHandle model, XudVector3* outCentre, XudVector3* outHalf, float* outRadius) {
    uint32_t meshes = 0u;
    XudVector3 min;
    XudVector3 max;

    if (xud_get_model_mesh_count(&meshes, context, model) != XUD_OK) {
        printf("hello_model: xud_get_model_mesh_count refused, so the run cannot say what was loaded\n");
        return;
    }
    if (xud_get_model_bounding_box(&min, &max, context, model) != XUD_OK) {
        printf("hello_model: xud_get_model_bounding_box refused, so the camera cannot be fitted\n");
        return;
    }

    outCentre->x = (min.x + max.x) / 2.0f;
    outCentre->y = (min.y + max.y) / 2.0f;
    outCentre->z = (min.z + max.z) / 2.0f;
    outHalf->x = fabsf(max.x - min.x) / 2.0f;
    outHalf->y = fabsf(max.y - min.y) / 2.0f;
    outHalf->z = fabsf(max.z - min.z) / 2.0f;
    *outRadius = sqrtf((outHalf->x * outHalf->x) + (outHalf->y * outHalf->y) + (outHalf->z * outHalf->z));

    printf("hello_model: %s -> %u primitive row(s), box (%.3f, %.3f, %.3f) to (%.3f, %.3f, %.3f)\n", XUD_HELLO_MODEL_PATH, meshes, min.x,
        min.y, min.z, max.x, max.y, max.z);
    printf("hello_model: centre (%.3f, %.3f, %.3f), half-diagonal %.3f world units, fit distance %.3f\n", outCentre->x, outCentre->y,
        outCentre->z, *outRadius, xud_hello_fit_distance(*outRadius));
}

/*-----------------------------------------------------------------------------------------------
 * The frame's content: the model's rows, then the box's wires
 *-----------------------------------------------------------------------------------------------*/

/* The model first and the outline second, and the order is the reason the outline is legible: the
   batch's rows are a list and the frame replays them in the order they were recorded, so the wires
   resolve against the geometry through the depth buffer rather than over it. The three arguments
   are the box's own three extents times the inflation, so the outline is the model's box rather
   than a cube around it: a flat model keeps a flat outline, and an outline that was a cube would
   be a second statement about the model rather than the one the query answered. */
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
    XudHandle skinned;
    XudVector3 centre = (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 0.0f };
    XudVector3 half = (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 0.0f };
    float radius = 0.0f;
    uint32_t frame = 0u;
    uint32_t refused = 0u;
    double now = 0.0;
    XudResult step = xud_init_context(&context, XUD_HELLO_WINDOW_WIDTH, XUD_HELLO_WINDOW_HEIGHT);

    printf("hello_model: xud_init_context(%u, %u) -> %s (xudlibc %s)\n", XUD_HELLO_WINDOW_WIDTH, XUD_HELLO_WINDOW_HEIGHT,
        xud_result_name(step), XUD_VERSION);
    if (step != XUD_OK) {
        return (step == XUD_UNSUPPORTED) ? 0 : 1;
    }

    /* The load, in one call and one path: the container is autodetected rather than trusted from
       the extension (see xud_model.c, which asks cgltf to sniff the magic), the buffers are loaded
       against the path's own directory prefix, and the validate runs AFTER that load because the
       accessor ranges it checks are ranges against bytes that load has to have brought. Nothing
       here names a mesh, a material or a buffer: the load spends one MESH slot per primitive and
       one MATERIAL slot per document material plus the default, and what a caller holds is one
       MODEL handle. */
    step = xud_load_model(&model, context, XUD_HELLO_MODEL_PATH);
    printf("hello_model: xud_load_model(%s) -> %s\n", XUD_HELLO_MODEL_PATH, xud_result_name(step));
    if (step != XUD_OK) {
        printf("hello_model: no model at %s, so there is nothing to draw\n", XUD_HELLO_MODEL_PATH);
        (void)xud_close_context(&context);
        return (step == XUD_NOT_FOUND) ? 0 : 1;
    }

    xud_hello_model_report(context, model, &centre, &half, &radius);
    if (!((radius > 0.0f))) {
        printf("hello_model: the box is empty, so there is no extent to fit the camera to\n");
        (void)xud_close_model(context, model);
        (void)xud_close_context(&context);
        return 1;
    }

    /* The skinned file, in the same run and for one line of output: this tree has no bone matrices
       and no second pipeline to read them through, so the gate refuses the document BEFORE it
       spends a slot (xud_model_check() runs above xud_model_open() for exactly that reason) and
       the answer is XUD_UNSUPPORTED rather than XUD_INVALID_ARG, because the document is well-formed
       - it is a model this library does not read - rather than a broken one. */
    step = xud_load_model(&skinned, context, XUD_HELLO_SKINNED_PATH);
    printf(
        "hello_model: xud_load_model(%s) -> %s (skinned: no bone matrices in this tree)\n", XUD_HELLO_SKINNED_PATH, xud_result_name(step));
    if (step == XUD_OK) {
        (void)xud_close_model(context, skinned);
    }

    /* The camera put in EVERY frame rather than once for the run, and taken away after the loop
       rather than inside one frame: a begin while a camera is already live REPLACES it rather than
       stacking it (xud.h), so the frame's own swing is the frame's own matrix, and a camera that
       began and ended INSIDE one frame is a no-op - the block's matrix is rewritten every frame
       from the camera state the renderer holds, so a frame that closed its own camera out drew
       the identity instead of the orbit. The scope is therefore the run, not the frame. */
    for (frame = 0u; frame < XUD_HELLO_FRAME_CAP; frame++) {
        step = xud_begin_drawing(context);
        if (step != XUD_OK) {
            printf("hello_model: frame %u xud_begin_drawing -> %s\n", frame, xud_result_name(step));
            break;
        }

        step = xud_begin_camera_3d(context, xud_hello_camera_at(centre, radius, now));
        if (step != XUD_OK) {
            printf("hello_model: frame %u xud_begin_camera_3d -> %s\n", frame, xud_result_name(step));
            refused++;
            break;
        }

        step = xud_hello_model_frame(context, model, centre, half);
        if (step != XUD_OK) {
            printf("hello_model: frame %u the scene -> %s\n", frame, xud_result_name(step));
            refused++;
            break;
        }

        step = xud_end_drawing(context);
        if (step != XUD_OK) {
            printf("hello_model: frame %u xud_end_drawing -> %s\n", frame, xud_result_name(step));
            break;
        }

        if (xud_get_time(&now, context) != XUD_OK) {
            printf("hello_model: the clock refused at frame %u, and the run stops here\n", frame);
            break;
        }
        if (now >= XUD_HELLO_RUN_SECONDS) {
            break;
        }
    }

    printf("hello_model: %u frames, %u refused, %.2f s\n", frame, refused, now);

    /* The camera taken away after the run rather than left live: the pair is a scope, and the
       frames drawn after here are drawn with no camera at all, which is the identity matrix the
       descriptor rung opens set 0's block with - nothing of this run's camera state reaches past
       here, which is the half of the pair's contract the example can show. */
    step = xud_end_camera_3d(context);
    if (step != XUD_OK) {
        printf("hello_model: xud_end_camera_3d -> %s\n", xud_result_name(step));
    }

    /* The model given back BEFORE the context: a MODEL slot is a list of MESH and MATERIAL slots of
       the SAME table, and xud_close_context() retires that table as its top rung - a context closed
       with a model still live would be a table freed under a record that names buffers the mesh
       rung below is about to destroy. xud_close_model() runs the mesh rung's and the material rung's
       own closes, in that order, because a mesh's record names the material it draws with. */
    step = xud_close_model(context, model);
    printf("hello_model: xud_close_model -> %s\n", xud_result_name(step));

    const XudResult closed = xud_close_context(&context);

    printf("hello_model: xud_close_context -> %s\n", xud_result_name(closed));

    return ((step != XUD_OK) || (closed != XUD_OK)) ? 1 : 0;
}
