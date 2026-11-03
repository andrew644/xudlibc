/***********************************************************************************************
 *   xud_mesh.h - the mesh rung: the record a MESH-kind slot of the handle layer holds, and the
 *                one host-visible block of the mesh's own five SoA buffers
 *
 *   Owns : XudMeshRecord - the object record the handle layer's MESH-kind slots have been
 *          waiting for - plus the rungs that fill and clear that field (xud_mesh_open(), xud_
 *          mesh_close(), xud_mesh_get(), xud_mesh_close_all()) and the pure halves above them
 *          (xud_mesh_check(), xud_mesh_layout(), xud_mesh_caps_support(), xud_mesh_fill_buffer_
 *          info(), xud_mesh_write_arena()), and the public pair xud.h declares above all of it:
 *          xud_upload_mesh()/xud_close_mesh().
 *   Layer: above the handle table and the allocator, below xud.h. It is the SECOND rung that
 *          fills a slot's record field (the material rung was the first) and the FIRST rung whose
 *          record owns driver objects - so its close calls a driver, its teardown needs the
 *          loader/device/allocator in hand, and the handle layer's note about "records are closed
 *          BEFORE the table goes" now has a kind under it that is not host-only.
 *
 *   What the rung owns and what it borrows: it owns the record, the mesh's five VkBuffers and the
 *   ONE host-visible block their bytes live in. It borrows the handle table (the slot and the
 *   record field through the handle layer's own two accessors), the allocator (the block - the
 *   mesh never calls vkAllocateMemory itself, the allocator is its one owner), the loader's devic
 *   e table and the device. It borrows nothing of the renderer: the mesh is not a frame object, it
 *   is an object a caller creates once and draws many times, which is the whole reason it is a ha
 *   ndle and the batch is not.
 *
 *   Why one block and five buffers, and why the block is host-visible: the settled decision's
 *   "one buffer per attribute stream (SoA)" is one rule read at two rungs - the batch's streams an
 *   d a mesh's streams are the same four streams, the same four widths and the same four formats,
 *   read off xud_batch.h's constants rather than restated here, which is what makes a mesh's stre
 *   ams bindable through the pipeline's existing vertex-input state at all. A mesh spends ONE blo
 *   ck because the repo's rule is "allocate once, suballocate always", and the one exception that
 *   rule has is a Vulkan fact: vkMapMemory maps a VkDeviceMemory, not a range, so the one carve a
 *   rung maps owns the whole block. The mesh's arena is mapped because the mesh's bytes are writt
 *   en once, by the CPU, at the moment the mesh is created - and never again. That is what makes t
 *   he transfer path the batch needs (a staging buffer, a command buffer, a submit, a fence) the
 *   wrong shape here: the batch's arena is rewritten every frame and read by a transfer, a mesh's
 *   is written once and read by the vertex stage for the mesh whole life. The repo's rule says pre
 *   fer the copy path "for anything transient", and a mesh is not transient.
 *
 *   What the record does NOT hold, and why that is the honest state rather than an oversight: no
 *   VkDescriptorSet, no sampler, no image. A mesh is geometry, not a material - the material's set
 *   -1 half is the texture step's and is still VK_NULL_HANDLE (see xud_material.h). And no per-mes
 *   h vertex-input state: the pipeline's four bindings and four attributes are the batch's and a m
 *   esh's streams have the same strides and formats, so a mesh is drawn by binding its own four b
 *   uffers to those same binding indices beside the rows that name it. That last half is NOT here
 *   today, and it landed one rung above this file rather than in it: the batch's xud_batch_record
 *   _mesh_draw() is the row that names a mesh's five buffers beside the arena, and xud_draw_mesh()
 *   / xud_draw_model() are xud.h's public pair above that. A mesh is therefore a set of buffers
 *   and a row, not a second pipeline - which is what the same strides and the same formats above
 *   are the whole of.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#ifndef XUD_XUD_MESH_H_
#define XUD_XUD_MESH_H_

#include "volk.h" /* Required for: VK_NO_PROTOTYPES - must precede <vulkan/vulkan.h>, see xud_loader.h */
#include "xud.h" /* Required for: XudResult, XudHandle, XudMeshData, XUD_HANDLE_KIND_MESH - the public surface this rung is the half of */
#include "xud_allocator.h" /* Required for: XudMemoryRange, xud_allocator_acquire_host_visible/release - the one block this rung owns */
#include "xud_batch.h" /* Required for: XUD_BATCH_STREAM_*, XUD_BATCH_INDEX_SIZE - the four streams and the index width, read rather than restated */
#include "xud_device.h" /* Required for: XudDevice, the VkDevice every row of this rung is asked of */
#include "xud_handle.h" /* Required for: XudHandleTable, xud_handle_validate/alloc/free, xud_handle_record_get/set - the slot this record lives in */
#include "xud_loader.h" /* Required for: XudLoader, the device-table rows this rung calls through */
#include "xud_physical.h" /* Required for: XudDeviceCaps, the vertex-input limits a mesh's streams are gated on */
#include "xud_vulkan.h" /* Required for: xud_result_from_vk, xud_loader_device_bound - the one VkResult funnel and the bound-handle check */
#include <vulkan/vulkan.h> /* Required for: VkBuffer, VkBufferCreateInfo, VK_NULL_HANDLE, the usage bits */

#include <assert.h> /* Required for: static_assert, the array shapes and the stream mirror below */
#include <stddef.h> /* Required for: size_t, the byte lengths of the write half */
#include <stdint.h>

/*-----------------------------------------------------------------------------------------------
 * The mesh's five buffers, and the streams they are named by
 *-----------------------------------------------------------------------------------------------*/

/* The four streams are the batch's four, by index and by width and by format, and the point of
   reading them off xud_batch.h rather than restating them here is a Vulkan fact: the pipeline's
   vertex-input state has four binding rows and four attribute rows whose strides and formats are
   the batch's, so a mesh's streams are bindable through that same state only if their widths are
   the same numbers. A mesh that chose different widths would be a mesh that needs a second pipel
   ine, and the pipeline rung's list is a row per (topology x variant), never a row per mesh. */
#define XUD_MESH_BUFFER_COUNT                                                                                                              \
    (XUD_BATCH_STREAM_COUNT                                                                                                                \
        + 1u) /* four attribute streams plus the one index buffer, the settled decision's one index buffer read at this rung too */
#define XUD_MESH_INDEX_SLOT                                                                                                                \
    XUD_BATCH_STREAM_COUNT /* the index buffer's slot, one past the four streams - the one place the five are one array */

static_assert(XUD_MESH_BUFFER_COUNT == (XUD_BATCH_STREAM_COUNT + 1u),
    "xud_mesh.h: the five buffers are the four streams plus one index buffer, so the count is the batch's stream count plus one");
static_assert(XUD_MESH_INDEX_SLOT == (XUD_MESH_BUFFER_COUNT - 1u),
    "xud_mesh.h: the index buffer is the last slot of the five, which is what makes one loop cover all five and no sixth");

/* The index width, read rather than remembered: xud_batch.h's XUD_BATCH_INDEX_SIZE is the width
   the batch's index buffer was created at and xud_frame.h pins XUD_FRAME_INDEX_TYPE against, so a
   mesh whose indices were four bytes and the batch's two would be two meshes needing two index t
   ypes. */
static_assert(XUD_BATCH_INDEX_SIZE == 4u,
    "xud_mesh.h: the index width this rung shares with the batch is the uint32 the frame loop's index type names");

/*-----------------------------------------------------------------------------------------------
 * The mesh's own bound, and the reason it is this rung's rather than the batch's
 *-----------------------------------------------------------------------------------------------*/

/* The batch's bound is a capacity bound (a batch that runs out grows, and the frame boundary is wha
   t grows it) and a mesh's bound is a size bound: a mesh is one object with one index range, and a
   mesh past this bound is a mesh the library refuses rather than one it grows. The number below is
   not invented - it is the largest vertex count whose arena is still one request the allocator's bl
   ock-growth ceiling can serve, the same form xud_batch.h states its own bound in. The arena is one
   vertex count times the four streams' widths plus the index buffer's three indices per vertex, an
   d a request past the ceiling is one the host-visible acquire answers XUD_OUT_OF_RESOURCES for; r
   efusing it here is that reason stated before a slot is spent rather than a driver answer read aft
   er one. What this bound does NOT cover is the vertex-input limits, which are caps facts rather th
   an constants and are xud_mesh_caps_support()'s half - and it covers no per-buffer limit either, be
   cause these headers carry no maxBufferSize at this library's floor (see that half's note).
*/
#define XUD_MESH_BYTES_PER_VERTEX                                                                                                          \
    (XUD_BATCH_VERTEX_SIZE                                                                                                                 \
        + (3u * XUD_BATCH_INDEX_SIZE)) /* one vertex of the arena, its three indices included - the number the bound below divides by */
#define XUD_MESH_MAX_VERTICES (XUD_ALLOCATOR_BLOCK_MAX_SIZE / XUD_MESH_BYTES_PER_VERTEX)

static_assert(XUD_MESH_BYTES_PER_VERTEX == 48u,
    "xud_mesh.h: one vertex of the arena is the four streams' 36 B plus the three indices it owns at 4 B each");
static_assert((XUD_MESH_MAX_VERTICES * XUD_MESH_BYTES_PER_VERTEX) <= XUD_ALLOCATOR_BLOCK_MAX_SIZE,
    "xud_mesh.h: the largest mesh this rung names has an arena one request the allocator's block ceiling can serve, so the bound is the "
    "allocator's own rather than an invented one");
static_assert(XUD_MESH_MAX_VERTICES >= 3u,
    "xud_mesh.h: the bound is at least one triangle, so a mesh refused by the bound is not refused because the bound is smaller than the "
    "smallest mesh");

/*-----------------------------------------------------------------------------------------------
 * What a MESH-kind slot records
 *-----------------------------------------------------------------------------------------------*/

/* The record is heap-allocated by xud_mesh_open() and stored in the slot's `record` field, exactly
   as the material record is. Unlike the material's, this one owns driver objects, so freeing it is
   not host-only: xud_mesh_close() unmaps the arena, destroys the five buffers and releases the blo
   ck before it frees, and xud_mesh_close_all() does the same for every slot the table is about to
   free. That is why both take the loader, the device and the allocator while the material's two to
   ok only the table.

   The five buffers are one array rather than four streams plus a separate member because the mesh
   has no per-stream live state to keep - no capacity, no live count, no append - only a size and a
   n offset, and the one loop that creates, binds, destroys and releases them is five rows wide. Th
   e batch keeps its streams in a struct of their own for the opposite reason: a stream has a cap
   acity, an element size and a live count, and a mesh has none of those three. */
typedef struct XudMeshRecord {
    uint32_t vertexCount; /* vertices this mesh holds, the one count every stream is sized from */
    uint32_t indexCount; /* indices it holds - a multiple of three, see xud_mesh_check() */
    uint32_t triangleCount; /* indexCount / 3, the count raylib's Mesh names and the count a test can read back */

    VkBuffer buffer[XUD_MESH_BUFFER_COUNT]; /* the Vulkan objects this record owns: VK_NULL_HANDLE until xud_mesh_open() creates each,
                                               destroyed by xud_mesh_close() */
    VkBufferCreateInfo bufferInfo[XUD_MESH_BUFFER_COUNT]; /* what each of the five was created with, kept so the record and the driver
                                                             cannot disagree about a size */

    XudMemoryRange arenaRange; /* the ONE host-visible block the five live in - the whole block, because a mapping is per-VkDeviceMemory
                                  rather than per-range */
    VkDeviceSize bindOffset[XUD_MESH_BUFFER_COUNT]; /* where each of the five was bound inside that block */

    VkDeviceSize streamBytes[XUD_BATCH_STREAM_COUNT]; /* vertexCount x the stream's element size */
    VkDeviceSize streamOffset[XUD_BATCH_STREAM_COUNT]; /* where each stream starts inside the arena */
    VkDeviceSize indexBytes; /* indexCount x the index width */
    VkDeviceSize indexOffset; /* where the index buffer starts, after the four streams */
    VkDeviceSize arenaBytes; /* the arena's total, what the block was asked for */

    void* arenaMapped; /* what vkMapMemory wrote, for the whole arena, mapped once at open and unmapped once at close */
    uint32_t arenaMappedLive; /* 1 while that mapping exists, 0 after it was unmapped - the same live-flag shape the batch's arena keeps */
} XudMeshRecord;

static_assert(sizeof(((XudMeshRecord*)0)->buffer) / sizeof(VkBuffer) == XUD_MESH_BUFFER_COUNT,
    "xud_mesh.h: the buffer array must be exactly the five the count names, or the create loop and the destroy loop disagree");
static_assert(sizeof(((XudMeshRecord*)0)->bindOffset) / sizeof(VkDeviceSize) == XUD_MESH_BUFFER_COUNT,
    "xud_mesh.h: one bind offset per buffer, so a buffer can never be bound at an offset its own row does not state");
static_assert(sizeof(((XudMeshRecord*)0)->bufferInfo) / sizeof(VkBufferCreateInfo) == XUD_MESH_BUFFER_COUNT,
    "xud_mesh.h: one create-info per buffer, so the size a buffer was created with is readable after the create");
static_assert(sizeof(((XudMeshRecord*)0)->streamBytes) / sizeof(VkDeviceSize) == XUD_BATCH_STREAM_COUNT,
    "xud_mesh.h: one byte count per attribute stream, the index buffer being the fifth and not one of these");
static_assert(sizeof(((XudMeshRecord*)0)->streamOffset) / sizeof(VkDeviceSize) == XUD_BATCH_STREAM_COUNT,
    "xud_mesh.h: one arena offset per attribute stream, the index buffer's offset being its own member");

/*-----------------------------------------------------------------------------------------------
 * The layout's alignment floor, and the pure half that takes an alignment per buffer
 *-----------------------------------------------------------------------------------------------*/

/* The arena's offsets are computed from an alignment per buffer rather than packed tight, and the
   alignment is an argument rather than a constant for one reason: the alignment a buffer's bytes
   must start at is a fact the driver reports (VkMemoryRequirements::alignment), and a layout that
   assumed an alignment would be a layout that bound a buffer at an offset the driver then refuse
   d. Taking it as an argument is what makes the arithmetic headless-testable - a test hands the
   same function the alignments a driver reported and asserts the offsets - and XUD_MESH_ALIGN_MIN
   is the floor the driver half uses when a driver reports less than this, because every element w
   idth here is a multiple of four and an offset at a multiple of four is what keeps every stream
   element naturally aligned for the free. */
#define XUD_MESH_ALIGN_MIN 4u
#define XUD_MESH_ALIGN_MAX                                                                                                                 \
    65536u /* the allocator's own alignment ceiling (XUD_ALLOCATOR_ALIGN_MAX), which is the highest alignment this layout will pad to      \
              before it refuses */

static_assert(XUD_MESH_ALIGN_MIN >= 4u,
    "xud_mesh.h: the layout's floor keeps every stream element aligned, which is what makes the narrowest stream (a four-byte colour) "
    "start on its own boundary");
static_assert(XUD_MESH_ALIGN_MIN <= XUD_MESH_ALIGN_MAX,
    "xud_mesh.h: the floor is at or below the ceiling, so a driver that reports the floor is a driver this layout serves");

/// Pure: round `value` up to the next multiple of `alignment`. The one arithmetic helper the layout
/// runs, written once so the layout and a test cannot round differently. alignment must not be zero.
VkDeviceSize xud_mesh_align_up(VkDeviceSize value, uint32_t alignment);

/*-----------------------------------------------------------------------------------------------
 * The layer's contract
 *-----------------------------------------------------------------------------------------------*/

/// Pure: the whole argument check of this rung, run before anything is spent and before the driver
/// is called. Refuses a mesh the library cannot describe rather than a mesh the driver would refus
/// e: a NULL data pointer or a NULL out-count; XUD_INVALID_ARG on a vertexCount of zero (a mesh of
/// no vertices is a mesh that names no stream), on an indexCount below three or not a multiple of
/// three (this rung has one topology, triangles, so an index count that is not a whole number of t
/// riangles is not a value the row of the pipeline list can name), on a NULL index array, on any i
/// ndex at or past vertexCount (an out-of-range index is not a refusal the driver would give later,
/// it is undefined behaviour at the draw - catching it here is the point of the accessor), on a co
/// unt pair whose byte total overflows uint32 arithmetic, or on a vertexCount past XUD_MESH_MAX_VE
/// RTICES. A NULL pos/uv/color/normal array is NOT a refusal: the mesh's streams all have to exist,
/// so a missing source array is filled with a documented default (zero position, zero uv, opaque w
/// hite, +Z normal) by xud_mesh_write_arena() below, which is what makes the invariant "every stre
/// am holds vertexCount vertices" true whatever the caller brought. XUD_OK otherwise, with *outTr
/// iangleCount filled when it is not NULL.
XudResult xud_mesh_check(const XudMeshData* data, uint32_t* outTriangleCount);

/// Pure: the caps gate. The limits that bound a mesh's streams are the vertex-input limits, and th
/// e reason they are the ones read here is that these headers carry no buffer-size limit at the floo
/// r this library is at: VkPhysicalDeviceLimits has no maxBufferSize (grepped in vulkan_core.h 1.4.
/// 363 - the field exists in VkPhysicalDeviceVulkan13Properties and in VkPhysicalDeviceMaintenance4
/// Properties, neither of which the caps chain), so a gate on it would be a gate on a number this t
/// ree never records. What IS recorded and DOES bound a mesh is the stride of each of the four stre
/// ams (maxVertexInputBindingStride) and the count of bindings and attributes they bind through - a
/// stream wider than the stride, or a device serving fewer than the four, is a mesh whose streams ca
/// nnot be bound through the pipeline's vertex-input state at all, and the answer is XUD_UNSUPPORTE
/// D rather than a narrower format chosen behind the caller's back. The batch gates the same three n
/// umbers for its own streams; this rung gates them for its own, because binding is this rung's act
/// and not the batch's. The arena's own ceiling is NOT here: it is a constant (XUD_MESH_MAX_VERTICE
/// S, divided out of the allocator's block ceiling) and xud_mesh_check() reads it.
XudResult xud_mesh_caps_support(const XudDeviceCaps* caps, const XudMeshData* data);

/// Pure: the arena's layout - the five byte counts, the five offsets and the arena's total - given
/// an alignment per buffer (NULL meaning "use XUD_MESH_ALIGN_MIN for every slot"). Nothing is writ
/// ten on a failure path, so a record that refuses is a record that still says nothing rather than o
/// ne that says half a mesh. Offsets are layed out back to back in slot order, each rounded up to th
/// at slot's own alignment, which is the same policy the allocator's free list uses (first-fit in row
/// order, deterministic) and the reason the appends, the binds and the write cannot drift apart. XUD
/// _INVALID_ARG on a NULL record or data, or on an alignment of zero; XUD_UNSUPPORTED on a mesh xud_
/// mesh_check() would refuse, or on an alignment past XUD_MESH_ALIGN_MAX (the allocator's own ceilin
/// g - an alignment above it is a request the allocator would refuse, and refusing here is that reas
/// on stated before a block is asked for).
XudResult xud_mesh_layout(const XudMeshData* data, const uint32_t* alignment, XudMeshRecord* outRecord);

/// Pure: stamp one of the five VkBufferCreateInfo rows - the size from the mesh's counts and the
/// usage bits this rung's own use of that buffer states (the four streams are VERTEX_BUFFER because
/// the pipeline's attribute rows read them; the index buffer is INDEX_BUFFER because vkCmdBindIndex
/// Buffer names it; neither is TRANSFER_DST because nothing copies into a mesh, the write is the map
/// ping). XUD_INVALID_ARG on a NULL out-info, a NULL record or a slot at or past the five.
XudResult xud_mesh_fill_buffer_info(const XudMeshRecord* record, uint32_t slot, VkBufferCreateInfo* outInfo);

/// Pure: the write that fills the mesh's bytes. `arena` is the destination - the driver half hands
/// this the pointer vkMapMemory wrote, a test hands it its own heap buffer - and `arenaSize` is how
/// many bytes of that destination exist, which is what makes the write's offsets a checked fact rat
/// her than an assumption: the layout's offsets are read from the record and the record's counts are
/// compared against the description's, so a record laid out for one mesh cannot be used to write an
/// other mesh's arrays. The five sources are the caller's arrays, with the documented defaults for a
/// NULL one. Writes nothing outside arenaSize. XUD_INVALID_ARG on a NULL record, data or arena, on a
/// destination smaller than the record's own arenaBytes, or on a record whose counts do not match th
/// e description's; XUD_UNSUPPORTED when the record's arena was never laid out (arenaBytes zero).
XudResult xud_mesh_write_arena(const XudMeshRecord* record, const XudMeshData* data, void* arena, VkDeviceSize arenaSize);

/// Driver: bring one mesh up, in the one order that leaves nothing reachable and unfilled. The slot
/// first (so the record has a slot to be found in), then the record, then the five create-infos and
/// the five buffers, then their reported alignments read back into xud_mesh_layout(), then the ONE
/// host-visible block, then the five binds, then the map, then the write. Every guard runs before the
/// call it guards; every failure path destroys what was already created and frees nothing that is
/// still reachable. On XUD_OK *outHandle names a live MESH slot whose record is no longer the sentin
/// el, and the mesh's bytes are in the block. XUD_INVALID_ARG on a NULL out-handle, table, loader, d
/// evice, caps or allocator, or on a mesh xud_mesh_check() refuses; XUD_UNSUPPORTED on a loader whose
/// device table lacks a row this rung needs, on a device that is not this loader's, on a mesh the cap
/// s gate refuses, or on a driver that reports an alignment this layout cannot pad to; XUD_OUT_OF_MEM
/// ORY on a host allocation that failed; XUD_OUT_OF_RESOURCES at the handle layer's slot bound; the a
/// llocator's and the driver's own codes on the rows they run.
XudResult xud_mesh_open(XudHandle* outHandle, XudHandleTable* table, XudLoader* loader, const XudDevice* device, const XudDeviceCaps* caps,
    XudAllocator* alloc, const XudMeshData* data);

/// Driver: release one live MESH slot - unmap the arena, destroy the five buffers, release the block,
/// clear the slot's record field BEFORE the slot is marked free (the material rung's rule: a free slot
/// must not still own a record, or two teardown paths would both free it), and free the record. The ha
/// ndle is validated against the MESH kind first, so a stale handle, a handle of another kind and a f
/// abricated one refuse before anything is destroyed. XUD_INVALID_ARG on a NULL table, loader, device
/// or allocator, on a device handle that does not exist (the caller's argument, the same answer every
/// rung in this tree gives for it), or on a handle that does not name a live MESH slot; XUD_UNSUPPOR
/// TED when the rows this rung needs are not this loader's rows; XUD_VULKAN_ERROR when the guard's own
/// vkDeviceWaitIdle call fails - the teardown still runs and the reason still travels, because skipp
/// ing the free would leak the block the driver still holds.
XudResult xud_mesh_close(XudHandleTable* table, XudHandle handle, XudLoader* loader, const XudDevice* device, XudAllocator* alloc);

/// Pure: the accessor one rung above reaches instead of the slot array - validate against the MESH kind,
/// then hand back the record of a live MESH slot. XUD_INVALID_ARG on a NULL out-record or on a handle th
/// at does not name a live MESH slot; XUD_OK with *outRecord pointing at the record the slot names.
XudResult xud_mesh_get(const XudHandleTable* table, XudHandle handle, const XudMeshRecord** outRecord);

/// Teardown: the rung above the handle table's own close - free every record any MESH slot still names
/// (live or released: xud_handle_free() leaves the record field as it was) and clear the field, so xud_ha
/// ndle_table_close() can free the arrays without leaking a record behind them. Unlike the material's, thi
/// s one calls a driver: a mesh's record owns five buffers and a block, and a block left behind is a block
/// the allocator's own close cannot see. The rows are destroyed in the same order xud_mesh_close() uses, an
/// d the allocator's wait-idle guard is xud_allocator_close()'s, not this one's - this rung frees no block,
/// it releases ranges inside blocks the allocator still owns, and the allocator's close is what frees the blo
/// cks after its own guard. XUD_INVALID_ARG on a NULL table, loader, device, allocator or out-count,
/// or on a device handle that does not exist; XUD_UNSUPPORTED when the rows this rung needs are not
/// this loader's rows; XUD_OK with 0 on a table with no MESH array (nothing was ever created, which
/// is a count of zero and not an error); XUD_VULKAN_ERROR when the guard's own vkDeviceWaitIdle call
/// fails, the walk still having run. The walk is idempotent because every record pointer is cleared
/// here, so a second call over the same table counts zero.
XudResult xud_mesh_close_all(XudHandleTable* table, XudLoader* loader, const XudDevice* device, XudAllocator* alloc, uint32_t* outClosed);

#endif /* XUD_XUD_MESH_H_ */
