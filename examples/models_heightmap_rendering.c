/***********************************************************************************************
 *
 *   models_heightmap_rendering.c - raylib's models_heightmap_rendering example on xudlibc
 *
 *   Owns : one context, one font, one texture, one material, one mesh and one orbital camera. The
 *          mesh is the generation rung's heightmap kind - XUD_MESH_KIND_HEIGHTMAP, this library's
 *          copy of raylib's GenMeshHeightmap (rmodels.c 1612) - cut from a grid of samples the
 *          example holds. Layer: examples. Public API only (xud.h).
 *
 *   Shape : raylib reads a 128x128 PNG and GenMeshHeightmap turns each texel's byte into a y. Here
 *          the same 128x128 grid is FILLED rather than read, from one stated surface below, because
 *          this library has no image loader and the repo's resources/ holds no image: the caller's
 *          samples are the input either way, so the analytic grid stands in for the file exactly as
 *          the file would, and the surface is printed as a formula rather than hidden in pixels. The
 *          same grid is also the diffuse texture, which is the inset raylib draws at the top right.
 *
 *   Grid  : xud_gen_mesh() spends the samples as WORLD units and lays row 0 at +Z: x runs from
 *          -width/2 at column 0, z falls from +length/2 as the rows rise. The sample loop below is
 *          therefore written against x and z and not against an image's row and column, so the inset
 *          in the corner is the land with north at its top edge and not a mirror of it.
 *
 *   Controls: none, as raylib's example has none - the camera orbits on its own. ESC quits.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include <math.h>  /* Required for: cosf, sinf, sqrtf, and the surface the samples come from */
#include <stdio.h> /* Required for: printf, and the file's own report */
#include <stdlib.h> /* Required for: malloc, free, the streams the mesh is cut into */

#include "xud.h" /* Required for: the shape description and the generation pair, xud_upload_mesh, the texture and material records, the mesh draw and the screen-fixed draw pair, the camera record and xud_update_camera, the grid and the context's own pair - the whole public API this example is written against */

#define XUD_LOG_NAME "models_heightmap_rendering"

#define XUD_HM_WIDTH 800u /* raylib's example's own extent (its file, lines 25 and 26) */
#define XUD_HM_HEIGHT 450u

#define XUD_HM_SAMPLES 128u /* raylib's heightmap.png is 128 texels on a side */
#define XUD_HM_TEXELS (XUD_HM_SAMPLES * XUD_HM_SAMPLES)

/* The extent GenMeshHeightmap is handed as its Vector3 size (raylib's example, line 41): 16 across,
   8 to the sky, 16 long. The samples go into the mesh in world units and the rung scales nothing, so
   the relief below is stated as the peak it must reach rather than as a byte that a rung would
   divide by 255.
*/
#define XUD_HM_RELIEF 8.0f

/* The mesh is cut centred on the origin - x and z from -extent/2 - and raylib's example then draws
   it at (-8, 0, -8) (its line 45), which parks the island's centre one quarter-width off the grid's.
   That placement is the example's, so it is kept here as a matrix rather than corrected.
*/
#define XUD_HM_MAP_X (-8.0f)
#define XUD_HM_MAP_Z (-8.0f)

#define XUD_HM_INSET_X ((int32_t)XUD_HM_WIDTH - (int32_t)XUD_HM_SAMPLES - 20) /* raylib's own corner, its line 74 */
#define XUD_HM_INSET_Y 20

#define XUD_HM_FONT_PATH "resources/fonts/NotoSansTC-Regular.ttf"
#define XUD_HM_LINE_MAX 96
#define XUD_HM_FONT_PIXELS 16u

/* The orbit raylib's CAMERA_ORBITAL moves at is the mode's own; this is the eye its example starts
   at (its file, lines 32 to 36), the target the origin, and the 45 degrees the same lines name.
*/
static const XudCamera3D xudHeightmapStartCamera = { .position = { .x = 18.0f, .y = 21.0f, .z = 18.0f },
    .target = { .x = 0.0f, .y = 0.0f, .z = 0.0f },
    .up = { .x = 0.0f, .y = 1.0f, .z = 0.0f },
    .fovy = 45.0f,
    .near = 0.1f,
    .far = 200.0f };

static const XudColor xudHeightmapRaywhite = { .r = 245u, .g = 245u, .b = 250u, .a = 255u };
static const XudColor xudHeightmapLightGray = { .r = 200u, .g = 200u, .b = 200u, .a = 255u };
static const XudColor xudHeightmapGray = { .r = 130u, .g = 130u, .b = 130u, .a = 255u };
static const XudColor xudHeightmapRed = { .r = 200u, .g = 0u, .b = 0u, .a = 255u };
static const XudColor xudHeightmapGreen = { .r = 0u, .g = 155u, .b = 80u, .a = 255u };
static const XudColor xudHeightmapDarkBlue = { .r = 0u, .g = 60u, .b = 120u, .a = 255u };
static const XudColor xudHeightmapWhite = { .r = 255u, .g = 255u, .b = 255u, .a = 255u };

/*-----------------------------------------------------------------------------------------------
 * The samples: the island the mesh is cut from, and the same island as a flat image
 *-----------------------------------------------------------------------------------------------*/

/* One sample of the surface, at a point of the grid stated in the mesh's own coordinates: x across
   from -1 to 1, z from -1 to 1, both already the column and row of the grid they will be spent as.
   The island is a cosine dome - 1 at the centre, 0 at the rim, and flat zero outside it, so the land
   meets the water without a step - and the ripple is a product of two sines at incommensurate rates,
   which is what keeps the crest from reading as a grid of bumps. The square on the dome is what makes
   the shore fall away faster than a cosine alone would. Deterministic by construction: no sample here
   depends on anything but its column and row.
*/
static float xud_heightmap_sample(uint32_t column, uint32_t row) {
    const float u = (float)column / (float)(XUD_HM_SAMPLES - 1u);
    const float v = (float)row / (float)(XUD_HM_SAMPLES - 1u);
    const float x = (u * 2.0f) - 1.0f;
    const float z = (v * 2.0f) - 1.0f;
    const float reach = sqrtf((x * x) + (z * z));
    const float dome = (reach < 1.0f) ? cosf(reach * 1.5707963267948966f) : 0.0f;
    const float ripple = 0.5f + 0.5f * sinf(7.0f * x) * cosf(6.0f * z);

    return XUD_HM_RELIEF * (dome * dome) * (0.35f + (0.65f * ripple));
}

/* The same grid as the inset's image: one byte per sample, the relief read as a height. This is the
   texture the mesh is drawn under as well, which is what raylib's example does with the PNG it loads
   (its line 44): the shading of the land and its picture are the same data.
*/
static void xud_heightmap_image(const float* heights, uint8_t* outTexels) {
    uint32_t texel = 0u;

    for (texel = 0u; texel < XUD_HM_TEXELS; texel++) {
        float lit = heights[texel] / XUD_HM_RELIEF;

        if (lit < 0.0f) {
            lit = 0.0f;
        }
        if (lit > 1.0f) {
            lit = 1.0f;
        }
        outTexels[texel * 4u] = (uint8_t)(lit * 255.0f);
        outTexels[(texel * 4u) + 1u] = (uint8_t)(lit * 255.0f);
        outTexels[(texel * 4u) + 2u] = (uint8_t)(lit * 255.0f);
        outTexels[(texel * 4u) + 3u] = 255u;
    }
}

/*-----------------------------------------------------------------------------------------------
 * The two calls the HUD needs: a file reader, and a text call that formats
 *-----------------------------------------------------------------------------------------------*/

/* The font file's bytes: xud_load_font() takes bytes rather than a path, so the caller reads the
   file. NULL when there is no file.
*/
static uint8_t* xud_heightmap_read(const char* path, uint32_t* outBytes) {
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
   bytes and the bytes become codepoints, because a font is keyed by codepoint and not by byte.
*/
static XudResult xud_heightmap_text(XudContext* context, XudHandle font, int32_t posX, int32_t posY, const char* format, XudColor color,
    unsigned int first, unsigned int second) {
    char line[XUD_HM_LINE_MAX];
    uint32_t codepoints[XUD_HM_LINE_MAX];
    uint32_t step = 0u;
    int written = 0;

    written = snprintf(line, sizeof(line), format, first, second);
    if ((written < 0) || ((size_t)written >= sizeof(line))) {
        return XUD_INVALID_ARG;
    }
    for (step = 0u; step < (uint32_t)written; step++) {
        codepoints[step] = (uint32_t)(uint8_t)line[step];
    }

    return xud_draw_text_screen(context, font, codepoints, (uint32_t)written, posX, posY, (float)XUD_HM_FONT_PIXELS, 0.0f, color);
}

/*-----------------------------------------------------------------------------------------------
 * The run
 *-----------------------------------------------------------------------------------------------*/

int main(void) {
    XudMeshShape shape = { .kind = XUD_MESH_KIND_HEIGHTMAP,
        .width = 16.0f,
        .length = 16.0f,
        .heights = NULL,
        .heightColumns = XUD_HM_SAMPLES,
        .heightRows = XUD_HM_SAMPLES };
    XudCamera3D camera = xudHeightmapStartCamera;
    XudFontData fontData = { .fileData = NULL, .fileBytes = 0u, .pixelSize = XUD_HM_FONT_PIXELS, .codepoints = NULL, .codepointCount = 0u };
    XudTextureData textureData = { .width = XUD_HM_SAMPLES,
        .height = XUD_HM_SAMPLES,
        .rgba = NULL,
        .addressMode = XUD_TEXTURE_ADDRESS_CLAMP_TO_EDGE,
        .filter = XUD_TEXTURE_FILTER_LINEAR };
    XudMaterial material = { .blendMode = XUD_BLEND_OPAQUE, .diffuseMap = XUD_HANDLE_NONE };
    float* heights = NULL;
    float* positions = NULL;
    float* texcoords = NULL;
    float* normals = NULL;
    uint32_t* indices = NULL;
    uint8_t* image = NULL;
    uint8_t* fontFile = NULL;
    uint32_t fontBytes = 0u;
    uint32_t frame = 0u;
    uint32_t shortFrames = 0u;
    uint32_t sample = 0u;
    float lowest = 0.0f;
    float highest = 0.0f;
    XudMeshCounts counts = { 0u, 0u };
    XudContext* context = NULL;
    XudHandle font;
    XudHandle texture;
    XudHandle materialHandle;
    XudHandle mesh;
    float placement[16];
    XudResult result;

    fontFile = xud_heightmap_read(XUD_HM_FONT_PATH, &fontBytes);
    if (fontFile == NULL) {
        printf("%s: no font at %s, so the HUD has nothing to draw with\n", XUD_LOG_NAME, XUD_HM_FONT_PATH);
        return 1;
    }

    result = xud_init_context(&context, XUD_HM_WIDTH, XUD_HM_HEIGHT, XUD_LOG_NAME);
    printf("%s: xud_init_context(%u, %u, \"%s\") -> %s (xudlibc %s)\n", XUD_LOG_NAME, XUD_HM_WIDTH, XUD_HM_HEIGHT, XUD_LOG_NAME,
        xud_result_name(result), XUD_VERSION);
    if (result != XUD_OK) {
        free(fontFile);
        return (result == XUD_UNSUPPORTED) ? 0 : 1;
    }

    fontData.fileData = fontFile;
    fontData.fileBytes = fontBytes;
    result = xud_load_font(&font, context, &fontData);
    if (result != XUD_OK) {
        printf("%s: xud_load_font -> %s\n", XUD_LOG_NAME, xud_result_name(result));
        (void)xud_close_context(&context);
        free(fontFile);
        return 1;
    }

    /* The samples, then the two numbers the rung answers for them, then the four streams sized from
       those two numbers: the fill writes exactly what the count said, which is what makes the block
       below provably big enough.
    */
    heights = malloc(XUD_HM_TEXELS * sizeof(float));
    image = malloc(XUD_HM_TEXELS * 4u);
    if ((heights == NULL) || (image == NULL)) {
        printf("%s: out of memory for the %u x %u samples\n", XUD_LOG_NAME, (unsigned int)XUD_HM_SAMPLES, (unsigned int)XUD_HM_SAMPLES);
        free(heights);
        free(image);
        (void)xud_close_font(context, font);
        (void)xud_close_context(&context);
        free(fontFile);
        return 1;
    }
    for (sample = 0u; sample < XUD_HM_TEXELS; sample++) {
        heights[sample] = xud_heightmap_sample(sample % XUD_HM_SAMPLES, sample / XUD_HM_SAMPLES);
        if (sample == 0u) {
            lowest = heights[sample];
            highest = heights[sample];
        }
        if (heights[sample] < lowest) {
            lowest = heights[sample];
        }
        if (heights[sample] > highest) {
            highest = heights[sample];
        }
    }
    shape.heights = heights;

    result = xud_gen_mesh_count(&counts, &shape);
    if (result != XUD_OK) {
        printf("%s: xud_gen_mesh_count -> %s\n", XUD_LOG_NAME, xud_result_name(result));
        free(heights);
        free(image);
        (void)xud_close_font(context, font);
        (void)xud_close_context(&context);
        free(fontFile);
        return 1;
    }
    positions = malloc((size_t)counts.vertexCount * 3u * sizeof(float));
    texcoords = malloc((size_t)counts.vertexCount * 2u * sizeof(float));
    normals = malloc((size_t)counts.vertexCount * 3u * sizeof(float));
    indices = malloc((size_t)counts.indexCount * sizeof(uint32_t));
    if ((positions == NULL) || (texcoords == NULL) || (normals == NULL) || (indices == NULL)) {
        printf("%s: out of memory for %u vertices and %u indices\n", XUD_LOG_NAME, (unsigned int)counts.vertexCount,
            (unsigned int)counts.indexCount);
        result = XUD_OUT_OF_MEMORY;
    } else {
        XudMeshData meshData = { 0 };
        XudMeshBuffers buffers = { .pos = positions, .texcoords = texcoords, .normal = normals, .index = indices };

        result = xud_gen_mesh(&meshData, &shape, &buffers);
        if (result == XUD_OK) {
            result = xud_upload_mesh(&mesh, context, &meshData);
        }
        printf("%s: %u x %u samples, %.03f to %.03f world units -> %s, %u vertices, %u indices\n", XUD_LOG_NAME,
            (unsigned int)XUD_HM_SAMPLES, (unsigned int)XUD_HM_SAMPLES, lowest, highest, xud_result_name(result),
            (unsigned int)counts.vertexCount, (unsigned int)counts.indexCount);
    }
    free(positions);
    free(texcoords);
    free(normals);
    free(indices);
    if (result != XUD_OK) {
        free(heights);
        free(image);
        (void)xud_close_font(context, font);
        (void)xud_close_context(&context);
        free(fontFile);
        return 1;
    }

    /* The island as an image, uploaded once: the texture the mesh is drawn under and the inset in the
       corner are the same handle, as they are the same file in the example above.
    */
    xud_heightmap_image(heights, image);
    textureData.rgba = image;
    result = xud_create_texture(&texture, context, &textureData);
    if (result == XUD_OK) {
        material.diffuseMap = texture;
        result = xud_create_material(&materialHandle, context, material);
    }
    printf("%s: texture and material -> %s\n", XUD_LOG_NAME, xud_result_name(result));
    if (result != XUD_OK) {
        (void)xud_close_mesh(context, mesh);
        free(heights);
        free(image);
        (void)xud_close_font(context, font);
        (void)xud_close_context(&context);
        free(fontFile);
        return 1;
    }

    /* The mesh is cut centred on the origin, so the whole placement is the example's own offset. */
    result = xud_matrix_translate(placement, (XudVector3) { .x = XUD_HM_MAP_X, .y = 0.0f, .z = XUD_HM_MAP_Z });
    if (result != XUD_OK) {
        printf("%s: xud_matrix_translate -> %s\n", XUD_LOG_NAME, xud_result_name(result));
    }
    if (xud_set_target_fps(context, 60u) != XUD_OK) {
        printf("%s: xud_set_target_fps -> refused, so the run free-runs\n", XUD_LOG_NAME);
    }

    for (;;) {
        bool shouldClose = false;
        bool pressed = false;

        result = xud_window_should_close(&shouldClose, context);
        if (result != XUD_OK) {
            printf("%s: frame %u xud_window_should_close -> %s\n", XUD_LOG_NAME, frame, xud_result_name(result));
            break;
        }
        if (shouldClose) {
            printf("%s: frame %u: the window was closed\n", XUD_LOG_NAME, frame);
            break;
        }
        if ((xud_is_key_pressed(&pressed, context, XUD_KEY_ESCAPE) == XUD_OK) && pressed) {
            printf("%s: frame %u: ESC\n", XUD_LOG_NAME, frame);
            break;
        }

        /* The orbital camera, the mode's own turn rate, exactly as raylib's example drives it. */
        result = xud_update_camera(context, &camera, XUD_CAMERA_MODE_ORBITAL);
        if (result != XUD_OK) {
            printf("%s: frame %u xud_update_camera(ORBITAL) -> %s\n", XUD_LOG_NAME, frame, xud_result_name(result));
            break;
        }

        result = xud_begin_drawing(context);
        if (result != XUD_OK) {
            printf("%s: frame %u xud_begin_drawing -> %s, and the run stops here\n", XUD_LOG_NAME, frame, xud_result_name(result));
            break;
        }
        (void)xud_clear_background(context, xudHeightmapRaywhite);

        /* The camera is begun here and NOT ended inside the frame; see examples/camera_first_person.c.
           The material is begun around the land alone and ended after it, because the live material is
           the row every later draw of the frame records into, and the grid would otherwise sample the
           island through it.
        */
        (void)xud_begin_camera_3d(context, camera);
        (void)xud_begin_material(context, materialHandle);
        result = xud_draw_mesh_ex(context, mesh, placement, xudHeightmapRed);
        (void)xud_end_material(context);
        if (result == XUD_OK) {
            result = xud_draw_grid(
                context, (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 0.0f }, 20u, 1.0f, xudHeightmapLightGray, xudHeightmapGray);
        }
        if (result != XUD_OK) {
            /* The batch's capacity doubles at the start of a frame, so a scene is drawn short in its
               first frames until the capacity reaches what it asks for.
            */
            if (shortFrames == 0u) {
                printf("%s: frame %u a draw refused -> %s; the scene is whole from the frame whose capacity fits it\n", XUD_LOG_NAME, frame,
                    xud_result_name(result));
            }
            shortFrames++;
        }

        /* The screen-fixed half: the island's own image in the corner with raylib's green frame (its
           file, lines 74 and 75), and the two lines that say what the mesh was cut from - the one thing
           a picture of the land cannot show.
        */
        result = xud_draw_texture(context, texture, XUD_HM_INSET_X, XUD_HM_INSET_Y, xudHeightmapWhite);
        if (result == XUD_OK) {
            result = xud_draw_rectangle_lines(
                context, XUD_HM_INSET_X, XUD_HM_INSET_Y, (int32_t)XUD_HM_SAMPLES, (int32_t)XUD_HM_SAMPLES, xudHeightmapGreen);
        }
        if (result == XUD_OK) {
            result = xud_heightmap_text(context, font, 10, 10, "HEIGHTMAP %u x %u SAMPLES", xudHeightmapDarkBlue,
                (unsigned int)XUD_HM_SAMPLES, (unsigned int)XUD_HM_SAMPLES);
        }
        if (result == XUD_OK) {
            result = xud_heightmap_text(context, font, 10, 32, "%u vertices, %u triangles", xudHeightmapDarkBlue,
                (unsigned int)counts.vertexCount, (unsigned int)(counts.indexCount / 3u));
        }
        if (result != XUD_OK) {
            if (shortFrames == 0u) {
                printf("%s: frame %u a HUD draw refused -> %s\n", XUD_LOG_NAME, frame, xud_result_name(result));
            }
            shortFrames++;
        }
        result = xud_end_drawing(context);
        if (result != XUD_OK) {
            printf("%s: frame %u xud_end_drawing -> %s, and the run stops here\n", XUD_LOG_NAME, frame, xud_result_name(result));
            break;
        }
        frame++;
    }

    /* The objects go in the reverse order they came: the camera, then the mesh, then the material that
       named the texture, then the texture, then the font, then the context.
    */
    (void)xud_end_camera_3d(context);
    result = xud_close_mesh(context, mesh);
    if (result != XUD_OK) {
        printf("%s: xud_close_mesh -> %s\n", XUD_LOG_NAME, xud_result_name(result));
    }
    result = xud_close_material(context, materialHandle);
    if (result != XUD_OK) {
        printf("%s: xud_close_material -> %s\n", XUD_LOG_NAME, xud_result_name(result));
    }
    result = xud_close_texture(context, texture);
    if (result != XUD_OK) {
        printf("%s: xud_close_texture -> %s\n", XUD_LOG_NAME, xud_result_name(result));
    }
    result = xud_close_font(context, font);
    if (result != XUD_OK) {
        printf("%s: xud_close_font -> %s\n", XUD_LOG_NAME, xud_result_name(result));
    }
    free(heights);
    free(image);
    free(fontFile);
    printf("%s: %u frame(s), %u of them short\n", XUD_LOG_NAME, frame, shortFrames);
    result = xud_close_context(&context);
    printf("%s: xud_close_context -> %s\n", XUD_LOG_NAME, xud_result_name(result));

    return (result == XUD_OK) ? 0 : 1;
}
