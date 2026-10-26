/***********************************************************************************************
 *
 *   xud_test_allocator.c - the GPU-memory allocator: blocks, the free list, the transient ring
 *
 *   Owns : the memory-type rule read as a search over the caps' rows, the allocate-info assembly, the find/release/range/ring rules and the
 * stats - the pure half, which calls no driver function and is what the headless run exercises completely - plus the GPU half that
 * allocates, suballocates, grows to a second block, releases, reuses and wraps real VkDeviceMemory. Layer: tests. Sees xud_allocator.h and
 * xud_core.h: the GPU half runs against a completed context, which is the one thing that owns an allocator.
 *
 *   Deterministic: fixed loop counts, no time, no randomness, no filesystem.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud.h" /* Required for: XudResult, XUD_VERSION, the XUD_GPU_TESTS gate */
#include "xud_allocator.h" /* Required for: XudAllocator, XudAllocBlock, XudAllocRange, XudMemoryRange, the select/fill/find/release/ring rungs and the stats */
#include "xud_core.h" /* Required for: XudContext, the owner the GPU half runs against */
#include "xud_handle.h" /* Required for: xud_handle_free, the stale-handle rule of the allocator triple checked against it */
#include "xud_test_harness.h" /* Required for: xud_check, xud_gpu_tests_enabled, the area entry points */
#include <stdlib.h> /* Required for: calloc, malloc and free, the synthetic records this area builds and frees */
#include <string.h> /* Required for: memset, the synthetic rows this area zeroes and compares */

/*-----------------------------------------------------------------------------------------------
 * The allocator's pure half, with no driver anywhere: a synthetic caps (the memory rows the physic
 * al layer records, spelled here rather than queried) and a synthetic allocator whose rows are wri
 * tten directly. That is the shape the repo's headless-test rule asks for - the free list, the type
 * rule, the create-info assembly and the cursor arithmetic take structs and call nothing - and it i
 * s the same stand-in the physical layer's pure test uses for a caps struct. The driver halves of t
 * his rung (acquire, ring_acquire, close) are the GPU test's; what is checked of them here is that
 * every guard fires BEFORE a row of the loader's table is read, which is what makes a headless run
 * refuse rather than call through NULL.
 *-----------------------------------------------------------------------------------------------*/
static void xud_test_allocator_pure(void) {
    XudDeviceCaps caps;
    XudAllocator* alloc = NULL;
    XudAllocator* fresh = NULL;
    XudMemoryRange range;
    XudMemoryRange held;
    VkMemoryAllocateInfo info;
    VkMemoryRequirements ask;
    XudLoader loader;
    XudDevice device;
    uint64_t fakeMemoryHandle = 0xF00Du;
    VkDeviceMemory fakeMemory = VK_NULL_HANDLE;
    uint32_t typeIndex = 0u;
    uint32_t rangeIndex = 0u;
    uint32_t blockCount = 0u;
    uint32_t liveCount = 0u;
    VkDeviceSize offset = 0u;
    VkDeviceSize bytes = 0u;
    VkDeviceSize cursor = 0u;

    memset(&caps, 0, sizeof(caps));
    memset(&loader, 0, sizeof(loader));
    memset(&device, 0, sizeof(device));
    memset(&ask, 0, sizeof(ask));
    fakeMemory = (VkDeviceMemory)&fakeMemoryHandle;

    /* Four memory rows over two heaps, spelled to make each rule below observable rather than to
       mirror this driver (the real rows are the GPU test's): type 0 is the device-local one, type
       1 the host-visible one on a heap too small for most requests, type 2 the protected one that
       is never a candidate, and type 3 a device-local row naming a heap the caps do not have.
     */
    caps.memoryProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    caps.memoryProperties.memoryProperties.memoryTypeCount = 4u;
    caps.memoryProperties.memoryProperties.memoryHeapCount = 2u;
    caps.memoryProperties.memoryProperties.memoryHeaps[0].size = (VkDeviceSize)1024u * 1024u;
    caps.memoryProperties.memoryProperties.memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
    caps.memoryProperties.memoryProperties.memoryHeaps[1].size = 256u;
    caps.memoryProperties.memoryProperties.memoryHeaps[1].flags = VK_MEMORY_HEAP_MULTI_INSTANCE_BIT;
    caps.memoryProperties.memoryProperties.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    caps.memoryProperties.memoryProperties.memoryTypes[0].heapIndex = 0u;
    caps.memoryProperties.memoryProperties.memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    caps.memoryProperties.memoryProperties.memoryTypes[1].heapIndex = 1u;
    caps.memoryProperties.memoryProperties.memoryTypes[2].propertyFlags
        = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_PROTECTED_BIT;
    caps.memoryProperties.memoryProperties.memoryTypes[2].heapIndex = 0u;
    caps.memoryProperties.memoryProperties.memoryTypes[3].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    caps.memoryProperties.memoryProperties.memoryTypes[3].heapIndex = 7u;

    /* The open guards, and the empty state: no array, no block, no ring. A context that never allo
       cates a buffer pays for this struct and nothing else.
     */
    xud_check(xud_allocator_open(NULL) == XUD_INVALID_ARG, "allocator/open-null", "is refused");
    xud_check(xud_allocator_open(&fresh) == XUD_OK, "allocator/open", "is a fresh allocator");
    xud_check(fresh != NULL, "allocator/open/filled", "and filled");
    xud_check(xud_allocator_open(&fresh) == XUD_INVALID_ARG, "allocator/open-twice",
        "is refused, because two allocators for one context would be two owners of the same blocks");
    xud_check(fresh->blockCount == 0u, "allocator/empty/blocks", "no block row");
    xud_check(fresh->blocks == NULL, "allocator/empty/block-array", "and no array at all until the first block");
    xud_check(fresh->ranges == NULL, "allocator/empty/range-array", "and no range array until the first range");
    xud_check(fresh->ringMemory == VK_NULL_HANDLE, "allocator/empty/ring", "and no ring block");
    xud_check(fresh->ringBlockIndex == XUD_ALLOCATOR_NO_BLOCK, "allocator/empty/ring-index", "and the sentinel, not index 0");

    /* The type rule's NULL guards, and the poison out-index: a refusal must leave it alone, which
       is what makes "the out-param was written only on XUD_OK" an observable answer.
     */
    typeIndex = 0xA5A5u;
    xud_check(xud_allocator_select_memory_type(NULL, &ask, &typeIndex) == XUD_INVALID_ARG, "allocator/type-null-caps", "is refused");
    xud_check(xud_allocator_select_memory_type(&caps, NULL, &typeIndex) == XUD_INVALID_ARG, "allocator/type-null-ask", "is refused");
    xud_check(xud_allocator_select_memory_type(&caps, &ask, NULL) == XUD_INVALID_ARG, "allocator/type-null-out", "is refused");
    xud_check(typeIndex == 0xA5A5u, "allocator/type-invalid-untouched", "a rejected call must leave the index alone");

    ask.size = 4096u;
    ask.alignment = 256u;
    ask.memoryTypeBits = 0u;
    xud_check(xud_allocator_select_memory_type(&caps, &ask, &typeIndex) == XUD_UNSUPPORTED, "allocator/type-no-bits",
        "a memoryTypeBits of zero means no type can serve the request, which is the repo's UNSUPPORTED rather than a bad argument");
    xud_check(typeIndex == 0xA5A5u, "allocator/type-unsupported-untouched", "the index is written only on XUD_OK");

    ask.memoryTypeBits = 0xFu;
    xud_check(xud_allocator_select_memory_type(&caps, &ask, &typeIndex) == XUD_OK, "allocator/type-any",
        "a request every row may be bound to selects");
    xud_check(typeIndex == 0u, "allocator/type-device-local-wins",
        "the device-local row, which is the design sketch's whole point: geometry and descriptor data live in device memory");

    ask.memoryTypeBits = (1u << 1);
    ask.size = 100u;
    xud_check(xud_allocator_select_memory_type(&caps, &ask, &typeIndex) == XUD_OK, "allocator/type-host-only", "selects");
    xud_check(typeIndex == 1u, "allocator/type-host-index", "the host-visible row, because it is the only candidate left");

    ask.size = 512u;
    xud_check(xud_allocator_select_memory_type(&caps, &ask, &typeIndex) == XUD_UNSUPPORTED, "allocator/type-heap-too-small",
        "VUID-vkAllocateMemory-pAllocateInfo-01713: the heap the type names must be big enough, and the validation layer here reports it "
        "as a hard error while the driver does not");

    ask.size = 100u;
    ask.memoryTypeBits = (1u << 2);
    xud_check(xud_allocator_select_memory_type(&caps, &ask, &typeIndex) == XUD_UNSUPPORTED, "allocator/type-protected-only",
        "a protected type is never a candidate: protectedMemory is a CHAINED feature struct this library does not chain and never enables, "
        "so the feature that needed it is not here");

    ask.memoryTypeBits = (1u << 3);
    xud_check(xud_allocator_select_memory_type(&caps, &ask, &typeIndex) == XUD_UNSUPPORTED, "allocator/type-heap-missing",
        "a type whose heapIndex is past the caps' heapCount is not a type at all");

    ask.memoryTypeBits = (1u << 1) | (1u << 2);
    xud_check(xud_allocator_select_memory_type(&caps, &ask, &typeIndex) == XUD_OK, "allocator/type-protected-skipped", "selects");
    xud_check(typeIndex == 1u, "allocator/type-protected-skipped/index",
        "the host-visible row, with the protected one skipped rather than preferred");

    ask.memoryTypeBits = (1u << 0) | (1u << 3);
    ask.size = 4096u;
    xud_check(xud_allocator_select_memory_type(&caps, &ask, &typeIndex) == XUD_OK, "allocator/type-broken-row-skipped", "selects");
    xud_check(typeIndex == 0u, "allocator/type-broken-row-index", "the row that names a real heap, not the one that names none");

    /* The create-info assembly: every field written, the stamp the header asserts, and pNext left
       NULL because nothing is chained - dedicated allocation is the entry AGENTS.md settled out, an
       d its extension-name constant is not in these headers at all.
     */
    xud_check(xud_allocator_fill_allocate_info(NULL, 2u, 8192u) == XUD_INVALID_ARG, "allocator/info-null-out", "is refused");
    memset(&info, 0xFF, sizeof(info));
    xud_check(xud_allocator_fill_allocate_info(&info, 2u, 8192u) == XUD_OK, "allocator/info", "completes");
    xud_check(
        info.sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, "allocator/info-stamped", "the stamp is written, not left at the poison");
    xud_check(info.pNext == NULL, "allocator/info-pnext-null", "and nothing is chained into it");
    xud_check(info.allocationSize == 8192u, "allocator/info-size", "the size is the request's");
    xud_check(info.memoryTypeIndex == 2u, "allocator/info-type", "and the type is the index the rule answered");

    /* The free-list scan, against rows written here rather than by the driver. One block of 4 KiB
       of type 0, one free range over all of it, and one retired row (size 0, the state a merged-
       away row is in) that must never be a candidate.
     */
    alloc = calloc(1u, sizeof(XudAllocator));
    xud_check(alloc != NULL, "allocator/synth-alloc", "the synthetic allocator must allocate");
    alloc->blocks = malloc(sizeof(XudAllocBlock) * 1u);
    xud_check(alloc->blocks != NULL, "allocator/synth-block-array", "and its one block row");
    alloc->blockCount = 1u;
    alloc->blockCapacity = 1u;
    alloc->blocks[0].memory = fakeMemory;
    alloc->blocks[0].size = 4096u;
    alloc->blocks[0].memoryTypeIndex = 0u;
    alloc->ringBlockIndex = XUD_ALLOCATOR_NO_BLOCK; /* no ring yet: the ring is one block of this
       layer, recorded in the block array, and until it exists its index is the sentinel rather th
       an a row */
    alloc->ranges = malloc(sizeof(XudAllocRange) * 4u);
    xud_check(alloc->ranges != NULL, "allocator/synth-range-array", "and its range rows");
    alloc->rangeCapacity = 4u;
    alloc->rangeCount = 2u;
    alloc->ranges[0].blockIndex = 0u;
    alloc->ranges[0].offset = 0u;
    alloc->ranges[0].size = 4096u;
    alloc->ranges[0].live = 0u;
    alloc->ranges[1].size = 0u; /* a row that names nothing, the state a coalesce leaves behind */

    rangeIndex = 0xA5A5u;
    xud_check(xud_allocator_find_range(NULL, 1024u, 256u, 1u, &rangeIndex, &offset) == XUD_INVALID_ARG, "allocator/find-null-alloc",
        "is refused");
    xud_check(xud_allocator_find_range(alloc, 1024u, 0u, 1u, &rangeIndex, &offset) == XUD_INVALID_ARG, "allocator/find-alignment-zero",
        "an alignment of zero is not an alignment");
    xud_check(
        xud_allocator_find_range(alloc, 1024u, (VkDeviceSize)XUD_ALLOCATOR_ALIGN_MAX + 1u, 1u, &rangeIndex, &offset) == XUD_INVALID_ARG,
        "allocator/find-alignment-past", "an alignment above this layer's bound is refused rather than carried into the scan");
    xud_check(xud_allocator_find_range(alloc, 1024u, 256u, 1u, NULL, &offset) == XUD_INVALID_ARG, "allocator/find-null-out", "is refused");
    xud_check(rangeIndex == 0xA5A5u, "allocator/find-invalid-untouched", "a rejected scan leaves the index alone");

    xud_check(xud_allocator_find_range(alloc, 1024u, 256u, 1u, &rangeIndex, &offset) == XUD_OK, "allocator/find-first-fit", "answers");
    xud_check(rangeIndex == 0u, "allocator/find-first-fit/index", "the one free row");
    xud_check(offset == 0u, "allocator/find-first-fit/offset", "at the range's own start, which is already aligned");

    xud_check(xud_allocator_find_range(alloc, 1024u, 256u, (1u << 1), &rangeIndex, &offset) == XUD_OK, "allocator/find-wrong-type-answers",
        "answers");
    xud_check(rangeIndex == XUD_ALLOCATOR_NO_RANGE, "allocator/find-wrong-type/none",
        "no candidate: the block's own memoryTypeIndex is what makes a range inside a block of the wrong type not a candidate, which is "
        "why the block's type is recorded rather than re-derived");

    xud_check(
        xud_allocator_find_range(alloc, 4097u, 256u, 1u, &rangeIndex, &offset) == XUD_OK, "allocator/find-too-big-answers", "answers");
    xud_check(rangeIndex == XUD_ALLOCATOR_NO_RANGE, "allocator/find-too-big/none",
        "a request larger than every free range is a miss, not an error - the caller above creates a block instead");

    /* Make the first half of the block live and the second half free, so the scan has to skip a l
       ive row and find the aligned start inside the free one.
     */
    alloc->ranges[0].offset = 0u;
    alloc->ranges[0].size = 2048u;
    alloc->ranges[0].live = 1u;
    alloc->ranges[0].generation = 0u;
    alloc->ranges[1].blockIndex = 0u;
    alloc->ranges[1].offset = 2048u;
    alloc->ranges[1].size = 2048u;
    alloc->ranges[1].live = 0u;
    alloc->ranges[1].generation = 0u;
    alloc->rangeCount = 2u;
    alloc->rangeLiveCount = 1u;
    alloc->bytesInUse = 2048u;

    xud_check(xud_allocator_find_range(alloc, 1024u, 256u, 1u, &rangeIndex, &offset) == XUD_OK, "allocator/find-skip-live", "answers");
    xud_check(rangeIndex == 1u, "allocator/find-skip-live/index", "the free row, with the live one skipped");
    xud_check(offset == 2048u, "allocator/find-skip-live/offset", "at the free row's own start");

    xud_check(xud_allocator_find_range(alloc, 2049u, 256u, 1u, &rangeIndex, &offset) == XUD_OK, "allocator/find-too-big-for-range",
        "answers, with no row named");
    xud_check(rangeIndex == XUD_ALLOCATOR_NO_RANGE, "allocator/find-too-big/none",
        "a 2049-byte request inside a 2048-byte free range has no start that fits, which is the arithmetic the acquire above turns into a "
        "new block rather than a refusal");

    /* The accessor on a live row: what a caller above reads is the row's own offset and size, not
       the request it made - the range handed out is AT LEAST the request, and the row is the truth.
     */
    memset(&held, 0, sizeof(held));
    held.memory = fakeMemory;
    held.offset = 0u;
    held.size = 2048u;
    held.blockIndex = 0u;
    held.rangeIndex = 0u;
    held.generation = 0u;
    xud_check(xud_allocator_range_get(alloc, &held, &range) == XUD_OK, "allocator/get-live", "answers");
    xud_check(range.offset == 0u, "allocator/get/offset", "the row's own offset");
    xud_check(range.size == 2048u, "allocator/get/size",
        "the row's own size, which is what a caller above reads rather than the request it made");
    xud_check(range.generation == 0u, "allocator/get/generation", "and the row's generation");

    /* The refusals, read BEFORE the release so each one is the check that fired rather than a
       consequence of a previous one: a range naming this allocator's rows but a different block
       (the cross-kind case of the handle layer, one rung lower), and a range whose generation was
       invented rather than kept.
     */
    memset(&held, 0, sizeof(held));
    held.memory = fakeMemory;
    held.offset = 0u;
    held.size = 1024u;
    held.blockIndex = 1u; /* a range from another allocator, naming this one's rows by index */
    held.rangeIndex = 0u;
    held.generation = 0u; /* matching, so it is the blockIndex compare that fires */
    xud_check(xud_allocator_release(alloc, &held) == XUD_INVALID_ARG, "allocator/release-crossblock",
        "a range whose blockIndex is not the row's is refused, which is the cross-kind case of the handle layer read one rung lower");

    memset(&held, 0, sizeof(held));
    held.memory = fakeMemory;
    held.offset = 0u;
    held.size = 2048u;
    held.blockIndex = 0u;
    held.rangeIndex = 0u;
    held.generation = 9u; /* a handle a caller fabricated rather than one it kept */
    xud_check(xud_allocator_release(alloc, &held) == XUD_INVALID_ARG, "allocator/release-fabricated", "is refused");

    /* The release, and the generation bump read at the one place a row goes free: the handle in hand
       stops naming it AND every handle that named it before stops naming it, which is the stale-
       handle rule of xud_handle_free() applied to the rows of this layer rather than to slots.
     */
    memset(&held, 0, sizeof(held));
    held.memory = fakeMemory;
    held.offset = 0u;
    held.size = 2048u;
    held.blockIndex = 0u;
    held.rangeIndex = 0u;
    held.generation = 0u;
    xud_check(xud_allocator_release(alloc, &held) == XUD_OK, "allocator/release", "completes");
    xud_check(alloc->ranges[0].live == 0u, "allocator/released/free", "the row is free");
    xud_check(alloc->ranges[0].generation == 1u, "allocator/released/generation-bumped",
        "and its generation moved, so the range in hand stops naming it");
    xud_check(alloc->rangeLiveCount == 0u, "allocator/released/live-count", "the live count moved");
    xud_check(alloc->bytesInUse == 0u, "allocator/released/bytes", "and so did the reserved bytes");

    xud_check(xud_allocator_release(alloc, &held) == XUD_INVALID_ARG, "allocator/release-stale",
        "the same range released again is refused by the generation compare, not a double release");
    xud_check(xud_allocator_range_get(alloc, &held, &range) == XUD_INVALID_ARG, "allocator/get-stale",
        "and a stale range is refused by the accessor too, so a caller cannot read a row that was reused");

    /* The coalesce, read as its result rather than as its loop: one free range over the whole 4 Ki
       B block, and the row merged into it retired to size 0 with its generation moved so nothing
       can name it. A create/destroy loop over one size therefore returns the same one free range
       every cycle rather than one more row per cycle, which is what keeps a long loop from spending
       the range bound.
     */
    xud_check(alloc->ranges[0].offset == 0u, "allocator/coalesced/offset",
        "the row released is the survivor, and it starts at the block's start");
    xud_check(alloc->ranges[0].size == 4096u, "allocator/coalesced/size", "and covers the whole block");
    xud_check(alloc->ranges[0].live == 0u, "allocator/coalesced/survivor-free", "and is free");
    xud_check(alloc->ranges[1].size == 0u, "allocator/coalesced/merged-away", "the row merged into it names nothing");
    xud_check(alloc->ranges[1].generation == 1u, "allocator/coalesced/merged-generation",
        "and its generation moved, so the next take can reuse the row without an old range naming it");
    xud_check(
        xud_allocator_find_range(alloc, 4096u, 256u, 1u, &rangeIndex, &offset) == XUD_OK, "allocator/coalesced/whole-block", "answers");
    xud_check(rangeIndex == 0u, "allocator/coalesced/whole-block/index", "one row, not two");
    xud_check(offset == 0u, "allocator/coalesced/whole-block/offset", "at the start");

    /* The ring's cursor, and the stats a caller above reads. Both are pure: the cursor is the only
       state the ring has, so a reset is one store and the stats are four reads.
     */
    alloc->ringCursor = 128u;
    xud_check(xud_allocator_ring_reset(NULL) == XUD_INVALID_ARG, "allocator/reset-null", "is refused");
    xud_check(xud_allocator_ring_reset(alloc) == XUD_OK, "allocator/reset", "completes");
    xud_check(alloc->ringCursor == 0u, "allocator/reset/cursor", "the cursor is back at the start, and nothing was freed or marked free");

    xud_check(xud_allocator_stats(NULL, &blockCount, &liveCount, &bytes, &cursor) == XUD_INVALID_ARG, "allocator/stats-null", "is refused");
    xud_check(xud_allocator_stats(alloc, NULL, &liveCount, &bytes, &cursor) == XUD_INVALID_ARG, "allocator/stats-null-out", "is refused");
    xud_check(xud_allocator_stats(alloc, &blockCount, &liveCount, &bytes, &cursor) == XUD_OK, "allocator/stats", "answers");
    xud_check(blockCount == 1u, "allocator/stats/blocks", "one block row");
    xud_check(liveCount == 0u, "allocator/stats/live", "nothing live after the release");
    xud_check(bytes == 0u, "allocator/stats/bytes", "and no byte reserved");
    xud_check(cursor == 0u, "allocator/stats/cursor", "the cursor the reset moved");

    /* The driver halves' guards, read headless: every one of them fires before a row of the loader's
       table is read, which is what makes a machine with no driver refuse rather than call through NUL
       L. The synthetic loader above is not bound to the synthetic device, so the bound-handle rule is
       the refusal, and it is the same rule xud_swapchain_close() states.
     */
    ask.size = 0u;
    ask.alignment = 256u;
    ask.memoryTypeBits = 1u;
    memset(&range, 0, sizeof(range));
    xud_check(xud_allocator_acquire(&range, NULL, &loader, &device, &caps, &ask) == XUD_INVALID_ARG, "allocator/acquire-null-alloc",
        "is refused");
    xud_check(
        xud_allocator_acquire(&range, alloc, NULL, &device, &caps, &ask) == XUD_INVALID_ARG, "allocator/acquire-null-loader", "is refused");
    xud_check(xud_allocator_acquire(&range, alloc, &loader, &device, &caps, &ask) == XUD_INVALID_ARG, "allocator/acquire-size-zero",
        "a zero allocationSize is the validation layer's own rule (VUID-VkMemoryAllocateInfo-allocationSize-07897/07899), enforced before "
        "the call because the driver here accepts 0 silently");
    ask.size = 4096u;
    ask.alignment = 0u;
    xud_check(xud_allocator_acquire(&range, alloc, &loader, &device, &caps, &ask) == XUD_INVALID_ARG, "allocator/acquire-alignment-zero",
        "is refused");
    ask.alignment = (VkDeviceSize)XUD_ALLOCATOR_ALIGN_MAX + 1u;
    xud_check(xud_allocator_acquire(&range, alloc, &loader, &device, &caps, &ask) == XUD_INVALID_ARG, "allocator/acquire-alignment-past",
        "is refused");
    ask.alignment = 256u;
    ask.memoryTypeBits = 0u;
    xud_check(xud_allocator_acquire(&range, alloc, &loader, &device, &caps, &ask) == XUD_UNSUPPORTED, "allocator/acquire-no-bits",
        "is unsupported, not a bad argument");
    ask.memoryTypeBits = 1u;
    xud_check(xud_allocator_acquire(&range, alloc, &loader, &device, &caps, &ask) == XUD_UNSUPPORTED, "allocator/acquire-unbound-loader",
        "the rows must be this device's rows, and a handle-less device fails the bound check before any row is read");
    xud_check(range.memory == VK_NULL_HANDLE, "allocator/acquire-refused-untouched", "a refusal leaves the out-range as it was");

    xud_check(xud_allocator_ring_acquire(&range, NULL, &loader, &device, &caps, 64u, 256u, 1u) == XUD_INVALID_ARG,
        "allocator/ring-null-alloc", "is refused");
    xud_check(xud_allocator_ring_acquire(&range, alloc, &loader, &device, &caps, 0u, 256u, 1u) == XUD_INVALID_ARG,
        "allocator/ring-size-zero", "is refused");
    xud_check(xud_allocator_ring_acquire(&range, alloc, &loader, &device, &caps, 64u, 0u, 1u) == XUD_INVALID_ARG,
        "allocator/ring-alignment-zero", "is refused");
    xud_check(xud_allocator_ring_acquire(&range, alloc, &loader, &device, &caps, 64u, 256u, 0u) == XUD_UNSUPPORTED,
        "allocator/ring-no-bits", "is unsupported");
    xud_check(xud_allocator_ring_acquire(&range, alloc, &loader, &device, &caps, (VkDeviceSize)XUD_ALLOCATOR_RING_SIZE + 1u, 256u, 1u)
            == XUD_OUT_OF_RESOURCES,
        "allocator/ring-bigger-than-ring",
        "a transient request larger than the whole ring would hand the same bytes out twice inside one lap, which is the one thing a ring "
        "may not do");
    xud_check(xud_allocator_ring_acquire(&range, alloc, &loader, &device, &caps, 64u, 256u, 1u) == XUD_UNSUPPORTED,
        "allocator/ring-unbound-loader", "is unsupported");

    /* The teardown's guards, headless: an empty allocator has nothing to guard and tears by itself,
       and an allocator that holds a block refuses before it starts, because freeing the struct woul
       d leak a handle the driver still holds.
     */
    xud_check(xud_allocator_close(NULL, &loader, &device) == XUD_INVALID_ARG, "allocator/close-null", "is refused");
    xud_check(xud_allocator_close(&fresh, NULL, &device) == XUD_INVALID_ARG, "allocator/close-null-loader", "is refused");
    xud_check(xud_allocator_close(&fresh, &loader, &device) == XUD_OK, "allocator/close-empty",
        "an allocator with no block in it has nothing to guard and tears by itself");
    xud_check(fresh == NULL, "allocator/close-empty/cleared", "and the out-pointer is cleared");
    xud_check(xud_allocator_close(&fresh, &loader, &device) == XUD_ALREADY, "allocator/close-empty-twice",
        "a second teardown is reported, not a double free");

    xud_check(xud_allocator_close(&alloc, &loader, &device) == XUD_UNSUPPORTED, "allocator/close-blocks-unbound",
        "an allocator that holds a block refuses before it starts, because freeing the struct would leak a handle the driver still holds");
    xud_check(alloc != NULL, "allocator/close-refused-kept", "and the refusal leaves the allocator alone, so the test frees it itself");

    free(alloc->ranges);
    free(alloc->blocks);
    free(alloc);
}

/*-----------------------------------------------------------------------------------------------
 * GPU-gated: the allocator against the driver. A completed context's allocator, its caps and its
 * device, and real VkDeviceMemory behind real suballocated ranges. What is being read is the shape
 * of the rung rather than the fact that it runs: two small requests inside ONE block (the suballo-
 * cation, which is the whole point of the rung), a request that does not fit the remainder and so
 * opens a second block (the growth, not a refusal), a release that makes the next request of the
 * same size land back at the offset the first one used (the free list plus the coalesce, read as an
 * offset rather than as a counter), the refusals the pure half states as rules being seen here ag-
 * ainst the real caps rather than a synthetic one, and the ring, whose cursor moves, wraps and com-
 * es back after a reset without one vkAllocateMemory more. The teardown is the context's own: xud_
 * close_context() closes the allocator before the device, and every block this test created is fre-
 * ed there, which is what the sanitizer run's leak verdict is about.
 *-----------------------------------------------------------------------------------------------*/
static void xud_test_gpu_allocator(void) {
    XudContext* context = NULL;
    XudMemoryRange small1;
    XudMemoryRange small2;
    XudMemoryRange big;
    XudMemoryRange reuse;
    XudMemoryRange refused;
    XudMemoryRange ring1;
    XudMemoryRange ring2;
    XudMemoryRange ring3;
    XudMemoryRange ringWrap;
    XudMemoryRange ringAgain;
    VkMemoryRequirements ask;
    VkDeviceSize heapMax = 0u;
    VkDeviceSize bytesBefore = 0u;
    VkDeviceSize bytesAfter = 0u;
    VkDeviceSize cursorBefore = 0u;
    VkDeviceSize cursor = 0u;
    VkDeviceSize baseBytes = 0u;
    uint32_t blockCount = 0u;
    uint32_t liveCount = 0u;
    uint32_t baseLive = 0u;
    uint32_t baseBlocks = 0u;
    uint32_t typeIndex = 0u;
    uint32_t heap = 0u;
    XudResult result = XUD_OK;

    memset(&small1, 0, sizeof(small1));
    memset(&small2, 0, sizeof(small2));
    memset(&big, 0, sizeof(big));
    memset(&reuse, 0, sizeof(reuse));
    memset(&refused, 0, sizeof(refused));
    memset(&ring1, 0, sizeof(ring1));
    memset(&ring2, 0, sizeof(ring2));
    memset(&ring3, 0, sizeof(ring3));
    memset(&ringWrap, 0, sizeof(ringWrap));
    memset(&ringAgain, 0, sizeof(ringAgain));

    result = xud_init_context(&context, 64u, 64u);
    xud_check(result == XUD_OK, "gpu-alloc/context-completes", "the ladder has to complete before anything can be allocated");
    if (context == NULL) {
        return;
    }

    xud_check(context->allocator != NULL, "gpu-alloc/allocator-owned",
        "a completed context owns the allocator rung, so the GPU-memory rung is wired");
    if (context->allocator == NULL) {
        (void)xud_close_context(&context);
        return;
    }

    /* The context's own bring-up already opened the descriptor rung, the render pass below the ren
       derer AND the batch rung below the renderer, and those three carved eight ranges out of three
       blocks: the per-frame uniform buffer is the first request the allocator ever saw (alignment 2
       56, so it sits in the 64 KiB block this layer opens first), the depth image is the second - i
       ts requirements ask for an alignment of 65536 on this driver, which no hole in the first bloc
       k can satisfy, so the image's block is a second VkDeviceMemory rather than a suballocation -
       and the batch's arena plus its four streams and its index buffer are six more, small enough th
       at they share blocks and need one more block only once the holes run out. Every number below i
       s therefore read as a delta on this baseline rather than as a constant, which is what keeps th
       e rung's rules about what moved rather than about absolute offsets.
     */
    xud_check(xud_allocator_stats(context->allocator, &baseBlocks, &baseLive, &baseBytes, &cursorBefore) == XUD_OK,
        "gpu-alloc/baseline-stats", "answers at the context's own state");
    xud_check(baseBlocks == 3u, "gpu-alloc/baseline-three-blocks",
        "three blocks: the descriptor rung's uniform buffer, the render pass's depth image (the first t"
        "hing that ever asked for an IMAGE's requirements rather than a buffer's, and an image's align"
        "ment is not a buffer's) and one more block the batch's six carves outgrew a hole in");
    xud_check(baseLive == 8u, "gpu-alloc/baseline-eight-live", "and eight live ranges, those eight carves");
    xud_check(baseBytes > 0u, "gpu-alloc/baseline-bytes", "and reserved bytes, so the rungs below this one really did ask the allocator");
    xud_check(cursorBefore == 0u, "gpu-alloc/baseline-cursor",
        "and the ring's cursor is still at the start, because nothing has asked for transient memory y"
        "et - including the batch, whose arena is static for the reason its own note gives");

    /* The caps' memory rows are the physical layer's, recorded at open, and the type rule reads the
       m rather than a constant: this machine answers 7 types over 1 heap of ~23 GiB, and the type t
       he rule picks is read back rather than assumed.
     */
    for (heap = 0u; heap < context->physicalDevice->caps.memoryProperties.memoryProperties.memoryHeapCount; heap++) {
        if (context->physicalDevice->caps.memoryProperties.memoryProperties.memoryHeaps[heap].size > heapMax) {
            heapMax = context->physicalDevice->caps.memoryProperties.memoryProperties.memoryHeaps[heap].size;
        }
    }
    xud_check(heapMax > 0u, "gpu-alloc/heap-recorded",
        "the physical layer's caps carry at least one heap row, which is what the type rule needs");

    ask.size = 4096u;
    ask.alignment = 256u;
    ask.memoryTypeBits = 0xFFFFFFFFu;
    xud_check(xud_allocator_select_memory_type(&context->physicalDevice->caps, &ask, &typeIndex) == XUD_OK, "gpu-alloc/type-rule-answers",
        "the caps' rows name a type for a request every type may serve");
    xud_check(typeIndex < context->physicalDevice->caps.memoryProperties.memoryProperties.memoryTypeCount, "gpu-alloc/type-index-in-range",
        "the index names a row of the caps, not a row past them");
    xud_check((context->physicalDevice->caps.memoryProperties.memoryProperties.memoryTypes[typeIndex].propertyFlags
                  & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
            != 0u,
        "gpu-alloc/type-is-device-local",
        "and it is a device-local one, which is the design sketch's point read as a fact: geometry and descriptor data live in device "
        "memory, not host memory");

    /* Two small requests, and the fact that they share one block: the second is a suballocation of
       the first block's remainder, not a second VkDeviceMemory. That is the whole point of the run
       g - the design sketch's "allocate once, suballocate always" rule, read as an offset and a bl
       ock count rather than as a comment.
     */
    xud_check(xud_allocator_acquire(&small1, context->allocator, context->loader, context->device, &context->physicalDevice->caps, &ask)
            == XUD_OK,
        "gpu-alloc/first", "completes");
    xud_check(small1.memory != VK_NULL_HANDLE, "gpu-alloc/first-memory", "names a real VkDeviceMemory");
    xud_check(small1.blockIndex < baseBlocks, "gpu-alloc/first-block-is-an-existing-one",
        "the request came out of a block that ALREADY existed rather than a new VkDeviceMemory - whic"
        "h hole the scan picked is the free list's row order, not the block count's, and the rule the"
        "rung exists for is that a request a hole can hold makes no allocation at all");
    xud_check(small1.offset > 0u, "gpu-alloc/first-offset",
        "past the start rather than at it: the rungs below this one have already carved this block,"
        " so the free list is doing its job on a block that was partly used before this test asked");
    xud_check(small1.size >= 4096u, "gpu-alloc/first-size", "at least the request, which is what the row records rather than the request");

    ask.size = 1024u;
    xud_check(xud_allocator_acquire(&small2, context->allocator, context->loader, context->device, &context->physicalDevice->caps, &ask)
            == XUD_OK,
        "gpu-alloc/second", "completes");
    xud_check(small2.blockIndex < baseBlocks, "gpu-alloc/second-is-a-suballocation",
        "the second request came out of a block that ALREADY existed rather than a new VkDeviceMemory -"
        " which hole the scan picked is the free list's row order, not the block count's (here it is the"
        " depth image's block, whose hole was the first free row), and the rule the rung exists for is t"
        "hat a request a hole can hold makes no allocation at all");
    xud_check(small2.memory != VK_NULL_HANDLE, "gpu-alloc/second-names-memory",
        "and it names a real VkDeviceMemory, so the hole it came out of is a real one");
    xud_check(small2.offset > 0u, "gpu-alloc/second-offset-past-a-carve",
        "at an offset past the start of its own block rather than at the start, which is a hole's rema"
        "inder rather than a fresh block");
    xud_check((small2.offset % 256u) == 0u, "gpu-alloc/second-alignment",
        "and aligned to the request's alignment, which is what the scan computes rather than hopes");
    xud_check(small2.generation == 0u, "gpu-alloc/second-generation", "a fresh row's generation");

    /* A request the first block's remainder cannot hold, and the fact that this is growth rather th
       an refusal: a second block, a second VkDeviceMemory, and a block count that moved.
     */
    ask.size = (VkDeviceSize)XUD_ALLOCATOR_BLOCK_MAX_SIZE;
    xud_check(
        xud_allocator_acquire(&big, context->allocator, context->loader, context->device, &context->physicalDevice->caps, &ask) == XUD_OK,
        "gpu-alloc/big-opens-a-second-block", "completes, because the request is a block this layer will grow to rather than a refusal");
    xud_check(big.blockIndex == baseBlocks, "gpu-alloc/big-block-index", "a block row past the baseline's two, not one of them");
    xud_check(big.memory != small1.memory, "gpu-alloc/big-different-memory", "and a different VkDeviceMemory");
    xud_check(big.size == (VkDeviceSize)XUD_ALLOCATOR_BLOCK_MAX_SIZE, "gpu-alloc/big-size-is-bound",
        "the block landed exactly on the growth ceiling, which is what says the doubling ladder reached the bound rather than stepping "
        "past it");

    xud_check(xud_allocator_stats(context->allocator, &blockCount, &liveCount, &bytesBefore, &cursorBefore) == XUD_OK,
        "gpu-alloc/stats-answers", "answers");
    xud_check(blockCount == baseBlocks + 1u, "gpu-alloc/stats-one-block-more",
        "one block more than the baseline's, which is the growth path rather than a refusal");
    xud_check(liveCount == baseLive + 3u, "gpu-alloc/stats-three-live",
        "three live ranges over them on top of the baseline the context's own bring-up left behind");
    xud_check(bytesBefore == (VkDeviceSize)XUD_ALLOCATOR_BLOCK_MAX_SIZE + 4096u + 1024u + baseBytes, "gpu-alloc/stats-bytes-exact",
        "the reserved bytes are the sum of the live ranges' own sizes, exactly, which is what makes the counter a fact rather than an "
        "estimate");
    xud_check(cursorBefore == 0u, "gpu-alloc/stats-cursor-untouched",
        "and the ring's cursor is still at the start, because nothing transient has been asked for");

    /* The release, and the reuse it makes possible: releasing the two small ranges and asking again
       for the first request's size lands at the offset the first one used, in the same VkDeviceMemo
       ry, which is the coalesced free range rather than a fresh block. The generation on the range
       that comes back is one above the released row's, which is what makes the stale range below a
       refusal rather than an alias.
     */
    xud_check(xud_allocator_release(context->allocator, &small1) == XUD_OK, "gpu-alloc/release-first", "completes");
    xud_check(xud_allocator_release(context->allocator, &small2) == XUD_OK, "gpu-alloc/release-second", "completes");
    xud_check(xud_allocator_release(context->allocator, &small1) == XUD_INVALID_ARG, "gpu-alloc/release-stale",
        "the same range released again is refused by the generation compare, so a caller cannot double-free a range");
    xud_check(xud_allocator_range_get(context->allocator, &small1, &reuse) == XUD_INVALID_ARG, "gpu-alloc/get-stale",
        "and the accessor refuses it too, so a caller cannot read a row that was reused");

    ask.size = 4096u;
    xud_check(
        xud_allocator_acquire(&reuse, context->allocator, context->loader, context->device, &context->physicalDevice->caps, &ask) == XUD_OK,
        "gpu-alloc/reuse", "completes");
    xud_check(reuse.blockIndex == small1.blockIndex, "gpu-alloc/reuse-block",
        "back in the SAME block the first request used, which is the coalesced free range rather than a"
        " fresh block");
    xud_check(reuse.offset == small1.offset, "gpu-alloc/reuse-offset",
        "exactly where the first request was, which is the coalesced free range rather than a fresh "
        "block");
    xud_check(
        reuse.memory == small1.memory, "gpu-alloc/reuse-same-memory", "and the same VkDeviceMemory, so no vkAllocateMemory ran at all");
    xud_check(reuse.generation == 1u, "gpu-alloc/reuse-generation",
        "one generation above the released row's, which is the stale-handle rule read at the one place a row goes free");

    xud_check(xud_allocator_stats(context->allocator, &blockCount, &liveCount, &bytesAfter, &cursor) == XUD_OK,
        "gpu-alloc/stats-after-reuse", "answers");
    xud_check(blockCount == baseBlocks + 1u, "gpu-alloc/stats-no-new-block-for-reuse",
        "the count did not move again, which is the whole cost argument of the rung");
    xud_check(bytesAfter == (bytesBefore - 1024u), "gpu-alloc/stats-bytes-exact-after-reuse",
        "the reserved bytes moved by exactly the 1024-byte range that was released and not asked for again, which is what makes the "
        "counter a fact rather than an estimate");

    /* The refusals the pure half states as rules, seen here against the REAL caps rather than a sy
       nthetic one: a request above the growth ceiling, a request above every heap, and a request n
       o memory type may serve. All three are refusals before vkAllocateMemory is asked, which is w
       hat the validation layer's own messages would have said if this layer had asked instead.
     */
    memset(&refused, 0, sizeof(refused));
    ask.size = (VkDeviceSize)XUD_ALLOCATOR_BLOCK_MAX_SIZE + 1u;
    ask.alignment = 256u;
    ask.memoryTypeBits = 0xFFFFFFFFu;
    xud_check(xud_allocator_acquire(&refused, context->allocator, context->loader, context->device, &context->physicalDevice->caps, &ask)
            == XUD_OUT_OF_RESOURCES,
        "gpu-alloc/above-block-bound",
        "is refused, because one block above the ceiling is one block the free list could never serve again");
    xud_check(refused.memory == VK_NULL_HANDLE, "gpu-alloc/above-block-untouched", "and the out-range was left as it was");

    ask.size = heapMax + 1u;
    xud_check(xud_allocator_acquire(&refused, context->allocator, context->loader, context->device, &context->physicalDevice->caps, &ask)
            == XUD_UNSUPPORTED,
        "gpu-alloc/above-heap",
        "is unsupported, because the type rule reads the caps' heap rows and refuses a request no heap can hold before the driver is asked "
        "(VUID-vkAllocateMemory-pAllocateInfo-01713)");
    xud_check(refused.memory == VK_NULL_HANDLE, "gpu-alloc/above-heap-untouched", "and the out-range was left as it was");

    ask.size = 4096u;
    ask.memoryTypeBits = 0u;
    xud_check(xud_allocator_acquire(&refused, context->allocator, context->loader, context->device, &context->physicalDevice->caps, &ask)
            == XUD_UNSUPPORTED,
        "gpu-alloc/no-type-may-serve",
        "is unsupported rather than a bad argument, because no type can serve it is not the same as the argument being wrong");
    xud_check(refused.memory == VK_NULL_HANDLE, "gpu-alloc/no-type-untouched", "and the out-range was left as it was");

    /* The ring: transient memory, which is the per-frame data of the design sketch's EndDrawing. N
       othing is freed and nothing is marked free - the reset moves a cursor - so the same offsets c
       ome back after it, which is what makes a frame loop that allocates the same shapes every fram
       e pay for nothing after the first. The wrap is the other half of the same contract: a reques
       t that does not fit between the cursor and the end of the ring starts the next lap at 0.
     */
    xud_check(xud_allocator_ring_acquire(
                  &ring1, context->allocator, context->loader, context->device, &context->physicalDevice->caps, 256u, 256u, 0xFFFFFFFFu)
            == XUD_OK,
        "gpu-alloc/ring-first", "completes, and creates the ring's one block");
    xud_check(ring1.memory != VK_NULL_HANDLE, "gpu-alloc/ring-memory", "the ring's own VkDeviceMemory");
    xud_check(context->allocator->ringBlockIndex != XUD_ALLOCATOR_NO_BLOCK, "gpu-alloc/ring-block-recorded",
        "and that block is recorded in the block array, so the teardown frees it with the rest rather than through a second path");
    xud_check(ring1.blockIndex == context->allocator->ringBlockIndex, "gpu-alloc/ring-block-index", "the range names that recorded block");
    xud_check(ring1.offset == 0u, "gpu-alloc/ring-first-offset", "at the ring's start");
    xud_check(
        ring1.memory != small1.memory, "gpu-alloc/ring-not-a-geometry-block", "the ring is its own block, not a slice of a static one");

    xud_check(xud_allocator_ring_acquire(
                  &ring2, context->allocator, context->loader, context->device, &context->physicalDevice->caps, 1024u, 256u, 0xFFFFFFFFu)
            == XUD_OK,
        "gpu-alloc/ring-second", "completes");
    xud_check(ring2.memory == ring1.memory, "gpu-alloc/ring-one-block", "the same ring block: the ring is one block, created once");
    xud_check(ring2.offset == 256u, "gpu-alloc/ring-second-offset", "right after the first, which is the cursor rather than a search");

    xud_check(xud_allocator_ring_acquire(
                  &ring3, context->allocator, context->loader, context->device, &context->physicalDevice->caps, 131072u, 256u, 0xFFFFFFFFu)
            == XUD_OK,
        "gpu-alloc/ring-third", "completes");
    xud_check(
        ring3.offset == 1280u, "gpu-alloc/ring-third-offset", "after the second, so the cursor moved by exactly the sizes handed out");

    xud_check(xud_allocator_ring_acquire(&ringWrap, context->allocator, context->loader, context->device, &context->physicalDevice->caps,
                  131072u, 256u, 0xFFFFFFFFu)
            == XUD_OK,
        "gpu-alloc/ring-wrap", "completes, by starting the next lap rather than by refusing");
    xud_check(ringWrap.offset == 0u, "gpu-alloc/ring-wrap-offset",
        "back at the start, because the request did not fit between the cursor and the end of the ring");
    xud_check(ringWrap.memory == ring1.memory, "gpu-alloc/ring-wrap-same-block",
        "and still the same one block, which is what a ring is: overwriting an earlier lap is the contract, not a bug");

    xud_check(xud_allocator_ring_acquire(&refused, context->allocator, context->loader, context->device, &context->physicalDevice->caps,
                  (VkDeviceSize)XUD_ALLOCATOR_RING_SIZE + 1u, 256u, 0xFFFFFFFFu)
            == XUD_OUT_OF_RESOURCES,
        "gpu-alloc/ring-bigger-than-ring", "is refused, because one lap would hand the same bytes out twice inside one frame");
    xud_check(refused.memory == VK_NULL_HANDLE, "gpu-alloc/ring-refusal-untouched", "and the out-range was left as it was");

    xud_check(xud_allocator_ring_reset(context->allocator) == XUD_OK, "gpu-alloc/ring-reset", "completes");
    xud_check(context->allocator->ringCursor == 0u, "gpu-alloc/ring-reset-cursor",
        "the cursor is back at the start, and nothing was freed or marked free");
    xud_check(xud_allocator_ring_acquire(
                  &ringAgain, context->allocator, context->loader, context->device, &context->physicalDevice->caps, 256u, 256u, 0xFFFFFFFFu)
            == XUD_OK,
        "gpu-alloc/ring-next-frame", "completes");
    xud_check(ringAgain.offset == ring1.offset, "gpu-alloc/ring-offsets-reused",
        "the same offset the first lap used, which is what makes a frame loop pay for nothing after the first frame");

    xud_check(xud_allocator_stats(context->allocator, &blockCount, &liveCount, &bytesAfter, &cursor) == XUD_OK,
        "gpu-alloc/stats-after-ring", "answers");
    xud_check(blockCount == baseBlocks + 2u, "gpu-alloc/stats-plus-ring-block",
        "the baseline's two static blocks plus the big request's plus the ring's one block, and no more");
    xud_check(cursor == 256u, "gpu-alloc/stats-cursor-moved", "the cursor says exactly what the ring handed out since the reset");

    if (context->validate != 0u) {
        xud_check(context->diagnostics->errorMessages == 0u, "gpu-alloc/validation-clean",
            "every allocation above was validation-clean, not just crash-free, which is the heap-size, the memoryTypeIndex and the "
            "allocationSize rules read against the caps rather than left to the driver");
    }

    result = xud_close_context(&context);
    xud_check(result == XUD_OK, "gpu-alloc/close-clean",
        "a context whose allocator holds five blocks and a ring tears back down with nothing refused, w"
        "hich is the allocator's close freeing every VkDeviceMemory it created before the device it wa"
        "s allocated from goes");
    xud_check(context == NULL, "gpu-alloc/close-clears", "and the out-param is cleared");
}

void xud_test_allocator(void) {
    xud_test_allocator_pure();
    if (xud_gpu_tests_enabled()) {
        xud_test_gpu_allocator();
    }
}
