/***********************************************************************************************
 *   xud_model.h - the model rung: the record behind a MODEL-kind slot of the handle layer, the
 *                 cgltf document above it, and the load/close pair xud.h declares on top of that
 *
 *   Owns : XudModelRecord - the object record the handle layer's MODEL-kind slots have been
 *          waiting for - plus the pure halves above them (xud_model_check(), xud_model_gate(),
 *          xud_model_stage_bind(), xud_model_read(), xud_model_bounds(), xud_model_path_dir(),
 *          xud_model_result_from_cgltf()), the accessor (xud_model_get()), the driver halves
 *          (xud_model_open(), xud_model_close()) and the teardown rung above the table's own
 *          close (xud_model_close_all()).
 *   Layer: above the mesh and the material rungs and below xud.h. It is the THIRD rung that fil
 *          ls a slot's record field and the first rung that owns other handles: a model is not
 *          an object of the driver's, it is a list of the objects the rungs below it own - one
 *          MESH slot per glTF primitive and one MATERIAL slot per glTF material - which is why
 *          this rung's close calls the two rungs below it rather than a driver row of its own.
 *
 *   What the rung owns and what it borrows: it owns the record, the three handle arrays inside
 *   it and the two bounding-box corners. It borrows the handle table (its own slot, and the ME
 *   SH and MATERIAL slots it allocates), the mesh rung (upload and teardown of every primitive
 *   it read), the material rung (upload and teardown of every material it read), and the loader,
 *   the device, the caps and the allocator, which it hands DOWN rather than using: nothing in
 *   this file calls a Vulkan entry point at all. cgltf is the one library it borrows, and it is
 *   borrowed at this rung only - the parse, the buffer load and the validate all happen above,
 *   in xud_load_model(), so every rung here takes an already-loaded document.
 *
 *   Why the stage is a host block and not a device one: cgltf hands back floats and uint32s in
 *   host arrays, and the mesh rung's own create is what copies a caller's arrays into the mesh's
 *   mapped arena. A second staging block on the device would be a transfer, a fence and a sub
 *   mit for bytes that are already on their way into a block - the mesh rung's note is the re
 *   ason they are. The stage is therefore transient: allocated per primitive, filled, uploaded,
 *   freed, and never named by the record. What the record keeps out of the stage is the one
 *   thing that cannot be recomputed after the stage is gone - the bounding box, which is ray
 *   lib's GetModelBoundingBox and is folded at the read rather than walked from the buffers
 *   later.
 *
 *   What the record does NOT hold, and why that is the honest state: no node, no skin, no anima
 *   tion, no camera, no light. This tree has one camera (AGENTS.md: 3D only, one camera, one
 *   depth buffer, one render pass), no per-object transform (the model matrix is the camera +
 *   transform step's and is identity today) and no skinning (no bone matrices, no second pipel
 *   ine to read them through), so a glTF node hierarchy would be parsed and then ignored, and
 *   the gate below refuses a document whose geometry hangs off anything other than a mesh's own
 *   primitives rather than pretending to have placed it.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#ifndef XUD_XUD_MODEL_H_
#define XUD_XUD_MODEL_H_

#include "cgltf.h" /* Required for: cgltf_data, cgltf_result, cgltf_alpha_mode - the document this rung reads */
#include "xud.h" /* Required for: XudResult, XudHandle, XudVector3, XudMeshData, XUD_HANDLE_KIND_MODEL - the public surface this rung is the half of */
#include "xud_allocator.h" /* Required for: XudAllocator, handed down to the mesh rung's block */
#include "xud_batch.h" /* Required for: XUD_BATCH_STREAM_*, XUD_BATCH_VERTEX_SIZE, XUD_BATCH_INDEX_SIZE - the four streams a primitive's accessors are read into */
#include "xud_descriptor.h" /* Required for: XudDescriptor, XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE - set 1's sampler array a model's map occupies one entry of */
#include "xud_device.h" /* Required for: XudDevice, the VkDevice every mesh this rung uploads belongs to */
#include "xud_handle.h" /* Required for: XudHandleTable, xud_handle_validate/alloc/free, xud_handle_record_get/set - the slot this record lives in */
#include "xud_loader.h" /* Required for: XudLoader, handed down to the mesh rung */
#include "xud_material.h" /* Required for: xud_material_open/close - the MATERIAL slots this rung creates and tears down */
#include "xud_mesh.h" /* Required for: XUD_MESH_BUFFER_COUNT, XUD_MESH_MAX_VERTICES, xud_mesh_open/close - the MESH slots this rung creates and tears down */
#include "xud_physical.h" /* Required for: XudDeviceCaps, handed down to the mesh rung's caps gate */
#include "xud_texture.h" /* Required for: XudTextureRecord, XUD_TEXTURE_FILTER_*, XUD_TEXTURE_ADDRESS_*, xud_texture_open/close/get - the TEXTURE slots a map occupies */

#include <assert.h> /* Required for: static_assert, the stage's padding-free layout below */
#include <stddef.h> /* Required for: size_t, cgltf_size's own */
#include <stdint.h>

/*-----------------------------------------------------------------------------------------------
 * The stage: one primitive's five host arrays, in one block
 *-----------------------------------------------------------------------------------------------
 *
 * The five are the mesh's five and the batch's four-plus-index by construction, and the point of
 *   reading the widths off xud_batch.h rather than naming them here is that the stage's bytes are
 *   handed to xud_mesh_open() as an XudMeshData, whose arrays are the batch's four streams. The
 *   block is one allocation rather than five because the repo's rule is "allocate once, suballoca
 *   te always" and a primitive is five arrays of one object. No padding appears anywhere in the
 *   layout below, and that is a fact this assert proves rather than a hope: every stream's elemen
 *   t width is a multiple of four and the index width is four, so every offset is a multiple of
 *   four whatever the vertex count is, which is what keeps every float array naturally aligned
 *   without a pad byte in sight.
 */

typedef struct XudModelStage {
    uint32_t vertexCount; /* vertices this primitive holds, the one count the four streams are sized from */
    uint32_t indexCount; /* indices it holds - a whole number of triangles, see xud_model_gate() */
    uint32_t stageBytes; /* the block's total, what xud_model_gate() asked for */
    uint32_t offset[XUD_MESH_BUFFER_COUNT]; /* where each of the five starts inside block */

    void* block; /* the ONE host allocation the five live in, freed by this rung's own open, never stored in a record */
    float* pos; /* vertexCount*3, into block */
    float* texcoords; /* vertexCount*2, into block */
    uint8_t* color; /* vertexCount*4, into block */
    float* normal; /* vertexCount*3, into block */
    uint32_t* index; /* indexCount, into block */
} XudModelStage;

static_assert(XUD_MESH_BUFFER_COUNT == (XUD_BATCH_STREAM_COUNT + 1u),
    "xud_model.h: the stage's five rows are the batch's four streams plus the one index buffer");
static_assert((XUD_BATCH_STREAM_POS_SIZE % 4u) == 0u && (XUD_BATCH_STREAM_UV_SIZE % 4u) == 0u && (XUD_BATCH_STREAM_COLOR_SIZE % 4u) == 0u
        && (XUD_BATCH_STREAM_NORMAL_SIZE % 4u) == 0u && (XUD_BATCH_INDEX_SIZE % 4u) == 0u,
    "xud_model.h: every stream element is a whole number of four-byte words, so the stage needs no padding and a float array is aligned");
static_assert(XUD_BATCH_VERTEX_SIZE
        == (XUD_BATCH_STREAM_POS_SIZE + XUD_BATCH_STREAM_UV_SIZE + XUD_BATCH_STREAM_COLOR_SIZE + XUD_BATCH_STREAM_NORMAL_SIZE),
    "xud_model.h: the stage's stride is the four streams' widths, read rather than remembered");

/* One primitive's stage, in bytes: the four streams at the vertex count plus the index buffer at
   the index count, with no padding to add (see the assert above). */
#define XUD_MODEL_STAGE_BYTES(vertexCount, indexCount) ((XUD_BATCH_VERTEX_SIZE * (vertexCount)) + (XUD_BATCH_INDEX_SIZE * (indexCount)))

/*-----------------------------------------------------------------------------------------------
 * The rung's own bounds
 *-----------------------------------------------------------------------------------------------
 *
 * Both are this rung's ceiling rather than a driver fact, and both are stated against the handle
 *   layer's own bound rather than against an invented number: a model is a list of slots, and the
 *   handle layer refuses a slot past its capacity with XUD_OUT_OF_RESOURCES. Refusing the count
 *   here is that refusal stated before the first slot is spent, so a model that will not fit is
 *   not a model that half-fits and then reports.
 */

#define XUD_MODEL_MAX_MESHES 512u /* primitives per model, the MESH slots one load spends */
#define XUD_MODEL_MAX_MATERIALS 1024u /* one MATERIAL slot per document material plus the default every untextured primitive draws with */
#define XUD_MODEL_MAX_TEXTURES                                                                                                             \
    XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE /* one TEXTURE slot per distinct base-color image a material names, and each one holds an entry of set 1's sampler array, which is the real ceiling */
#define XUD_MODEL_PATH_MAX 1024u /* the longest model path xud_load_model() takes, and the longest directory prefix it derives */

#define XUD_MODEL_NO_IMAGE 0xFFFFFFFFu /* the image index a material that names no base-color image answers, past any document's count */

static_assert(XUD_MODEL_MAX_MESHES <= XUD_HANDLE_SLOT_MAX_CAPACITY,
    "xud_model.h: a model's meshes are slots of the handle layer, so its own bound cannot exceed the table's");
static_assert(XUD_MODEL_MAX_MATERIALS <= XUD_HANDLE_SLOT_MAX_CAPACITY,
    "xud_model.h: a model's materials are slots of the handle layer too, so its bound cannot exceed the table's");
static_assert(XUD_MODEL_MAX_TEXTURES <= XUD_HANDLE_SLOT_MAX_CAPACITY,
    "xud_model.h: a model's textures are slots of the handle layer too, so its bound cannot exceed the table's");
static_assert(XUD_MODEL_MAX_TEXTURES <= XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE,
    "xud_model.h: every texture a model holds a sampler-array entry, so its own bound cannot exceed the array's");
static_assert(XUD_MODEL_MAX_MESHES > 0u, "xud_model.h: a model must be able to name at least one mesh");
static_assert(XUD_MODEL_NO_IMAGE >= XUD_MODEL_MAX_TEXTURES,
    "xud_model.h: the no-image sentinel is past the texture bound, so an index names an image or names nothing");
static_assert(XUD_MODEL_PATH_MAX > 1u, "xud_model.h: a path must be at least one character plus a terminator");

/*-----------------------------------------------------------------------------------------------
 * What a MODEL-kind slot records
 *-----------------------------------------------------------------------------------------------
 *
 * Four handle arrays and one allocation behind them: `mesh` and `meshMaterial` are meshCount
 *   rows each, `material` is materialCount and `texture` is textureCount, so one block of
 *   (2*meshCount + materialCount + textureCount) handles carries all four and one free retires
 *   them. The block is a field of the record rather than a local because it is what the teardown
 *   rung has to free, and the four pointers are read-only views into it.
 *
 * `meshMaterial` is per-mesh rather than per-material because a draw names the mesh it draws and
 *   the material it draws it with, and the pair the renderer's wanted-material state holds is a
 *   handle and a variant, not an index into a list. `material` is the distinct set the load crea
 *   ted, and it is a separate array rather than a walk of `meshMaterial` because two primitives
 *   can share one material and a teardown that walked the per-mesh array would close the shared
 *   slot twice. `texture` is the same distinct-set rule one rung lower: two materials can name
 *   the same image, and the image decodes once rather than once per material. The textures are
 *   the model's to own because the load made them and the caller cannot name them - no public
 *   query hands the handles out - so the model's close retires them and their sampler-array
 *   entries with them, and a texture the model closed is a texture nothing else can still name.
 */

typedef struct XudModelRecord {
    uint32_t meshCount; /* MESH slots this load created, one per glTF primitive */
    uint32_t materialCount; /* MATERIAL slots this load created, one per document material plus the default */
    uint32_t textureCount; /* TEXTURE slots this load created, one per distinct base-color image a material names */
    uint8_t* handleBlock; /* the ONE allocation the four arrays below live in */
    XudHandle* mesh; /* meshCount handles: the MESH slot one primitive was uploaded into */
    XudHandle* meshMaterial; /* meshCount handles: the MATERIAL slot that mesh draws with */
    XudHandle* material; /* materialCount handles: the MATERIAL slots this load created */
    XudHandle* texture; /* textureCount handles: the TEXTURE slots the materials' maps sample */
    XudVector3 boundsMin; /* the model's bounding box, folded from every stage at the read */
    XudVector3 boundsMax;
} XudModelRecord;

static_assert(
    sizeof(XudHandle) == 12u, "xud_model.h: the handle arrays are 12-byte rows, so the block's arithmetic is the handle's own layout");
static_assert((sizeof(XudModelRecord) % 4u) == 0u, "xud_model.h: the record stays 4-byte aligned whatever the handle block's count is");

/*-----------------------------------------------------------------------------------------------
 * The layer's contract
 *-----------------------------------------------------------------------------------------------
 *
 * The split is the repo's: the document's shape, its byte layout, its stage arithmetic and its
 *   bounding box are pure and run with no driver present, and only xud_model_open(), xud_model_
 *   close() and xud_model_close_all() touch one - and even those call no Vulkan row of their own,
 *   because every driver object a model owns belongs to a mesh the mesh rung below creates.
 *
 * The two codes are the two halves of every refusal in this tree, read at a document rather than
 *   at a device: XUD_INVALID_ARG is "this document is broken, absent or contradicts itself" (no
 *   position, two accessors that disagree about the vertex count, an index past the vertices, a
 *   buffer whose bytes were never loaded) and XUD_UNSUPPORTED is "this document is well-formed
 *   and names something this rung does not read" (a LINE primitive, a Draco-compressed prim, a
 *   morph target, a sparse accessor, a tangent, a UV_1). A glTF that is well-formed but outside
 *   this library is therefore reported as unsupported rather than as a bad argument, which is
 *   what makes the answer a caller can act on the same in either case.
 */

/// Pure: the whole document gate, run before a slot is spent. Walks every mesh's every primitive
/// through xud_model_gate() and sums the counts, so the two out-counts are the two array sizes
/// xud_model_open() will need. XUD_INVALID_ARG on a NULL document or out-count, on a document
/// with no mesh or no primitive at all, on a primitive whose accessors disagree about the vertex
/// count, on a primitive with no POSITION, on a count pair past this rung's own bounds, on a
/// primitive whose buffer bytes were never loaded, or on a base-color sampler that spells a filter
/// or wrap value the glTF spec does not give; XUD_UNSUPPORTED when the asset block carries
/// no version, when the version does not start with '2', or when any primitive names a feature
/// this rung does not read. On XUD_OK *outMeshCount is the primitive total and *outMaterialCount
/// is the document's material count plus one.
XudResult xud_model_check(const cgltf_data* data, uint32_t* outMeshCount, uint32_t* outMaterialCount);

/// Pure: the gate of ONE primitive, and the size of its stage. Same answers as the walk above,
/// for one primitive rather than for the document, and it is the rung xud_model_read() runs again
/// before it reads - which is what makes the walk and the read refuse the same documents rather
/// than two lists that have to be kept in step. XUD_INVALID_ARG on a NULL document or out-count,
/// on an index pair outside the document, on a primitive with no POSITION, on accessors that disa
/// gree about the vertex count, on an index count that is not a whole number of triangles, on an
/// accessor that reads past its buffer view, or on a count pair past this rung's bounds; XUD_UN
/// SUPPORTED on a primitive whose mode is not TRIANGLES, whose indices are sparse or absent, who
/// se accessors are sparse, whose component type is not one this rung converts, or that carries
/// a Draco block, a morph target, a material mapping, a tangent, a joint set or a second UV.
XudResult xud_model_gate(const cgltf_data* data, uint32_t meshIndex, uint32_t primitiveIndex, uint32_t* outVertexCount,
    uint32_t* outIndexCount, uint32_t* outStageBytes);

/// Pure: bind a stage over a block the caller allocated. The offsets are the gate's, the five
/// pointers are the offsets read off `block`, and nothing is written - binding is arithmetic, so
/// a test can bind a block of its own and read the five pointers back. XUD_INVALID_ARG on a NULL
/// block or stage, on a zero vertex count, on an index count that is not a whole number of tria
/// ngles, or on a block smaller than the stage's own byte total.
XudResult xud_model_stage_bind(void* block, uint32_t blockBytes, uint32_t vertexCount, uint32_t indexCount, XudModelStage* outStage);

/// Pure: the read that fills a bound stage, and the XudMeshData that describes it. The stage's
/// counts are compared against the primitive's, so a stage bound for one primitive cannot be
/// used to read another one's arrays. Missing NORMAL, COLOR_0 and TEXCOORD_0 are filled with the
/// mesh description's documented defaults (+Z, opaque white, zero) rather than left as NULL, bec
/// ause the defaults belong to the stream and this rung is what writes the stream. XUD_INVALID_A
/// RG on a NULL document, stage or out-mesh, on an index pair outside the document, on a stage w
/// hose counts do not match the primitive's, or on an accessor that answered fewer elements than
/// its own count; XUD_UNSUPPORTED on a primitive xud_model_gate() refuses.
XudResult xud_model_read(
    const cgltf_data* data, uint32_t meshIndex, uint32_t primitiveIndex, const XudModelStage* stage, XudMeshData* outMesh);

/// Pure: fold one stage's positions into a bounding box. Either in-corner may be NULL, which is
/// the "no box yet" state rather than a zero corner: the first vertex seeds both corners, so a
/// model whose geometry sits entirely at negative coordinates still gets a box that contains it.
/// XUD_INVALID_ARG on a NULL stage or out-corner, or on a stage with no vertices; XUD_OK otherwi
/// se, with both corners filled.
XudResult xud_model_bounds(
    const XudModelStage* stage, const XudVector3* inMin, const XudVector3* inMax, XudVector3* outMin, XudVector3* outMax);

/// Pure: the directory prefix of a model path, which is what cgltf_load_buffers() needs in order
/// to resolve a glTF's external buffer URIs against the file they were shipped with. The prescri
/// ptor is the last '/' of the path plus one, or the empty prefix when the path has none, and a
/// path with no slash is therefore a document whose buffers are read from the working directory
/// rather than refused. XUD_INVALID_ARG on a NULL out-buffer or path, or on an empty path; XUD_U
/// NSUPPORTED when the prefix does not fit outCapacity. *outLength is the prefix's length, not
/// counting the terminator.
XudResult xud_model_path_dir(char* outDir, uint32_t outCapacity, uint32_t* outLength, const char* path);

/// Pure: cgltf's answer as this library's. One row per cgltf_result and no fallthrough: a code
/// outside the enum is XUD_INTERNAL_ERROR rather than XUD_OK, because an unclassified refusal is
/// an invariant of the library above that broke, not a document the caller can fix.
XudResult xud_model_result_from_cgltf(cgltf_result result);

/// Pure: the image a material's base-color texture names, or XUD_MODEL_NO_IMAGE when the material
/// carries no map at all - no pbrMetallicRoughness block, no baseColorTexture, or a texture view
/// whose source image is absent. A baseColorTexture that samples a texcoord other than 0 is
/// XUD_UNSUPPORTED rather than a silent answer: the mesh's one UV stream is TEXCOORD_0, and a map
/// whose coordinates come from a stream this rung does not read would be sampled with the wrong
/// ones rather than with none. XUD_INVALID_ARG on a NULL document or out-index, or on a material
/// index at or past the document's count; XUD_OK with the index written in either shape, the
/// no-map one being XUD_MODEL_NO_IMAGE.
XudResult xud_model_image_for_material(const cgltf_data* data, uint32_t materialIndex, uint32_t* outImageIndex);

/// Pure: the sampler a base-color texture names, folded onto the texture rung's two tokens - the
/// XUD_TEXTURE_FILTER_* one and the XUD_TEXTURE_ADDRESS_* one a texture opened for the image
/// carries. The fold loses the information this rung cannot hold (XudTextureData has one filter
/// for Vulkan's two filter slots and one wrap mode for its two wrap axes) and the rule for each
/// loss is the axis the spec lists first: when the magnifying and the minifying filters map to
/// different tokens the magnifying one wins, the mip interpolation of a mipmap min-filter being the
/// texture rung's fixed LINEAR, and wrapS wins over wrapT. A texture with no sampler, a sampler
/// with a key absent and a sampler the document never names all answer the glTF defaults, LINEAR
/// and CLAMP_TO_EDGE. XUD_INVALID_ARG on a NULL document or out-token, or on a filter or wrap
/// value the glTF spec does not give; XUD_OK with both tokens written in.
XudResult xud_model_sampler_tokens(const cgltf_data* data, const cgltf_texture* texture, uint32_t* outFilter, uint32_t* outAddressMode);

/// Pure: decode one document image into a width*height array of RGBA bytes, four per texel. The
/// bytes are stb_image's own allocation in the shape its load answers - the caller frees them
/// with stbi_image_free() - and the decode asks for four channels so an RGB source arrives with
/// an alpha filled in rather than a third channel the texture rung's format would have to invent.
/// An image read from a buffer view uses the buffer bytes the load already brought in, and one
/// read from a URI uses the URI resolved against `dir` (NULL treated as the empty prefix), which
/// is the model's own directory prefix for the reason xud_model_path_dir() states for buffers.
/// XUD_INVALID_ARG on a NULL document, out-width, out-height or out-rgba, on an image index at
/// or past the document's count, or on an image that names neither a buffer view nor a URI, or on
/// a buffer view whose buffer was never loaded or that reads past it; XUD_UNSUPPORTED when a
/// URI-resolved path does not fit; XUD_MALFORMED when stb_image refuses the bytes; XUD_OK with
/// *outWidth, *outHeight and *outRgba written in.
XudResult xud_model_image_decode(
    const cgltf_data* data, uint32_t imageIndex, const char* dir, uint32_t* outWidth, uint32_t* outHeight, uint8_t** outRgba);

/// Pure: the accessor one rung above reaches instead of the slot array. XUD_INVALID_ARG on a NULL
/// out-record or on a handle that does not name a live MODEL slot of this table; XUD_OK with *ou
/// tRecord naming the record the slot names.
XudResult xud_model_get(const XudHandleTable* table, XudHandle handle, const XudModelRecord** outRecord);

/// Driver: bring one model up, in the order that leaves nothing reachable and unfilled: the gate
/// first (so no slot is spent for a document that will be refused), then the distinct base-color
/// images decoded and opened as TEXTURE slots holding entries of set 1's sampler array (so a
/// map's decode cannot be the step that spends the MODEL slot), then the MODEL slot, then the
/// record, then the MATERIAL slots (one per document material plus the default, each naming the
/// texture its base-color image decoded to), then per primitive its stage block, its read, its
/// MESH slot through the mesh rung below and its stage freed. Every refusal retires what the
/// steps above created, in the reverse order - the sampler-array entries given back before the
/// textures they name are destroyed - so a model that refused part-way leaves no live mesh,
/// material or texture behind. `dir` is the model path's directory prefix, the prefix a URI
/// image resolves against, and `descriptor` is set 1's sampler array the map's entry is bound
/// into. On XUD_OK *outHandle names a live MODEL slot whose record is no longer the sentinel and
/// whose meshCount is the primitive total. XUD_INVALID_ARG on a NULL out-handle, table, loader,
/// device, caps, descriptor, allocator or document, or on a device handle that does not exist;
/// XUD_UNSUPPORTED on a loader that is closed, on a device that is not this loader's, on a
/// document this rung does not read or that names more distinct images than set 1's sampler array
/// can hold, or on a URI image whose path does not fit; XUD_MALFORMED when a decode refuses the
/// bytes; the mesh, texture, descriptor and handle layers' own codes on the rows they run.
XudResult xud_model_open(XudHandle* outHandle, XudHandleTable* table, XudLoader* loader, const XudDevice* device, const XudDeviceCaps* caps,
    XudDescriptor* descriptor, XudAllocator* alloc, const cgltf_data* data, const char* dir);

/// Driver: release one live MODEL slot - close every MESH slot this load created through the mesh
/// rung below, close every MATERIAL slot it created through the material rung, retire every TEXT
/// URE slot it created (the sampler-array entry given back before the view and sampler it names
/// are destroyed, in the meshes-before-materials order: a mesh names its material and a material
/// names its map, so each rung goes down before the rung that names it), then clear the slot's
/// record field BEFORE the record is freed (the material rung's rule: a free slot must not still
/// own a record, or this path and the table's would both free it) and free the record and its
/// handle block. The handle is validated against the MODEL kind first, so a stale handle, a hand
/// le of another kind and a fabricated one refuse before anything is closed. XUD_INVALID_ARG on a
/// NULL table, loader, device, descriptor or allocator, or on a handle that does not name a live
/// MODEL slot; XUD_UNSUPPORTED on a closed loader or a device that is not this loader's; the
/// mesh rung's own code when one of its closes refused.
XudResult xud_model_close(
    XudHandleTable* table, XudHandle handle, XudLoader* loader, const XudDevice* device, XudDescriptor* descriptor, XudAllocator* alloc);

/// Teardown: the rung above the handle table's own close, and above the mesh, the material and
/// the texture rungs' - it runs FIRST of the four, so the MESH, MATERIAL and TEXTURE slots a
/// model created are closed through their own rungs here rather than left for their close_all to
/// find after the model's record is gone. Every MODEL slot's record is closed (live or released:
/// xud_handle_free() leaves the record field as it was) and the field cleared, so
/// xud_handle_table_close() can free the arrays without leaking a record behind them and the
/// mesh rung's walk below cannot double-free a mesh this walk already closed. XUD_INVALID_ARG on
/// a NULL table, loader, device, descriptor or allocator, or on a device handle that does not
/// exist; XUD_UNSUPPORTED on a closed loader or a device that is not this loader's; XUD_OK with
/// 0 on a table with no MODEL array.
XudResult xud_model_close_all(
    XudHandleTable* table, XudLoader* loader, const XudDevice* device, XudDescriptor* descriptor, XudAllocator* alloc, uint32_t* outClosed);

#endif /* XUD_XUD_MODEL_H_ */
