/***********************************************************************************************
 *
 *   models_mesh_generation.c - raylib's models_mesh_generation example on xudlibc
 *
 *   Owns : one context, one font, one checked texture, one material, nine meshes and one orbital
 *          camera. Eight of the nine are cut by this library's generation rung - the eight kinds of
 *          XudMeshShape, this library's copy of raylib's GenMeshPlane, GenMeshCube, GenMeshSphere,
 *          GenMeshHemiSphere, GenMeshCylinder, GenMeshTorus, GenMeshKnot and GenMeshPoly (raylib.h
 *          1646 to 1683) as one call over a described shape - and the ninth is cut by hand below,
 *          the same single triangle raylib's example writes by hand as GenMeshCustom (its file,
 *          line 145) so the raw path, arrays the caller filled and xud_upload_mesh() over them, is
 *          on show beside the rung that fills them. Layer: examples. Public API only (xud.h).
 *
 *   Shape : raylib's example calls eight GenMesh* functions and assigns eight Models. This library's
 *          rung is one function over a described shape, so the nine solids here are nine records in
 *          the table below and one loop over it - the table is the part of the example that states
 *          what is drawn, and it is where the parameters raylib passes as four positional arguments
 *          get names.
 *
 *   Missing : nothing. The pair this example is built on - xud_gen_mesh_count() and xud_gen_mesh() -
 *          came in with it, and the count half exists because this library allocates nothing on the
 *          caller's behalf: raylib's GenMesh* hands back arrays only UnloadMesh() may free, so it
 *          never has to ask how big the solid is before it cuts it. Here the counts are asked for,
 *          the arrays are sized from them, and the same validation runs in both halves - which is
 *          what makes the fill's writes provably inside the block malloc was given.
 *
 *   Texture : the checked image raylib generates at 2x2 texels (its file, line 29) is eight texels
 *          here, four checks across, because the generators' texture coordinates run 0 to 1 across
 *          each face and a 2x2 image with REPEAT is one check per face rather than a checker. Its
 *          filter is NEAREST: a two-colour checker minified through a linear filter is a grey blur,
 *          which is what raylib's default sampler does to it.
 *
 *   Controls: the left mouse button, or the left and right arrow keys, move to the next solid - the
 *          same three controls raylib's example answers (its file, lines 66 to 79). The camera
 *          orbits on its own, as CAMERA_ORBITAL does there. ESC quits.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include <stdarg.h> /* Required for: va_start, va_end, the TextFormat half of the HUD below */
#include <stdio.h>  /* Required for: printf, snprintf, and the file's own report */
#include <stdlib.h> /* Required for: malloc, free, the arrays the solids are cut into */

#include "xud.h" /* Required for: the shape description and the two generation calls, xud_upload_mesh and the mesh draw pair, the material and texture records, the camera record and xud_update_camera, the grid, the screen-fixed text and quad pair, the input queries and the context's own pair - the whole public API this example is written against */

#define XUD_MESH_GEN_WIDTH 800u /* raylib's example's own extent (its file, lines 18 and 19) */
#define XUD_MESH_GEN_HEIGHT 450u

#define XUD_MESH_GEN_FONT_PATH "resources/fonts/NotoSansTC-Regular.ttf"
#define XUD_MESH_GEN_FONT_PIXELS 16u
#define XUD_MESH_GEN_LINE_MAX 96

/* The nine solids: the rung's eight kinds, and the hand-written triangle. One name each, because the
   HUD prints the one being drawn, which is raylib's example's switch at its file, line 96.
*/
#define XUD_MESH_GEN_COUNT 9u

/* The largest solid the table below cuts, so the block the nine are cut into is sized once: the knot
   at 128 steps along its curve and 16 around its tube is the widest, at 2048 vertices and 12288
   indices. A bigger shape in the table is not a mistake the code hides - xud_gen_mesh() writes
   exactly the counts xud_gen_mesh_count() answered, so an undersized block is a buffer the example
   sized wrong, and the check below says so before the fill runs.
*/
#define XUD_MESH_GEN_VERTICES 2048u
#define XUD_MESH_GEN_INDICES 12288u

/* The checked texture: four checks across, each one texel, in the two colours raylib's example puts
   in its GenImageChecked (its file, line 29).
*/
#define XUD_MESH_GEN_CHECK_TEXELS 8u

/* The orbit raylib's CAMERA_ORBITAL moves at is the mode's own; this is the eye it starts at (its
   file, line 48), the target the origin, and the 45 degrees the same line names.
*/
static const XudCamera3D xudMeshGenStartCamera = { .position = { .x = 5.0f, .y = 5.0f, .z = 5.0f },
    .target = { .x = 0.0f, .y = 0.0f, .z = 0.0f },
    .up = { .x = 0.0f, .y = 1.0f, .z = 0.0f },
    .fovy = 45.0f,
    .near = 0.1f,
    .far = 200.0f };

static const XudColor xudMeshGenRaywhite = { .r = 245u, .g = 245u, .b = 250u, .a = 255u };
static const XudColor xudMeshGenLightGray = { .r = 200u, .g = 200u, .b = 200u, .a = 255u };
static const XudColor xudMeshGenGray = { .r = 130u, .g = 130u, .b = 130u, .a = 255u };
static const XudColor xudMeshGenSky = { .r = 130u, .g = 170u, .b = 205u, .a = 255u };
static const XudColor xudMeshGenBlue = { .r = 0u, .g = 120u, .b = 240u, .a = 255u };
static const XudColor xudMeshGenDarkBlue = { .r = 0u, .g = 60u, .b = 120u, .a = 255u };
static const XudColor xudMeshGenRed = { .r = 220u, .g = 40u, .b = 40u, .a = 255u };
static const XudColor xudMeshGenGreen = { .r = 0u, .g = 180u, .b = 60u, .a = 255u };
static const XudColor xudMeshGenWhite = { .r = 255u, .g = 255u, .b = 255u, .a = 255u };

/* One row of the table: what to cut, and what to call it on the HUD. The description is the shape the
   rung reads; the eight fields it has no use for are filled anyway, because a description is one
   record for every kind and a kind ignores the numbers it has no use for.
*/
typedef struct XudMeshGenEntry {
    const char* name;
    XudMeshShape shape;
} XudMeshGenEntry;

static uint8_t* xud_mesh_gen_read(const char* path, uint32_t* outBytes);
static XudResult xud_mesh_gen_text(
    XudContext* context, XudHandle font, int32_t posX, int32_t posY, const char* format, XudColor color, ...);
static XudResult xud_mesh_gen_hud(XudContext* context, XudHandle font, uint32_t pick, const XudMeshCounts* counts);

/*-----------------------------------------------------------------------------------------------
 * The nine solids
 *-----------------------------------------------------------------------------------------------*/

/* The table, in the order raylib's example fills its nine Models (its file, lines 32 to 40), with
   the same solids: a 2x2 plane cut 4 by 3, a box of 2 x 1 x 2, a sphere of radius 2 at 32 by 32, a
   dome of the same radius at 16 by 16, a cylinder of radius 1 and height 2 at 16 slices, a torus of
   ring 4 and tube 0.25, a knot, and a pentagon of radius 2. The knot is the one solid whose numbers
   are not raylib's: its curve turns three times around, so the steps ALONG it are the sampling that
   matters, and 128 of them against 16 around the tube is the ratio that reads as a tube. raylib's
   GenMeshKnot takes a size that scales the whole solid after the fact and a radius that is the tube
   of par_shapes' trefoil before it; this rung has no post-scale, so radius is the curve's reach and
   tubeRadius the tube's own thickness, both in world units.
*/
static const XudMeshGenEntry xudMeshGenTable[XUD_MESH_GEN_COUNT - 1u] = { { "PLANE",
                                                                              { .kind = XUD_MESH_KIND_PLANE,
                                                                                  .radius = 1.0f,
                                                                                  .tubeRadius = 0.25f,
                                                                                  .width = 2.0f,
                                                                                  .height = 1.0f,
                                                                                  .length = 2.0f,
                                                                                  .slices = 4u,
                                                                                  .stacks = 3u } },
    { "CUBE",
        { .kind = XUD_MESH_KIND_CUBE,
            .radius = 1.0f,
            .tubeRadius = 0.25f,
            .width = 2.0f,
            .height = 1.0f,
            .length = 2.0f,
            .slices = 4u,
            .stacks = 3u } },
    { "SPHERE",
        { .kind = XUD_MESH_KIND_SPHERE,
            .radius = 2.0f,
            .tubeRadius = 0.25f,
            .width = 2.0f,
            .height = 1.0f,
            .length = 2.0f,
            .slices = 32u,
            .stacks = 32u } },
    { "HEMISPHERE",
        { .kind = XUD_MESH_KIND_HEMISPHERE,
            .radius = 2.0f,
            .tubeRadius = 0.25f,
            .width = 2.0f,
            .height = 1.0f,
            .length = 2.0f,
            .slices = 16u,
            .stacks = 16u } },
    { "CYLINDER",
        { .kind = XUD_MESH_KIND_CYLINDER,
            .radius = 1.0f,
            .tubeRadius = 0.25f,
            .width = 2.0f,
            .height = 2.0f,
            .length = 2.0f,
            .slices = 16u,
            .stacks = 3u } },
    { "TORUS",
        { .kind = XUD_MESH_KIND_TORUS,
            .radius = 4.0f,
            .tubeRadius = 0.25f,
            .width = 2.0f,
            .height = 1.0f,
            .length = 2.0f,
            .slices = 32u,
            .stacks = 16u } },
    { "KNOT",
        { .kind = XUD_MESH_KIND_KNOT,
            .radius = 2.0f,
            .tubeRadius = 0.5f,
            .width = 2.0f,
            .height = 1.0f,
            .length = 2.0f,
            .slices = 128u,
            .stacks = 16u } },
    { "POLY",
        { .kind = XUD_MESH_KIND_POLY,
            .radius = 2.0f,
            .tubeRadius = 0.25f,
            .width = 2.0f,
            .height = 1.0f,
            .length = 2.0f,
            .slices = 5u,
            .stacks = 3u } } };

/*-----------------------------------------------------------------------------------------------
 * The machinery the run needs: a file reader, a text call that formats, and the checked texture
 *-----------------------------------------------------------------------------------------------*/

/* The font file's bytes: xud_load_font() takes bytes rather than a path, so the public header
   cannot read the file for the caller and the caller reads it here. NULL when there is no file.
*/
static uint8_t* xud_mesh_gen_read(const char* path, uint32_t* outBytes) {
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
   bytes, the bytes become codepoints - the public text pair takes a uint32_t array because a font is
   keyed by codepoint and not by byte - and the row is drawn screen-fixed at the caller's pixel.
*/
static XudResult xud_mesh_gen_text(
    XudContext* context, XudHandle font, int32_t posX, int32_t posY, const char* format, XudColor color, ...) {
    char line[XUD_MESH_GEN_LINE_MAX];
    uint32_t codepoints[XUD_MESH_GEN_LINE_MAX];
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

    return xud_draw_text_screen(context, font, codepoints, (uint32_t)written, posX, posY, (float)XUD_MESH_GEN_FONT_PIXELS, 0.0f, color);
}

/* The checked image, in bytes: four checks across, each one texel, in the two colours raylib's
   example gives its GenImageChecked. The texture data is one caller array the create call reads once,
   so the image lives on the stack of the caller that uploads it.
*/
static void xud_mesh_gen_checker(uint8_t* outTexels) {
    uint32_t row = 0u;
    uint32_t column = 0u;

    for (row = 0u; row < XUD_MESH_GEN_CHECK_TEXELS; row++) {
        for (column = 0u; column < XUD_MESH_GEN_CHECK_TEXELS; column++) {
            const uint32_t byte = (row * XUD_MESH_GEN_CHECK_TEXELS) + column;
            const uint32_t check = (((row / 2u) + (column / 2u)) % 2u) == 0u ? 1u : 0u;

            outTexels[byte * 4u] = (check != 0u) ? xudMeshGenRed.r : xudMeshGenGreen.r;
            outTexels[(byte * 4u) + 1u] = (check != 0u) ? xudMeshGenRed.g : xudMeshGenGreen.g;
            outTexels[(byte * 4u) + 2u] = (check != 0u) ? xudMeshGenRed.b : xudMeshGenGreen.b;
            outTexels[(byte * 4u) + 3u] = 255u;
        }
    }
}

/*-----------------------------------------------------------------------------------------------
 * The ninth solid, cut by hand, and the HUD
 *-----------------------------------------------------------------------------------------------*/

/* raylib's GenMeshCustom (its file, line 145): three vertices and the one triangle between them,
   laid on the floor at the origin with +Y normals, the same triangle the same example writes one
   float at a time. It is here because it is the only solid in the run that the generation rung did
   not cut - the raw path is arrays the caller filled and xud_upload_mesh() over them, and with the
   eight above it the two ways a mesh arrives are on show together. Its counts are the caller's own
   arithmetic, which is the difference between the two paths: nothing validated them.
*/
static const float xudMeshGenCustomPos[9] = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 2.0f, 2.0f, 0.0f, 0.0f };
static const float xudMeshGenCustomTexcoords[6] = { 0.0f, 0.0f, 0.5f, 1.0f, 1.0f, 0.0f };
static const float xudMeshGenCustomNormals[9] = { 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f };
static const uint32_t xudMeshGenCustomIndex[3] = { 0u, 2u, 1u };

/* The HUD: the controls strip raylib's example draws at (30, 400) with the solid's name at the top
   right (its file, lines 88 to 108), and one line the example above does not have - how many
   vertices and triangles the solid on screen is made of, which is the one thing the generation rung
   answers that a picture of its output cannot show. The boxes are opaque: the row these quads record
   into is the opaque one, so a faded colour here would be a full one with a misleading name.
*/
static XudResult xud_mesh_gen_hud(XudContext* context, XudHandle font, uint32_t pick, const XudMeshCounts* counts) {
    const char* name = "CUSTOM (triangle)";
    XudResult result;

    if (pick < (XUD_MESH_GEN_COUNT - 1u)) {
        name = xudMeshGenTable[pick].name;
    }

    result = xud_draw_rectangle(context, 30, 400, 310, 30, xudMeshGenSky);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_draw_rectangle_lines(context, 30, 400, 310, 30, xudMeshGenDarkBlue);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_mesh_gen_text(context, font, 40, 407, "MOUSE LEFT BUTTON to CYCLE PROCEDURAL MESHES", xudMeshGenBlue);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_mesh_gen_text(context, font, 520, 10, "%s", xudMeshGenDarkBlue, name);
    if (result != XUD_OK) {
        return result;
    }

    return xud_mesh_gen_text(context, font, 520, 32, "%u vertices, %u triangles", xudMeshGenDarkBlue,
        (unsigned int)counts[pick].vertexCount, (unsigned int)(counts[pick].indexCount / 3u));
}

/*-----------------------------------------------------------------------------------------------
 * The run
 *-----------------------------------------------------------------------------------------------*/

int main(void) {
    /* The one triangle the hand-written ninth solid is: the arrays above, described. */
    const XudMeshData customMesh = { .vertexCount = 3u,
        .indexCount = 3u,
        .pos = xudMeshGenCustomPos,
        .texcoords = xudMeshGenCustomTexcoords,
        .normal = xudMeshGenCustomNormals,
        .index = xudMeshGenCustomIndex };
    float place[16];
    float customMatrix[16];
    uint8_t checker[XUD_MESH_GEN_CHECK_TEXELS * XUD_MESH_GEN_CHECK_TEXELS * 4u];
    XudMeshCounts counts[XUD_MESH_GEN_COUNT] = { { 0u, 0u } };
    XudHandle meshes[XUD_MESH_GEN_COUNT];
    XudTextureData textureData = { .width = XUD_MESH_GEN_CHECK_TEXELS,
        .height = XUD_MESH_GEN_CHECK_TEXELS,
        .rgba = NULL,
        .addressMode = XUD_TEXTURE_ADDRESS_REPEAT,
        .filter = XUD_TEXTURE_FILTER_NEAREST };
    XudMaterial material = { .blendMode = XUD_BLEND_OPAQUE, .diffuseMap = XUD_HANDLE_NONE };
    XudCamera3D camera = xudMeshGenStartCamera;
    XudFontData fontData
        = { .fileData = NULL, .fileBytes = 0u, .pixelSize = XUD_MESH_GEN_FONT_PIXELS, .codepoints = NULL, .codepointCount = 0u };
    uint8_t* fontFile = NULL;
    uint32_t fontBytes = 0u;
    uint32_t step = 0u;
    uint32_t frame = 0u;
    uint32_t pick = 0u;
    uint32_t shortFrames = 0u;
    uint32_t uploaded = 0u;
    XudContext* context = NULL;
    XudHandle font;
    XudHandle texture;
    XudHandle materialHandle;
    XudResult result;

    for (step = 0u; step < XUD_MESH_GEN_COUNT; step++) {
        meshes[step] = XUD_HANDLE_NONE;
    }

    fontFile = xud_mesh_gen_read(XUD_MESH_GEN_FONT_PATH, &fontBytes);
    if (fontFile == NULL) {
        printf("models_mesh_generation: no font at %s, so the HUD has nothing to draw with\n", XUD_MESH_GEN_FONT_PATH);
        return 1;
    }

    result = xud_init_context(&context, XUD_MESH_GEN_WIDTH, XUD_MESH_GEN_HEIGHT, "models_mesh_generation");
    printf("models_mesh_generation: xud_init_context(%u, %u, \"%s\") -> %s (xudlibc %s)\n", XUD_MESH_GEN_WIDTH, XUD_MESH_GEN_HEIGHT,
        "models_mesh_generation", xud_result_name(result), XUD_VERSION);
    if (result != XUD_OK) {
        free(fontFile);
        return (result == XUD_UNSUPPORTED) ? 0 : 1;
    }

    fontData.fileData = fontFile;
    fontData.fileBytes = fontBytes;
    result = xud_load_font(&font, context, &fontData);
    if (result != XUD_OK) {
        printf("models_mesh_generation: xud_load_font -> %s\n", xud_result_name(result));
        (void)xud_close_context(&context);
        free(fontFile);
        return 1;
    }

    /* The checked texture, then the material that names it. The material's row is the pipeline the
       meshes draw under - the mesh list has a textured variant and an untextured one, and the row
       picks between them by whether the material below names a texture - so the pair has to be live
       for the draws, which is what xud_begin_material() is for. It is begun once, before the loop:
       the renderer owns the frame loop and re-applies the pair at every frame opening.
    */
    xud_mesh_gen_checker(checker);
    textureData.rgba = checker;
    result = xud_create_texture(&texture, context, &textureData);
    if (result != XUD_OK) {
        printf("models_mesh_generation: xud_create_texture -> %s\n", xud_result_name(result));
        (void)xud_close_font(context, font);
        (void)xud_close_context(&context);
        free(fontFile);
        return 1;
    }
    material.diffuseMap = texture;
    result = xud_create_material(&materialHandle, context, material);
    if (result != XUD_OK) {
        printf("models_mesh_generation: xud_create_material -> %s\n", xud_result_name(result));
        (void)xud_close_texture(context, texture);
        (void)xud_close_font(context, font);
        (void)xud_close_context(&context);
        free(fontFile);
        return 1;
    }

    /* The material is NOT begun here. It is begun inside the frame, around the one mesh draw, and
       ended after it: the live material is the row every draw of the frame records into, so a material
       that names a texture and stays live for the whole frame would have the GRID sampled through the
       checker - a line has no texture coordinates of its own, and the texel it lands on tints it. The
       pair's scope is therefore the solid and nothing else.
    */

    /* The nine solids, cut and uploaded before the loop begins: the rung's eight, then the triangle.
       The arrays are one block reused nine times, because xud_upload_mesh() copies what it is given
       into the arena the context owns and nothing here reads the CPU side after the upload.
    */
    for (step = 0u; step < (XUD_MESH_GEN_COUNT - 1u); step++) {
        XudMeshData meshData = { 0 };
        XudMeshBuffers buffers = { 0 };
        float* vertices = NULL;
        float* texcoords = NULL;
        float* normals = NULL;
        uint32_t* indices = NULL;

        result = xud_gen_mesh_count(&counts[step], &xudMeshGenTable[step].shape);
        if (result != XUD_OK) {
            printf("models_mesh_generation: %s xud_gen_mesh_count -> %s\n", xudMeshGenTable[step].name, xud_result_name(result));
            break;
        }
        if ((counts[step].vertexCount > XUD_MESH_GEN_VERTICES) || (counts[step].indexCount > XUD_MESH_GEN_INDICES)) {
            printf("models_mesh_generation: %s wants %u vertices and %u indices, the block holds %u and %u\n", xudMeshGenTable[step].name,
                (unsigned int)counts[step].vertexCount, (unsigned int)counts[step].indexCount, (unsigned int)XUD_MESH_GEN_VERTICES,
                (unsigned int)XUD_MESH_GEN_INDICES);
            result = XUD_INVALID_ARG;
            break;
        }
        vertices = malloc((size_t)counts[step].vertexCount * 3u * sizeof(float));
        texcoords = malloc((size_t)counts[step].vertexCount * 2u * sizeof(float));
        normals = malloc((size_t)counts[step].vertexCount * 3u * sizeof(float));
        indices = malloc((size_t)counts[step].indexCount * sizeof(uint32_t));
        if ((vertices == NULL) || (texcoords == NULL) || (normals == NULL) || (indices == NULL)) {
            printf("models_mesh_generation: %s out of memory for its arrays\n", xudMeshGenTable[step].name);
            free(vertices);
            free(texcoords);
            free(normals);
            free(indices);
            result = XUD_OUT_OF_MEMORY;
            break;
        }
        buffers.pos = vertices;
        buffers.texcoords = texcoords;
        buffers.normal = normals;
        buffers.index = indices;

        /* The fill, and the description it writes: counts and the four stream pointers, ready for the
           upload and for a cast, with nothing assembled by hand in between.
        */
        result = xud_gen_mesh(&meshData, &xudMeshGenTable[step].shape, &buffers);
        if (result == XUD_OK) {
            result = xud_upload_mesh(&meshes[step], context, &meshData);
            if (result == XUD_OK) {
                uploaded++;
            }
        }
        printf("models_mesh_generation: %s -> %s, %u vertices, %u indices\n", xudMeshGenTable[step].name, xud_result_name(result),
            (unsigned int)counts[step].vertexCount, (unsigned int)counts[step].indexCount);
        free(vertices);
        free(texcoords);
        free(normals);
        free(indices);
        if (result != XUD_OK) {
            break;
        }
    }
    counts[XUD_MESH_GEN_COUNT - 1u].vertexCount = customMesh.vertexCount;
    counts[XUD_MESH_GEN_COUNT - 1u].indexCount = customMesh.indexCount;
    if (result == XUD_OK) {
        result = xud_upload_mesh(&meshes[XUD_MESH_GEN_COUNT - 1u], context, &customMesh);
        if (result == XUD_OK) {
            uploaded++;
        }
        printf("models_mesh_generation: CUSTOM -> %s, %u vertices, %u indices\n", xud_result_name(result),
            (unsigned int)customMesh.vertexCount, (unsigned int)customMesh.indexCount);
    }
    if (result != XUD_OK) {
        for (step = 0u; step < XUD_MESH_GEN_COUNT; step++) {
            if (meshes[step].kind != XUD_HANDLE_KIND_NONE) {
                (void)xud_close_mesh(context, meshes[step]);
            }
        }
        (void)xud_close_material(context, materialHandle);
        (void)xud_close_texture(context, texture);
        (void)xud_close_font(context, font);
        (void)xud_close_context(&context);
        free(fontFile);
        return 1;
    }

    (void)xud_matrix_identity(customMatrix);
    if (xud_set_target_fps(context, 60u) != XUD_OK) {
        printf("models_mesh_generation: xud_set_target_fps -> refused, so the run free-runs\n");
    }

    for (;;) {
        bool shouldClose = false;
        bool pressed = false;

        result = xud_window_should_close(&shouldClose, context);
        if (result != XUD_OK) {
            printf("models_mesh_generation: frame %u xud_window_should_close -> %s\n", frame, xud_result_name(result));
            break;
        }
        if (shouldClose) {
            printf("models_mesh_generation: frame %u: the window was closed\n", frame);
            break;
        }
        if ((xud_is_key_pressed(&pressed, context, XUD_KEY_ESCAPE) == XUD_OK) && pressed) {
            printf("models_mesh_generation: frame %u: ESC\n", frame);
            break;
        }
        if ((xud_is_mouse_button_pressed(&pressed, context, XUD_MOUSE_BUTTON_LEFT) == XUD_OK) && pressed) {
            pick = (pick + 1u) % XUD_MESH_GEN_COUNT;
        }
        if ((xud_is_key_pressed(&pressed, context, XUD_KEY_RIGHT) == XUD_OK) && pressed) {
            pick = (pick + 1u) % XUD_MESH_GEN_COUNT;
        }
        if ((xud_is_key_pressed(&pressed, context, XUD_KEY_LEFT) == XUD_OK) && pressed) {
            pick = (pick + XUD_MESH_GEN_COUNT - 1u) % XUD_MESH_GEN_COUNT;
        }

        /* The orbital camera: the mode's own turn rate, the mouse free. This library's mode list is
           raylib's rcamera.h set, so the example's CAMERA_ORBITAL is this constant.
        */
        result = xud_update_camera(context, &camera, XUD_CAMERA_MODE_ORBITAL);
        if (result != XUD_OK) {
            printf("models_mesh_generation: frame %u xud_update_camera(ORBITAL) -> %s\n", frame, xud_result_name(result));
            break;
        }

        result = xud_begin_drawing(context);
        if (result != XUD_OK) {
            printf("models_mesh_generation: frame %u xud_begin_drawing -> %s, and the run stops here\n", frame, xud_result_name(result));
            break;
        }
        (void)xud_clear_background(context, xudMeshGenRaywhite);

        /* The camera is begun here and NOT ended inside the frame: the end is a per-frame teardown
           on the renderer and a per-run teardown at the bottom of main, so a pair that opened and
           closed inside one frame is a no-op and a stray end is refused. See the longer note in
           examples/camera_first_person.c. Each frame's begin replaces the live record.
        */
        (void)xud_begin_camera_3d(context, camera);
        /* The solid, at the origin as raylib's example draws all nine there (its file, line 85), and
           1cm above the floor when it is the PLANE: the plane is cut ON y = 0 and so is the grid, and
           two surfaces at one height are a z-fight the depth buffer settles differently every frame.
           The other eight hang across the floor as they do in the example above, half of them under.
        */
        (void)xud_matrix_translate(place, (XudVector3) { .x = 0.0f, .y = (pick == 0u) ? 0.01f : 0.0f, .z = 0.0f });
        (void)xud_begin_material(context, materialHandle);
        result = xud_draw_mesh_ex(context, meshes[pick], (pick == (XUD_MESH_GEN_COUNT - 1u)) ? customMatrix : place, xudMeshGenWhite);
        (void)xud_end_material(context);
        if (result == XUD_OK) {
            result
                = xud_draw_grid(context, (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 0.0f }, 10u, 1.0f, xudMeshGenLightGray, xudMeshGenGray);
        }
        if (result != XUD_OK) {
            /* The batch's capacity doubles at the start of a frame, so the first frames of a scene are
               drawn short until the capacity reaches what it asks for. Every frame redraws everything,
               so a short one costs only the shapes that did not fit.
            */
            if (shortFrames == 0u) {
                printf("models_mesh_generation: frame %u a draw refused -> %s; the scene is whole from the frame whose capacity fits it\n",
                    frame, xud_result_name(result));
            }
            shortFrames++;
        }

        result = xud_mesh_gen_hud(context, font, pick, counts);
        if (result != XUD_OK) {
            if (shortFrames == 0u) {
                printf("models_mesh_generation: frame %u a HUD draw refused -> %s\n", frame, xud_result_name(result));
            }
            shortFrames++;
        }
        result = xud_end_drawing(context);
        if (result != XUD_OK) {
            printf("models_mesh_generation: frame %u xud_end_drawing -> %s, and the run stops here\n", frame, xud_result_name(result));
            break;
        }
        frame++;
    }

    /* The objects go in the reverse order they came: the camera, then the meshes, then the material
       that named the texture, then the texture, then the font, then the context. A mesh cannot be
       closed while the material that samples it is still live in the frame loop.
    */
    (void)xud_end_camera_3d(context);
    for (step = 0u; step < XUD_MESH_GEN_COUNT; step++) {
        if (meshes[step].kind != XUD_HANDLE_KIND_NONE) {
            result = xud_close_mesh(context, meshes[step]);
            if (result != XUD_OK) {
                printf("models_mesh_generation: xud_close_mesh(%u) -> %s\n", step, xud_result_name(result));
            }
        }
    }
    result = xud_close_material(context, materialHandle);
    if (result != XUD_OK) {
        printf("models_mesh_generation: xud_close_material -> %s\n", xud_result_name(result));
    }
    result = xud_close_texture(context, texture);
    if (result != XUD_OK) {
        printf("models_mesh_generation: xud_close_texture -> %s\n", xud_result_name(result));
    }
    result = xud_close_font(context, font);
    if (result != XUD_OK) {
        printf("models_mesh_generation: xud_close_font -> %s\n", xud_result_name(result));
    }
    free(fontFile);
    printf("models_mesh_generation: %u frame(s), %u of them short, %u mesh(es) uploaded\n", frame, shortFrames, (unsigned int)uploaded);
    result = xud_close_context(&context);
    printf("models_mesh_generation: xud_close_context -> %s\n", xud_result_name(result));

    return (result == XUD_OK) ? 0 : 1;
}
