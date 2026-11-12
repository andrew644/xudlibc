/***********************************************************************************************
 *
 *   models_mesh_picking.c - raylib's models_mesh_picking example on xudlibc
 *
 *   Owns : one context, one font, one camera record, one generated mesh and its CPU twin. The scene is
 *          the collision rung's - a generated solid, the box around it, the triangle the ray is tested
 *          against, the sphere, the floor quad, the grid - and the example is about the two ways a ray
 *          hits a mesh: against the box, which is six comparisons, and against the triangles, which
 *          walks them. Layer: examples. Public API only (xud.h).
 *
 *   The tower : raylib's example loads turret.obj and picks it (its file, lines 35 to 40). The solid
 *          here is cut by this library's generation rung - a twelve-sided cylinder, the tower's shape -
 *          for two reasons: the example then exercises the new rung and the new triangle cast against
 *          the CPU mesh they share, and it carries no asset with it. The CPU XudMeshData is kept beside
 *          the uploaded one on purpose: the cast is a CPU test over the arrays the caller holds, exactly
 *          as raylib's GetRayCollisionMesh() is a CPU test over the Model's mesh, and the GPU row the
 *          draw reads is not what the hit is computed against.
 *
 *   Missing : nothing. The two calls this example is built on came in with the picking work -
 *          xud_get_screen_to_world_ray() (GetScreenToWorldRay, raylib.h 1083), which folds a pixel into
 *          a world ray through the swapchain's own extent, and xud_get_ray_collision_mesh()
 *          (GetRayCollisionMesh, raylib.h 1670), which says where the ray meets a mesh. The barycentrics
 *          raylib's example prints beside its hit have no counterpart here: this library's
 *          XudRayCollision is raylib's RayCollision - hit, distance, point, normal - and carries no
 *          triangle index, so there is no triangle to compute them over and the three floats raylib
 *          reads out of its own VBO are the one line of its HUD this clone leaves out.
 *
 *   Controls: mouse motion drags the first-person view; the RIGHT button switches between the mesh
 *          cast and the box cast, which is the example's whole point - the same click answers twice, and
 *          the two answers are printed beside each other. ESC quits. The cursor is hidden while the
 *          mouse looks around and shown again when the view stops, as raylib's example does.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include <stdarg.h> /* Required for: va_start, va_end, the TextFormat half of the HUD below */
#include <stdio.h>  /* Required for: printf, snprintf, and the file's own report */
#include <stdlib.h> /* Required for: malloc, free, the tower's arrays */

#include "xud.h" /* Required for: xud_get_screen_to_world_ray and the collision rung's ray record, xud_gen_mesh_count and xud_gen_mesh and xud_upload_mesh, the camera record and xud_update_camera, the shapes and wire and grid family, xud_draw_ray and xud_draw_bounding_box, the cursor pair and the input queries, the screen-fixed text and quad pair, and the context's own pair - the whole public API this example is written against */

#define XUD_MESH_PICK_WIDTH 800u /* raylib's example's own extent (its file, line 20) */
#define XUD_MESH_PICK_HEIGHT 450u

#define XUD_MESH_PICK_FONT_PATH "resources/fonts/NotoSansTC-Regular.ttf"
#define XUD_MESH_PICK_FONT_PIXELS 10u
#define XUD_MESH_PICK_LINE_MAX 128

/* The tower: a twelve-sided cylinder of radius 2 and height 8, which is the size of the turret the
   example above loads (its mesh spans about 8 units tall), cut centred on the origin so it stands on
   the floor once it is raised by half its height.
*/
#define XUD_MESH_PICK_TOWER_SIDES 12u
#define XUD_MESH_PICK_TOWER_RADIUS 2.0f
#define XUD_MESH_PICK_TOWER_HEIGHT 8.0f
#define XUD_MESH_PICK_TOWER_Y (XUD_MESH_PICK_TOWER_HEIGHT * 0.5f)

/* The floor's half-edge, raylib's example's 50 (its file, line 43) and the four corners it names. */
#define XUD_MESH_PICK_FLOOR_HALF 50.0f

/* The test triangle and the test sphere, both raylib's example's (its file, lines 44 and 45). */
static const XudVector3 xudMeshPickTriangle[3]
    = { { .x = 10.0f, .y = 10.0f, .z = 0.0f }, { .x = 10.0f, .y = 0.0f, .z = 10.0f }, { .x = 0.0f, .y = 0.0f, .z = 0.0f } };
static const XudVector3 xudMeshPickSphereCenter = { .x = -15.0f, .y = 10.0f, .z = 0.0f };
#define XUD_MESH_PICK_SPHERE_RADIUS 10.0f

/* The first-person eye, raylib's example's start (its file, line 31), and the two modes its RIGHT
   button switches between.
*/
static const XudCamera3D xudMeshPickStartCamera = { .position = { .x = 10.0f, .y = 8.0f, .z = 10.0f },
    .target = { .x = 0.0f, .y = 8.0f, .z = 0.0f },
    .up = { .x = 0.0f, .y = 1.0f, .z = 0.0f },
    .fovy = 45.0f,
    .near = 0.1f,
    .far = 200.0f };

static const XudColor xudMeshPickSky = { .r = 130u, .g = 170u, .b = 205u, .a = 255u };
static const XudColor xudMeshPickLightGray = { .r = 200u, .g = 200u, .b = 200u, .a = 255u };
static const XudColor xudMeshPickGray = { .r = 130u, .g = 130u, .b = 130u, .a = 255u };
static const XudColor xudMeshPickDarkGray = { .r = 80u, .g = 80u, .b = 80u, .a = 255u };
static const XudColor xudMeshPickRed = { .r = 220u, .g = 40u, .b = 40u, .a = 255u };
static const XudColor xudMeshPickOrange = { .r = 250u, .g = 160u, .b = 0u, .a = 255u };
static const XudColor xudMeshPickBlue = { .r = 0u, .g = 120u, .b = 240u, .a = 255u };
static const XudColor xudMeshPickGreen = { .r = 0u, .g = 180u, .b = 60u, .a = 255u };
static const XudColor xudMeshPickPurple = { .r = 160u, .g = 80u, .b = 200u, .a = 255u };
static const XudColor xudMeshPickBlack = { .r = 0u, .g = 0u, .b = 0u, .a = 255u };
static const XudColor xudMeshPickWhite = { .r = 255u, .g = 255u, .b = 255u, .a = 255u };

static uint8_t* xud_mesh_pick_read(const char* path, uint32_t* outBytes);
static XudResult xud_mesh_pick_text(
    XudContext* context, XudHandle font, int32_t posX, int32_t posY, const char* format, XudColor color, ...);

/*-----------------------------------------------------------------------------------------------
 * The machinery: a file reader, a text call that formats, a placement matrix
 *-----------------------------------------------------------------------------------------------*/

/* The font file's bytes: xud_load_font() takes bytes rather than a path, so the caller reads the file
   here. NULL when there is no file.
*/
static uint8_t* xud_mesh_pick_read(const char* path, uint32_t* outBytes) {
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

/* raylib's DrawText(TextFormat(...)) as one call: snprintf into a row of bytes, the bytes into
   codepoints, the row drawn screen-fixed at the caller's pixel.
*/
static XudResult xud_mesh_pick_text(
    XudContext* context, XudHandle font, int32_t posX, int32_t posY, const char* format, XudColor color, ...) {
    char line[XUD_MESH_PICK_LINE_MAX];
    uint32_t codepoints[XUD_MESH_PICK_LINE_MAX];
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

    return xud_draw_text_screen(context, font, codepoints, (uint32_t)written, posX, posY, (float)XUD_MESH_PICK_FONT_PIXELS, 0.0f, color);
}

/*-----------------------------------------------------------------------------------------------
 * The run
 *-----------------------------------------------------------------------------------------------*/

int main(void) {
    XudMeshShape towerShape = { .kind = XUD_MESH_KIND_CYLINDER,
        .radius = XUD_MESH_PICK_TOWER_RADIUS,
        .tubeRadius = 0.25f,
        .width = 1.0f,
        .height = XUD_MESH_PICK_TOWER_HEIGHT,
        .length = 1.0f,
        .slices = XUD_MESH_PICK_TOWER_SIDES,
        .stacks = 3u };
    float towerMatrix[16];
    XudVector3 floorCorners[4];
    XudMeshData towerData = { 0 };
    XudMeshCounts towerCounts = { 0u, 0u };
    XudRay ray = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } };
    XudCamera3D camera = xudMeshPickStartCamera;
    XudFontData fontData
        = { .fileData = NULL, .fileBytes = 0u, .pixelSize = XUD_MESH_PICK_FONT_PIXELS, .codepoints = NULL, .codepointCount = 0u };
    uint8_t* fontFile = NULL;
    uint32_t fontBytes = 0u;
    float* vertices = NULL;
    float* texcoords = NULL;
    float* normals = NULL;
    uint32_t* indices = NULL;
    uint32_t frame = 0u;
    uint32_t shortFrames = 0u;
    uint32_t hitsMesh = 0u;
    uint32_t hitsBox = 0u;
    bool cursorHidden = false;
    bool againstMesh = true;
    XudContext* context = NULL;
    XudHandle font;
    XudHandle tower;
    XudResult result;

    /* The four floor corners, raylib's quad (its file, line 43): the plane the eye walks on, and the
       first thing the ray is tested against, so the walk stops at the floor's edge rather than letting
       the eye fall through the scene. Wound counter-clockwise seen from above, which is the winding the
       quad's cast splits along the (0, 1, 3) diagonal.
    */
    floorCorners[0] = (XudVector3) { .x = -XUD_MESH_PICK_FLOOR_HALF, .y = 0.0f, .z = XUD_MESH_PICK_FLOOR_HALF };
    floorCorners[1] = (XudVector3) { .x = XUD_MESH_PICK_FLOOR_HALF, .y = 0.0f, .z = XUD_MESH_PICK_FLOOR_HALF };
    floorCorners[2] = (XudVector3) { .x = XUD_MESH_PICK_FLOOR_HALF, .y = 0.0f, .z = -XUD_MESH_PICK_FLOOR_HALF };
    floorCorners[3] = (XudVector3) { .x = -XUD_MESH_PICK_FLOOR_HALF, .y = 0.0f, .z = -XUD_MESH_PICK_FLOOR_HALF };

    fontFile = xud_mesh_pick_read(XUD_MESH_PICK_FONT_PATH, &fontBytes);
    if (fontFile == NULL) {
        printf("models_mesh_picking: no font at %s, so the HUD has nothing to draw with\n", XUD_MESH_PICK_FONT_PATH);
        return 1;
    }

    result = xud_init_context(&context, XUD_MESH_PICK_WIDTH, XUD_MESH_PICK_HEIGHT);
    printf("models_mesh_picking: xud_init_context(%u, %u) -> %s (xudlibc %s)\n", XUD_MESH_PICK_WIDTH, XUD_MESH_PICK_HEIGHT,
        xud_result_name(result), XUD_VERSION);
    if (result != XUD_OK) {
        free(fontFile);
        return (result == XUD_UNSUPPORTED) ? 0 : 1;
    }

    fontData.fileData = fontFile;
    fontData.fileBytes = fontBytes;
    result = xud_load_font(&font, context, &fontData);
    if (result != XUD_OK) {
        printf("models_mesh_picking: xud_load_font -> %s\n", xud_result_name(result));
        (void)xud_close_context(&context);
        free(fontFile);
        return 1;
    }

    /* The tower: the count, then arrays sized from it, then the cut, then the upload - and the CPU
       arrays stay alive, because the cast this example is about reads them and not the GPU row. The
       mesh's handle and this record are two views of one solid: the handle is what the draw opens, the
       record is what the collision rung walks, and the two agree because the rung copies nothing and
       the example moves nothing.
    */
    result = xud_gen_mesh_count(&towerCounts, &towerShape);
    if (result == XUD_OK) {
        vertices = malloc((size_t)towerCounts.vertexCount * 3u * sizeof(float));
        texcoords = malloc((size_t)towerCounts.vertexCount * 2u * sizeof(float));
        normals = malloc((size_t)towerCounts.vertexCount * 3u * sizeof(float));
        indices = malloc((size_t)towerCounts.indexCount * sizeof(uint32_t));
        if ((vertices == NULL) || (texcoords == NULL) || (normals == NULL) || (indices == NULL)) {
            printf("models_mesh_picking: out of memory for the tower's arrays\n");
            result = XUD_OUT_OF_MEMORY;
        }
    }
    if (result == XUD_OK) {
        XudMeshBuffers towerBuffers = { .pos = vertices, .texcoords = texcoords, .normal = normals, .index = indices };

        /* The fill writes the description - counts and the four stream pointers - and the description
           is what both the upload and the cast below read, so the two cannot drift apart.
        */
        result = xud_gen_mesh(&towerData, &towerShape, &towerBuffers);
    }
    if (result == XUD_OK) {
        result = xud_upload_mesh(&tower, context, &towerData);
    }
    printf("models_mesh_picking: tower -> %s, %u vertices, %u indices\n", xud_result_name(result), (unsigned int)towerCounts.vertexCount,
        (unsigned int)towerCounts.indexCount);
    if (result != XUD_OK) {
        free(vertices);
        free(texcoords);
        free(normals);
        free(indices);
        (void)xud_close_font(context, font);
        (void)xud_close_context(&context);
        free(fontFile);
        return 1;
    }

    /* How the tower stands on the floor, and the same matrix the mesh cast is thrown through: the cast
       and the draw must agree about where the solid is, which is the note raylib's example leaves about
       its own transform (its file, lines 88 to 90).
    */
    (void)xud_matrix_translate(towerMatrix, (XudVector3) { .x = 0.0f, .y = XUD_MESH_PICK_TOWER_Y, .z = 0.0f });

    if (xud_set_target_fps(context, 60u) != XUD_OK) {
        printf("models_mesh_picking: xud_set_target_fps -> refused, so the run free-runs\n");
    }

    for (;;) {
        /* The frame's report: what the ray hit, and how far, so the run says something when nobody is
           moving the mouse and the HUD's numbers cannot be checked against anything.
        */
        bool shouldClose = false;
        bool pressed = false;
        XudVector2 pixel = { .x = 0.0f, .y = 0.0f };
        XudRayCollision boxHit = { false, 0.0f, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } };
        XudRayCollision meshHit = { false, 0.0f, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } };
        XudRayCollision floorHit = { false, 0.0f, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } };
        XudRayCollision triangleHit = { false, 0.0f, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } };
        XudRayCollision sphereHit = { false, 0.0f, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } };
        XudBoundingBox towerBox = { .min = { .x = -XUD_MESH_PICK_TOWER_RADIUS, .y = 0.0f, .z = -XUD_MESH_PICK_TOWER_RADIUS },
            .max = { .x = XUD_MESH_PICK_TOWER_RADIUS, .y = XUD_MESH_PICK_TOWER_HEIGHT, .z = XUD_MESH_PICK_TOWER_RADIUS } };
        const char* hitName = "nothing";

        result = xud_window_should_close(&shouldClose, context);
        if (result != XUD_OK) {
            printf("models_mesh_picking: frame %u xud_window_should_close -> %s\n", frame, xud_result_name(result));
            break;
        }
        if (shouldClose) {
            printf("models_mesh_picking: frame %u: the window was closed\n", frame);
            break;
        }
        if ((xud_is_key_pressed(&pressed, context, XUD_KEY_ESCAPE) == XUD_OK) && pressed) {
            printf("models_mesh_picking: frame %u: ESC\n", frame);
            break;
        }
        /* RIGHT, raylib's example's own key (its file, line 65): the cast switches, the cursor goes
           with the mode it belongs to - hidden while the mouse looks around, shown again when it does
           not - and the eye goes back to where it started, which is the example's behaviour when the
           mode changes and the reason the walk does not carry over a mode switch.
        */
        if ((xud_is_mouse_button_pressed(&pressed, context, XUD_MOUSE_BUTTON_RIGHT) == XUD_OK) && pressed) {
            againstMesh = !againstMesh;
            camera = xudMeshPickStartCamera;
            if (againstMesh != false) {
                if (cursorHidden == false) {
                    result = xud_hide_cursor(context);
                    if (result != XUD_OK) {
                        printf(
                            "models_mesh_picking: xud_hide_cursor -> %s, so the look drags an absolute cursor\n", xud_result_name(result));
                    }
                    cursorHidden = true;
                }
            } else if (cursorHidden != false) {
                result = xud_show_cursor(context);
                if (result != XUD_OK) {
                    printf("models_mesh_picking: xud_show_cursor -> %s\n", xud_result_name(result));
                }
                cursorHidden = false;
            }
            printf("models_mesh_picking: frame %u: picking against the %s\n", frame, (againstMesh != false) ? "MESH" : "BOUNDING BOX");
        }

        /* The walk: FIRST_PERSON, so the eye moves by the camera's own forward and right, mouse deltas
           turn it, and the whole thing is raylib's rcamera.h UpdateCamera with the record the caller
           owns. The mode needs the deltas, which is why the cursor is hidden while it is live.
        */
        result = xud_update_camera(context, &camera, XUD_CAMERA_MODE_FIRST_PERSON);
        if (result != XUD_OK) {
            printf("models_mesh_picking: frame %u xud_update_camera(FIRST_PERSON) -> %s\n", frame, xud_result_name(result));
            break;
        }

        result = xud_begin_drawing(context);
        if (result != XUD_OK) {
            printf("models_mesh_picking: frame %u xud_begin_drawing -> %s, and the run stops here\n", frame, xud_result_name(result));
            break;
        }
        (void)xud_clear_background(context, xudMeshPickSky);

        /* The camera, begun here and NOT ended inside the frame: see the note in examples/camera_first
           _person.c. Each frame's begin replaces the live record.
        */
        (void)xud_begin_camera_3d(context, camera);

        /* The ray: the pixel under the pointer folded through the swapchain's extent and the live
           camera's view-projection, inverted - xud_get_screen_to_world_ray() refuses a frame with no
           camera live rather than answering with a ray built from the identity, which is why the begin
           above is not decoration.
        */
        result = xud_get_mouse_position(&pixel, context);
        if (result == XUD_OK) {
            result = xud_get_screen_to_world_ray(&ray, context, pixel, &camera);
        }
        if (result != XUD_OK) {
            printf("models_mesh_picking: frame %u the ray -> %s\n", frame, xud_result_name(result));
        } else {
            /* The two casts over the same solid, which is the example: the box is the cheap test the
               broad phase of a picking pass runs first, and the mesh is what the pointer is actually
               on. Both are given the same placement matrix, because a cast against a solid at the origin
               while the draw puts it 4 units up is the bug raylib's own note warns about.
            */
            (void)xud_get_ray_collision_box(&boxHit, ray, towerBox);
            (void)xud_get_ray_collision_mesh(&meshHit, ray, &towerData, towerMatrix);
            (void)xud_get_ray_collision_quad(&floorHit, ray, floorCorners[0], floorCorners[1], floorCorners[2], floorCorners[3]);
            (void)xud_get_ray_collision_triangle(&triangleHit, ray, xudMeshPickTriangle[0], xudMeshPickTriangle[1], xudMeshPickTriangle[2]);
            (void)xud_get_ray_collision_sphere(&sphereHit, ray, xudMeshPickSphereCenter, XUD_MESH_PICK_SPHERE_RADIUS);

            if ((againstMesh != false) && (meshHit.hit != false)) {
                hitName = "the tower's mesh";
                hitsMesh++;
            } else if (boxHit.hit != false) {
                hitName = "the tower's box";
                hitsBox++;
            } else if (triangleHit.hit != false) {
                hitName = "the triangle";
            } else if (sphereHit.hit != false) {
                hitName = "the sphere";
            } else if (floorHit.hit != false) {
                hitName = "the floor";
            }
        }

        /* The scene, raylib's example's (its file, lines 98 to 127): the solid, the box around it, the
           two test solids, the ray, the hit and the floor's grid. The box is drawn as its twelve edges
           so the difference the example is about is visible - the box is bigger than the cylinder it
           wraps, and a click in the gap between the two is a hit on one cast and a miss on the other.
        */
        result = xud_draw_mesh_ex(context, tower, towerMatrix, xudMeshPickWhite);
        if (result == XUD_OK) {
            result = xud_draw_bounding_box(context, towerBox, (againstMesh != false) ? xudMeshPickOrange : xudMeshPickRed);
        }
        if (result == XUD_OK) {
            result = xud_draw_line_3d(context, xudMeshPickTriangle[0], xudMeshPickTriangle[1], xudMeshPickPurple);
        }
        if (result == XUD_OK) {
            result = xud_draw_line_3d(context, xudMeshPickTriangle[1], xudMeshPickTriangle[2], xudMeshPickPurple);
        }
        if (result == XUD_OK) {
            result = xud_draw_line_3d(context, xudMeshPickTriangle[2], xudMeshPickTriangle[0], xudMeshPickPurple);
        }
        if (result == XUD_OK) {
            result = xud_draw_sphere_wires(context, xudMeshPickSphereCenter, XUD_MESH_PICK_SPHERE_RADIUS, xudMeshPickGreen);
        }
        if (result == XUD_OK) {
            result = xud_draw_ray(context, ray, xudMeshPickBlue);
        }
        if ((result == XUD_OK) && (meshHit.hit != false)) {
            result = xud_draw_cube(context, meshHit.point, 1.0f, 1.0f, 1.0f, xudMeshPickRed);
        }
        if ((result == XUD_OK) && (meshHit.hit != false)) {
            result = xud_draw_line_3d(context, meshHit.point,
                (XudVector3) { .x = meshHit.point.x + meshHit.normal.x,
                    .y = meshHit.point.y + meshHit.normal.y,
                    .z = meshHit.point.z + meshHit.normal.z },
                xudMeshPickRed);
        }
        if ((result == XUD_OK) && (boxHit.hit != false)) {
            result = xud_draw_cube(context, boxHit.point, 1.0f, 1.0f, 1.0f, xudMeshPickOrange);
        }
        if (result == XUD_OK) {
            result = xud_draw_grid(
                context, (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 0.0f }, 10u, 10.0f, xudMeshPickLightGray, xudMeshPickGray);
        }
        if (result != XUD_OK) {
            if (shortFrames == 0u) {
                printf("models_mesh_picking: frame %u a draw refused -> %s; the scene is whole from the frame whose capacity fits it\n",
                    frame, xud_result_name(result));
            }
            shortFrames++;
        }

        /* The HUD: the mode, the answer of the cast that was asked for, and the answer of the other one
           beside it, which is the example's point in three lines. raylib's example draws the same words
           at the same pixels without a box behind them (its file, lines 129 to 139); the strip here is
           the opaque row, so the words are readable over the sky and over the tower.
        */
        result = xud_draw_rectangle(context, 10, 10, 360, 84, xudMeshPickGray);
        if (result == XUD_OK) {
            result = xud_mesh_pick_text(context, font, 20, 16, "%s", xudMeshPickBlack,
                (againstMesh != false) ? "Hit on mesh: RIGHT switches to bounding box" : "Hit on box: RIGHT switches to mesh");
        }
        if (result == XUD_OK) {
            result = xud_mesh_pick_text(context, font, 20, 32, "%s", xudMeshPickBlack, hitName);
        }
        if (result == XUD_OK) {
            result = xud_mesh_pick_text(context, font, 20, 48, "mesh: %s d %.2f  box: %s d %.2f", xudMeshPickDarkGray,
                (meshHit.hit != false) ? "hit" : "----", (double)meshHit.distance, (boxHit.hit != false) ? "hit" : "----",
                (double)boxHit.distance);
        }
        if (result == XUD_OK) {
            result = xud_mesh_pick_text(context, font, 20, 64, "point %.2f %.2f %.2f", xudMeshPickDarkGray,
                (double)((againstMesh != false) ? meshHit : boxHit).point.x, (double)((againstMesh != false) ? meshHit : boxHit).point.y,
                (double)((againstMesh != false) ? meshHit : boxHit).point.z);
        }
        if (result == XUD_OK) {
            result = xud_mesh_pick_text(context, font, 20, 80, "normal %.2f %.2f %.2f", xudMeshPickDarkGray,
                (double)((againstMesh != false) ? meshHit : boxHit).normal.x, (double)((againstMesh != false) ? meshHit : boxHit).normal.y,
                (double)((againstMesh != false) ? meshHit : boxHit).normal.z);
        }
        if (result != XUD_OK) {
            if (shortFrames == 0u) {
                printf("models_mesh_picking: frame %u a HUD draw refused -> %s\n", frame, xud_result_name(result));
            }
            shortFrames++;
        }

        result = xud_end_drawing(context);
        if (result != XUD_OK) {
            printf("models_mesh_picking: frame %u xud_end_drawing -> %s, and the run stops here\n", frame, xud_result_name(result));
            break;
        }
        frame++;
    }

    /* The objects go in the reverse order they came, and the cursor goes back before the window does:
       a context that closed with the cursor hidden leaves the pointer in a mode the next program to
       own the window did not ask for.
    */
    (void)xud_end_camera_3d(context);
    if (cursorHidden != false) {
        (void)xud_show_cursor(context);
    }
    result = xud_close_mesh(context, tower);
    if (result != XUD_OK) {
        printf("models_mesh_picking: xud_close_mesh -> %s\n", xud_result_name(result));
    }
    free(vertices);
    free(texcoords);
    free(normals);
    free(indices);
    result = xud_close_font(context, font);
    if (result != XUD_OK) {
        printf("models_mesh_picking: xud_close_font -> %s\n", xud_result_name(result));
    }
    free(fontFile);
    printf("models_mesh_picking: %u frame(s), %u of them short, %u mesh hit(s), %u box hit(s)\n", frame, shortFrames, hitsMesh, hitsBox);
    result = xud_close_context(&context);
    printf("models_mesh_picking: xud_close_context -> %s\n", xud_result_name(result));

    return (result == XUD_OK) ? 0 : 1;
}
