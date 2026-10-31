/***********************************************************************************************
 *   xud_batch.h - the batch rung: the dynamic SoA attribute streams, the one index buffer,
 *                 the mapped staging arena the CPU-side bytes are written into, the recorded
 *                 draw rows (each naming a topology and a variant of the pipeline list), the
 *                 flush-on-pipeline/material-change decision and the topology state a shape
 *                 moves without flushing
 *
 *   Owns : XudBatch - four stream VkBuffers (pos / uv / color / normal), one index VkBuffer,
 *          one host-visible staging VkBuffer that OWNS one of the allocator's blocks (not a suba
 *          llocation of one: vkMapMemory maps a VkDeviceMemory rather than a range, so the one c
 *          arve this rung maps cannot share a block with any other mapped carve) and mapped for t
 *          he whole life of the batch, the recorded draw rows, the topology/variant/material state
 *          the pending rows are accumulated under, and the two rows
 *          of vertex-input state the pipeline rung below is handed.
 *   Layer: above the descriptor and the allocator, below the frame loop. This is the rung of
 *          the plan's layer ladder ("xudrl renderer (passes, pipelines, BATCHES, buffers)") and
 *          the rung the repo's "dynamic per-attribute stream buffers + one index buffer (SoA)"
 *          decision settled on 2026-10-23 lands in. The renderer owns one per context, opened
 *          after the render pass and BEFORE the pipeline (the pipeline's vertex-input state is th
 *          e thing that names these buffers, so they must exist before it and outlive it) and clo
 *          sed in the reverse slot, after the pipeline has been retired and before the frame loo
 *          p's teardown guard runs.
 *
 *   Why SoA and not interleaved, in one line: the settled decision says SoA, and the reason it
 *   is still the reason is that a stream is the unit of both the copy and the grow - a batch
 *   that appends only positions copies one region and grows one buffer, and an interleaved
 *   stream would copy and grow the whole vertex every time.
 *
 *   What this rung does NOT do, and why: no vkCmdBindVertexBuffers, no vkCmdDrawIndexed, no
 *   push constant and no descriptor bind - those are the frame loop's calls, recorded into the
 *   command buffer the frame loop owns, and this rung hands the rows they take. No second
 *   pipeline variant and no sorting: the transparent half of the plan's opaque/transparent pair
 *   is the pipeline rung's and sorted transparency is a property of the draw rows recorded
 *   here, which the next step reads.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#ifndef XUD_BATCH_H_
#define XUD_BATCH_H_

#include "volk.h" /* Required for: VK_NO_PROTOTYPES - must precede <vulkan/vulkan.h>, see xud_loader.h */
#include "xud.h" /* Required for: XudResult - the batch layer's own error codes */
#include "xud_allocator.h" /* Required for: XudAllocator, XudMemoryRange - the one owner of the memory every buffer here is bound to */
#include "xud_device.h" /* Required for: XudDevice - the handle every create/bind/copy row is asked of */
#include "xud_loader.h" /* Required for: XudLoader - the Volk rows every call here is made through */
#include "xud_physical.h" /* Required for: XudDeviceCaps - the vertex-input limits and the memory rows the staging arena is chosen from */
#include "xud_vulkan.h" /* Required for: xud_result_from_vk - every VkResult here goes through that one funnel */
#include <vulkan/vulkan.h> /* Required for: VkBuffer, VkBufferCreateInfo, VkBufferCopy, the two vertex-input rows, the buffer-usage and format enums - never linked */

#include <assert.h> /* Required for: static_assert, the counts that cannot disagree */
#include <stdbool.h>
#include <stdint.h>

/*-----------------------------------------------------------------------------------------------
 * The four streams, and what one vertex costs
 *-----------------------------------------------------------------------------------------------*/

/* The four attribute streams of the settled SoA decision, in the order the pipeline's attribute
   locations and the GLSL's in-locations use. The order is load-bearing in three places at once
   (the stream array, the binding array, the attribute array and the shader's `in` locations), so
   it is stated once here and read everywhere. */
#define XUD_BATCH_STREAM_COUNT 4u
#define XUD_BATCH_STREAM_POS 0u
#define XUD_BATCH_STREAM_UV 1u
#define XUD_BATCH_STREAM_COLOR 2u
#define XUD_BATCH_STREAM_NORMAL 3u

/* The element type of each stream, as a VkFormat and as a byte width. These are the bytes the
   driver sees, so the width is stated next to the format rather than derived in a shader: the
   note in xud_descriptor.h about "the C record and the GLSL block are the same bytes only if
   the field order and the widths agree" applies here unchanged, and the four widths below are
   what XUD_BATCH_VERTEX_SIZE is the sum of.

   R32G32B32_SFLOAT rather than a core-1.4 VK_FORMAT_VEC3 because VK_FORMAT_VEC3 is a core-1.4
   name that these headers do carry, but a vertex-input format of that family is a
   VkFormatComponentType pair the pipeline's attribute row would have to validate, and the
   plain scalar forms are the ones every driver here answers for. */
#define XUD_BATCH_FORMAT_POS VK_FORMAT_R32G32B32_SFLOAT
#define XUD_BATCH_FORMAT_UV VK_FORMAT_R32G32_SFLOAT
#define XUD_BATCH_FORMAT_COLOR VK_FORMAT_R8G8B8A8_UNORM
#define XUD_BATCH_FORMAT_NORMAL VK_FORMAT_R32G32B32_SFLOAT

#define XUD_BATCH_STREAM_POS_SIZE 12u
#define XUD_BATCH_STREAM_UV_SIZE 8u
#define XUD_BATCH_STREAM_COLOR_SIZE 4u
#define XUD_BATCH_STREAM_NORMAL_SIZE 12u
#define XUD_BATCH_VERTEX_SIZE                                                                                                              \
    (XUD_BATCH_STREAM_POS_SIZE + XUD_BATCH_STREAM_UV_SIZE + XUD_BATCH_STREAM_COLOR_SIZE + XUD_BATCH_STREAM_NORMAL_SIZE)

/* The element width and the element format of one stream, read off its index. A conditional
   chain rather than a lookup table because the four widths and the four formats are already
   constants above: a table would be a second place they could disagree with the first, and
   every call site below passes a constant index, so the chain folds. */
#define XUD_BATCH_STREAM_ELEMENT(index)                                                                                                    \
    (((index) == XUD_BATCH_STREAM_POS)                                                                                                     \
            ? XUD_BATCH_STREAM_POS_SIZE                                                                                                    \
            : (((index) == XUD_BATCH_STREAM_UV)                                                                                            \
                      ? XUD_BATCH_STREAM_UV_SIZE                                                                                           \
                      : (((index) == XUD_BATCH_STREAM_COLOR) ? XUD_BATCH_STREAM_COLOR_SIZE : XUD_BATCH_STREAM_NORMAL_SIZE)))
#define XUD_BATCH_STREAM_FORMAT(index)                                                                                                     \
    (((index) == XUD_BATCH_STREAM_POS)                                                                                                     \
            ? XUD_BATCH_FORMAT_POS                                                                                                         \
            : (((index) == XUD_BATCH_STREAM_UV)                                                                                            \
                      ? XUD_BATCH_FORMAT_UV                                                                                                \
                      : (((index) == XUD_BATCH_STREAM_COLOR) ? XUD_BATCH_FORMAT_COLOR : XUD_BATCH_FORMAT_NORMAL)))

/* The draw array's growth step, on the same doubling sequence the rest of this tree uses. The
   batch's draw rows are one row per pipeline/material change, so a frame's worth of changes is
   the count this is sized for rather than a bound a caller has to know about. */
#define XUD_BATCH_DRAW_MIN 16u

/* The index width. uint32 rather than uint16 because VK_INDEX_FORMAT_UINT16 is a core-1.1 name
   and this library's instance sits at the core-1.0 floor (xud_instance.h's XUD_INSTANCE_API_FLOOR
   - the same reason vkCmdDrawIndexed's index format is not a queried thing here at all: vkCmd
   DrawIndexed takes no format, the buffer's contents are whatever the pipeline's attribute
   reads, and a 32-bit index is the one width that never needs a second index buffer for a
   batch over 65535 vertices). */
#define XUD_BATCH_INDEX_SIZE 4u

/* The batch's capacity bounds. The initial capacity is small on the honest side (a batch that
   never grows pays one allocation) and the maximum is the point past which doubling stops being
   a growth policy and becomes "this batch is full", which is a refusal rather than a silent
   truncation. The bound below is the allocator's, not an invented one: see the static_asserts und
   er it, which are the arithmetic that keeps the smallest batch's arena inside the FIRST block th
   e allocator opens and the largest batch's arena inside one request the block-growth ceiling can
   serve - the arena is a static carve (see the note under the struct), so the number that bounds i
   t is the block ceiling rather than a ring's size, and a grow that doubled past it is refused be
   fore vkCreateBuffer is asked. */
#define XUD_BATCH_INITIAL_VERTICES 64u
#define XUD_BATCH_MAX_VERTICES 4096u

/* The index capacity that goes with a vertex capacity: 3/2 indices per vertex, rounded up, which
   is the spend of a mesh whose vertices are SPLIT per face - the box's spelling, 24 vertices and
   36 indices, exactly this number - and NOT a bound for a mesh whose vertices are SHARED. A closed
   manifold triangulated surface has F = 2V-4 faces by Euler (3F = 2E and V-E+F = 2), so it needs
   3F = 6V-12 indices, FOUR times this headroom, and the sphere is the first shape in the tree tha
   t
   spends it: 17 vertices and 90 indices, exactly 6V-12 (xud_shapes.h asserts that, and the tests a
   rea
   reads it off the arena). So 3n/2 is a capacity CHOICE rather than a bound - it is the headroom fo
   r
   the flat-shaded shapes this rung draws today, and a shape that shares its vertices trips the INDE
   X
   bound while the vertex bound still has room, which is why every shape's append reads BOTH. A bat
   ch
   that is asked to hold more shared-vertex geometry than this headroom is not resized here: the gr
   ow
   path doubles both, so the ratio is kept rather than corrected, and a caller who wants a finer sha
   red
   mesh asks for a bigger capacity. Rounded up. */
#define XUD_BATCH_INDEX_CAPACITY(v) (((v) * 3u + 1u) / 2u)

#define XUD_BATCH_COPY_REGION_COUNT                                                                                                        \
    (XUD_BATCH_STREAM_COUNT + 1u) /* four streams plus the index buffer, one region each - five destinations is five vkCmdCopyBuffer       \
                                     calls, see xud_batch_upload's doc */

static_assert(XUD_BATCH_VERTEX_SIZE
        == (XUD_BATCH_STREAM_POS_SIZE + XUD_BATCH_STREAM_UV_SIZE + XUD_BATCH_STREAM_COLOR_SIZE + XUD_BATCH_STREAM_NORMAL_SIZE),
    "one vertex is the four streams' widths added, which is the number the arena's offsets are built from");
static_assert(XUD_BATCH_VERTEX_SIZE == 36u, "the settled decision's 36 B/vertex across four buffers, read rather than remembered");
static_assert(XUD_BATCH_STREAM_COUNT == (XUD_BATCH_STREAM_NORMAL + 1u), "the four stream rows are contiguous from pos up to normal");
static_assert(XUD_BATCH_COPY_REGION_COUNT == 5u, "one copy region per stream plus one for the index buffer");
static_assert(XUD_BATCH_STREAM_ELEMENT(XUD_BATCH_STREAM_COLOR) == 4u,
    "the chain above answers the width the constants state, for the narrowest stream of the four");
static_assert(XUD_BATCH_STREAM_ELEMENT(XUD_BATCH_STREAM_POS) + XUD_BATCH_STREAM_ELEMENT(XUD_BATCH_STREAM_UV)
            + XUD_BATCH_STREAM_ELEMENT(XUD_BATCH_STREAM_COLOR) + XUD_BATCH_STREAM_ELEMENT(XUD_BATCH_STREAM_NORMAL)
        == XUD_BATCH_VERTEX_SIZE,
    "the chain above and the sum of the four constants are one number, which is what makes the arena's offsets built from the chain and "
    "the buffer sizes built from the constants agree");
static_assert((XUD_BATCH_VERTEX_SIZE % 4u) == 0u,
    "a stream offset that is a multiple of a vertex size is a multiple of 4, which is what keeps every arena offset a whole element");
static_assert(
    XUD_BATCH_MAX_VERTICES >= XUD_BATCH_INITIAL_VERTICES, "the bound is above the start, or the first grow step is already past it");
static_assert(
    (XUD_BATCH_INITIAL_VERTICES * XUD_BATCH_VERTEX_SIZE + XUD_BATCH_INDEX_CAPACITY(XUD_BATCH_INITIAL_VERTICES) * XUD_BATCH_INDEX_SIZE)
        <= XUD_ALLOCATOR_BLOCK_MIN_SIZE,
    "the smallest batch's staging arena fits the allocator's SMALLEST block, which is the block the arena gets when it owns one - the "
    "arena is mapped, and a mapped carve owns its block rather than sharing one");
static_assert((XUD_BATCH_MAX_VERTICES * XUD_BATCH_VERTEX_SIZE + XUD_BATCH_INDEX_CAPACITY(XUD_BATCH_MAX_VERTICES) * XUD_BATCH_INDEX_SIZE)
        <= XUD_ALLOCATOR_BLOCK_MAX_SIZE,
    "the largest batch's staging arena is one request the allocator's block-growth ceiling can serve, which is what makes the bound the "
    "allocator's own rather than an invented one");

/*-----------------------------------------------------------------------------------------------
 * The topology, the pipeline variant and the material slot a draw row names
 *-----------------------------------------------------------------------------------------------*/

/* The two variants of the plan's opaque/transparent pair, as the DRAW ROW's index into them
   rather than as a pipeline handle: a draw row that carried a VkPipeline would be a row whose
   pipeline had gone stale the moment the pipeline rung recreated one, and the row's whole point
   is that it is a record. BOTH rows exist today - the pipeline rung owns a pipeline per row of
   this list under one layout (todo.md's D1, 2026-10-26: raylib's blending is an enum-indexed
   list of modes, so the list is the shape and growing it is a row, not a rename) - and the
   frame loop binds the row this index names, so naming a variant here is naming a pipeline
   choice. OPAQUE is the DEFAULT because the transparent row is only correct against a caller
   that drew back-to-front (todo.md's D2: raylib's painter's-algorithm semantics, the library
   never sorts and no sort key is added to this row). */
#define XUD_BATCH_VARIANT_OPAQUE 0u
#define XUD_BATCH_VARIANT_TRANSPARENT 1u
#define XUD_BATCH_VARIANT_COUNT 2u

/* The two topologies a draw row can name, as the row's index into the pipeline list's topology
   axis rather than as a VkPrimitiveTopology: a row that carried a pipeline handle would be a row
   whose pipeline had gone stale the moment the pipeline rung recreated one, and the row's whole
   point is that it is a record. This is raylib's own axis: its draw row carries `mode` (reference
   /raylib/rlgl.h 407-413, "Drawing mode: LINES, TRIANGLES, QUADS") and the replay is
   `glDrawArrays(batch->draws[i].mode, vertexOffset, vertexCount)` (rlgl.h 3116), so the mode IS a
   per-row index into a list of topologies and the Vulkan form of that list is a pipeline per row
   (topology is `VkPipelineInputAssemblyStateCreateInfo::topology`, which is pipeline state).
   TRIANGLES is the DEFAULT because it is the row the batch's own appends have drawn through since
   S16 and the row the pipeline's XUD_PIPELINE_TOPOLOGY_TRIANGLE names.

   POINT_LIST is NOT in this list, and that is a raylib reading rather than an omission: raylib's
   DrawPoint3D is documented "actually a small line" (reference/raylib/raylib.h 1586) and is built
   with rlBegin(RL_LINES), so a point in the shape this library is modelled on is a line and needs
   no third row. A POINT_LIST row is a row a later step adds when a caller wants a point sprite, in
   the same shape as XUD_BATCH_VARIANT_COUNT grew (todo.md's D1: growing the list is a row, never a
   rename).

   The two axes are ONE list, indexed by the pipeline rung's XUD_PIPELINE_ROW(topology, variant):
   the batch's row names both and the frame loop binds one pipeline per row, so the pair has to be
   one number for the batch's row and the pipeline's object to stay the same thing. */
#define XUD_BATCH_TOPOLOGY_TRIANGLES 0u
#define XUD_BATCH_TOPOLOGY_LINES 1u
#define XUD_BATCH_TOPOLOGY_COUNT 2u

#define XUD_BATCH_NO_MATERIAL 0xFFFFFFFFu /* "no material slot yet", the same sentinel shape xud_handle.h uses for an unfilled slot */

static_assert(XUD_BATCH_VARIANT_COUNT == (XUD_BATCH_VARIANT_TRANSPARENT + 1u), "the two variants are contiguous from opaque up");
static_assert(XUD_BATCH_TOPOLOGY_COUNT == (XUD_BATCH_TOPOLOGY_LINES + 1u), "the two topologies are contiguous from triangles up");
static_assert(XUD_BATCH_NO_MATERIAL > 0u, "the sentinel is not a material slot any live row can name");

/*-----------------------------------------------------------------------------------------------
 * The batch's own objects, and what each was created with
 *-----------------------------------------------------------------------------------------------*/

typedef struct XudBatchStream XudBatchStream;
struct XudBatchStream {
    VkBuffer buffer; /* the stream's own VkBuffer, VK_NULL_HANDLE until xud_batch_open() creates it */
    VkBufferCreateInfo bufferInfo; /* what vkCreateBuffer was handed for it */
    XudMemoryRange range; /* the allocator's range its memory lives in */
    VkFormat format; /* XUD_BATCH_FORMAT_* for this row */
    VkDeviceSize elementSize; /* XUD_BATCH_STREAM_*_SIZE for this row */
    VkDeviceSize capacityBytes; /* vertexCapacity * elementSize, the size the buffer was created for */
    VkVertexInputBindingDescription binding; /* the pipeline's binding row for this stream, and the argument of vkCmdBindVertexBuffers */
    VkVertexInputAttributeDescription attribute; /* the pipeline's attribute row for this stream */
};

typedef struct XudBatchDraw XudBatchDraw;
struct XudBatchDraw {
    uint32_t firstIndex; /* where in the index buffer this draw starts */
    uint32_t indexCount; /* how many indices it takes - vkCmdDrawIndexed's whole argument pair */
    uint32_t topology; /* which topology this run's indices are assembled as - the row of the
                            pipeline list the frame loop binds beside this draw, and raylib's own
                            field: rlDrawCall's `mode` (rlgl.h 407), replayed by glDrawArrays at
                            rlgl.h 3116 */
    uint32_t variant; /* which of the plan's two pipeline variants this draw uses */
    uint32_t materialIndex; /* which material slot, XUD_BATCH_NO_MATERIAL while no material exists */
};

typedef struct XudBatch XudBatch;
struct XudBatch {
    XudBatchStream stream[XUD_BATCH_STREAM_COUNT];

    VkBuffer indexBuffer; /* the one index buffer of the settled decision */
    VkBufferCreateInfo indexBufferInfo;
    XudMemoryRange indexRange;

    VkBuffer stagingBuffer; /* the host-visible arena the CPU-side bytes are written into */
    VkBufferCreateInfo stagingBufferInfo;
    XudMemoryRange stagingRange;
    void* stagingMapped; /* what vkMapMemory wrote, for the WHOLE arena, mapped once at open and unmapped once at close - the repo's "no
                            vkMapMemory per frame without a matching vkUnmapMemory" rule read as "map it once, always" */
    uint32_t stagingMappedLive; /* 1 while that mapping exists, 0 after xud_batch_close() unmapped it */

    /* --- the arena's layout, computed once per capacity by xud_batch_layout() and kept here so
       the copy regions and the appends read the same offsets the buffer sizes were built from.
       The arena is a fixed layout, not a bump allocator: the batch's content is rewritten from
       offset 0 every frame, which is what makes "the arena is one buffer, rewritten in place" true
       without a second cursor to keep honest. (The arena itself is NOT transient: see the note in
       xud_batch_open's doc on why it is carved from the static blocks rather than from the alloca
       tor's ring, whose cursor wraps.) --- */
    VkDeviceSize streamOffset[XUD_BATCH_STREAM_COUNT];
    VkDeviceSize indexOffset;
    VkDeviceSize stagingBytes; /* the arena's total, what the staging buffer was created for */

    uint32_t vertexCapacity; /* vertices every stream can hold, the one capacity all four share */
    uint32_t indexCapacity; /* indices the index buffer can hold */
    uint32_t vertexCount; /* live vertices - the invariant's subject: every stream holds this many */
    uint32_t indexCount; /* live indices */

    /* --- the rows the pipeline rung below and the frame loop above are handed --- */
    VkVertexInputBindingDescription bindings[XUD_BATCH_STREAM_COUNT];
    VkVertexInputAttributeDescription attributes[XUD_BATCH_STREAM_COUNT];
    VkBufferCopy copyRegions[XUD_BATCH_COPY_REGION_COUNT];

    /* --- the flush state: what the pending batch of draws was accumulated under --- */
    uint32_t currentTopology; /* the topology the pending draws are assembled as - raylib's
                                    RLGL.currentBatch->draws[].mode, moved by a shape's
                                    xud_batch_set_topology rather than by the caller, because in
                                    raylib it is the SHAPE that calls rlBegin(mode) */
    uint32_t currentVariant; /* the variant the pending draws use */
    uint32_t currentMaterial; /* the material slot they use, XUD_BATCH_NO_MATERIAL while none exists */
    uint32_t pendingFirstIndex; /* where the pending draws' indices start */
    uint32_t pendingIndexCount; /* how many indices they take */

    /* --- the recorded draws, and the counter a test reads --- */
    XudBatchDraw* draws; /* heap array, grown on the doubling sequence below */
    uint32_t drawCount; /* rows above that are live */
    uint32_t drawCapacity; /* rows allocated for them */
    uint32_t growCount; /* how many times the capacity doubled - the counter a test reads to know the growth path ran more than once */
};

static_assert(sizeof(((XudBatch*)0)->bindings) / sizeof(VkVertexInputBindingDescription) == XUD_BATCH_STREAM_COUNT,
    "the binding array must be exactly the count the pipeline's vertex-input row will claim");
static_assert(sizeof(((XudBatch*)0)->attributes) / sizeof(VkVertexInputAttributeDescription) == XUD_BATCH_STREAM_COUNT,
    "the attribute array must be exactly the count the pipeline's vertex-input row will claim");
static_assert(sizeof(((XudBatch*)0)->copyRegions) / sizeof(VkBufferCopy) == XUD_BATCH_COPY_REGION_COUNT,
    "the copy-region array must be exactly the count vkCmdCopyBuffer will be handed");

/*-----------------------------------------------------------------------------------------------
 * The layer's contract
 *-----------------------------------------------------------------------------------------------*/

/// Pure: the arena's layout for one vertex capacity - the four stream offsets, the index offset,
/// the arena total and the index capacity. This is the "vertex/index arithmetic" and the
/// "capacity-doubling bounds" half of the rung with the driver calls taken out of it: every
/// number below is a fact about the layout, checkable with no device present, and the buffer
/// sizes, the copy-region sizes and the append offsets are all read off these same rows, so
/// an append that would run past a buffer is a refusal rather than an out-of-bounds write.
/// XUD_INVALID_ARG on a NULL batch; XUD_UNSUPPORTED when the capacity is 0 or above the bound
/// the static_asserts above prove fit the allocator's block ceiling.
XudResult xud_batch_layout(XudBatch* batch, uint32_t vertexCapacity);

/// Pure: the capacity the next grow step would use, or 0 when the capacity is already at the
/// bound. Doubling is the whole policy - the same doubling the allocator's blocks and the
/// handle table's slots use - and the bound is the allocator's, so a batch that has reached it is
/// full rather than "about to ask for more memory than the allocator's blocks can hold". XUD_INV
/// ALID_ARG on a NULL out-pointer; XUD_OK with *outNext 0 at the bound.
XudResult xud_batch_next_capacity(uint32_t vertexCapacity, uint32_t* outNext);

/// Pure: the mask of the caps' memory types the caps call host-visible, which is the one require
/// ment the staging arena's allocator request carries. The arena is the only buffer of this rung
/// that CPU code writes into, so "host-visible" is a requirement rather than a preference, and a
/// caps that answers no host-visible type answers 0 bits, which is what makes the arena's allocat
/// ion XUD_UNSUPPORTED rather than a mapping of memory CPU code cannot read. XUD_INVALID_ARG on a
/// NULL caps or out-pointer; XUD_INVALID_ARG when the caps' type count is above the header's own
/// bound, which is the same row xud_allocator.h checks rather than a second rule.
XudResult xud_batch_host_visible_bits(const XudDeviceCaps* caps, uint32_t* outMemoryTypeBits);

/// Pure: the one binding row of one stream - binding number = the stream index, stride = the
/// stream's element size, rate PER_VERTEX. Gated on the caps' maxVertexInputBindings and
/// maxVertexInputAttributes: the repo's "check before you use" rule read at the vertex-input
/// limits, so a device that answers fewer than four of either gets XUD_UNSUPPORTED rather than
/// a pipeline whose vertex-input state the driver would reject. XUD_INVALID_ARG on a NULL
/// out-row; XUD_UNSUPPORTED when the caps cannot hold this row.
XudResult xud_batch_fill_binding(VkVertexInputBindingDescription* outRow, const XudDeviceCaps* caps, uint32_t streamIndex);

/// Pure: the one attribute row of one stream - location = the stream index (the GLSL's `in`
/// location), binding = the stream index, format = the stream's format, offset 0 (each stream is
/// its own buffer, so an attribute never sits at a non-zero offset inside one - that is what
/// SoA buys against an interleaved stream). Same caps gate as the binding row above. XUD_
/// INVALID_ARG on a NULL out-row or a stream index past the four; XUD_UNSUPPORTED when the caps
/// cannot hold it.
XudResult xud_batch_fill_attribute(VkVertexInputAttributeDescription* outRow, const XudDeviceCaps* caps, uint32_t streamIndex);

/// Pure: the batch's whole vertex-input state, into the struct's own rows - the four bindings and
/// the four attributes, in stream order, each built by the two calls above so the rows the
/// pipeline is handed and the rows vkCmdBindVertexBuffers is handed are one set of bytes. XUD_
/// INVALID_ARG on a NULL batch; XUD_UNSUPPORTED when the caps cannot hold four of either.
XudResult xud_batch_fill_vertex_input(XudBatch* batch, const XudDeviceCaps* caps);

/// Pure: the batch's own five copy rows, in stream order then the index buffer's, each from its
/// arena offset to offset 0 in its own buffer, each the live byte count rather than the capacity.
/// One row per destination rather than one array for one call, because vkCmdCopyBuffer names ONE
/// source and ONE destination buffer: five destinations is five calls, each carrying the one row
/// below, and the arena stays the single source of all five. XUD_INVALID_ARG on a NULL batch;
/// XUD_UNSUPPORTED when a live byte count is 0 (a copy of nothing is not a region, and vkCmdCopy
/// Buffer's VUID makes a zero-size region a hard error).
XudResult xud_batch_fill_copy_regions(XudBatch* batch);

/// Pure: the invariant the settled decision states - "every stream holds the same vertex count".
/// Read here as the two halves of it: every stream's capacityBytes divided by its elementSize is
/// the batch's own vertexCapacity (so one vertex index addresses all four buffers legally and
/// never the fifth byte of a shorter one), and the live vertex/index counts are inside those
/// capacities. XUD_OK when the batch holds the invariant; XUD_UNSUPPORTED when it does not,
/// which is a broken state of this rung's own record rather than a bad argument from a caller.
XudResult xud_batch_check_invariant(const XudBatch* batch);

/// Pure: the flush decision. A batch is flushed when the draw being appended would change the
/// pipeline variant or the material slot it was accumulated under, or when appending its
/// vertices/indices would pass the capacity. That is the whole rule of "flush on pipeline/
/// material change", and it is pure because the decision is arithmetic on the batch's own rows
/// - the driver work a flush does is the frame loop's, one rung up. The topology is NOT part of
/// this decision and takes no argument here: a topology change is a row split inside the SAME batch
/// (raylib's rlBegin(mode) at reference/raylib/rlgl.h 1454-1478 ends in `drawCounter++`, not in a
/// render batch) and xud_batch_set_topology below is the call that does it; this rung keeps the two
/// apart because they cost different things - a row split costs one more recorded row and one more
/// pipeline bind in the same command buffer, a flush costs a submission. XUD_INVALID_ARG on a NULL
/// batch/out-flush or a variant at or past the variant list; XUD_OK with *outFlush 1 or 0.
XudResult xud_batch_should_flush(uint32_t* outFlush, const XudBatch* batch, uint32_t requestedVariant, uint32_t requestedMaterial,
    uint32_t appendVertices, uint32_t appendIndices);

/// Pure: record one draw row - the index range of the pending batch plus the topology/variant/
/// material it was accumulated under - and reset the pending pair for the next batch. This is the
/// batch's own half of "the draw's whole argument pair": firstIndex/indexCount, the two arguments
/// of vkCmdDrawIndexed that the frame loop will hand, plus the two numbers the frame loop binds a
/// pipeline with beside that draw - which is the Vulkan form of raylib's replay reading `mode` off
/// the row itself (rlgl.h 3116's glDrawArrays(batch->draws[i].mode, ...)). The row array doubles
/// on the sequence the rest of this tree uses, so a record is never refused for being at a bound;
/// XUD_INVALID_ARG on a NULL batch, a topology at or past the topology list or a variant at or
/// past the variant list; XUD_OUT_OF_MEMORY only when that doubling step fails on the host.
XudResult xud_batch_record_draw(XudBatch* batch, uint32_t topology, uint32_t variant, uint32_t materialIndex);

/// Pure: move the batch's topology state, and close the pending run into a draw row of the SAME
/// batch first if the state actually changes and the run has content in it. This is raylib's
/// `rlBegin(mode)` read at its other half (reference/raylib/rlgl.h 1454-1478): on a mode change it
/// closes the current run (`drawCounter++`, a new row) and then writes the new mode into the new
/// row - it draws nothing and it does not end the batch. The SHAPE calls this rather than the
/// caller, because in raylib it is the shape function that calls rlBegin (rshapes.c's DrawLine3D
/// opens with `rlBegin(RL_LINES)`, and raylib.h 1585's own comment says so), and a caller has no
/// topology to ask for: a line is a line because of its geometry. This is the one place a batch
/// records a row mid-frame, and it is what makes a frame that mixes topologies more than one row -
/// the fact the frame loop's multi-draw replay was already built for. It is NOT a flush: the batch
/// stays open, its earlier bytes stay where they are, and the frame's upload still happens once at
/// the boundary. XUD_INVALID_ARG on a NULL batch or a topology at or past the topology list.
XudResult xud_batch_set_topology(XudBatch* batch, uint32_t topology);

/// Pure: ONE recorded draw row by index, so the frame loop above can replay the flush list without
/// reaching into the batch's own heap array (the repo's accessor rule, one rung below). XUD_INVALID_ARG
/// on a NULL batch/out-row or an index at or above the batch's recorded count.
XudResult xud_batch_draw_get(const XudBatch* batch, uint32_t index, XudBatchDraw* outRow);

/// Pure: append one vertex's four attribute values into the arena at the live vertex index, and
/// bump the count. The write is a memcpy into the mapped arena - the only host write this rung
/// makes, and the barrier that makes it visible to the transfer is the frame loop's, one rung
/// up. XUD_INVALID_ARG on a NULL batch or a NULL attribute array; XUD_OUT_OF_MEMORY when the
/// arena is not mapped (there is nowhere to write, and the grow that would map a bigger one is
/// xud_batch_grow's) or when the batch is at capacity.
XudResult xud_batch_append_vertex(XudBatch* batch, const float pos[3], const float uv[2], const uint8_t color[4], const float normal[3]);

/// Pure: append one index. XUD_INVALID_ARG on a NULL batch or an index at or above the live
/// vertex count (an index that names no vertex is a bad argument, not a draw that reads past
/// the streams); XUD_OUT_OF_MEMORY when the index buffer is at capacity.
XudResult xud_batch_append_index(XudBatch* batch, uint32_t index);

/// Pure: reset the live counts to 0, which is what a fresh frame's batch starts from. The
/// capacities and the buffers stay - the arena's content is simply overwritten next frame, which i
/// s the same property the allocator's ring note states for the ring, except that this arena is s
/// tatic, so it is overwritten in place rather than handed out again. The flush state resets with
/// the counts: the next frame's first shape starts at the default topology and the opaque variant,
/// not at whatever the last frame's last shape left behind. XUD_INVALID_ARG on a NULL batch.
XudResult xud_batch_reset(XudBatch* batch);

/// Pure: the counters a caller above reads - the live vertices, the live indices, the capacity,
/// the recorded draws and the arena's total. The headless-testable half of the whole rung.
/// XUD_INVALID_ARG on a NULL batch or out-pointer.
XudResult xud_batch_stats(const XudBatch* batch, uint32_t* outVertexCount, uint32_t* outIndexCount, uint32_t* outVertexCapacity,
    uint32_t* outDrawCount, VkDeviceSize* outStagingBytes);

/// Driver: the whole rung up in one call. Order: the arena's layout for the initial capacity, the
/// staging VkBuffer (usage TRANSFER_SRC, sized to the arena), its memory requirements, the STATI
/// C range for it at a memoryTypeBits narrowed to the caps' host-visible rows (the narrowing is a
/// preference, not a lie: every type in the narrowed mask is also in the buffer's own mask, so th
/// e bind below is still valid against the real requirements), the bind, the map of the whole ar
/// ena, then the four stream VkBuffers (usage VERTEX_BUFFER | TRANSFER_DST) and the index VkBuf
/// fer (usage INDEX_BUFFER | TRANSFER_DST), each bound to a range of the allocator. The arena is
/// carved from the static blocks rather than from the allocator's transient ring, and the reason i
/// s the ring's own contract: a ring is a cursor that wraps, so anything it handed out at one lap i
/// s overwritten at a later one, and this arena holds the frame's bytes from the moment the append
/// s wrote them to the moment the copies read them - a static carve is the one that lives that lon
/// g. Every destroy row this teardown needs is checked before the create that would otherwise lea
/// k, and the map row and the unmap row are checked before the map. XUD_UNSUPPORTED when the caps h
/// ave no host-visible type the staging buffer may bind to, or when the type the allocator's own r
/// ule picks for the narrowed mask is not host-visible (the check-before-you-use rule read at the
/// mapping rather than at the map: an unmappable arena is a batch that cannot be written, not a b
/// atch that writes into a type the driver will refuse). XUD_INVALID_ARG on a NULL out-pointer or
/// one that already owns a batch, a NULL loader/device/caps/allocator, a handle-less device; XUD_O
/// UT_OF_MEMORY/XUD_OUT_OF_RESOURCES on a host or driver allocation failure; XUD_VULKAN_ERROR oth
/// erwise. On any error *outBatch stays NULL and everything created so far is destroyed in rever
/// se order.
XudResult xud_batch_open(XudBatch** outBatch, XudLoader* loader, const XudDevice* device, const XudDeviceCaps* caps, XudAllocator* alloc);

/// Driver: double the capacity - destroy the six buffers and release their ranges, lay out the
/// doubled capacity, create and bind the six again. The grow is a recreate at a frame boundary,
/// never inside a recording, which is what the repo's "no vkCreateBuffer inside a frame" rule
/// asks for and what makes the frame loop's refusal of an over-capacity append the only honest
/// answer mid-frame. A grow that FAILS leaves the batch retired - no buffers, no mapping - because
/// the recreate retires before it creates, so the caller above closes the batch rather than appen
/// ding into it. XUD_UNSUPPORTED at the bound (the batch is full, and full is a decision, not a
/// growth step); XUD_INVALID_ARG on a NULL batch/loader/device/caps/allocator or a batch that own
/// s no buffers; the driver-half codes of the creates above.
XudResult xud_batch_grow(XudBatch* batch, XudLoader* loader, const XudDevice* device, const XudDeviceCaps* caps, XudAllocator* alloc);

/// Driver: record the batch's upload into a command buffer that has already been begun - five
/// vkCmdCopyBuffer calls, each carrying the one region for one destination, from the mapped arena
/// into the five buffers. Five calls rather than one call of five regions because vkCmdCopyBuffer
/// names ONE source and ONE destination buffer and the five destinations are five buffers - that
/// is why the SoA decision's unit of copying is a stream. The
/// call is legal only OUTSIDE a render pass instance, so the frame loop records it before
/// vkCmdBeginRenderPass; the two barriers that make the host write visible to the transfer and
/// the transfer visible to the vertex shader are the frame loop's too, and are named there.
/// XUD_INVALID_ARG on a NULL batch/loader/device or a handle-less batch/arena; XUD_UNSUPPORTED
/// when the arena is not mapped or the copy row is nil; XUD_OK only when every region has a
/// non-zero size, which xud_batch_fill_copy_regions() is the guard of.
XudResult xud_batch_upload(XudBatch* batch, XudLoader* loader, const XudDevice* device, VkCommandBuffer commandBuffer);

/// Driver: the teardown - unmap the arena FIRST (the repo's rule that a mapped range has a
/// matching unmap, read at the one place a batch dies), then destroy the six buffers in the
/// reverse of their creation and release their ranges, then free. The ranges go free AFTER the
/// buffers that name them are gone, which is the ownership direction the allocator states: a
/// buffer bound to memory that has been freed is a buffer whose contents the driver is still
/// reading. XUD_ALREADY when the pointer owns no batch; XUD_INVALID_ARG on a NULL pointer-to-
/// pointer, a NULL loader/device/allocator or a handle-less device; XUD_UNSUPPORTED when a dest
/// roy row is nil while the object it would destroy is live (the struct is not freed, because fre
/// eing it would leak the handles silently).
XudResult xud_batch_close(XudBatch** ioBatch, const XudDevice* device, XudLoader* loader, XudAllocator* alloc);

#endif /* XUD_BATCH_H_ */
