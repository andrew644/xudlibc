/***********************************************************************************************
 *   xud_allocator.h - the GPU-memory allocator rung: the one owner of every VkDeviceMemory in
 *                     this tree, the free list that suballocates inside the ones it created,
 *                     and the transient ring the per-frame data comes out of
 *
 *   Owns : XudAllocator (the block rows vkAllocateMemory handed it, the range rows that
 *          suballocate inside them, and the one ring block) and the contract of the rung -
 *          bring-up, the memory-type rule read off the physical layer's recorded rows, create-
 *          info assembly, first-fit suballocation with a generation bump on reuse, release with
 *          coalescing, the transient ring's cursor, the counters a caller above reads, and
 *          teardown.
 *   Layer: above the device and below the renderer. It is the rung the design sketch's "allocate
 *          once, suballocate always" rule belongs to, and the last rung of the ladder under the
 *          renderer: every buffer, image and staging ring the shapes/batch/descriptor steps land
 *          is a range of this layer, and nothing above it may call vkAllocateMemory of its own.
 *          AGENTS.md settled this allocator on 2026-10-23 as the library's own rather than VMA's,
 *          for a reason that is still true here: the free list over (memoryTypeIndex, size,
 *          alignment) triples is exactly the pure half that stays testable with no device present,
 *          and an allocator hidden behind VMA's interface would leave that half unexercisable.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#ifndef XUD_XUD_ALLOCATOR_H_
#define XUD_XUD_ALLOCATOR_H_

#include "volk.h" /* Required for: VK_NO_PROTOTYPES - must precede <vulkan/vulkan.h>, see xud_loader.h */
#include "xud.h" /* Required for: XudResult - the allocator layer's own error codes */
#include "xud_device.h" /* Required for: XudDevice - the handle every allocate/free row is asked of, and the wait-idle guard the teardown runs through */
#include "xud_loader.h" /* Required for: XudLoader - the Volk rows every call here is made through */
#include "xud_physical.h" /* Required for: XudDeviceCaps - the memory rows this layer's rules are read off */
#include "xud_vulkan.h" /* Required for: xud_result_from_vk - every VkResult here goes through that one funnel */
#include <vulkan/vulkan.h> /* Required for: VkDeviceMemory, VkMemoryAllocateInfo, VkMemoryRequirements, the memory-type and heap rows, the create-info stamp - never linked */

#include <assert.h> /* Required for: static_assert, the compile-time checks below */
#include <stdbool.h>
#include <stdint.h>

/*-----------------------------------------------------------------------------------------------
 * Why this rung is xud_allocator and not a vkAllocateMemory call inside the renderer
 *-----------------------------------------------------------------------------------------------*/

/* AGENTS.md's rule "no vkAllocateMemory inside a frame" and "one GPU-memory allocator owns all
   buffers/images" are rules of a layer, not of a caller: a block this layer creates has to be
   freed exactly once, and vkFreeMemory returns void, so a block nobody owns is a block nobody
   can prove was freed. Owning the rows here is what makes that a fact. It is also what makes the
   suballocation rule a fact: a range is an (offset, size) pair inside a block this layer created,
   and the generation bump on reuse is the same stale-handle discipline the handle layer states one
   rung up, read here at the one place a range goes free. */

/*-----------------------------------------------------------------------------------------------
 * The facts this layer's shape rests on
 *-----------------------------------------------------------------------------------------------*/

/* Header facts, grepped in /usr/include/vulkan/vulkan_core.h (Vulkan-Headers 1.4.363), no driver
   call: VkMemoryAllocateInfo is 3783 = { sType, pNext, allocationSize, memoryTypeIndex }, so one
   VkDeviceMemory per allocation and the type is an INDEX into the caps' memoryTypes[], not a mask;
   VkMemoryRequirements is 3790 = { size, alignment, memoryTypeBits }, the bitmask of types a
   buffer/image may be bound to, and the allocate info picks ONE index that must be set in it; the
   stamp is VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO = 5 (212); PFN_vkAllocateMemory (4551) returns
   VkResult and PFN_vkFreeMemory (4552) returns void - the same zero-state-before-the-call rule
   vkDestroyDevice/vkGetDeviceQueue follow. VK_MAX_MEMORY_TYPES is 32U and VK_MAX_MEMORY_HEAPS is
   16U, the fixed bounds of the arrays inside the core VkPhysicalDeviceMemoryProperties the caps
   embed. Naming trap, already recorded in AGENTS.md: grepping VK_MEMORY_TYPE across the whole
   /usr/include/vulkan/ tree gives zero hits - a memory type is VkMemoryType { propertyFlags,
   heapIndex }, a property-flag/heap-index pair keyed by its index, so the caps row a step calls
   "UNIFORM_BUFFER" is a label for a row and not a constant anyone can write. */
static_assert(VK_MAX_MEMORY_TYPES == 32u, "xud_allocator.h: the caps' memoryTypes[] bound as read here must be the header's own");
static_assert(VK_MAX_MEMORY_HEAPS == 16u, "xud_allocator.h: the caps' memoryHeaps[] bound as read here must be the header's own");
static_assert(sizeof(VkDeviceSize) == 8u,
    "xud_allocator.h: every size and offset below is a VkDeviceSize, and the arithmetic below assumes it is 64-bit");
static_assert(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO == 5u,
    "xud_allocator.h: the stamp xud_allocator_fill_allocate_info() writes is the header's own value, asserted not copied");

/* The two bounds of one block this layer creates. Neither is provable - nothing about Vulkan says
   how big a block should be - so both are this layer's own policy, and both are the reason the
   rule "allocate once, suballocate always" is a rule rather than a wish: a block below MIN is a
   vkAllocateMemory per object, which is the churn the rule forbids, so a request that has to
   create a block creates one of at least MIN and rounds up to the next doubling step. MAX is the
   ceiling of that doubling sequence: a request larger than MAX is refused rather than served by
   one unbounded block, because an unbounded host-side free list is an unbounded bookkeeping cost.
   The doubling shape is the one the rest of this tree uses. */
#define XUD_ALLOCATOR_BLOCK_MIN_SIZE (64u * 1024u)
#define XUD_ALLOCATOR_BLOCK_MAX_SIZE (16u * 1024u * 1024u)

static_assert(XUD_ALLOCATOR_BLOCK_MIN_SIZE > 0u,
    "xud_allocator.h: a block must have room for at least one byte, or the first block is a growth step and a zero-size allocation is what "
    "the validation layer refuses here");
static_assert(XUD_ALLOCATOR_BLOCK_MIN_SIZE <= XUD_ALLOCATOR_BLOCK_MAX_SIZE,
    "xud_allocator.h: the doubling sequence must start at or below the bound it stops at");
static_assert(XUD_ALLOCATOR_BLOCK_MAX_SIZE % XUD_ALLOCATOR_BLOCK_MIN_SIZE == 0u,
    "xud_allocator.h: the doubling sequence must reach the bound exactly, so the loop that grows it cannot step past it and refuse a "
    "request one row too early");

/* The transient ring's one size. The design sketch's "per-frame data comes from a ring/transient
   allocation" is a fixed window, not an unbounded arena: a transient request larger than the whole
   ring is a caller bug rather than a refusal worth growing for, and the renderer step is what
   decides how big this window has to be. A wrap is legal because the contract of the ring is that
   nothing it hands out lives past the frame it was asked for in - see the note below. */
#define XUD_ALLOCATOR_RING_SIZE (256u * 1024u)

static_assert(XUD_ALLOCATOR_RING_SIZE >= XUD_ALLOCATOR_BLOCK_MIN_SIZE,
    "xud_allocator.h: the ring is one block of this layer, so it must be at least as large as the smallest block this layer will create");

/* A free range is only split when the remainder is worth a range row of its own. Below this, the
   whole range is handed out and the tail is wasted inside a live range rather than carried as a
   free row that can never serve a request of its own - a free row smaller than the smallest thing
   this layer will ever ask for is bookkeeping that buys nothing. */
#define XUD_ALLOCATOR_SPLIT_MIN (64u)

static_assert(XUD_ALLOCATOR_SPLIT_MIN > 0u, "xud_allocator.h: a split must leave at least one byte, or a split is a zero-size free range");

/* The two bounds of this layer's own host-side arrays, in the doubling shape the rest of the tree
   uses (xud_instance.c, xud_physical.c, xud_swapchain.c's image query, xud_handle.c's slot
   arrays). A block count at MAX and a range count at MAX are refusals reported, not cut arrays: a
   cut block array is a VkDeviceMemory that is no longer reachable and therefore never freed. */
#define XUD_ALLOCATOR_BLOCK_MIN_CAPACITY 4u
#define XUD_ALLOCATOR_BLOCK_MAX_CAPACITY 256u
#define XUD_ALLOCATOR_RANGE_MIN_CAPACITY 16u
#define XUD_ALLOCATOR_RANGE_MAX_CAPACITY 4096u

static_assert(XUD_ALLOCATOR_BLOCK_MIN_CAPACITY > 0u, "xud_allocator.h: a block array must start with room for at least one row");
static_assert(XUD_ALLOCATOR_BLOCK_MIN_CAPACITY <= XUD_ALLOCATOR_BLOCK_MAX_CAPACITY,
    "xud_allocator.h: the doubling sequence must start at or below the bound it stops at");
static_assert(XUD_ALLOCATOR_BLOCK_MAX_CAPACITY % XUD_ALLOCATOR_BLOCK_MIN_CAPACITY == 0u,
    "xud_allocator.h: the doubling sequence must reach the bound exactly");
static_assert(XUD_ALLOCATOR_RANGE_MIN_CAPACITY > 0u, "xud_allocator.h: a range array must start with room for at least one row");
static_assert(XUD_ALLOCATOR_RANGE_MIN_CAPACITY <= XUD_ALLOCATOR_RANGE_MAX_CAPACITY,
    "xud_allocator.h: the doubling sequence must start at or below the bound it stops at");
static_assert(XUD_ALLOCATOR_RANGE_MAX_CAPACITY % XUD_ALLOCATOR_RANGE_MIN_CAPACITY == 0u,
    "xud_allocator.h: the doubling sequence must reach the bound exactly");

/* The alignment ceiling. VkMemoryRequirements::alignment is a VkDeviceSize and a driver may answer
   anything, so the ceiling is this layer's own: rounding an offset up to an alignment is (offset +
   align - 1) rounded down, and an alignment above this bound is not rounded at all - it is refused
   as a bad argument rather than honoured with an expression that can overflow. The bound used to be
   256, which was a fact about the BUFFERS this tree creates (the largest element its own vertex st
   reams name is 16 bytes, and the caps' minAlignmentForExtendedData is at or below 256); it is not
   a fact about an IMAGE. Probed today on this machine: vkGetImageMemoryRequirements for the depth
   attachment xud_renderpass.c creates - VK_FORMAT_D32_SFLOAT, VK_IMAGE_TILING_OPTIMAL, one use of
   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, at the swapchain's extent - answers size 1397440,
   alignment 65536, memoryTypeBits 0x7. 64 KiB is the tile-aligned form lavvk picks for a depth att
   achment under OPTIMAL tiling, and it is a requirement, not a suggestion: an image bound at an off
   set that does not satisfy it is an image whose texels the fixed-form depth hardware reads are no
   t where the library thinks they are. So the bound is the largest alignment this layer will pad t
   o, and 65536 is that answer with the headroom this driver's own answer gives it. A request above
   it is REPORTED (XUD_INVALID_ARG from the acquire, XUD_UNSUPPORTED from the rung above that maps a
   driver fact to that code), never quietly served at a smaller alignment - a depth buffer carved a
   t 256 when the driver asked for 65536 is a depth buffer that works on this driver and is wrong o
   n another, which is the opposite of the check-before-you-use rule. The overflow itself is still
   checked on both sides of the rounding, in xud_allocator_align_up(). */
#define XUD_ALLOCATOR_ALIGN_MAX (65536u)

static_assert(XUD_ALLOCATOR_ALIGN_MAX >= XUD_ALLOCATOR_BLOCK_MIN_SIZE,
    "xud_allocator.h: a block must be at least as large as the largest alignment this layer will pad to, or a padded carve is unreachable");

static_assert(
    XUD_ALLOCATOR_ALIGN_MAX > 0u, "xud_allocator.h: an alignment of zero is not an alignment, and the rounding below divides by it");

/* The sentinel xud_allocator_find_range() writes when no free range can serve the request - the
   repo's sentinel pattern (XUD_PHYSICAL_NO_QUEUE_FAMILY is the other one in this tree), chosen
   rather than an error code because "no room right now" is not a refusal: it is the question
   "create a block instead", and the caller above decides. */
#define XUD_ALLOCATOR_NO_RANGE 0xFFFFFFFFu

static_assert(XUD_ALLOCATOR_NO_RANGE > XUD_ALLOCATOR_RANGE_MAX_CAPACITY,
    "xud_allocator.h: the sentinel must be above every range index this layer can ever hand out");

/* The sentinel xud_allocator_ring_acquire() leaves in ringBlockIndex until the ring's block exists -
   the repo's sentinel pattern again (XUD_PHYSICAL_NO_QUEUE_FAMILY and XUD_ALLOCATOR_NO_RANGE are the
   other two in this tree), and the reason it is a value rather than a NULL pointer is that a block
   row is an index, not a handle. */
#define XUD_ALLOCATOR_NO_BLOCK 0xFFFFFFFFu

static_assert(XUD_ALLOCATOR_NO_BLOCK > XUD_ALLOCATOR_BLOCK_MAX_CAPACITY,
    "xud_allocator.h: the no-ring sentinel must be above every block index this layer can ever hand out");

/*-----------------------------------------------------------------------------------------------
 * What a block and a range record
 *-----------------------------------------------------------------------------------------------*/

/* One VkDeviceMemory this layer created. `memory` is the only Vulkan handle owned here above the
   device: there is no vkDestroyMemory, teardown is vkFreeMemory, and the row is what makes that
   call reachable after the fact. `size` is what vkAllocateMemory was ASKED for, which is what the
   free list may hand out - a driver is free to back a block with more, but nothing here may assume
   more than what was asked. `memoryTypeIndex` is the caps row it was allocated at, kept so the
   free list can re-check a range against a later request's memoryTypeBits without re-deriving it.
 */
typedef struct XudAllocBlock XudAllocBlock;
struct XudAllocBlock {
    VkDeviceMemory memory; /* the VkDeviceMemory vkAllocateMemory wrote, VK_NULL_HANDLE until created */
    VkDeviceSize size; /* what vkAllocateMemory was asked for */
    uint32_t memoryTypeIndex; /* the caps' memoryTypes[] row it was allocated at */
};

/* One suballocation range inside one block - live or free. This array IS the free list: a range is
   free when live is 0, and the only difference between a free range and a live one is that bit.
   Keeping both in one array is what makes release-and-coalesce one pass rather than a free list
   that has to be spliced back into a separate live list. `blockIndex` is an index into the block
   array above, not a copy of the handle: a block's handle never changes once created, and an index
   is what survives a growth step of the block array. `generation` is bumped when a range row is
   taken again, which is the stale-range rule read at the one place a range goes free. */
typedef struct XudAllocRange XudAllocRange;
struct XudAllocRange {
    uint32_t blockIndex; /* which block row of the allocator this range lives in */
    VkDeviceSize offset; /* where it starts inside that block */
    VkDeviceSize size; /* how long it is */
    uint32_t live; /* 0 = free (never used, or released), 1 = live */
    uint32_t generation; /* bumped when this row is taken again, so a range kept across a release stops naming it */
};

/* The currency a caller above holds: a range it was handed, plus enough identity to re-validate it.
   `memory` is BORROWED - the allocator owns it and this layer is the only place that frees it, so a
   caller above must never call vkFreeMemory on it (it cannot: it has no loader row of its own). The
   two indices plus the generation are the same index+generation+tag discipline xud.h states for a
   XudHandle, one rung down: a range kept across a release, or across a close, is refused rather than
   used to write into a block that has been recycled. */
typedef struct XudMemoryRange XudMemoryRange;
struct XudMemoryRange {
    VkDeviceMemory memory; /* BORROWED: the block this range lives in */
    VkDeviceSize offset;
    VkDeviceSize size;
    uint32_t blockIndex;
    uint32_t rangeIndex;
    uint32_t generation;
};

typedef struct XudAllocator XudAllocator;
struct XudAllocator {
    XudAllocBlock* blocks; /* the VkDeviceMemory rows this layer created, heap array, doubled to the bound above */
    uint32_t blockCount; /* rows above that are live */
    uint32_t blockCapacity; /* rows allocated for them, a step of the doubling sequence */
    XudAllocRange* ranges; /* the suballocations inside them, live and free, heap array */
    uint32_t rangeCount; /* rows above that exist at all (live or free) - the high mark */
    uint32_t rangeCapacity; /* rows allocated for them */
    uint32_t rangeLiveCount; /* rows above that are live - the counter xud_allocator_stats() reports */
    VkDeviceSize bytesInUse; /* the sum of every live range's size - the other counter it reports */

    /* --- the transient ring: one block, one cursor, no free list --- */
    VkDeviceMemory ringMemory; /* the ring's one VkDeviceMemory, VK_NULL_HANDLE until xud_allocator_ring_acquire() creates it */
    VkDeviceSize ringSize; /* what it was created with - XUD_ALLOCATOR_RING_SIZE once the block exists, 0 until */
    VkDeviceSize ringCursor; /* where the next transient request starts */
    uint32_t ringBlockIndex; /* which block row the ring is, XUD_ALLOCATOR_NO_BLOCK until the block
                        exists. The ring is one block of this layer, recorded in the block array like
                        every other one so xud_allocator_close() frees it with the rest and there is
                        no second teardown path for it; it is an index rather than a position because
                        other blocks can be created after it. */
    uint32_t ringMemoryTypeIndex; /* the caps row the ring block was allocated at, kept for the teardown's record */
};

/*-----------------------------------------------------------------------------------------------
 * The layer's contract
 *-----------------------------------------------------------------------------------------------*/

/// Pure: bring one allocator up for one context. Every array starts NULL, every count and cursor
/// starts 0, the ring block does not exist yet - the "nothing has been allocated on the GPU yet"
/// state, which is what keeps a context that never draws from paying for a block. The struct itself
/// is the only host allocation this call makes. XUD_INVALID_ARG on a NULL out-pointer or one that
/// already owns an allocator (two allocators for one context would be two owners of the same blocks,
/// and each would free them once); XUD_OUT_OF_MEMORY when the struct cannot be allocated.
XudResult xud_allocator_open(XudAllocator** outAlloc);

/// Pure: the memory-type rule, read entirely off the physical layer's recorded memory rows, with no
/// driver call. The answer is one index into the caps' memoryTypes[] that satisfies the request: the
/// type's bit must be SET in requirements.memoryTypeBits (VkMemoryRequirements is a bitmask of the
/// types a buffer/image may be bound to, and the allocate info picks one index that must be set in
/// it), the type's heap must exist in the caps and must be big enough for the size (VUID-vkAllocate
/// Memory-pAllocateInfo-01713, which the validation layer here reports as a hard error and the drive
/// r does not), and a type carrying VK_MEMORY_PROPERTY_PROTECTED_BIT is never a candidate - protecte
/// dMemory is a CHAINED feature struct this library does not chain and never enables, so a protected
/// allocation is a feature that is not here, which is the repo's "anything unverified stays off" rule
/// read at the allocation rung. Of the types left, a device-local one is preferred (the design sket
/// ch's whole point is that geometry and descriptor data live in device memory) and the lowest-index
/// remaining type is the fallback, because a type that is not device-local is still a type the requ
/// est may be bound to, not an absence of one. XUD_INVALID_ARG on a NULL argument; XUD_UNSUPPORTED
/// when no type satisfies it.
XudResult xud_allocator_select_memory_type(
    const XudDeviceCaps* caps, const VkMemoryRequirements* requirements, uint32_t* outMemoryTypeIndex);

/// Pure: stamp one VkMemoryAllocateInfo - the whole create-info assembly of this rung, and the
/// reason the headless run can assert it at all. sType is written, pNext is cleared (this layer
/// chains nothing: VkMemoryDedicatedAllocateInfo is the dedicated-allocation entry AGENTS.md settled
/// out, and its extension-name constant is not in these headers at all), allocationSize and memoryT
/// ypeIndex are the two this layer's rules decided. XUD_INVALID_ARG on a NULL out-info.
XudResult xud_allocator_fill_allocate_info(VkMemoryAllocateInfo* outInfo, uint32_t memoryTypeIndex, VkDeviceSize allocationSize);

/// Pure: the free-list rule - the first free range that can serve the request. "Can serve" is three
/// things: the block that holds the range was allocated at a type whose bit is set in the request's
/// memoryTypeBits, the range is at least as long as the request once the start is rounded up to the
/// request's alignment, and the range is free. First-fit, in row order, is this layer's policy: it
/// is deterministic (the testing rule), it is what makes a reused range the most recently freed one
/// rather than an arbitrary one, and it costs no sorting. XUD_INVALID_ARG on a NULL allocator or
/// out-parameter, or on an alignment above XUD_ALLOCATOR_ALIGN_MAX (see the note above); XUD_OK with
/// the range index and the ALIGNED offset inside it; XUD_OK with XUD_ALLOCATOR_NO_RANGE written when
/// no free range fits, which is not a refusal but the question "create a block instead".
XudResult xud_allocator_find_range(const XudAllocator* alloc, VkDeviceSize size, VkDeviceSize alignment, uint32_t memoryTypeBits,
    uint32_t* outRangeIndex, VkDeviceSize* outOffset);

/// Driver half: the one call that hands out GPU memory. The rule is the two halves above read
/// together - find a free range, and when there is none create a block of at least XUD_ALLOCATOR_
/// BLOCK_MIN_SIZE (or the request's own size when that is larger, rounded up along the doubling
/// sequence to XUD_ALLOCATOR_BLOCK_MAX_SIZE), at the type xud_allocator_select_memory_type() chose,
/// through loader->deviceTable.vkAllocateMemory with the create-info xud_allocator_fill_allocate_in
/// fo() assembled. The range is then split when the remainder is worth a row of its own (XUD_ALLO
/// CATOR_SPLIT_MIN, see the note above) and the row that serves the request is marked live with its
/// generation already at the value the returned range carries. Every guard runs BEFORE the driver
/// call: a zero size, an alignment above the bound, a memoryTypeBits of zero (no type can serve it),
/// a type index the caps do not have, a size above the chosen type's heap, a protected type the
/// device did not enable, and a range array at its bound are all refusals that call nothing. XUD_O
/// UT_OF_RESOURCES when vkAllocateMemory answers OUT_OF_DEVICE_MEMORY/OUT_OF_HOST_MEMORY, XUD_LOST
/// when it answers DEVICE_LOST, XUD_VULKAN_ERROR for any other non-VK_SUCCESS, XUD_OUT_OF_MEMORY
/// when a host growth step fails. On XUD_OK *outRange names a live range of at least the requested
/// size; on any error it is left untouched, so a caller cannot read a half-filled range.
XudResult xud_allocator_acquire(XudMemoryRange* outRange, XudAllocator* alloc, XudLoader* loader, const XudDevice* device,
    const XudDeviceCaps* caps, const VkMemoryRequirements* requirements);

/// Pure: release one live range. The row is marked free and its generation is bumped in the same
/// step, so the range in hand stops naming it and every range that named it before stops naming it
/// too - the stale-range rule read at the one place a range goes free, the same shape xud_handle_f
/// ree() states one rung up. Neighbouring free ranges in the SAME block are then coalesced into one,
/// which is what makes the free list not decay: a create/destroy loop over the same size returns the
/// same one free range rather than one more row per cycle. The block itself is never freed here -
/// there is no "this block is now empty" teardown, because vkFreeMemory on a block the renderer is
/// still reading would be a hazard, and the teardown that frees blocks is xud_allocator_close().
/// XUD_INVALID_ARG on a NULL argument, or on a range that does not name a live range of this
/// allocator (the index bounds, the generation equality and the live bit are the check, so a stale
/// range, a range from another allocator and a fabricated one are all refused here rather than
/// double-releasing a row or coalescing over a live one).
XudResult xud_allocator_release(XudAllocator* alloc, const XudMemoryRange* range);

/// Pure: the accessor that re-reads a range the caller above kept, exactly as xud_swapchain_images
/// _get() re-reads an index. A range is a value, and a value can be stale; this is the call that
/// turns it back into the truth of the allocator's own rows, so a caller above that kept a range
/// across a release or a close reads the current offset/size/memory rather than a copy that may
/// address a recycled block. XUD_INVALID_ARG on a NULL argument, or on a range that does not name a
/// live range of this allocator (same check as xud_allocator_release()); XUD_OK with *outCurrent
/// filled from the allocator's own row.
XudResult xud_allocator_range_get(const XudAllocator* alloc, const XudMemoryRange* range, XudMemoryRange* outCurrent);

/// Driver half: the transient ring. The first call creates the ring's one block (XUD_ALLOCATOR_RIN
/// G_SIZE, at the type the request's memoryTypeBits allows) and every call after that moves the
/// cursor. A request that does not fit between the cursor and the end of the ring wraps to the start
/// of the ring, which is the whole contract of a ring: nothing it hands out lives past the frame it
/// was asked for in, so overwriting what an earlier frame wrote is legal and is the point - the
/// renderer step is what guarantees that contract, and this layer's job is that the cursor arithmetic
/// is exact and that a request larger than the whole ring is refused rather than served by a wrap
/// that would hand out the same bytes twice. Every guard runs before the driver call, and the ring
/// block is created at most once, so the steady state of a frame loop calls nothing at all. XUD_INV
/// ALID_ARG on a NULL argument, a zero size, or an alignment above the bound; XUD_OUT_OF_RESOURCES
/// when the request is larger than the whole ring; the same driver-half codes as xud_allocator_acq
/// uire() on the one call that creates the block. On XUD_OK *outRange names a live range inside the
/// ring - live in the sense that it is counted, so a caller that releases it coalesces it back.
XudResult xud_allocator_ring_acquire(XudMemoryRange* outRange, XudAllocator* alloc, XudLoader* loader, const XudDevice* device,
    const XudDeviceCaps* caps, VkDeviceSize size, VkDeviceSize alignment, uint32_t memoryTypeBits);

/// Pure: rewind the ring cursor to 0, which is what EndDrawing does once per frame. The cursor is
/// the only state the ring has, so this is the whole reset - nothing is freed and nothing is marked
/// free, because the ring's blocks are one block and its old contents are simply overwritten next
/// frame. XUD_INVALID_ARG on a NULL allocator; XUD_OK otherwise, including on a ring whose block was
/// never created (a reset of an empty ring is already in effect, and is not an error).
XudResult xud_allocator_ring_reset(XudAllocator* alloc);

/// Pure: the counters a caller above reads - how many blocks exist, how many range rows are live,
/// how many bytes are in use, and where the ring cursor is. This is the headless-testable half of
/// the whole rung: the free list's invariants (every live range's size sums to bytesInUse, every
/// range row is inside its block, no two live ranges overlap) are checkable with no device present,
/// which is the reason AGENTS.md settled this allocator as ours rather than VMA's. XUD_INVALID_ARG
/// on a NULL allocator or out-parameter; XUD_OK otherwise, with zeros for an allocator that has
/// never allocated.
XudResult xud_allocator_stats(const XudAllocator* alloc, uint32_t* outBlockCount, uint32_t* outRangeLiveCount, VkDeviceSize* outBytesInUse,
    VkDeviceSize* outRingCursor);

/// Teardown: free every VkDeviceMemory this layer created, and the ring block with them. The guard
/// runs first, for the same reason xud_swapchain_close() states it: a presentation or a submission
/// may still be reading the very blocks being freed, and xud_device_wait_idle() is the boundary. A
/// guard that cannot run (XUD_UNSUPPORTED) is a teardown this rung must not start; a LOST device is
/// still freed and still reported, the same rule xud_device_close() follows, because skipping a free
/// would leak memory the driver still holds. vkFreeMemory returns void, so each call follows the
/// zero-state-before-the-call rule of the other void entry points in this tree: the row's handle is
/// read, cleared, and only then handed over, so a teardown that runs twice cannot double-free the
/// same handle. Then the host arrays are freed and cleared before the struct, so an allocator torn
/// down twice cannot double-free either. XUD_ALREADY on a pointer-to-pointer that is NULL-in-hand;
/// XUD_INVALID_ARG on a NULL pointer-to-pointer, a NULL loader, or a NULL device.
XudResult xud_allocator_close(XudAllocator** alloc, XudLoader* loader, const XudDevice* device);

#endif /* XUD_XUD_ALLOCATOR_H_ */
