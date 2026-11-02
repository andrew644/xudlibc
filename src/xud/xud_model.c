/***********************************************************************************************
 *   xud_model.c - the model rung: the record behind a MODEL-kind slot, the cgltf document above
 *                 it, and the load/close pair xud.h declares on top of that
 *
 *   Owns : the record's three handle arrays and its two bounding-box corners, the stage block
 *          every primitive is read through, and the two halves of the pair xud.h declares.
 *   Layer: above the mesh and the material rungs and below xud.h.
 *
 *   What this file does NOT do, and why that is the whole point of it: it calls no Vulkan entry
 *   point. A model is a list of the objects the rungs below it own, and every driver object on
 *   that list - the five buffers per primitive, the two buffers per material - belongs to a mesh
 *   or a material that rung already creates and tears down. What this file adds on top of them
 *   is a document, a stage, and a bound: the glTF's accessors are read into one host block per
 *   primitive, handed to the mesh rung as an XudMeshData, and the block is freed before the next
 *   primitive starts. A model that refused part-way is therefore not a model that left a live
 *   mesh and a live material behind, because the rollback below closes exactly the slots the
 *   steps above created rather than the slots the record claims to own.
 *
 *   The one library this file borrows is cgltf, and it is borrowed above the rung rather than
 *   inside it: the parse, the buffer load and the validate all happen in xud_load_model(), so
 *   every rung here takes an already-loaded document and every pure rung here can be driven by a
 *   document a test built itself. The gate is what makes that honest rather than convenient - a
 *   document whose accessors disagree about the vertex count, whose index reads past its buffer
 *   view, or whose geometry hangs off a Draco block, is refused before a slot is spent, and the
 *   refusal is XUD_UNSUPPORTED when the document is well-formed and names something this rung
 *   does not read (a LINE mode, a morph target, a tangent, a UV_1, a sparse accessor) rather
 *   than XUD_INVALID_ARG, which is the answer for a document that is broken.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud_model.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xud_core.h" /* Required for: struct XudContext, the one bundle the public pair and the two public queries take */
#include "xud_device.h" /* Required for: XudDevice, the handle this rung checks exists */
#include "xud_handle.h" /* Required for: xud_handle_validate/alloc/free, xud_handle_record_get/set */
#include "xud_loader.h" /* Required for: xud_loader_is_open, xud_loader_device_bound */
#include "xud_material.h" /* Required for: xud_material_open/close, XUD_BLEND_* */
#include "xud_mesh.h" /* Required for: xud_mesh_open/close, XUD_MESH_BUFFER_COUNT, XUD_MESH_MAX_VERTICES */

/*-----------------------------------------------------------------------------------------------
 * The component types this rung converts, and the slots a primitive's attributes land in
 *-----------------------------------------------------------------------------------------------
 *
 * The masks are built from cgltf's own enum rather than from numbers, and they are the whole
 *   reason the gate refuses a component type rather than converting it wrongly: POSITION, TEX
 *   COORD_0 and NORMAL are read as float32 because the mesh's four streams ARE float32 and a
 *   half-float or a u16 position would need a conversion this rung has no place to state. COLOR_
 *   _0 additionally takes the three unsigned integer widths because the mesh's color stream is
 *   four bytes per vertex and a glTF that ships packed bytes is the common case rather than an
 *   exotic one; the two signed widths are refused rather than read, because the [0,1] remapping
 *   a signed normalized color needs ((v + 1) * 0.5) is a rule the mesh's stream does not carry
 *   and inventing it here would be a second convention. INDICES takes the three unsigned widths
 *   because that is all glTF allows, and refuses float32 indices rather than rounding them.
 */

#define XUD_MODEL_SLOT_POS XUD_BATCH_STREAM_POS /* which of the four streams an attribute landed in */
#define XUD_MODEL_SLOT_UV XUD_BATCH_STREAM_UV
#define XUD_MODEL_SLOT_COLOR XUD_BATCH_STREAM_COLOR
#define XUD_MODEL_SLOT_NORMAL XUD_BATCH_STREAM_NORMAL
#define XUD_MODEL_INDEX_SLOT XUD_MESH_INDEX_SLOT /* the fifth row of the map, the primitive's indices */

static XudResult xud_model_accessor_gate(const cgltf_accessor* accessor, cgltf_type wantType, uint32_t componentMask, uint32_t* outCount) {
    cgltf_size elementSize;
    cgltf_size stride;
    cgltf_size span;

    if (outCount == NULL) {
        return XUD_INVALID_ARG;
    }
    *outCount = 0u;
    if (accessor == NULL) {
        return XUD_UNSUPPORTED;
    }
    if (accessor->is_sparse) {
        return XUD_UNSUPPORTED;
    }
    if (accessor->component_type == cgltf_component_type_invalid) {
        return XUD_INVALID_ARG;
    }
    if ((componentMask & (1u << (uint32_t)accessor->component_type)) == 0u) {
        return XUD_UNSUPPORTED;
    }
    if (accessor->type != wantType) {
        return XUD_UNSUPPORTED;
    }
    if (accessor->buffer_view == NULL || accessor->buffer_view->buffer == NULL) {
        return XUD_INVALID_ARG;
    }
    if (accessor->buffer_view->buffer->data == NULL) {
        return XUD_INVALID_ARG; /* the document's buffers were never loaded, which is a broken document rather than an unreadable one */
    }
    if (accessor->buffer_view->size == 0u) {
        return XUD_INVALID_ARG;
    }
    if ((accessor->buffer_view->offset + accessor->buffer_view->size) > accessor->buffer_view->buffer->size) {
        return XUD_INVALID_ARG;
    }
    if (accessor->count == 0u) {
        return XUD_INVALID_ARG;
    }
    if (accessor->count > (cgltf_size)UINT32_MAX) {
        return XUD_INVALID_ARG;
    }
    elementSize = cgltf_calc_size(accessor->type, accessor->component_type);
    if (elementSize == 0u) {
        return XUD_UNSUPPORTED;
    }
    stride = (accessor->stride != 0u) ? accessor->stride : elementSize;
    if (stride < elementSize) {
        return XUD_INVALID_ARG;
    }
    span = accessor->offset + (stride * (accessor->count - 1u)) + elementSize;
    if (span > accessor->buffer_view->size) {
        return XUD_INVALID_ARG;
    }
    *outCount = (uint32_t)accessor->count;
    return XUD_OK;
}

/* The map of ONE primitive: the five rows a primitive's attribute list and index accessor land
   in, gated against the type and component mask each row requires. This is the one place the
   names are read, and it is the reason xud_model_gate() and xud_model_read() cannot drift apart
   - the read calls this map rather than repeating the name table below. */
static XudResult xud_model_primitive_map(const cgltf_primitive* prim, const cgltf_accessor** outAccessor, uint32_t* outCount) {
    const cgltf_attribute* attr;
    const cgltf_accessor* seen[XUD_MESH_BUFFER_COUNT];
    uint32_t count[XUD_MESH_BUFFER_COUNT];
    uint32_t floatMask;
    uint32_t colorMask;
    uint32_t indexMask;
    uint32_t slot;
    uint32_t a;
    uint32_t s;
    XudResult result;

    for (s = 0u; s < XUD_MESH_BUFFER_COUNT; s++) {
        seen[s] = NULL;
        count[s] = 0u;
    }
    floatMask = (1u << (uint32_t)cgltf_component_type_r_32f);
    colorMask = floatMask | (1u << (uint32_t)cgltf_component_type_r_8u) | (1u << (uint32_t)cgltf_component_type_r_16u)
        | (1u << (uint32_t)cgltf_component_type_r_32u);
    indexMask = (1u << (uint32_t)cgltf_component_type_r_8u) | (1u << (uint32_t)cgltf_component_type_r_16u)
        | (1u << (uint32_t)cgltf_component_type_r_32u);

    if (prim == NULL || outAccessor == NULL || outCount == NULL) {
        return XUD_INVALID_ARG;
    }
    if (prim->attributes == NULL || prim->attributes_count == 0u) {
        return XUD_INVALID_ARG;
    }
    for (a = 0u; a < prim->attributes_count; a++) {
        attr = &prim->attributes[a];
        if (attr->data == NULL) {
            return XUD_INVALID_ARG; /* the attribute names an accessor index the document does not carry */
        }
        switch (attr->type) {
        case cgltf_attribute_type_position:
            slot = XUD_MODEL_SLOT_POS;
            if (attr->index != 0u) {
                return XUD_INVALID_ARG;
            }
            result = xud_model_accessor_gate(attr->data, cgltf_type_vec3, floatMask, &count[slot]);
            break;
        case cgltf_attribute_type_normal:
            slot = XUD_MODEL_SLOT_NORMAL;
            if (attr->index != 0u) {
                return XUD_INVALID_ARG;
            }
            result = xud_model_accessor_gate(attr->data, cgltf_type_vec3, floatMask, &count[slot]);
            break;
        case cgltf_attribute_type_texcoord:
            slot = XUD_MODEL_SLOT_UV;
            if (attr->index != 0u) {
                return XUD_UNSUPPORTED; /* UV_1 and up: the mesh's one UV stream is the whole reason */
            }
            result = xud_model_accessor_gate(attr->data, cgltf_type_vec2, floatMask, &count[slot]);
            break;
        case cgltf_attribute_type_color:
            slot = XUD_MODEL_SLOT_COLOR;
            if (attr->index != 0u) {
                return XUD_UNSUPPORTED;
            }
            if (attr->data->type == cgltf_type_vec4) {
                result = xud_model_accessor_gate(attr->data, cgltf_type_vec4, colorMask, &count[slot]);
            } else if (attr->data->type == cgltf_type_vec3) {
                result = xud_model_accessor_gate(attr->data, cgltf_type_vec3, colorMask, &count[slot]);
            } else {
                return XUD_UNSUPPORTED;
            }
            break;
        default:
            return XUD_UNSUPPORTED; /* tangent, joints, weights, and a custom _0 name: none is read */
        }
        if (result != XUD_OK) {
            return result;
        }
        if (seen[slot] != NULL) {
            return XUD_INVALID_ARG; /* two attributes name the same stream, so the stream has no meaning */
        }
        seen[slot] = attr->data;
    }
    if (seen[XUD_MODEL_SLOT_POS] == NULL) {
        return XUD_INVALID_ARG; /* a primitive with no POSITION names no vertex */
    }
    if (prim->indices == NULL) {
        return XUD_UNSUPPORTED; /* the mesh's one index buffer is the whole reason a primitive must be indexed */
    }
    result = xud_model_accessor_gate(prim->indices, cgltf_type_scalar, indexMask, &count[XUD_MODEL_INDEX_SLOT]);
    if (result != XUD_OK) {
        return result;
    }
    for (s = 0u; s < XUD_MESH_BUFFER_COUNT; s++) {
        outAccessor[s] = seen[s];
        outCount[s] = count[s];
    }
    return XUD_OK;
}

/* The gate of ONE primitive, and the size of its stage. */
XudResult xud_model_gate(const cgltf_data* data, uint32_t meshIndex, uint32_t primitiveIndex, uint32_t* outVertexCount,
    uint32_t* outIndexCount, uint32_t* outStageBytes) {
    const cgltf_primitive* prim;
    const cgltf_accessor* accessor[XUD_MESH_BUFFER_COUNT];
    uint32_t count[XUD_MESH_BUFFER_COUNT];
    uint32_t vertexCount;
    uint32_t indexCount;
    uint32_t s;
    XudResult result;

    if (outVertexCount == NULL || outIndexCount == NULL || outStageBytes == NULL) {
        return XUD_INVALID_ARG;
    }
    *outVertexCount = 0u;
    *outIndexCount = 0u;
    *outStageBytes = 0u;
    if (data == NULL) {
        return XUD_INVALID_ARG;
    }
    if (meshIndex >= (uint32_t)data->meshes_count || primitiveIndex >= (uint32_t)data->meshes[meshIndex].primitives_count) {
        return XUD_INVALID_ARG;
    }
    prim = &data->meshes[meshIndex].primitives[primitiveIndex];
    if (prim->type != cgltf_primitive_type_triangles) {
        return XUD_UNSUPPORTED; /* POINTS, LINES and LINE_LOOP: the one pipeline draws triangles */
    }
    if (prim->has_draco_mesh_compression) {
        return XUD_UNSUPPORTED; /* a Draco block is decoded by a decoder this tree does not link */
    }
    if (prim->targets_count != 0u) {
        return XUD_UNSUPPORTED; /* morph targets need a second attribute set and a blend the shader does not do */
    }
    if (prim->mappings_count != 0u) {
        return XUD_UNSUPPORTED; /* mesh LOD selects a primitive per distance, and the camera is the only one in this tree */
    }
    result = xud_model_primitive_map(prim, accessor, count);
    if (result != XUD_OK) {
        return result;
    }
    vertexCount = count[XUD_MODEL_SLOT_POS];
    if (vertexCount == 0u || vertexCount > XUD_MESH_MAX_VERTICES) {
        return XUD_INVALID_ARG;
    }
    for (s = 0u; s < XUD_BATCH_STREAM_COUNT; s++) {
        if (accessor[s] != NULL && count[s] != vertexCount) {
            return XUD_INVALID_ARG; /* the four streams of one primitive describe different vertex counts */
        }
    }
    indexCount = count[XUD_MODEL_INDEX_SLOT];
    if (indexCount < 3u || (indexCount % 3u) != 0u) {
        return XUD_INVALID_ARG; /* the mesh's index buffer is whole triangles, and a partial one is a broken document */
    }
    *outVertexCount = vertexCount;
    *outIndexCount = indexCount;
    *outStageBytes = XUD_MODEL_STAGE_BYTES(vertexCount, indexCount);
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The document walk, the stage, and the read
 *-----------------------------------------------------------------------------------------------
 *
 * The walk is the gate run over every primitive of every mesh, and the two counts it answers are
 *   the two array sizes xud_model_open() will spend: the walk exists so the open can refuse a
 *   document that will not fit the handle layer BEFORE it spends the first slot rather than after
 *   it has spent forty of them. The asset version check is the one thing the walk checks that the
 *   gate does not, and it is XUD_UNSUPPORTED rather than XUD_INVALID_ARG because a 1.0 document is
 *   a well-formed document this library does not read - a glTF 1.0 asset puts its geometry in
 *   arrays rather than in accessors, so the accessors this rung reads are not merely empty, they
 *   were never a thing a 1.0 file could have.
 */

XudResult xud_model_check(const cgltf_data* data, uint32_t* outMeshCount, uint32_t* outMaterialCount) {
    uint32_t meshCount = 0u;
    uint32_t vertexCount = 0u;
    uint32_t indexCount = 0u;
    uint32_t stageBytes = 0u;
    uint32_t m;
    uint32_t p;
    XudResult result;

    if (outMeshCount == NULL || outMaterialCount == NULL) {
        return XUD_INVALID_ARG;
    }
    *outMeshCount = 0u;
    *outMaterialCount = 0u;
    if (data == NULL) {
        return XUD_INVALID_ARG;
    }
    if (data->asset.version == NULL) {
        return XUD_UNSUPPORTED;
    }
    if (data->asset.version[0] != '2') {
        return XUD_UNSUPPORTED; /* a 1.0 asset has no accessors to read, and a 3.0 asset is not a format cgltf parses */
    }
    if (data->meshes_count == 0u) {
        return XUD_INVALID_ARG;
    }
    if (data->meshes_count > (cgltf_size)XUD_MODEL_MAX_MESHES) {
        return XUD_INVALID_ARG;
    }
    if (data->materials_count > (cgltf_size)(XUD_MODEL_MAX_MATERIALS - 1u)) {
        return XUD_INVALID_ARG;
    }
    for (m = 0u; m < (uint32_t)data->meshes_count; m++) {
        for (p = 0u; p < (uint32_t)data->meshes[m].primitives_count; p++) {
            result = xud_model_gate(data, m, p, &vertexCount, &indexCount, &stageBytes);
            if (result != XUD_OK) {
                return result;
            }
            meshCount++;
        }
    }
    if (meshCount == 0u) {
        return XUD_INVALID_ARG; /* every mesh of the document carries zero primitives, so nothing can be drawn */
    }
    *outMeshCount = meshCount;
    *outMaterialCount = (uint32_t)data->materials_count + 1u; /* the default every untextured primitive draws with */
    return XUD_OK;
}

/* The five stream widths, in the order the stage lays them out. The array exists because the
   width macros are token-pasted and a loop variable cannot be pasted into one; the assert below
   is what keeps the array in step with the stride the batch and the mesh both already state. */
static const uint32_t xudModelStreamSize[XUD_BATCH_STREAM_COUNT]
    = { XUD_BATCH_STREAM_POS_SIZE, XUD_BATCH_STREAM_UV_SIZE, XUD_BATCH_STREAM_COLOR_SIZE, XUD_BATCH_STREAM_NORMAL_SIZE };

static_assert((XUD_BATCH_STREAM_POS_SIZE + XUD_BATCH_STREAM_UV_SIZE + XUD_BATCH_STREAM_COLOR_SIZE + XUD_BATCH_STREAM_NORMAL_SIZE)
        == XUD_BATCH_VERTEX_SIZE,
    "xud_model.c: the stage's five widths are the batch's stride, read rather than remembered");

#define XUD_MODEL_COLOR_CHUNK                                                                                                              \
    256u /* vertices of COLOR_0 unpacked at a time, so the conversion needs a stack row rather than a second block */

static_assert(XUD_MODEL_COLOR_CHUNK > 0u, "xud_model.c: the color chunk must be at least one vertex");

XudResult xud_model_stage_bind(void* block, uint32_t blockBytes, uint32_t vertexCount, uint32_t indexCount, XudModelStage* outStage) {
    uint32_t offset = 0u;
    uint32_t s;

    if (block == NULL || outStage == NULL) {
        return XUD_INVALID_ARG;
    }
    if (vertexCount == 0u) {
        return XUD_INVALID_ARG;
    }
    if (indexCount < 3u || (indexCount % 3u) != 0u) {
        return XUD_INVALID_ARG;
    }
    if (blockBytes < XUD_MODEL_STAGE_BYTES(vertexCount, indexCount)) {
        return XUD_INVALID_ARG;
    }
    outStage->vertexCount = vertexCount;
    outStage->indexCount = indexCount;
    for (s = 0u; s < XUD_BATCH_STREAM_COUNT; s++) {
        outStage->offset[s] = offset;
        offset += vertexCount * xudModelStreamSize[s];
    }
    outStage->offset[XUD_MESH_INDEX_SLOT] = offset;
    offset += indexCount * XUD_BATCH_INDEX_SIZE;
    outStage->stageBytes = offset;
    outStage->block = block;
    outStage->pos = (float*)((uint8_t*)block + outStage->offset[XUD_MODEL_SLOT_POS]);
    outStage->texcoords = (float*)((uint8_t*)block + outStage->offset[XUD_MODEL_SLOT_UV]);
    outStage->color = (uint8_t*)((uint8_t*)block + outStage->offset[XUD_MODEL_SLOT_COLOR]);
    outStage->normal = (float*)((uint8_t*)block + outStage->offset[XUD_MODEL_SLOT_NORMAL]);
    outStage->index = (uint32_t*)((uint8_t*)block + outStage->offset[XUD_MODEL_INDEX_SLOT]);
    return XUD_OK;
}

/* The read that fills a bound stage. The gate runs again first, and the stage's own counts are
   compared against the gate's, so a stage bound for one primitive cannot be used to read another
   one's arrays - which is the same rule the mesh rung's create states about its five arrays, one
   level up. The three optional attributes are filled with the mesh description's documented defa
   ults rather than left as NULL because the defaults belong to the stream and this rung is what
   writes the stream; a caller who wants a different default wants a different mesh rung. */
XudResult xud_model_read(
    const cgltf_data* data, uint32_t meshIndex, uint32_t primitiveIndex, const XudModelStage* stage, XudMeshData* outMesh) {
    const cgltf_primitive* prim;
    const cgltf_accessor* accessor[XUD_MESH_BUFFER_COUNT];
    const cgltf_accessor* src;
    uint32_t count[XUD_MESH_BUFFER_COUNT];
    uint32_t vertexCount = 0u;
    uint32_t indexCount = 0u;
    uint32_t stageBytes = 0u;
    uint32_t colorComponents = 0u;
    uint32_t answered = 0u;
    uint32_t v = 0u;
    uint32_t i = 0u;
    uint32_t k = 0u;
    float chunk[XUD_MODEL_COLOR_CHUNK * 4u];
    float value;
    XudResult result;

    if (data == NULL || stage == NULL || outMesh == NULL) {
        return XUD_INVALID_ARG;
    }
    result = xud_model_gate(data, meshIndex, primitiveIndex, &vertexCount, &indexCount, &stageBytes);
    if (result != XUD_OK) {
        return result;
    }
    if (stage->vertexCount != vertexCount || stage->indexCount != indexCount) {
        return XUD_INVALID_ARG; /* the stage was bound for a different primitive than this read names */
    }
    prim = &data->meshes[meshIndex].primitives[primitiveIndex];
    result = xud_model_primitive_map(prim, accessor, count);
    if (result != XUD_OK) {
        return result;
    }

    src = accessor[XUD_MODEL_SLOT_POS];
    answered = (uint32_t)cgltf_accessor_unpack_floats(src, stage->pos, (cgltf_size)(vertexCount * 3u));
    if (answered < (vertexCount * 3u)) {
        return XUD_INVALID_ARG;
    }
    src = accessor[XUD_MODEL_SLOT_UV];
    if (src != NULL) {
        answered = (uint32_t)cgltf_accessor_unpack_floats(src, stage->texcoords, (cgltf_size)(vertexCount * 2u));
        if (answered < (vertexCount * 2u)) {
            return XUD_INVALID_ARG;
        }
    } else {
        for (v = 0u; v < (vertexCount * 2u); v++) {
            stage->texcoords[v] = 0.0f;
        }
    }
    src = accessor[XUD_MODEL_SLOT_NORMAL];
    if (src != NULL) {
        answered = (uint32_t)cgltf_accessor_unpack_floats(src, stage->normal, (cgltf_size)(vertexCount * 3u));
        if (answered < (vertexCount * 3u)) {
            return XUD_INVALID_ARG;
        }
    } else {
        for (v = 0u; v < vertexCount; v++) {
            stage->normal[(v * 3u)] = 0.0f;
            stage->normal[(v * 3u) + 1u] = 0.0f;
            stage->normal[(v * 3u) + 2u] = 1.0f;
        }
    }
    src = accessor[XUD_MODEL_SLOT_COLOR];
    if (src != NULL) {
        colorComponents = (src->type == cgltf_type_vec4) ? 4u : 3u;
        for (v = 0u; v < vertexCount; v += XUD_MODEL_COLOR_CHUNK) {
            uint32_t batch = (vertexCount - v);
            if (batch > XUD_MODEL_COLOR_CHUNK) {
                batch = XUD_MODEL_COLOR_CHUNK;
            }
            answered = (uint32_t)cgltf_accessor_unpack_floats(src, chunk, (cgltf_size)(batch * colorComponents));
            if (answered < (batch * colorComponents)) {
                return XUD_INVALID_ARG;
            }
            for (i = 0u; i < batch; i++) {
                for (k = 0u; k < 4u; k++) {
                    value = (k < colorComponents) ? chunk[(i * colorComponents) + k] : 1.0f;
                    if (value < 0.0f) {
                        value = 0.0f;
                    }
                    if (value > 1.0f) {
                        value = 1.0f;
                    }
                    stage->color[((v + i) * 4u) + k] = (uint8_t)(value * 255.0f);
                }
            }
        }
    } else {
        for (v = 0u; v < vertexCount; v++) {
            stage->color[(v * 4u)] = 255u;
            stage->color[(v * 4u) + 1u] = 255u;
            stage->color[(v * 4u) + 2u] = 255u;
            stage->color[(v * 4u) + 3u] = 255u;
        }
    }
    answered = (uint32_t)cgltf_accessor_unpack_indices(prim->indices, stage->index, XUD_BATCH_INDEX_SIZE, (cgltf_size)indexCount);
    if (answered < indexCount) {
        return XUD_INVALID_ARG;
    }
    for (i = 0u; i < indexCount; i++) {
        if (stage->index[i] >= vertexCount) {
            return XUD_INVALID_ARG; /* an index past the vertices is UB at the draw, so it is refused here */
        }
    }
    outMesh->vertexCount = vertexCount;
    outMesh->indexCount = indexCount;
    outMesh->pos = stage->pos;
    outMesh->texcoords = stage->texcoords;
    outMesh->color = stage->color;
    outMesh->normal = stage->normal;
    outMesh->index = stage->index;
    return XUD_OK;
}

/* Fold one stage's positions into a bounding box. Either in-corner may be NULL, and that is the
   "no box yet" state rather than a corner at the origin: seeding from a zero corner would make a
   model whose geometry sits entirely at negative coordinates get a box that does not contain it.
   The box is folded here rather than walked from the buffers later because the stage is gone by
   the time a caller could ask for it, and because raylib's GetModelBoundingBox is a query that
   must answer in constant time. */
XudResult xud_model_bounds(
    const XudModelStage* stage, const XudVector3* inMin, const XudVector3* inMax, XudVector3* outMin, XudVector3* outMax) {
    float low[3];
    float high[3];
    uint32_t v;
    uint32_t c;

    if (stage == NULL || outMin == NULL || outMax == NULL) {
        return XUD_INVALID_ARG;
    }
    if (stage->vertexCount == 0u) {
        return XUD_INVALID_ARG;
    }
    if (inMin != NULL && inMax != NULL) {
        low[0] = inMin->x;
        low[1] = inMin->y;
        low[2] = inMin->z;
        high[0] = inMax->x;
        high[1] = inMax->y;
        high[2] = inMax->z;
    } else {
        for (c = 0u; c < 3u; c++) {
            low[c] = stage->pos[c];
            high[c] = stage->pos[c];
        }
    }
    for (v = 0u; v < stage->vertexCount; v++) {
        for (c = 0u; c < 3u; c++) {
            if (stage->pos[(v * 3u) + c] < low[c]) {
                low[c] = stage->pos[(v * 3u) + c];
            }
            if (stage->pos[(v * 3u) + c] > high[c]) {
                high[c] = stage->pos[(v * 3u) + c];
            }
        }
    }
    outMin->x = low[0];
    outMin->y = low[1];
    outMin->z = low[2];
    outMax->x = high[0];
    outMax->y = high[1];
    outMax->z = high[2];
    return XUD_OK;
}

/* The directory prefix a document's buffer URIs resolve against. A path with no slash is not refu
   sed, it answers the empty prefix, because a glTF shipped in the working directory is the normal
   case for a test rather than an accident of the caller. */
XudResult xud_model_path_dir(char* outDir, uint32_t outCapacity, uint32_t* outLength, const char* path) {
    uint32_t length = 0u;
    uint32_t prefix = 0u;

    if (outDir == NULL || outLength == NULL || path == NULL) {
        return XUD_INVALID_ARG;
    }
    *outLength = 0u;
    if (path[0] == '\0') {
        return XUD_INVALID_ARG;
    }
    if (outCapacity == 0u) {
        return XUD_UNSUPPORTED;
    }
    for (; path[length] != '\0'; length++) {
        if (path[length] == '/') {
            prefix = length + 1u;
        }
    }
    if (prefix >= outCapacity) {
        return XUD_UNSUPPORTED;
    }
    for (length = 0u; length < prefix; length++) {
        outDir[length] = path[length];
    }
    outDir[prefix] = '\0';
    *outLength = prefix;
    return XUD_OK;
}

/* cgltf's answer as this library's. One row per code and no fallthrough: a code outside the enum
   is XUD_INTERNAL_ERROR rather than XUD_OK, because an unclassified refusal is an invariant of the
   library above that broke, not a document the caller can fix. */
XudResult xud_model_result_from_cgltf(cgltf_result result) {
    switch (result) {
    case cgltf_result_success:
        return XUD_OK;
    case cgltf_result_data_too_short:
        return XUD_MALFORMED;
    case cgltf_result_unknown_format:
        return XUD_MALFORMED;
    case cgltf_result_invalid_json:
        return XUD_MALFORMED;
    case cgltf_result_invalid_gltf:
        return XUD_MALFORMED;
    case cgltf_result_invalid_options:
        return XUD_INVALID_ARG;
    case cgltf_result_file_not_found:
        return XUD_NOT_FOUND;
    case cgltf_result_io_error:
        return XUD_INTERNAL_ERROR;
    case cgltf_result_out_of_memory:
        return XUD_OUT_OF_MEMORY;
    case cgltf_result_legacy_gltf:
        return XUD_UNSUPPORTED; /* a 1.0 asset: the accessors this rung reads were never a thing it could have */
    case cgltf_result_max_enum:
        return XUD_INTERNAL_ERROR;
    default:
        return XUD_INTERNAL_ERROR;
    }
}

/* The accessor one rung above reaches instead of the slot array. */
XudResult xud_model_get(const XudHandleTable* table, XudHandle handle, const XudModelRecord** outRecord) {
    const XudModelRecord* record = NULL;
    void* slotRecord = NULL;

    if (outRecord == NULL) {
        return XUD_INVALID_ARG;
    }
    *outRecord = NULL;
    if (table == NULL) {
        return XUD_INVALID_ARG;
    }
    if (xud_handle_validate(table, handle, XUD_HANDLE_KIND_MODEL) != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    if (xud_handle_record_get(table, XUD_HANDLE_KIND_MODEL, handle.index, &slotRecord) != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    record = (const XudModelRecord*)slotRecord;
    if (record == NULL) {
        return XUD_INTERNAL_ERROR; /* a live slot whose record field is still the sentinel, which the open's own order forbids */
    }
    *outRecord = record;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The driver half: the open, the close, and the teardown rung
 *-----------------------------------------------------------------------------------------------
 *
 * The order is the order that leaves nothing reachable and unfilled: the gate first, so no slot is
 *   spent for a document that will be refused; then the MODEL slot; then the record and its one
 *   handle block; then the MATERIAL slots, because a mesh's record names the material it draws
 *   with and the material must exist before the mesh that names it; then per primitive its stage,
 *   its read, its MESH slot through the mesh rung below, and its stage freed. Every refusal below
 *   that point closes exactly what the steps above created, in the reverse order, and the mesh
 *   rung's own note about the staging block is the reason the stage is host memory rather than a
 *   second device block.
 *
 * The rollback counts rather than walking the record, and that is the whole reason the record's
 *   counts are not trusted during a failure: a record mid-open has a meshCount that is the docu
 *   ment's total rather than the number of slots spent so far, so a rollback that walked it would
 *   close slots that were never allocated.
 */

/* The rollback both halves share: close the slots that were actually created, in the reverse order
   the open created them. The return codes are dropped on the floor on purpose - a rollback cannot
   report, and a mesh or a material whose close refused is a leak the teardown rung's own walk will
   find rather than a corruption this walk can cause. */
static void xud_model_retire(XudHandleTable* table, XudModelRecord* record, XudLoader* loader, const XudDevice* device, XudAllocator* alloc,
    uint32_t meshesCreated, uint32_t materialsCreated) {
    uint32_t i;

    for (i = 0u; i < meshesCreated; i++) {
        (void)xud_mesh_close(table, record->mesh[i], loader, device, alloc);
    }
    for (i = 0u; i < materialsCreated; i++) {
        (void)xud_material_close(table, record->material[i]);
    }
}

XudResult xud_model_open(XudHandle* outHandle, XudHandleTable* table, XudLoader* loader, const XudDevice* device, const XudDeviceCaps* caps,
    XudAllocator* alloc, const cgltf_data* data) {
    XudModelRecord* record = NULL;
    XudModelStage stage;
    XudMeshData mesh;
    XudVector3 boundsMin = { 0.0f, 0.0f, 0.0f };
    XudVector3 boundsMax = { 0.0f, 0.0f, 0.0f };
    XudHandle meshHandle;
    void* stageBlock = NULL;
    uint32_t meshCount = 0u;
    uint32_t materialCount = 0u;
    uint32_t vertexCount = 0u;
    uint32_t indexCount = 0u;
    uint32_t stageBytes = 0u;
    uint32_t handleBytes = 0u;
    uint32_t materialsCreated = 0u;
    uint32_t meshesCreated = 0u;
    uint32_t flat = 0u;
    uint32_t m;
    uint32_t p;
    uint32_t firstBox = 1u;
    XudResult result = XUD_INTERNAL_ERROR;

    if (outHandle == NULL || table == NULL || loader == NULL || device == NULL || caps == NULL || alloc == NULL) {
        return XUD_INVALID_ARG;
    }
    if (!xud_loader_is_open(loader)) {
        return XUD_UNSUPPORTED;
    }
    if (device->device == VK_NULL_HANDLE) {
        return XUD_INVALID_ARG;
    }
    if (!xud_loader_device_bound(loader, device->device)) {
        return XUD_UNSUPPORTED;
    }
    result = xud_model_check(data, &meshCount, &materialCount);
    if (result != XUD_OK) {
        return result;
    }
    result = xud_handle_alloc(outHandle, table, XUD_HANDLE_KIND_MODEL);
    if (result != XUD_OK) {
        return result;
    }
    record = (XudModelRecord*)calloc(1, sizeof(XudModelRecord));
    if (record == NULL) {
        (void)xud_handle_free(table, *outHandle);
        return XUD_OUT_OF_MEMORY;
    }
    handleBytes = ((2u * meshCount) + materialCount) * (uint32_t)sizeof(XudHandle);
    record->handleBlock = calloc(1, handleBytes);
    if (record->handleBlock == NULL) {
        free(record);
        (void)xud_handle_free(table, *outHandle);
        return XUD_OUT_OF_MEMORY;
    }
    record->mesh = (XudHandle*)((uint8_t*)record->handleBlock);
    record->meshMaterial = (XudHandle*)((uint8_t*)record->handleBlock + (meshCount * (uint32_t)sizeof(XudHandle)));
    record->material = (XudHandle*)((uint8_t*)record->handleBlock + ((2u * meshCount) * (uint32_t)sizeof(XudHandle)));
    record->meshCount = meshCount;
    record->materialCount = materialCount;

    for (m = 0u; m < materialCount; m++) {
        uint32_t variant = XUD_BLEND_OPAQUE;
        if (m < ((uint32_t)data->materials_count)) {
            if (data->materials[m].alpha_mode == cgltf_alpha_mode_blend) {
                variant = XUD_BLEND_TRANSPARENT;
            }
        }
        result = xud_material_open(&record->material[m], table, variant);
        if (result != XUD_OK) {
            goto fail;
        }
        materialsCreated++;
    }
    for (m = 0u; m < (uint32_t)data->meshes_count; m++) {
        for (p = 0u; p < (uint32_t)data->meshes[m].primitives_count; p++) {
            result = xud_model_gate(data, m, p, &vertexCount, &indexCount, &stageBytes);
            if (result != XUD_OK) {
                goto fail;
            }
            stageBlock = calloc(1, stageBytes);
            if (stageBlock == NULL) {
                result = XUD_OUT_OF_MEMORY;
                goto fail;
            }
            result = xud_model_stage_bind(stageBlock, stageBytes, vertexCount, indexCount, &stage);
            if (result != XUD_OK) {
                goto fail;
            }
            result = xud_model_read(data, m, p, &stage, &mesh);
            if (result != XUD_OK) {
                goto fail;
            }
            if (firstBox == 1u) {
                result = xud_model_bounds(&stage, NULL, NULL, &boundsMin, &boundsMax);
            } else {
                result = xud_model_bounds(&stage, &boundsMin, &boundsMax, &boundsMin, &boundsMax);
            }
            if (result != XUD_OK) {
                goto fail;
            }
            firstBox = 0u;
            result = xud_mesh_open(&meshHandle, table, loader, device, caps, alloc, &mesh);
            if (result != XUD_OK) {
                goto fail;
            }
            record->mesh[flat] = meshHandle;
            if (data->meshes[m].primitives[p].material != NULL) {
                cgltf_size materialIndex = cgltf_material_index(data, data->meshes[m].primitives[p].material);
                if (materialIndex >= (cgltf_size)data->materials_count) {
                    materialIndex = (cgltf_size)(materialCount - 1u);
                }
                record->meshMaterial[flat] = record->material[materialIndex];
            } else {
                record->meshMaterial[flat] = record->material[materialCount - 1u];
            }
            flat++;
            meshesCreated++;
            free(stageBlock);
            stageBlock = NULL;
        }
    }
    record->boundsMin = boundsMin;
    record->boundsMax = boundsMax;
    result = xud_handle_record_set(table, XUD_HANDLE_KIND_MODEL, outHandle->index, record);
    if (result != XUD_OK) {
        goto fail;
    }
    return XUD_OK;

fail:
    if (stageBlock != NULL) {
        free(stageBlock);
    }
    (void)xud_handle_record_set(table, XUD_HANDLE_KIND_MODEL, outHandle->index, NULL);
    xud_model_retire(table, record, loader, device, alloc, meshesCreated, materialsCreated);
    if (record != NULL) {
        free(record->handleBlock);
        free(record);
    }
    (void)xud_handle_free(table, *outHandle);
    return (result != XUD_OK) ? result : XUD_INTERNAL_ERROR;
}

/* Release one live MODEL slot. The record's field is cleared BEFORE the record is freed, which is
   the material rung's rule one level up: a free slot must not still own a record, or this path and
   the teardown rung's own walk would both free it. The meshes are closed before the materials bec
   ause a mesh's record names the material it draws with, so a material that is still named by a
   live mesh is a state a caller could draw through. */
XudResult xud_model_close(XudHandleTable* table, XudHandle handle, XudLoader* loader, const XudDevice* device, XudAllocator* alloc) {
    XudModelRecord* record = NULL;
    void* slotRecord = NULL;
    uint32_t i;
    XudResult result;

    if (table == NULL || loader == NULL || device == NULL || alloc == NULL) {
        return XUD_INVALID_ARG;
    }
    if (!xud_loader_is_open(loader)) {
        return XUD_UNSUPPORTED;
    }
    if (device->device == VK_NULL_HANDLE) {
        return XUD_INVALID_ARG;
    }
    if (!xud_loader_device_bound(loader, device->device)) {
        return XUD_UNSUPPORTED;
    }
    if (xud_handle_validate(table, handle, XUD_HANDLE_KIND_MODEL) != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    if (xud_handle_record_get(table, XUD_HANDLE_KIND_MODEL, handle.index, &slotRecord) != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    record = (XudModelRecord*)slotRecord;
    if (record == NULL) {
        return XUD_INTERNAL_ERROR;
    }
    for (i = 0u; i < record->meshCount; i++) {
        result = xud_mesh_close(table, record->mesh[i], loader, device, alloc);
        if (result != XUD_OK) {
            return result;
        }
    }
    for (i = 0u; i < record->materialCount; i++) {
        result = xud_material_close(table, record->material[i]);
        if (result != XUD_OK) {
            return result;
        }
    }
    (void)xud_handle_record_set(table, XUD_HANDLE_KIND_MODEL, handle.index, NULL);
    free(record->handleBlock);
    free(record);
    (void)xud_handle_free(table, handle);
    return XUD_OK;
}

/* The rung above the handle table's own close, and above the mesh and the material rungs': it runs
   FIRST of the four, so the MESH and MATERIAL slots a model created are closed through their own
   rungs here rather than left for their close_all to find after the model's record is gone. Every
   MODEL slot's record is closed, live or released: xud_handle_free() leaves the record field as it
   was, which is what makes a released slot's record reachable here and a double free impossible in
   the mesh rung's walk below. */
XudResult xud_model_close_all(XudHandleTable* table, XudLoader* loader, const XudDevice* device, XudAllocator* alloc, uint32_t* outClosed) {
    XudModelRecord* record = NULL;
    uint32_t closed = 0u;
    uint32_t i;
    uint32_t k;
    XudResult result;
    void* slotRecord = NULL;

    if (table == NULL || loader == NULL || device == NULL || alloc == NULL || outClosed == NULL) {
        return XUD_INVALID_ARG;
    }
    *outClosed = 0u;
    if (table->slots[XUD_HANDLE_KIND_MODEL] == NULL) {
        return XUD_OK;
    }
    if (!xud_loader_is_open(loader)) {
        return XUD_UNSUPPORTED;
    }
    if (device->device == VK_NULL_HANDLE) {
        return XUD_INVALID_ARG;
    }
    if (!xud_loader_device_bound(loader, device->device)) {
        return XUD_UNSUPPORTED;
    }
    for (i = 0u; i < table->slotCount[XUD_HANDLE_KIND_MODEL]; i++) {
        if (xud_handle_record_get(table, XUD_HANDLE_KIND_MODEL, i, &slotRecord) != XUD_OK) {
            return XUD_INVALID_ARG;
        }
        record = (XudModelRecord*)slotRecord;
        if (record == NULL) {
            continue;
        }
        for (k = 0u; k < record->meshCount; k++) {
            result = xud_mesh_close(table, record->mesh[k], loader, device, alloc);
            if (result != XUD_OK) {
                return result;
            }
        }
        for (k = 0u; k < record->materialCount; k++) {
            result = xud_material_close(table, record->material[k]);
            if (result != XUD_OK) {
                return result;
            }
        }
        if (xud_handle_record_set(table, XUD_HANDLE_KIND_MODEL, i, NULL) != XUD_OK) {
            return XUD_INVALID_ARG;
        }
        free(record->handleBlock);
        free(record);
        closed++;
    }
    *outClosed = closed;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The public pair, and the two public queries
 *-----------------------------------------------------------------------------------------------
 *
 * This is the only place in the tree that borrows cgltf's own three steps, and the order is the
 *   document's: parse, then load the buffers, then validate. The buffers are loaded before the val
 *   idate because cgltf_validate() checks the accessor ranges against the bytes a document actually
 *   has, and a document whose buffers were never loaded has no bytes to check against - which is
 *   also why the gate's own "buffer data is NULL" refusal is XUD_INVALID_ARG rather than a silent
 *   read of nothing. The directory prefix is derived here rather than taken from the caller because
 *   a glTF's external buffers are relative to the file they were shipped with, not to the working
 *   directory, and a caller who passes a path has already said which file.
 *
 * The document is freed on every path out, including the success path, and that is the whole point
 *   of the stage: the mesh rung's create copied the bytes the accessors pointed at into the mesh's
 *   mapped arena, so nothing below this function can still be reading cgltf's arrays. A model is
 *   therefore a list of the objects the rungs below own and never a live document, which is what
 *   makes raylib's Model-with-a-mesh-pair shape the right one to refuse at xud.h.
 */

XudResult xud_load_model(XudHandle* outHandle, XudContext* context, const char* path) {
    cgltf_options options;
    cgltf_data* document = NULL;
    cgltf_result cgResult = cgltf_result_invalid_options;
    char dir[XUD_MODEL_PATH_MAX];
    uint32_t dirLength = 0u;
    XudResult result = XUD_INTERNAL_ERROR;

    if (outHandle == NULL || context == NULL || path == NULL) {
        return XUD_INVALID_ARG;
    }
    if (path[0] == '\0') {
        return XUD_INVALID_ARG;
    }
    if (context->handleTable == NULL || context->loader == NULL || context->device == NULL || context->physicalDevice == NULL
        || context->allocator == NULL) {
        return XUD_UNSUPPORTED;
    }
    memset(&options, 0, sizeof(options));
    options.type = cgltf_file_type_invalid; /* autodetect the container rather than trust the extension */
    cgResult = cgltf_parse_file(&options, path, &document);
    if (cgResult != cgltf_result_success) {
        if (document != NULL) {
            cgltf_free(document);
        }
        return xud_model_result_from_cgltf(cgResult);
    }
    result = xud_model_path_dir(dir, XUD_MODEL_PATH_MAX, &dirLength, path);
    if (result != XUD_OK) {
        cgltf_free(document);
        return result;
    }
    cgResult = cgltf_load_buffers(&options, document, (dirLength > 0u) ? dir : NULL);
    if (cgResult != cgltf_result_success) {
        cgltf_free(document);
        return xud_model_result_from_cgltf(cgResult);
    }
    cgResult = cgltf_validate(document);
    if (cgResult != cgltf_result_success) {
        cgltf_free(document);
        return xud_model_result_from_cgltf(cgResult);
    }
    result = xud_model_open(
        outHandle, context->handleTable, context->loader, context->device, &context->physicalDevice->caps, context->allocator, document);
    cgltf_free(document);
    return result;
}

XudResult xud_close_model(XudContext* context, XudHandle handle) {
    if (context == NULL) {
        return XUD_INVALID_ARG;
    }
    if (context->handleTable == NULL || context->loader == NULL || context->device == NULL || context->allocator == NULL) {
        return XUD_UNSUPPORTED;
    }
    return xud_model_close(context->handleTable, handle, context->loader, context->device, context->allocator);
}

XudResult xud_get_model_mesh_count(uint32_t* outCount, XudContext* context, XudHandle handle) {
    const XudModelRecord* record = NULL;
    XudResult result;

    if (outCount == NULL || context == NULL) {
        return XUD_INVALID_ARG;
    }
    *outCount = 0u;
    if (context->handleTable == NULL) {
        return XUD_UNSUPPORTED;
    }
    result = xud_model_get(context->handleTable, handle, &record);
    if (result != XUD_OK) {
        return result;
    }
    *outCount = record->meshCount;
    return XUD_OK;
}

XudResult xud_get_model_bounding_box(XudVector3* outMin, XudVector3* outMax, XudContext* context, XudHandle handle) {
    const XudModelRecord* record = NULL;
    XudResult result;

    if (outMin == NULL || outMax == NULL || context == NULL) {
        return XUD_INVALID_ARG;
    }
    *outMin = (XudVector3) { 0.0f, 0.0f, 0.0f };
    *outMax = (XudVector3) { 0.0f, 0.0f, 0.0f };
    if (context->handleTable == NULL) {
        return XUD_UNSUPPORTED;
    }
    result = xud_model_get(context->handleTable, handle, &record);
    if (result != XUD_OK) {
        return result;
    }
    *outMin = record->boundsMin;
    *outMax = record->boundsMax;
    return XUD_OK;
}
