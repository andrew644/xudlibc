/***********************************************************************************************
 *
 *   xud_test_model.c - the model rung and the public model API above it
 *
 *   Owns : the pure half of the rung (xud_model_check(), xud_model_gate(), xud_model_stage_bind(
 *          ), xud_model_read(), xud_model_bounds(), xud_model_path_dir(), xud_model_result_from_
 *          cgltf(), xud_model_get()), the driver half against a real context (xud_model_open(), x
 *          _model_close(), xud_model_close_all()), the public pair and the two public queries, an
 *          d the one glTF document every check below is stated against - built in memory as a GLB
 *          rather than read from a file. What is NOT here: the mesh arena's layout and the write t
 *          hat fills it (the mesh area's), the material record's variant axis (the material area's
 *          ), the handle layer's validation order (the handle area's), and cgltf's own parsing (th
 *          e vendor area's, which checks that the vendored header compiles and links).
 *
 *   Layer: tests, above the mesh, the material and the handle areas and below the core area. The
 *   pure half runs with no driver present - a handle table is one host allocation, a stage is one
 *   more, and a cgltf document is host memory - so the gate, the stage arithmetic, the read and t
 *          he bounding box are all answerable with no device in sight. The GPU half runs the same
 *   document through the rung's own open, which is where the MESH and MATERIAL slots the record n
 *   ames and the teardown order above the mesh and the material rungs are checked.
 *
 *   What the refusals are stated against, and why that shape is honest rather than convenient: th
 *   e document is edited after it parsed rather than twelve files are written for it. The gate is
 *   what refuses, and a document whose accessor says count 10 is the same document a file would h
 *   ave produced - parsing it again would be a check of cgltf's JSON, which the vendor area owns.
 *   Every refusal below therefore names the field that was poked, so a reader can tell which inpu
 *   t of the gate is being asked about.
 *
 *   Deterministic: one fixed document, one fixed triangle, fixed loop counts, no time, no random
 *   ness. The one file the GPU half writes is a fixed name under a fixed byte count, and it is re
 *   moved before the half returns.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include <stdio.h> /* Required for: fopen, fwrite and remove, the one GLB file the GPU half round-trips through xud_load_model() */
#include <stdlib.h> /* Required for: calloc and free, the stage block and the synthetic record this area allocates and frees */
#include <string.h> /* Required for: memset, the cgltf options this area builds itself */

#include "xud.h" /* Required for: XudResult, XudHandle, XudVector3, XudContext, XudMeshData and the public model API */
#include "xud_batch.h" /* Required for: XUD_BATCH_VERTEX_SIZE, XUD_BATCH_INDEX_SIZE, XUD_BATCH_STREAM_* - the stage's layout, read rather than restated */
#include "xud_core.h" /* Required for: struct XudContext, whose rungs the public pair and the driver half are handed */
#include "xud_handle.h" /* Required for: XudHandleTable, xud_handle_alloc/free, xud_handle_record_set - the slot this record lives in */
#include "xud_material.h" /* Required for: XudMaterialRecord and xud_material_get - the MATERIAL slots this rung creates */
#include "xud_mesh.h" /* Required for: XUD_MESH_BUFFER_COUNT, XUD_MESH_INDEX_SLOT, XudMeshRecord and xud_mesh_get - the MESH slots this rung creates */
#include "xud_model.h" /* Required for: XudModelRecord, XudModelStage and every rung this area checks */
#include "xud_test_harness.h" /* Required for: xud_check, xud_gpu_tests_enabled, the area entry points */

/*-----------------------------------------------------------------------------------------------
 * The one document every check below is stated against
 *-----------------------------------------------------------------------------------------------
 *
 * One triangle, three vertices, five accessors, two materials plus the default. The buffer is 114
 *   bytes and the layout is the one the bufferViews and the accessors below agree about, so the doc
 *   ument is a fact the test can read rather than a fact it has to trust. The floats are copied as
 *   the platform's own bytes rather than spelled out, which is the little-endian the glTF format a
 *   sked for on the platform this run is built for - the same round trip that makes the read's chec
 *   ks below meaningful is what makes this one meaningful.
 */

static const char xudTestModelJson[]
    = "{\"asset\":{\"version\":\"2.0\",\"generator\":\"xudlibc\"},"
      "\"buffers\":[{\"byteLength\":114}],"
      "\"bufferViews\":["
      "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36,\"type\":\"VEC3\"},"
      "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":36,\"type\":\"VEC3\"},"
      "{\"buffer\":0,\"byteOffset\":72,\"byteLength\":24,\"type\":\"VEC2\"},"
      "{\"buffer\":0,\"byteOffset\":96,\"byteLength\":12,\"componentType\":5121,\"type\":\"VEC4\"},"
      "{\"buffer\":0,\"byteOffset\":102,\"byteLength\":6,\"type\":\"SCALAR\",\"componentType\":5123}],"
      "\"accessors\":["
      "{\"bufferView\":0,\"byteOffset\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
      "{\"bufferView\":1,\"byteOffset\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
      "{\"bufferView\":2,\"byteOffset\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
      "{\"bufferView\":3,\"byteOffset\":0,\"componentType\":5121,\"count\":3,\"normalized\":true,\"type\":\"VEC4\"},"
      "{\"bufferView\":4,\"byteOffset\":0,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],"
      "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2,\"COLOR_0\":3},"
      "\"indices\":4,\"material\":0}]}],"
      "\"materials\":[{\"name\":\"opaque\"},{\"name\":\"blend\",\"alphaMode\":\"BLEND\"}]}";

static const float xudTestModelPos[9] = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
static const float xudTestModelNormal[9] = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f };
static const float xudTestModelUv[6] = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f };
static const uint8_t xudTestModelColor[12] = { 255u, 0u, 0u, 255u, 0u, 255u, 0u, 255u, 0u, 0u, 255u, 255u };
static const uint16_t xudTestModelIndex[3] = { 0u, 1u, 2u };

#define XUD_TEST_MODEL_BUFFER_BYTES 114u /* the one buffer every bufferView and accessor above agrees about */
#define XUD_TEST_MODEL_MESHES 1u /* one mesh */
#define XUD_TEST_MODEL_PRIMITIVES 1u /* one primitive */
#define XUD_TEST_MODEL_MATERIALS 2u /* the document's two materials, plus the default the rung adds */
#define XUD_TEST_MODEL_VERTICES 3u
#define XUD_TEST_MODEL_INDICES 3u

static uint32_t xud_test_model_put_u32(unsigned char* at, uint32_t value) {
    at[0] = (unsigned char)(value & 0xFFu);
    at[1] = (unsigned char)((value >> 8) & 0xFFu);
    at[2] = (unsigned char)((value >> 16) & 0xFFu);
    at[3] = (unsigned char)((value >> 24) & 0xFFu);
    return 4u;
}

static void xud_test_model_put_f32(unsigned char* at, float value) { memcpy(at, &value, sizeof(value)); }

/* The GLB container: the header, the JSON chunk padded to four, the BIN chunk padded to four. The
   container exists so the document can be built in memory at all - a .gltf file would need a secon
   d file for its buffer, and a data-URI would need a base64 blob the reader cannot check against th
   e arrays above.
*/
static uint32_t xud_test_model_build_glb(unsigned char* out, uint32_t outCapacity, const unsigned char* bin, uint32_t binLength) {
    uint32_t jsonLength = 0u;
    uint32_t jsonPad = 0u;
    uint32_t binPad = 0u;
    uint32_t total = 0u;
    uint32_t written = 0u;
    uint32_t i = 0u;

    for (jsonLength = 0u; xudTestModelJson[jsonLength] != '\0'; jsonLength++) {
        /* the length of the literal, walked rather than trusted */
    }
    jsonPad = (jsonLength + 3u) & ~3u;
    binPad = (binLength + 3u) & ~3u;
    total = 12u + 8u + jsonPad + 8u + binPad;
    if (total > outCapacity) {
        return 0u;
    }
    out[written] = 'g';
    out[written + 1u] = 'l';
    out[written + 2u] = 'T';
    out[written + 3u] = 'F';
    written += 4u;
    written += xud_test_model_put_u32(out + written, 2u);
    written += xud_test_model_put_u32(out + written, total);
    written += xud_test_model_put_u32(out + written, jsonPad);
    out[written] = 'J';
    out[written + 1u] = 'S';
    out[written + 2u] = 'O';
    out[written + 3u] = 'N';
    written += 4u;
    for (i = 0u; i < jsonLength; i++) {
        out[written + i] = (unsigned char)xudTestModelJson[i];
    }
    for (i = jsonLength; i < jsonPad; i++) {
        out[written + i] = ' ';
    }
    written += jsonPad;
    written += xud_test_model_put_u32(out + written, binPad);
    out[written] = 'B';
    out[written + 1u] = 'I';
    out[written + 2u] = 'N';
    out[written + 3u] = 0;
    written += 4u;
    for (i = 0u; i < binLength; i++) {
        out[written + i] = bin[i];
    }
    for (i = binLength; i < binPad; i++) {
        out[written + i] = 0u;
    }
    written += binPad;
    return written;
}

static uint32_t xud_test_model_build_bin(unsigned char* out, uint32_t outCapacity) {
    uint32_t i = 0u;

    if (outCapacity < XUD_TEST_MODEL_BUFFER_BYTES) {
        return 0u;
    }
    for (i = 0u; i < 9u; i++) {
        xud_test_model_put_f32(out + (i * 4u), xudTestModelPos[i]);
    }
    for (i = 0u; i < 9u; i++) {
        xud_test_model_put_f32(out + 36u + (i * 4u), xudTestModelNormal[i]);
    }
    for (i = 0u; i < 6u; i++) {
        xud_test_model_put_f32(out + 72u + (i * 4u), xudTestModelUv[i]);
    }
    for (i = 0u; i < 12u; i++) {
        out[96u + i] = xudTestModelColor[i];
    }
    for (i = 0u; i < 3u; i++) {
        out[102u + (i * 2u)] = (unsigned char)(xudTestModelIndex[i] & 0xFFu);
        out[103u + (i * 2u)] = (unsigned char)((xudTestModelIndex[i] >> 8) & 0xFFu);
    }
    return XUD_TEST_MODEL_BUFFER_BYTES;
}

/* A fresh document, loaded and validated, or NULL. Every refusal below gets its own so a poke in
   one check cannot be the reason the next check passed.
*/
static cgltf_data* xud_test_model_document(const unsigned char* glb, uint32_t glbLength) {
    cgltf_options options;
    cgltf_data* document = NULL;

    memset(&options, 0, sizeof(options));
    options.type = cgltf_file_type_invalid;
    if (cgltf_parse(&options, glb, (cgltf_size)glbLength, &document) != cgltf_result_success) {
        return NULL;
    }
    if (cgltf_load_buffers(&options, document, NULL) != cgltf_result_success) {
        cgltf_free(document);
        return NULL;
    }
    if (cgltf_validate(document) != cgltf_result_success) {
        cgltf_free(document);
        return NULL;
    }
    return document;
}

static void xud_test_model_pure(void) {
    unsigned char glb[4096];
    unsigned char bin[XUD_TEST_MODEL_BUFFER_BYTES];
    uint32_t glbLength = 0u;
    uint32_t binLength = 0u;
    cgltf_data* document = NULL;
    uint32_t meshCount = 0u;
    uint32_t materialCount = 0u;
    uint32_t vertexCount = 0u;
    uint32_t indexCount = 0u;
    uint32_t stageBytes = 0u;
    uint32_t k = 0u;
    char* droppedName = NULL;
    XudModelStage stage;
    XudMeshData mesh;
    XudVector3 boundsMin;
    XudVector3 boundsMax;
    XudVector3 firstMin;
    XudVector3 firstMax;
    XudResult result = XUD_OK;

    binLength = xud_test_model_build_bin(bin, XUD_TEST_MODEL_BUFFER_BYTES);
    xud_check(binLength == XUD_TEST_MODEL_BUFFER_BYTES, "bin-built",
        "the document's one buffer is built from the five arrays above, so the bytes and the arrays are one fact");
    glbLength = xud_test_model_build_glb(glb, (uint32_t)sizeof(glb), bin, binLength);
    xud_check(glbLength > 12u, "glb-built", "and the container holds it, so the document can be parsed from memory");
    xud_check(glb[4] == 2u && glb[16] == 'J' && glb[17] == 'S' && glb[18] == 'O' && glb[19] == 'N', "glb-header",
        "the container's version word and the JSON chunk's magic are the ones cgltf's own header constants ask for, and the chunk's length "
        "is the padded length, which is the container format's rule");

    document = xud_test_model_document(glb, glbLength);
    xud_check(document != NULL, "document-parsed", "the fixed document parses, loads and validates");
    if (document == NULL) {
        return;
    }

    /* The document walk: the two counts the open will spend, and the two bounds it refuses past. */
    xud_check(xud_model_check(NULL, &meshCount, &materialCount) == XUD_INVALID_ARG, "check-null-document",
        "no document is a bad argument rather than an empty one");
    result = xud_model_check(document, &meshCount, &materialCount);
    xud_check(result == XUD_OK, "check-ok", "the fixed document passes the walk");
    xud_check(document->meshes[0].primitives[0].type == cgltf_primitive_type_triangles, "document-mode-default",
        "the document omits the primitive's mode and the parser's default is TRIANGLES, which is the format's own default");
    xud_check(meshCount == XUD_TEST_MODEL_MESHES, "check-mesh-count", "and one primitive is one MESH slot");
    xud_check(materialCount == (XUD_TEST_MODEL_MATERIALS + 1u), "check-material-count",
        "and two document materials plus the default are three MATERIAL slots");
    xud_check(xud_model_check(document, NULL, &materialCount) == XUD_INVALID_ARG, "check-null-out-count",
        "an out-count with nowhere to be written is refused");

    /* The gate of one primitive, and the refusals reached by editing the parsed document. */
    xud_check(xud_model_gate(document, 0u, 0u, &vertexCount, &indexCount, &stageBytes) == XUD_OK, "gate-ok",
        "the fixed primitive passes its own gate");
    xud_check(vertexCount == XUD_TEST_MODEL_VERTICES, "gate-vertex-count", "and the vertex count is POSITION's");
    xud_check(indexCount == XUD_TEST_MODEL_INDICES, "gate-index-count", "and the index count is the index accessor's");
    xud_check(stageBytes == XUD_MODEL_STAGE_BYTES(XUD_TEST_MODEL_VERTICES, XUD_TEST_MODEL_INDICES), "gate-stage-bytes",
        "and the stage is the four streams at that count plus the index buffer at that count");
    xud_check(stageBytes == (XUD_BATCH_VERTEX_SIZE * XUD_TEST_MODEL_VERTICES) + (XUD_BATCH_INDEX_SIZE * XUD_TEST_MODEL_INDICES),
        "gate-stage-bytes-against-the-batch",
        "and the same total read off the batch's own widths, so the stage and the streams cannot drift apart");
    xud_check(xud_model_gate(document, XUD_TEST_MODEL_MESHES, 0u, &vertexCount, &indexCount, &stageBytes) == XUD_INVALID_ARG,
        "gate-mesh-index-past", "a mesh index at the document's count is refused, not read");
    xud_check(xud_model_gate(document, 0u, XUD_TEST_MODEL_PRIMITIVES, &vertexCount, &indexCount, &stageBytes) == XUD_INVALID_ARG,
        "gate-primitive-index-past", "and so is a primitive index at the mesh's count");
    xud_check(xud_model_gate(document, 0u, 0u, NULL, &indexCount, &stageBytes) == XUD_INVALID_ARG, "gate-null-out-count",
        "an out-count with nowhere to be written is refused");

    document->meshes[0].primitives[0].type = cgltf_primitive_type_lines;
    xud_check(xud_model_gate(document, 0u, 0u, &vertexCount, &indexCount, &stageBytes) == XUD_UNSUPPORTED, "gate-line-mode",
        "a LINES primitive is a well-formed document this rung does not draw");
    cgltf_free(document);
    document = xud_test_model_document(glb, glbLength);
    xud_check(document != NULL, "document-reparsed-lines", "the same document again, for the next refusal");
    if (document == NULL) {
        return;
    }

    document->accessors[2].count = 4u;
    xud_check(xud_model_gate(document, 0u, 0u, &vertexCount, &indexCount, &stageBytes) == XUD_INVALID_ARG, "gate-counts-disagree",
        "two accessors that name different vertex counts leave a stream with no meaning");
    cgltf_free(document);
    document = xud_test_model_document(glb, glbLength);
    xud_check(document != NULL, "document-reparsed-counts", "the same document again, for the next refusal");
    if (document == NULL) {
        return;
    }

    document->accessors[0].offset = 40u;
    xud_check(xud_model_gate(document, 0u, 0u, &vertexCount, &indexCount, &stageBytes) == XUD_INVALID_ARG, "gate-accessor-past-view",
        "an accessor that reads past its buffer view is a broken document");
    cgltf_free(document);
    document = xud_test_model_document(glb, glbLength);
    xud_check(document != NULL, "document-reparsed-offset", "the same document again, for the next refusal");
    if (document == NULL) {
        return;
    }

    document->accessors[4].count = 2u;
    xud_check(xud_model_gate(document, 0u, 0u, &vertexCount, &indexCount, &stageBytes) == XUD_INVALID_ARG, "gate-two-indices",
        "an index count that is not a whole number of triangles is a broken document");
    cgltf_free(document);
    document = xud_test_model_document(glb, glbLength);
    xud_check(document != NULL, "document-reparsed-triangles", "the same document again, for the next refusal");
    if (document == NULL) {
        return;
    }

    document->accessors[0].is_sparse = 1;
    xud_check(xud_model_gate(document, 0u, 0u, &vertexCount, &indexCount, &stageBytes) == XUD_UNSUPPORTED, "gate-sparse-accessor",
        "a sparse accessor is well-formed and is not read - the read would owe it a second buffer view");
    cgltf_free(document);
    document = xud_test_model_document(glb, glbLength);
    xud_check(document != NULL, "document-reparsed-sparse", "the same document again, for the next refusal");
    if (document == NULL) {
        return;
    }

    document->meshes[0].primitives[0].attributes[2].index = 1;
    xud_check(xud_model_gate(document, 0u, 0u, &vertexCount, &indexCount, &stageBytes) == XUD_UNSUPPORTED, "gate-second-uv",
        "a UV_1 is well-formed and is not read - the mesh's one UV stream is the reason");
    cgltf_free(document);
    document = xud_test_model_document(glb, glbLength);
    xud_check(document != NULL, "document-reparsed-uv", "the same document again, for the next refusal");
    if (document == NULL) {
        return;
    }

    document->meshes[0].primitives[0].attributes[1].type = cgltf_attribute_type_tangent;
    xud_check(xud_model_gate(document, 0u, 0u, &vertexCount, &indexCount, &stageBytes) == XUD_UNSUPPORTED, "gate-tangent",
        "a TANGENT is well-formed and is not read - the mesh's four streams are the reason");
    cgltf_free(document);
    document = xud_test_model_document(glb, glbLength);
    xud_check(document != NULL, "document-reparsed-tangent", "the same document again, for the next refusal");
    if (document == NULL) {
        return;
    }

    droppedName = document->meshes[0].primitives[0].attributes[0].name;
    for (k = 0u; (k + 1u) < (uint32_t)document->meshes[0].primitives[0].attributes_count; k++) {
        document->meshes[0].primitives[0].attributes[k] = document->meshes[0].primitives[0].attributes[k + 1u];
    }
    document->meshes[0].primitives[0].attributes_count -= 1u;
    xud_check(xud_model_gate(document, 0u, 0u, &vertexCount, &indexCount, &stageBytes) == XUD_INVALID_ARG, "gate-no-position",
        "a primitive whose POSITION row is dropped is refused, and every row that survives is valid on its own");
    cgltf_free(document);
    /* The row that was dropped is no longer in the array, so the name string it used to point at is
       no longer reachable from the document and cgltf_free() above cannot free it. It is freed here,
       alongside the row, so the sanitizer run of this area stays leak-free rather than blaming a row
       this area removed.
    */
    free(droppedName);
    document = xud_test_model_document(glb, glbLength);
    xud_check(document != NULL, "document-reparsed-duplicate", "the same document again, for the duplicate row below");
    if (document == NULL) {
        return;
    }
    document->meshes[0].primitives[0].attributes[2].type = cgltf_attribute_type_color;
    document->meshes[0].primitives[0].attributes[2].index = 0;
    document->meshes[0].primitives[0].attributes[2].data = document->meshes[0].primitives[0].attributes[3].data;
    xud_check(xud_model_gate(document, 0u, 0u, &vertexCount, &indexCount, &stageBytes) == XUD_INVALID_ARG, "gate-two-colors",
        "two COLOR_0 name the one COLOR stream, so the stream has no meaning rather than two candidates");
    cgltf_free(document);
    document = xud_test_model_document(glb, glbLength);
    xud_check(document != NULL, "document-reparsed-position", "the same document again, for the read below");
    if (document == NULL) {
        return;
    }

    /* The stage: one block, five arrays, no padding. The offsets are stated against the batch's own
       widths rather than against numbers, so the layout below is the layout the streams already state.
    */
    xud_check(xud_model_gate(document, 0u, 0u, &vertexCount, &indexCount, &stageBytes) == XUD_OK, "gate-for-stage",
        "the primitive is gated again, so the stage below is bound for a primitive that passed");
    {
        void* block = calloc(1, stageBytes);
        xud_check(block != NULL, "stage-block", "the stage's one block is allocated");
        if (block == NULL) {
            cgltf_free(document);
            return;
        }
        xud_check(xud_model_stage_bind(block, stageBytes - 1u, vertexCount, indexCount, &stage) == XUD_INVALID_ARG, "stage-block-too-small",
            "a block one byte short of the stage's own total is refused, not bound short");
        result = xud_model_stage_bind(block, stageBytes, vertexCount, indexCount, &stage);
        xud_check(result == XUD_OK, "stage-bound", "the block is bound over the five arrays");
        xud_check(stage.offset[XUD_BATCH_STREAM_POS] == 0u, "stage-pos-at-base", "POSITION starts at the block's base");
        xud_check(stage.offset[XUD_BATCH_STREAM_UV] == (vertexCount * XUD_BATCH_STREAM_POS_SIZE), "stage-uv-offset",
            "and UV starts one POSITION-stride in, which is the batch's stride for one vertex");
        xud_check(stage.offset[XUD_BATCH_STREAM_COLOR] == (vertexCount * (XUD_BATCH_STREAM_POS_SIZE + XUD_BATCH_STREAM_UV_SIZE)),
            "stage-color-offset", "and COLOR starts after both");
        xud_check(stage.offset[XUD_BATCH_STREAM_NORMAL]
                == (vertexCount * (XUD_BATCH_STREAM_POS_SIZE + XUD_BATCH_STREAM_UV_SIZE + XUD_BATCH_STREAM_COLOR_SIZE)),
            "stage-normal-offset", "and NORMAL starts after the three");
        xud_check(stage.offset[XUD_MESH_INDEX_SLOT]
                == (vertexCount
                    * (XUD_BATCH_STREAM_POS_SIZE + XUD_BATCH_STREAM_UV_SIZE + XUD_BATCH_STREAM_COLOR_SIZE + XUD_BATCH_STREAM_NORMAL_SIZE)),
            "stage-index-offset", "and the index buffer starts after the four streams, which is the batch's stride at this vertex count");
        xud_check(stage.stageBytes == stageBytes, "stage-total",
            "and the total is the gate's, so the block the gate asked for is the block this bind wrote into");
        xud_check(xud_model_stage_bind(NULL, stageBytes, vertexCount, indexCount, &stage) == XUD_INVALID_ARG, "stage-null-block",
            "a block with nowhere to bind over is refused");
        xud_check(xud_model_stage_bind(block, stageBytes, 0u, indexCount, &stage) == XUD_INVALID_ARG, "stage-zero-vertices",
            "a stage with no vertices has no arrays to bind");
        xud_check(xud_model_stage_bind(block, stageBytes, vertexCount, 2u, &stage) == XUD_INVALID_ARG, "stage-two-indices",
            "and an index count that is not a whole number of triangles has no triangles to bind");

        result = xud_model_read(document, 0u, 0u, &stage, &mesh);
        xud_check(result == XUD_OK, "read-ok", "the fixed primitive is read into the bound stage");
        xud_check(mesh.vertexCount == XUD_TEST_MODEL_VERTICES && mesh.indexCount == XUD_TEST_MODEL_INDICES, "read-counts",
            "and the mesh description says what was read");
        xud_check(mesh.pos == stage.pos && mesh.index == stage.index, "read-points-at-the-stage",
            "and the description points at the stage rather than at a copy, which is what makes the stage the one block a primitive's "
            "bytes pass through");
        xud_check(stage.pos[3] == 1.0f && stage.pos[7] == 1.0f, "read-positions",
            "and the positions are the document's, so the accessor's own offset and stride were honoured");
        xud_check(stage.normal[2] == 1.0f && stage.normal[5] == 1.0f && stage.normal[8] == 1.0f, "read-normals",
            "and the normals are the document's rather than the default, because the document carries them");
        xud_check(stage.texcoords[2] == 1.0f && stage.texcoords[5] == 1.0f, "read-texcoords", "and the UVs are the document's");
        xud_check(stage.color[0] == 255u && stage.color[1] == 0u && stage.color[3] == 255u && stage.color[5] == 255u, "read-colors",
            "and the packed ubyte colors are scaled into the four-byte stream, alpha included");
        xud_check(stage.index[0] == 0u && stage.index[1] == 1u && stage.index[2] == 2u, "read-indices",
            "and the u16 indices were widened into the four-byte stream the mesh's buffer is");

        result = xud_model_bounds(&stage, NULL, NULL, &boundsMin, &boundsMax);
        xud_check(result == XUD_OK, "bounds-first-stage", "the first stage seeds both corners");
        xud_check(boundsMin.x == 0.0f && boundsMin.y == 0.0f && boundsMin.z == 0.0f, "bounds-min",
            "and the low corner is the document's lowest vertex");
        xud_check(boundsMax.x == 1.0f && boundsMax.y == 1.0f && boundsMax.z == 0.0f, "bounds-max",
            "and the high corner is the highest, so the box contains the triangle rather than the origin");
        result = xud_model_bounds(&stage, &boundsMin, &boundsMax, &firstMin, &firstMax);
        xud_check(result == XUD_OK, "bounds-folded-again", "folding the same stage into the box it produced changes nothing");
        xud_check(firstMin.x == boundsMin.x && firstMax.x == boundsMax.x, "bounds-idempotent",
            "and the box is the same box, which is what makes a per-primitive fold and a whole-model fold agree");
        xud_check(xud_model_bounds(&stage, &boundsMin, NULL, &firstMin, &firstMax) == XUD_OK, "bounds-half-box",
            "an in-corner that is only half a box is the no-box state, so the stage seeds both corners again");
        xud_check(firstMin.x == 0.0f && firstMax.x == 1.0f, "bounds-half-box-values",
            "and the values are the stage's, which is the reason a box seeded at the origin would have been wrong");
        xud_check(xud_model_bounds(NULL, NULL, NULL, &firstMin, &firstMax) == XUD_INVALID_ARG, "bounds-null-stage",
            "a stage with no positions has no box");

        /* The read's two own refusals, reached by editing what the read reads. */
        ((uint8_t*)document->buffers[0].data)[102u] = 5u;
        xud_check(xud_model_read(document, 0u, 0u, &stage, &mesh) == XUD_INVALID_ARG, "read-index-past-vertices",
            "an index at or past the vertex count is refused here rather than being UB at the draw");
        ((uint8_t*)document->buffers[0].data)[102u] = 0u;
        cgltf_free(document);
        document = xud_test_model_document(glb, glbLength);
        xud_check(document != NULL, "document-reparsed-index", "the same document again, for the stage mismatch below");
        if (document == NULL) {
            free(block);
            return;
        }
        {
            void* other = calloc(1, XUD_MODEL_STAGE_BYTES(4u, 3u));
            XudModelStage mismatch;
            xud_check(other != NULL, "mismatch-block", "a block sized for four vertices");
            if (other == NULL) {
                free(block);
                cgltf_free(document);
                return;
            }
            xud_check(xud_model_stage_bind(other, XUD_MODEL_STAGE_BYTES(4u, 3u), 4u, 3u, &mismatch) == XUD_OK, "mismatch-bound",
                "and bound for four vertices, which is a stage that binds fine on its own");
            xud_check(xud_model_read(document, 0u, 0u, &mismatch, &mesh) == XUD_INVALID_ARG, "read-stage-mismatch",
                "and reading the three-vertex primitive through it is refused, so a stage cannot be used for another primitive's arrays");
            free(other);
        }
        free(block);
        cgltf_free(document);
        document = NULL;
    }

    /* The path's directory prefix, which is what cgltf_load_buffers() resolves a document's external
       buffer URIs against. A path with no slash answers the empty prefix rather than a refusal, bec
       ause a document shipped in the working directory is the normal case rather than an accident.
    */
    {
        char dir[XUD_MODEL_PATH_MAX];
        uint32_t dirLength = 0u;

        xud_check(xud_model_path_dir(dir, XUD_MODEL_PATH_MAX, &dirLength, "/tmp/xud/a.glb") == XUD_OK, "path-dir-ok",
            "a full path answers its prefix");
        xud_check(dirLength == 9u && dir[0] == '/' && dir[8] == '/', "path-dir-value",
            "and the prefix is everything through the last slash, so a sibling buffer is found beside the file");
        xud_check(xud_model_path_dir(dir, XUD_MODEL_PATH_MAX, &dirLength, "a.glb") == XUD_OK, "path-dir-no-slash",
            "a path with no slash is not refused");
        xud_check(dirLength == 0u && dir[0] == '\0', "path-dir-empty",
            "and it answers the empty prefix, which is the working directory the document was read in");
        xud_check(xud_model_path_dir(dir, XUD_MODEL_PATH_MAX, &dirLength, "") == XUD_INVALID_ARG, "path-dir-empty-path",
            "an empty path names no file at all");
        xud_check(xud_model_path_dir(NULL, XUD_MODEL_PATH_MAX, &dirLength, "a/b.glb") == XUD_INVALID_ARG, "path-dir-null-out",
            "a prefix with nowhere to be written is refused");
        xud_check(xud_model_path_dir(dir, 4u, &dirLength, "/tmp/xud/a.glb") == XUD_UNSUPPORTED, "path-dir-too-long",
            "a prefix that does not fit the caller's buffer is refused rather than truncated");
        xud_check(xud_model_path_dir(dir, XUD_MODEL_PATH_MAX, &dirLength, NULL) == XUD_INVALID_ARG, "path-dir-null-path",
            "and a path with no name is refused");
    }

    /* cgltf's codes as this library's, one row per code and no fallthrough. */
    xud_check(xud_model_result_from_cgltf(cgltf_result_success) == XUD_OK, "map-success", "success maps to success");
    xud_check(xud_model_result_from_cgltf(cgltf_result_file_not_found) == XUD_NOT_FOUND, "map-not-found",
        "a path that names no file is not found, which is the one code a caller can act on by asking for a path");
    xud_check(xud_model_result_from_cgltf(cgltf_result_io_error) == XUD_INTERNAL_ERROR, "map-io-error",
        "a file that exists and could not be read is not a document a caller can fix");
    xud_check(xud_model_result_from_cgltf(cgltf_result_invalid_options) == XUD_INVALID_ARG, "map-invalid-options",
        "options this library built wrong are a bug in this library, and the code says so");
    xud_check(xud_model_result_from_cgltf(cgltf_result_out_of_memory) == XUD_OUT_OF_MEMORY, "map-out-of-memory",
        "and a refusal that was an allocation is the same code the rungs here use for one");
    xud_check(xud_model_result_from_cgltf(cgltf_result_data_too_short) == XUD_MALFORMED, "map-data-too-short",
        "a container that promised more bytes than it carried is a malformed document");
    xud_check(xud_model_result_from_cgltf(cgltf_result_unknown_format) == XUD_MALFORMED, "map-unknown-format",
        "and a container whose magic is not a container is malformed rather than unsupported");
    xud_check(
        xud_model_result_from_cgltf(cgltf_result_invalid_json) == XUD_MALFORMED, "map-invalid-json", "and so is JSON that does not parse");
    xud_check(xud_model_result_from_cgltf(cgltf_result_invalid_gltf) == XUD_MALFORMED, "map-invalid-gltf",
        "and so is a document cgltf's own validate refuses, which is the same answer the gate here gives");
    xud_check(xud_model_result_from_cgltf(cgltf_result_legacy_gltf) == XUD_UNSUPPORTED, "map-legacy-gltf",
        "but a 1.0 asset is unsupported rather than malformed: it is well-formed and names a format this rung does not read");
    xud_check(xud_model_result_from_cgltf(cgltf_result_max_enum) == XUD_INTERNAL_ERROR, "map-max-enum",
        "and a code outside the list is an invariant of the library above that broke, not a document");

    /* The record's slot, without a driver: the accessor's three refusals and the sentinel state. The
       record below is synthetic - the driver half's open is what allocates a real one - so this area
       frees it itself rather than leaving it for a rung that would need a device to run.
    */
    {
        XudHandleTable* table = NULL;
        XudHandle modelHandle = { 0u, 0u, 0u };
        XudHandle materialHandle = { 0u, 0u, 0u };
        XudHandle stale = { 0u, 0u, 0u };
        XudModelRecord* record = NULL;
        const XudModelRecord* gotRecord = NULL;

        xud_check(xud_handle_table_open(&table) == XUD_OK, "table-opened", "a table of the rungs below's own");
        if (table == NULL) {
            return;
        }
        xud_check(xud_handle_alloc(&modelHandle, table, XUD_HANDLE_KIND_MODEL) == XUD_OK, "model-slot-allocated",
            "a MODEL slot is allocated, which is the state before the record exists");
        xud_check(xud_model_get(table, modelHandle, &gotRecord) == XUD_INTERNAL_ERROR, "get-sentinel",
            "and the accessor refuses a live slot whose record field is still the sentinel, which is the open's own order stated as a "
            "check");
        record = (XudModelRecord*)calloc(1, sizeof(XudModelRecord));
        xud_check(record != NULL, "synthetic-record", "the record the slot will name");
        if (record == NULL) {
            (void)xud_handle_free(table, modelHandle);
            (void)xud_handle_table_close(&table);
            return;
        }
        xud_check(xud_handle_record_set(table, XUD_HANDLE_KIND_MODEL, modelHandle.index, record) == XUD_OK, "record-attached",
            "and the field is filled");
        xud_check(xud_model_get(table, modelHandle, &gotRecord) == XUD_OK, "get-ok", "the accessor reads the slot back");
        xud_check(gotRecord == record, "get-same-record", "and hands the record the slot names rather than a copy of it");
        xud_check(xud_handle_alloc(&materialHandle, table, XUD_HANDLE_KIND_MATERIAL) == XUD_OK, "material-slot-allocated",
            "a slot of another kind in the same table, at the same index");
        xud_check(xud_model_get(table, materialHandle, &gotRecord) == XUD_INVALID_ARG, "get-cross-kind",
            "and reaching it as a model is refused, which is the reason the tag is not redundant with the array's identity");
        stale = modelHandle;
        stale.generation = modelHandle.generation + 1u;
        xud_check(xud_model_get(table, stale, &gotRecord) == XUD_INVALID_ARG, "get-stale",
            "and a handle kept across the release that freed the slot is refused by the generation bump");
        xud_check(xud_model_get(table, modelHandle, NULL) == XUD_INVALID_ARG, "get-null-out-record",
            "an out-record with nowhere to be written is refused");
        xud_check(xud_model_get(NULL, modelHandle, &gotRecord) == XUD_INVALID_ARG, "get-null-table", "no table is refused");
        xud_check(xud_model_close_all(table, NULL, NULL, NULL, &meshCount) == XUD_INVALID_ARG, "close-all-null-rungs",
            "the teardown rung refuses a NULL loader, device or allocator before it walks anything");
        xud_check(xud_handle_record_set(table, XUD_HANDLE_KIND_MODEL, modelHandle.index, NULL) == XUD_OK, "record-cleared",
            "the field is cleared before the record is freed, which is the rule every close in this tree states");
        free(record);
        xud_check(xud_handle_free(table, modelHandle) == XUD_OK, "model-slot-freed", "and the slot is released");
        xud_check(xud_model_get(table, modelHandle, &gotRecord) == XUD_INVALID_ARG, "get-after-free",
            "and the handle no longer names a live slot, so a model kept past its close is refused rather than dangling");
        xud_check(xud_handle_table_close(&table) == XUD_OK, "table-closed", "and the table closes with no record left in it");
        xud_check(table == NULL, "table-cleared", "and the pointer is cleared, so a table closed twice cannot double-free");
    }
}

/*-----------------------------------------------------------------------------------------------
 * The GPU half
 *-----------------------------------------------------------------------------------------------
 *
 * The same document, through the rung's own open. What the pure half could not answer is what this
 *   half asks: that the MESH and MATERIAL slots the record names are really spent, that the teardown
 *   rung above the mesh and the material rungs is the order that leaves the mesh's mapped arena gone
 *   before the mesh rung's own close looks at it, and that the public load path - the one place cglt
 *   f's parse, buffer load and validate run in this library - reaches the same record from a file. T
 *   he two files this half writes are fixed names under fixed byte counts, and both are removed bef
 *   ore the half returns.
 */

static void xud_test_model_gpu(void) {
    XudContext* context = NULL;
    XudHandle modelHandle = { 0u, 0u, 0u };
    XudHandle loadedHandle = { 0u, 0u, 0u };
    XudHandle stale = { 0u, 0u, 0u };
    XudHandle answeredHandle = { 0u, 0u, 0u };
    const XudModelRecord* record = NULL;
    const XudMeshRecord* meshRecord = NULL;
    const XudMaterialRecord* materialRecord = NULL;
    XudVector3 boundsMin;
    XudVector3 boundsMax;
    XudResult result = XUD_OK;
    uint32_t live = 0u;
    uint32_t closed = 0u;
    uint32_t answered = 0u;
    unsigned char glb[4096];
    unsigned char bin[XUD_TEST_MODEL_BUFFER_BYTES];
    uint32_t glbLength = 0u;
    uint32_t binLength = 0u;
    cgltf_data* document = NULL;
    FILE* file = NULL;

    result = xud_init_context(&context, 64u, 64u);
    xud_check(result == XUD_OK, "gpu-model/context",
        "this machine has a loader, so the ladder completes and the rungs below have a device to be handed");
    if (context == NULL) {
        return;
    }

    binLength = xud_test_model_build_bin(bin, XUD_TEST_MODEL_BUFFER_BYTES);
    glbLength = xud_test_model_build_glb(glb, (uint32_t)sizeof(glb), bin, binLength);
    document = xud_test_model_document(glb, glbLength);
    xud_check(document != NULL, "gpu-model/document",
        "the fixed document is in hand, so the open below is stated against a document that parsed");
    if (document == NULL) {
        xud_close_context(&context);
        return;
    }

    result = xud_model_open(
        &modelHandle, context->handleTable, context->loader, context->device, &context->physicalDevice->caps, context->allocator, document);
    xud_check(result == XUD_OK, "gpu-model/open", "the fixed document opens against a real device");
    if (result != XUD_OK) {
        cgltf_free(document);
        xud_close_context(&context);
        return;
    }
    xud_check(modelHandle.kind == XUD_HANDLE_KIND_MODEL, "gpu-model/handle-kind",
        "the handle the open hands out is tagged MODEL, which is the reason the accessor below can refuse a handle of another kind");

    xud_check(xud_model_get(context->handleTable, modelHandle, NULL) == XUD_INVALID_ARG, "gpu-model/get-null-out",
        "an out-record with nowhere to be written is refused");
    result = xud_model_get(context->handleTable, modelHandle, &record);
    xud_check(result == XUD_OK, "gpu-model/get", "the record reads back through the accessor, which is the only way to reach it");
    if (record == NULL) {
        (void)xud_model_close(context->handleTable, modelHandle, context->loader, context->device, context->allocator);
        cgltf_free(document);
        xud_close_context(&context);
        return;
    }

    xud_check(record->meshCount == XUD_TEST_MODEL_MESHES, "gpu-model/mesh-count", "one primitive spent one MESH slot");
    xud_check(record->materialCount == (XUD_TEST_MODEL_MATERIALS + 1u), "gpu-model/material-count",
        "two document materials and the default spent three MATERIAL slots");
    xud_check(xud_handle_table_live_count(context->handleTable, XUD_HANDLE_KIND_MESH, &live) == XUD_OK, "gpu-model/mesh-live-query",
        "the live count answers");
    xud_check(live == record->meshCount, "gpu-model/mesh-live",
        "and the slots the record names are the live slots of the kind, so the record cannot be naming a slot that was never spent");
    xud_check(xud_handle_table_live_count(context->handleTable, XUD_HANDLE_KIND_MATERIAL, &live) == XUD_OK, "gpu-model/material-live-query",
        "the live count answers again");
    xud_check(live == record->materialCount, "gpu-model/material-live",
        "and so for the MATERIAL kind, which is the default material's slot included");

    xud_check(xud_mesh_get(context->handleTable, record->mesh[0], &meshRecord) == XUD_OK, "gpu-model/mesh-slot-live",
        "the MESH slot the record names is still live and still filled");
    xud_check(meshRecord != NULL, "gpu-model/mesh-record", "and the record is there");
    if (meshRecord != NULL) {
        xud_check(meshRecord->vertexCount == XUD_TEST_MODEL_VERTICES && meshRecord->indexCount == XUD_TEST_MODEL_INDICES,
            "gpu-model/mesh-counts",
            "and the mesh rung holds the counts the gate answered, so the stage's bytes reached the arena rather than a copy of them");
        xud_check(meshRecord->buffer[XUD_MESH_INDEX_SLOT] != VK_NULL_HANDLE, "gpu-model/mesh-buffers",
            "and the mesh's index buffer is a real VkBuffer, which is the stage's fifth array stated as a buffer");
    }
    xud_check(xud_material_get(context->handleTable, record->material[0], &materialRecord) == XUD_OK, "gpu-model/material-slot-live",
        "the first MATERIAL slot the record names is live");
    xud_check(materialRecord != NULL && materialRecord->variant == XUD_BATCH_VARIANT_OPAQUE, "gpu-model/material-variant",
        "and it is the opaque variant, because the document's first material is opaque and the variant is chosen from the material rather "
        "than from the primitive");
    xud_check(xud_material_get(context->handleTable, record->material[1], &materialRecord) == XUD_OK, "gpu-model/second-material-slot-live",
        "the second MATERIAL slot the record names is live");
    xud_check(materialRecord != NULL && materialRecord->variant == XUD_BATCH_VARIANT_TRANSPARENT, "gpu-model/second-material-variant",
        "and it is the transparent variant, so the document's alphaMode reached the variant axis rather than being dropped");
    xud_check(
        record->meshMaterial[0].index == record->material[0].index && record->meshMaterial[0].generation == record->material[0].generation,
        "gpu-model/mesh-material-binding",
        "and the mesh names the document's first material rather than the default, which is the one place a document's material survives "
        "to a draw");
    xud_check(record->meshMaterial[0].kind == XUD_HANDLE_KIND_MATERIAL && record->mesh[0].kind == XUD_HANDLE_KIND_MESH,
        "gpu-model/nested-kinds",
        "and every handle the record names carries the kind of the rung it names, so a record's own array cannot be read as another "
        "rung's");

    xud_check(record->boundsMin.x == 0.0f && record->boundsMin.y == 0.0f && record->boundsMin.z == 0.0f, "gpu-model/bounds-min",
        "the box's low corner is the document's lowest vertex");
    xud_check(record->boundsMax.x == 1.0f && record->boundsMax.y == 1.0f && record->boundsMax.z == 0.0f, "gpu-model/bounds-max",
        "and the high corner is the highest, so the box was folded at the load rather than walked later - the stage is gone by the time a "
        "caller could ask");

    xud_check(
        xud_close_model(context, modelHandle) == XUD_OK, "gpu-model/close", "the public close releases the model and everything it names");
    xud_check(xud_model_get(context->handleTable, modelHandle, &record) == XUD_INVALID_ARG, "gpu-model/get-after-close",
        "the handle no longer names a live slot");
    xud_check(xud_handle_table_live_count(context->handleTable, XUD_HANDLE_KIND_MESH, &live) == XUD_OK,
        "gpu-model/mesh-live-after-close-query", "the live count answers after the close");
    xud_check(live == 0u, "gpu-model/mesh-live-after-close",
        "and the MESH slots the model named are gone, which is the mesh rung's close having run below this one");
    xud_check(xud_handle_table_live_count(context->handleTable, XUD_HANDLE_KIND_MATERIAL, &live) == XUD_OK,
        "gpu-model/material-live-after-close-query", "the live count answers again");
    xud_check(
        live == 0u, "gpu-model/material-live-after-close", "and so are the MATERIAL slots, which is the default material's slot included");

    xud_check(xud_model_close_all(context->handleTable, context->loader, context->device, context->allocator, &closed) == XUD_OK,
        "gpu-model/close-all-idempotent", "the teardown rung runs again with nothing left and is not refused");
    xud_check(closed == 0u, "gpu-model/close-all-count",
        "and it closed nothing, so the close above the mesh and the material rungs is idempotent rather than a double free waiting for the "
        "table");

    /* The public load path, which is the only place cgltf_parse_file(), cgltf_load_buffers() and cgltf_validate()
       run in this library. The file is the same bytes the pure half parsed from memory, so the record below is
       stated against a document whose every field the test already checked.
    */
    file = fopen("xud_test_model_gpu.glb", "wb");
    xud_check(
        file != NULL, "gpu-model/file-written", "the fixed name is writable in the run directory, so the public path has a file to read");
    if (file != NULL) {
        (void)fwrite(glb, 1u, glbLength, file);
        (void)fclose(file);

        result = xud_load_model(&loadedHandle, context, "xud_test_model_gpu.glb");
        xud_check(result == XUD_OK, "gpu-model/load", "the same bytes reach the same record through the public path");
        xud_check(loadedHandle.kind == XUD_HANDLE_KIND_MODEL, "gpu-model/load-kind", "and the handle it hands out is tagged MODEL");
        if (result == XUD_OK) {
            xud_check(xud_get_model_mesh_count(&answered, context, loadedHandle) == XUD_OK, "gpu-model/mesh-count-query",
                "the public count query answers");
            xud_check(answered == XUD_TEST_MODEL_MESHES, "gpu-model/mesh-count",
                "and answers one, which is the record's own count read through the public accessor rather than the rung's");
            xud_check(xud_get_model_bounding_box(&boundsMin, &boundsMax, context, loadedHandle) == XUD_OK, "gpu-model/bounding-box-query",
                "the public bounding box query answers");
            xud_check(boundsMin.x == 0.0f && boundsMin.y == 0.0f && boundsMin.z == 0.0f && boundsMax.x == 1.0f && boundsMax.y == 1.0f
                    && boundsMax.z == 0.0f,
                "gpu-model/bounding-box",
                "and answers the box the pure half's fold produced, so the public query and the rung's fold cannot disagree");
            xud_check(xud_close_model(context, loadedHandle) == XUD_OK, "gpu-model/load-close",
                "and the loaded model closes the same way the opened one did");
            xud_check(xud_handle_table_live_count(context->handleTable, XUD_HANDLE_KIND_MESH, &live) == XUD_OK,
                "gpu-model/mesh-live-after-load-close-query", "the live count answers");
            xud_check(live == 0u, "gpu-model/mesh-live-after-load-close", "and leaves nothing behind");
        }

        xud_check(xud_load_model(&answeredHandle, context, "xud_test_model_absent.glb") == XUD_NOT_FOUND, "gpu-model/load-absent",
            "a path that names no file is not found, which is the one cgltf code a caller can act on by asking for another path");
        xud_check(xud_load_model(&answeredHandle, context, NULL) == XUD_INVALID_ARG, "gpu-model/load-null-path",
            "a path with no name is refused before a file is opened");

        file = fopen("xud_test_model_garbage.gltf", "wb");
        xud_check(
            file != NULL, "gpu-model/garbage-written", "and a second fixed name is writable, so the malformed answer has a file behind it");
        if (file != NULL) {
            (void)fwrite("not a glTF document at all\n", 1u, 26u, file);
            (void)fclose(file);
            xud_check(xud_load_model(&answeredHandle, context, "xud_test_model_garbage.gltf") == XUD_MALFORMED, "gpu-model/load-garbage",
                "and a file that is not a document is malformed rather than not found, which is the answer a caller can only act on by "
                "reporting the path");
            if (xud_load_model(&answeredHandle, context, "xud_test_model_garbage.gltf") == XUD_OK) {
                (void)xud_close_model(context, answeredHandle);
            }
            xud_check(remove("xud_test_model_garbage.gltf") == 0, "gpu-model/garbage-removed",
                "the garbage file is removed before the half returns");
        }
        xud_check(remove("xud_test_model_gpu.glb") == 0, "gpu-model/file-removed",
            "the fixed file is removed before the half returns, so a run leaves no state behind");
    }

    /* The public refusals that need no file at all, stated against a handle that was never live. */
    stale = loadedHandle;
    stale.generation = loadedHandle.generation + 1u;
    xud_check(xud_close_model(context, stale) == XUD_INVALID_ARG, "gpu-model/close-stale",
        "a model handle kept past the close that freed its slot is refused by the generation bump");
    xud_check(xud_get_model_mesh_count(&answered, context, stale) == XUD_INVALID_ARG, "gpu-model/count-stale",
        "and so for the count query, which is the same validation the close runs");
    xud_check(xud_get_model_bounding_box(&boundsMin, &boundsMax, context, stale) == XUD_INVALID_ARG, "gpu-model/box-stale",
        "and so for the box query");
    xud_check(xud_get_model_bounding_box(NULL, &boundsMax, context, loadedHandle) == XUD_INVALID_ARG, "gpu-model/box-null-out",
        "an out-corner with nowhere to be written is refused");
    xud_check(
        xud_get_model_mesh_count(&answered, NULL, loadedHandle) == XUD_INVALID_ARG, "gpu-model/count-no-context", "no context is refused");
    xud_check(xud_get_model_mesh_count(&answered, context, answeredHandle) == XUD_INVALID_ARG, "gpu-model/count-not-a-model",
        "and a handle that was never live is refused, which is the reason the queries below cannot be a walk of the whole table");

    cgltf_free(document);
    document = NULL;
    xud_close_context(&context);
}

void xud_test_model(void) {
    xud_test_model_pure();
    if (xud_gpu_tests_enabled()) {
        xud_test_model_gpu();
    }
}
