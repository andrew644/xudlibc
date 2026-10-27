/***********************************************************************************************
 *   xud_allocator.c - the GPU-memory allocator rung
 *   Owns: everything that reads or writes the allocator's own rows - the memory-type rule read
 *         off the physical layer's recorded memory rows, the create-info assembly, the free list
 *         (first-fit suballocation, release, coalescing, a generation bump on reuse), the
 *         transient ring's cursor, the counters, and the teardown that frees every VkDeviceMemory
 *         this layer created.
 *   Layer: above the device, below the renderer. Two halves, and the split is the one the repo's
 *         headless-test rule asks for: the free list, the type rule, the create-info assembly and
 *         the cursor arithmetic call no driver function and are exercised by the default test run
 *         against a synthetic allocator, exactly as the physical layer's pure half is exercised
 *         against a synthetic caps struct. Only xud_allocator_acquire(), xud_allocator_acquire_hos
 *         t_visible(), xud_allocator_ring_acquire() and xud_allocator_close() touch the driver, and
 *         every one of them runs its guards before the call. The second acquire is the one the
 *         mapped carves need: vkMapMemory maps a VkDeviceMemory rather than a range, so a mapped
 *         carve owns its block and is never suballocated - see the note above the definition and
 *         the doc comment above the declaration for the VUID and the driver's answer.
 ***********************************************************************************************/

#include "xud_allocator.h"
#include "xud.h"

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/*-----------------------------------------------------------------------------------------------
 * The one arithmetic helper, and why overflow is checked rather than assumed away
 *-----------------------------------------------------------------------------------------------*/

/* Round `value` up to the next multiple of `alignment`, which is what every offset this layer hands
   out has to be. The obvious expression, (value + alignment - 1) / alignment * alignment, overflows
   for a value near UINT64_MAX, and a VkMemoryRequirements::alignment is a VkDeviceSize a driver
   could answer large even though XUD_ALLOCATOR_ALIGN_MAX refuses the ones this tree will never see.
   The overflow is therefore checked on both sides: before, on the (alignment - 1) addition, and
   after, on the multiply. Answers false when there is no representable rounded value, which the
   callers above turn into a refusal rather than a wrap. */
static bool xud_alloc_align_up(VkDeviceSize value, VkDeviceSize alignment, VkDeviceSize* outRounded) {
    VkDeviceSize blocks = 0u;

    if (alignment == 0u)
        return false;

    if (value > (VkDeviceSize)UINT64_MAX - (alignment - 1u))
        return false;

    blocks = (value + (alignment - 1u)) / alignment;
    if (blocks > ((VkDeviceSize)UINT64_MAX / alignment))
        return false;

    *outRounded = blocks * alignment;
    return true;
}

/* The block size this layer would ask for, given a request: the smallest step of the doubling
   sequence that starts at XUD_ALLOCATOR_BLOCK_MIN_SIZE and reaches XUD_ALLOCATOR_BLOCK_MAX_SIZE
   that is at least the request. Answers false when the request is above the bound, which is a
   refusal rather than one unbounded block - see the note on the two constants in the header. */
static bool xud_alloc_block_size(VkDeviceSize wanted, VkDeviceSize* outSize) {
    VkDeviceSize size = (VkDeviceSize)XUD_ALLOCATOR_BLOCK_MIN_SIZE;

    while (size < wanted) {
        if (size >= (VkDeviceSize)XUD_ALLOCATOR_BLOCK_MAX_SIZE)
            return false;
        size = size * 2u;
    }

    *outSize = size;
    return true;
}

/*-----------------------------------------------------------------------------------------------
 * The host-side growth steps
 *-----------------------------------------------------------------------------------------------*/

/* Room for `extra` more rows, in the doubling shape the rest of this tree uses. Grown BEFORE the
   rows are written, so a growth failure is a refusal that left no half-written row behind - the
   same reason xud_handle_reserve() is called before the slot it reserves is touched. */
static XudResult xud_alloc_reserve_blocks(XudAllocator* alloc, uint32_t extra) {
    XudAllocBlock* grown = NULL;
    uint32_t oldCapacity = alloc->blockCapacity;
    uint32_t newCapacity = XUD_ALLOCATOR_BLOCK_MIN_CAPACITY;
    size_t needed = 0u;

    if (alloc->blockCount + extra > XUD_ALLOCATOR_BLOCK_MAX_CAPACITY)
        return XUD_OUT_OF_RESOURCES;

    while (newCapacity < (alloc->blockCount + extra)) {
        if (newCapacity >= XUD_ALLOCATOR_BLOCK_MAX_CAPACITY)
            break;
        newCapacity = newCapacity * 2u;
    }
    if (newCapacity < (alloc->blockCount + extra))
        return XUD_OUT_OF_RESOURCES;

    if (newCapacity <= oldCapacity)
        return XUD_OK;

    needed = (size_t)newCapacity * sizeof(XudAllocBlock);
    grown = malloc(needed);
    if (grown == NULL)
        return XUD_OUT_OF_MEMORY;

    if (oldCapacity != 0u) {
        memcpy(grown, alloc->blocks, (size_t)oldCapacity * sizeof(XudAllocBlock));
        free(alloc->blocks);
    }
    memset((char*)grown + (size_t)oldCapacity * sizeof(XudAllocBlock), 0, (size_t)(newCapacity - oldCapacity) * sizeof(XudAllocBlock));

    alloc->blocks = grown;
    alloc->blockCapacity = newCapacity;
    return XUD_OK;
}

static XudResult xud_alloc_reserve_ranges(XudAllocator* alloc, uint32_t extra) {
    XudAllocRange* grown = NULL;
    uint32_t oldCapacity = alloc->rangeCapacity;
    uint32_t newCapacity = XUD_ALLOCATOR_RANGE_MIN_CAPACITY;
    size_t needed = 0u;

    if (alloc->rangeCount + extra > XUD_ALLOCATOR_RANGE_MAX_CAPACITY)
        return XUD_OUT_OF_RESOURCES;

    while (newCapacity < (alloc->rangeCount + extra)) {
        if (newCapacity >= XUD_ALLOCATOR_RANGE_MAX_CAPACITY)
            break;
        newCapacity = newCapacity * 2u;
    }
    if (newCapacity < (alloc->rangeCount + extra))
        return XUD_OUT_OF_RESOURCES;

    if (newCapacity <= oldCapacity)
        return XUD_OK;

    needed = (size_t)newCapacity * sizeof(XudAllocRange);
    grown = malloc(needed);
    if (grown == NULL)
        return XUD_OUT_OF_MEMORY;

    if (oldCapacity != 0u) {
        memcpy(grown, alloc->ranges, (size_t)oldCapacity * sizeof(XudAllocRange));
        free(alloc->ranges);
    }
    memset((char*)grown + (size_t)oldCapacity * sizeof(XudAllocRange), 0, (size_t)(newCapacity - oldCapacity) * sizeof(XudAllocRange));

    alloc->ranges = grown;
    alloc->rangeCapacity = newCapacity;
    return XUD_OK;
}

/* A row of each array, reused or fresh. A block row is never reused - a block is created and freed
   one at a time, and there is no "empty block" state to reclaim (see the note in the header about
   why the block itself is not freed at release time). A range row IS reused: a row whose size is 0
   is a row that names nothing, either because it was never used (generation 0) or because it was
   merged away by a coalesce (generation bumped), and taking it again bumps the generation so that
   the never-used and the reused states are never confused - the same shape as xud_handle_alloc().
 */
static uint32_t xud_alloc_take_range_row(XudAllocator* alloc) {
    uint32_t row = 0u;
    uint32_t highMark = alloc->rangeCount;

    for (row = 0u; row < highMark; row++) {
        if (alloc->ranges[row].size == 0u)
            break;
    }
    if (row == highMark) {
        alloc->rangeCount = highMark + 1u;
        /* A fresh row is already the zero state: size 0, live 0, generation 0. */
        return row;
    }

    /* Reusing a row that was merged away: bump, so a range that named it before stops naming it.
       A never-used row keeps generation 0, which is the state nothing ever held. */
    if (alloc->ranges[row].generation != 0u)
        alloc->ranges[row].generation = alloc->ranges[row].generation + 1u;

    return row;
}

/* Retire a range row: it names nothing any more. The generation bump is what makes a range that
   named it stop naming it, and the size clear is what makes it a row the next take can reuse. */
static void xud_alloc_retire_range(XudAllocator* alloc, uint32_t row) {
    alloc->ranges[row].size = 0u;
    alloc->ranges[row].live = 0u;
    alloc->ranges[row].generation = alloc->ranges[row].generation + 1u;
}

/* Coalesce the free range at `row` with its free neighbours in the same block, until nothing more
   can be merged. This is what makes the free list not decay: a create/destroy loop over one size
   returns the same one free range every cycle rather than one more row per cycle, which is what
   keeps a long loop from spending the range bound. Offsets and sizes are bounded by the block
   bound above, so the adjacency sums below cannot overflow a VkDeviceSize. */
static void xud_alloc_coalesce(XudAllocator* alloc, uint32_t row) {
    bool merged = true;
    uint32_t other = 0u;
    XudAllocRange* keep = &alloc->ranges[row];

    while (merged) {
        merged = false;
        for (other = 0u; other < alloc->rangeCount; other++) {
            XudAllocRange* scan = &alloc->ranges[other];

            if (other == row)
                continue;
            if ((scan->size == 0u) || (scan->live != 0u))
                continue;
            if (scan->blockIndex != keep->blockIndex)
                continue;

            if ((scan->offset + scan->size) == keep->offset) {
                keep->offset = scan->offset;
                keep->size = keep->size + scan->size;
                xud_alloc_retire_range(alloc, other);
                merged = true;
            } else if ((keep->offset + keep->size) == scan->offset) {
                keep->size = keep->size + scan->size;
                xud_alloc_retire_range(alloc, other);
                merged = true;
            }
        }
    }
}

/*-----------------------------------------------------------------------------------------------
 * The pure half: the memory-type rule, the create-info assembly, the free-list scan
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_allocator_open(XudAllocator** outAlloc) {
    XudAllocator* alloc = NULL;

    if ((outAlloc == NULL) || (*outAlloc != NULL)) {
        return XUD_INVALID_ARG;
    }

    alloc = calloc(1u, sizeof(XudAllocator));
    if (alloc == NULL) {
        return XUD_OUT_OF_MEMORY;
    }

    /* Every array starts NULL and every count starts 0: the "nothing has been allocated yet" state,
       and the reason a context that never draws pays for nothing but this struct. */
    alloc->ringMemory = VK_NULL_HANDLE;
    alloc->ringSize = 0u;
    alloc->ringCursor = 0u;
    alloc->ringBlockIndex = XUD_ALLOCATOR_NO_BLOCK;
    alloc->ringMemoryTypeIndex = 0u;

    *outAlloc = alloc;
    return XUD_OK;
}

XudResult xud_allocator_select_memory_type(
    const XudDeviceCaps* caps, const VkMemoryRequirements* requirements, uint32_t* outMemoryTypeIndex) {
    const VkPhysicalDeviceMemoryProperties* mem = NULL;
    uint32_t index = 0u;
    uint32_t fallback = XUD_ALLOCATOR_NO_RANGE;
    uint32_t typeCount = 0u;

    if ((caps == NULL) || (requirements == NULL) || (outMemoryTypeIndex == NULL)) {
        return XUD_INVALID_ARG;
    }
    if (requirements->memoryTypeBits == 0u) {
        return XUD_UNSUPPORTED;
    }

    /* Two hops, not one: the caps' field is itself named properties, and its type is the *2 wrapper
       whose core struct is the one that holds the rows - the member-path trap xud_physical.c states.
     */
    mem = &caps->memoryProperties.memoryProperties;
    typeCount = mem->memoryTypeCount;
    if (typeCount > VK_MAX_MEMORY_TYPES) {
        return XUD_INTERNAL_ERROR;
    }

    for (index = 0u; index < typeCount; index++) {
        const VkMemoryType* type = &mem->memoryTypes[index];

        if ((requirements->memoryTypeBits & (1u << index)) == 0u) {
            continue;
        }
        if (type->heapIndex >= mem->memoryHeapCount) {
            continue;
        }
        /* A protected memory type is never a candidate here, and the reason is structural rather
           than a driver answer: protectedMemory is VkPhysicalDeviceProtectedMemoryFeatures (5871),
           a CHAINED feature struct - the same pNext shape the physical layer's "the driver chained
           something we did not ask for" check depends on, and the same shape AGENTS.md keeps as a
           future gate. This library chains nothing into the caps' features (probed: that row came
           back NULL) and enables nothing but its three wanted core features, so the device that w
           ould be asked for a protected allocation never has protectedMemory enabled - which is
           exactly the message the validation layer answers here. Skipping the type is the honest
           form of that refusal: the feature that needed it is not available, so the allocation th
           at needed it does not get a type.
         */
        if ((type->propertyFlags & VK_MEMORY_PROPERTY_PROTECTED_BIT) != 0u) {
            continue;
        }
        /* VUID-vkAllocateMemory-pAllocateInfo-01713, which the validation layer reports as a hard
           error and lavvk does not: the heap the type names must be big enough for the request. */
        if (requirements->size > mem->memoryHeaps[type->heapIndex].size) {
            continue;
        }

        if ((type->propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0u) {
            *outMemoryTypeIndex = index;
            return XUD_OK;
        }
        if (fallback == XUD_ALLOCATOR_NO_RANGE) {
            fallback = index;
        }
    }

    if (fallback == XUD_ALLOCATOR_NO_RANGE) {
        return XUD_UNSUPPORTED;
    }
    *outMemoryTypeIndex = fallback;
    return XUD_OK;
}

XudResult xud_allocator_fill_allocate_info(VkMemoryAllocateInfo* outInfo, uint32_t memoryTypeIndex, VkDeviceSize allocationSize) {
    if (outInfo == NULL) {
        return XUD_INVALID_ARG;
    }

    outInfo->sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    /* Nothing is chained here: VkMemoryDedicatedAllocateInfo is the dedicated-allocation entry
       AGENTS.md settled out, and its extension-name constant is not in these headers at all. */
    outInfo->pNext = NULL;
    outInfo->allocationSize = allocationSize;
    outInfo->memoryTypeIndex = memoryTypeIndex;

    return XUD_OK;
}

XudResult xud_allocator_find_range(const XudAllocator* alloc, VkDeviceSize size, VkDeviceSize alignment, uint32_t memoryTypeBits,
    uint32_t* outRangeIndex, VkDeviceSize* outOffset) {
    uint32_t row = 0u;

    if ((alloc == NULL) || (outRangeIndex == NULL) || (outOffset == NULL)) {
        return XUD_INVALID_ARG;
    }
    if ((alignment == 0u) || (alignment > (VkDeviceSize)XUD_ALLOCATOR_ALIGN_MAX)) {
        return XUD_INVALID_ARG;
    }

    for (row = 0u; row < alloc->rangeCount; row++) {
        const XudAllocRange* range = &alloc->ranges[row];
        const XudAllocBlock* block = NULL;
        VkDeviceSize aligned = 0u;
        uint32_t blockTypeBit = 0u;

        if ((range->size == 0u) || (range->live != 0u)) {
            continue;
        }
        if (range->blockIndex >= alloc->blockCount) {
            continue;
        }
        block = &alloc->blocks[range->blockIndex];
        if (block->memoryTypeIndex >= VK_MAX_MEMORY_TYPES) {
            continue;
        }

        /* The block's own type must be one the request may be bound to. A range inside a block of
           the wrong type is not a candidate, which is why the block's type index is recorded. */
        blockTypeBit = 1u << block->memoryTypeIndex;
        if ((memoryTypeBits & blockTypeBit) == 0u) {
            continue;
        }
        if (!xud_alloc_align_up(range->offset, alignment, &aligned)) {
            continue;
        }
        if (aligned < range->offset) {
            continue;
        }
        if ((aligned - range->offset) + size > range->size) {
            continue;
        }

        *outRangeIndex = row;
        *outOffset = aligned;
        return XUD_OK;
    }

    *outRangeIndex = XUD_ALLOCATOR_NO_RANGE;
    *outOffset = 0u;
    return XUD_OK;
}

/* Turn a free range into the live range that serves one request, plus whatever free rows the waste
   becomes. The head waste (the gap between the range's start and the aligned start) is always split,
   because it sits at a boundary the next alignment-aware scan can reach; the tail is absorbed into
   the live range when it is below XUD_ALLOCATOR_SPLIT_MIN, because a free row that small can serve
   nothing this layer will ever ask for and is bookkeeping that buys nothing. The range handed out is
   therefore AT LEAST the requested size - the request's size is a minimum, and the caller above reads
   the range's own size.
 */
static XudResult xud_allocator_take_range(
    XudAllocator* alloc, uint32_t row, VkDeviceSize alignedOffset, VkDeviceSize size, XudMemoryRange* outRange) {
    XudAllocBlock* block = NULL;
    XudAllocRange* chosen = NULL;
    XudAllocRange* head = NULL;
    XudAllocRange* tail = NULL;
    XudResult grown = XUD_OK;
    uint32_t headRow = 0u;
    uint32_t tailRow = 0u;
    VkDeviceSize headSize = 0u;
    VkDeviceSize tailSize = 0u;
    uint32_t blockIndex = 0u;

    chosen = &alloc->ranges[row];
    blockIndex = chosen->blockIndex;
    headSize = alignedOffset - chosen->offset;
    tailSize = chosen->size - headSize - size;

    /* Room for both waste rows before anything is written, so a refusal leaves the rows exactly as
       they were. */
    if (headSize != 0u) {
        grown = xud_alloc_reserve_ranges(alloc, 2u);
    } else if (tailSize >= (VkDeviceSize)XUD_ALLOCATOR_SPLIT_MIN) {
        grown = xud_alloc_reserve_ranges(alloc, 1u);
    } else {
        grown = XUD_OK;
    }
    if (grown != XUD_OK) {
        return grown;
    }

    chosen = &alloc->ranges[row];
    chosen->offset = alignedOffset;
    if (tailSize < (VkDeviceSize)XUD_ALLOCATOR_SPLIT_MIN) {
        chosen->size = chosen->size - tailSize; /* the tail is absorbed, see the note above */
    } else {
        chosen->size = size;
    }
    chosen->live = 1u;
    alloc->rangeLiveCount = alloc->rangeLiveCount + 1u;
    alloc->bytesInUse = alloc->bytesInUse + chosen->size;

    if (headSize != 0u) {
        headRow = xud_alloc_take_range_row(alloc);
        head = &alloc->ranges[headRow];
        head->blockIndex = blockIndex;
        head->offset = alignedOffset - headSize;
        head->size = headSize;
        head->live = 0u;
    }
    if (tailSize >= (VkDeviceSize)XUD_ALLOCATOR_SPLIT_MIN) {
        tailRow = xud_alloc_take_range_row(alloc);
        tail = &alloc->ranges[tailRow];
        tail->blockIndex = blockIndex;
        tail->offset = alignedOffset + size;
        tail->size = tailSize;
        tail->live = 0u;
    }

    block = &alloc->blocks[blockIndex];
    outRange->memory = block->memory;
    outRange->offset = chosen->offset;
    outRange->size = chosen->size;
    outRange->blockIndex = blockIndex;
    outRange->rangeIndex = row;
    outRange->generation = chosen->generation;

    return XUD_OK;
}

XudResult xud_allocator_acquire(XudMemoryRange* outRange, XudAllocator* alloc, XudLoader* loader, const XudDevice* device,
    const XudDeviceCaps* caps, const VkMemoryRequirements* requirements) {
    XudResult typeRule = XUD_UNSUPPORTED;
    XudResult grown = XUD_OK;
    VkMemoryAllocateInfo info;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize blockWanted = 0u;
    VkDeviceSize freeOffset = 0u;
    uint32_t typeIndex = 0u;
    uint32_t freeRow = XUD_ALLOCATOR_NO_RANGE;
    uint32_t blockIndex = 0u;

    if ((outRange == NULL) || (alloc == NULL) || (loader == NULL) || (device == NULL) || (caps == NULL) || (requirements == NULL)) {
        return XUD_INVALID_ARG;
    }

    /* Every guard below runs before the driver call, and the first ones are the shapes the
       validation layer here reports as hard errors: a zero allocationSize (VUID-VkMemoryAllocateIn
       fo-allocationSize-07897/07899, and lavvk accepts 0 silently), a memoryTypeBits of zero (no
       type can serve the request, which is the repo's XUD_UNSUPPORTED rather than a bad argument),
       and an alignment that is not an alignment or is above this layer's bound.
     */
    if (requirements->size == 0u) {
        return XUD_INVALID_ARG;
    }
    if ((requirements->alignment == 0u) || (requirements->alignment > (VkDeviceSize)XUD_ALLOCATOR_ALIGN_MAX)) {
        return XUD_INVALID_ARG;
    }
    if (requirements->memoryTypeBits == 0u) {
        return XUD_UNSUPPORTED;
    }

    /* A block this layer creates must be freeable, and the guard that bounds the free is the same
       one xud_swapchain_close() states: a block this layer cannot free is a block it must not crea
       te. Both rows belong to this device's table, and a handle-less device fails the bound check.
     */
    if (!xud_loader_device_bound(loader, device->device)) {
        return XUD_UNSUPPORTED;
    }
    if ((loader->deviceTable.vkAllocateMemory == NULL) || (loader->deviceTable.vkFreeMemory == NULL)) {
        return XUD_UNSUPPORTED;
    }

    typeRule = xud_allocator_select_memory_type(caps, requirements, &typeIndex);
    if (typeRule != XUD_OK) {
        return typeRule;
    }

    if (xud_alloc_block_size(requirements->size, &blockWanted) == false) {
        return XUD_OUT_OF_RESOURCES;
    }

    /* The free-list scan first, so a reused range costs no driver call at all - which is the whole
       point of "allocate once, suballocate always" read as a cost rather than a wish. */
    if (xud_allocator_find_range(alloc, requirements->size, requirements->alignment, requirements->memoryTypeBits, &freeRow, &freeOffset)
        != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    if (freeRow == XUD_ALLOCATOR_NO_RANGE) {
        /* No free range: create a block. The create-info is the assembly above, so the record of
           what was asked and the thing that was asked cannot disagree. */
        if (xud_alloc_reserve_blocks(alloc, 1u) != XUD_OK) {
            return XUD_OUT_OF_RESOURCES;
        }
        if (xud_alloc_reserve_ranges(alloc, 3u) != XUD_OK) {
            return XUD_OUT_OF_RESOURCES;
        }

        grown = xud_allocator_fill_allocate_info(&info, typeIndex, blockWanted);
        if (grown != XUD_OK) {
            return grown;
        }

        /* vkAllocateMemory returns VkResult, so the answer goes through the one funnel every layer
           above uses. A failed create leaves memory at VK_NULL_HANDLE, which is what keeps the block
           row from recording a handle that does not exist. */
        grown = xud_result_from_vk(loader->deviceTable.vkAllocateMemory(device->device, &info, NULL, &memory));
        if (grown != XUD_OK) {
            return grown;
        }

        blockIndex = alloc->blockCount;
        alloc->blockCount = blockIndex + 1u;
        alloc->blocks[blockIndex].memory = memory;
        alloc->blocks[blockIndex].size = blockWanted;
        alloc->blocks[blockIndex].memoryTypeIndex = typeIndex;

        freeRow = xud_alloc_take_range_row(alloc);
        alloc->ranges[freeRow].blockIndex = blockIndex;
        alloc->ranges[freeRow].offset = 0u;
        alloc->ranges[freeRow].size = blockWanted;
        alloc->ranges[freeRow].live = 0u;
        freeOffset = 0u;
    }

    return xud_allocator_take_range(alloc, freeRow, freeOffset, requirements->size, outRange);
}

/* The acquire a host-visible carve makes, and the reason a second acquire exists at all: vkMapMem
   ory maps a VkDeviceMemory, not a range, and the validation layer here refuses a second map of a
   memory that is already mapped (VUID-vkMapMemory-memory-00678, and lavvk answers it as VK_ERROR_M
   EMORY_MAP_FAILED rather than forgiving it). A suballocated carve inside a block someone else alr
   eady mapped is therefore a carve that cannot be mapped, so the one carve a rung maps has to own
   the block it is mapped in. That is the repo's "allocate once, suballocate always" rule with its
   one exception stated: suballocation is for the bytes a frame reads through a transfer, and a hos
   t mapping is a whole-memory decision, so it takes a block of its own and is never split.
 */
XudResult xud_allocator_acquire_host_visible(XudMemoryRange* outRange, XudAllocator* alloc, XudLoader* loader, const XudDevice* device,
    const XudDeviceCaps* caps, const VkMemoryRequirements* requirements) {
    VkMemoryAllocateInfo info;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize blockWanted = 0u;
    uint32_t typeIndex = 0u;
    uint32_t blockIndex = 0u;
    uint32_t rangeRow = 0u;
    XudResult grown = XUD_OK;

    if ((outRange == NULL) || (alloc == NULL) || (loader == NULL) || (device == NULL) || (caps == NULL) || (requirements == NULL)) {
        return XUD_INVALID_ARG;
    }

    /* The same request-side guards xud_allocator_acquire() runs before its driver call, and the
       same two decisions: the type the caps allow and the block this layer would create. The two
       acquires differ in where the bytes come from, never in what they are.
     */
    if (requirements->size == 0u) {
        return XUD_INVALID_ARG;
    }
    if ((requirements->alignment == 0u) || (requirements->alignment > (VkDeviceSize)XUD_ALLOCATOR_ALIGN_MAX)) {
        return XUD_INVALID_ARG;
    }
    if (requirements->memoryTypeBits == 0u) {
        return XUD_UNSUPPORTED;
    }
    if (!xud_loader_device_bound(loader, device->device)) {
        return XUD_UNSUPPORTED;
    }
    if ((loader->deviceTable.vkAllocateMemory == NULL) || (loader->deviceTable.vkFreeMemory == NULL)) {
        return XUD_UNSUPPORTED;
    }
    if (xud_allocator_select_memory_type(caps, requirements, &typeIndex) != XUD_OK) {
        return XUD_UNSUPPORTED;
    }
    if (xud_alloc_block_size(requirements->size, &blockWanted) == false) {
        return XUD_OUT_OF_RESOURCES;
    }

    /* No free-list scan here at all: the range below is the whole block, so there is nothing to
       split and nothing to reuse - and reusing a range would be reusing a memory that may already
       carry a mapping, which is the refusal stated above.
     */
    if ((xud_alloc_reserve_blocks(alloc, 1u) != XUD_OK) || (xud_alloc_reserve_ranges(alloc, 1u) != XUD_OK)) {
        return XUD_OUT_OF_RESOURCES;
    }

    grown = xud_allocator_fill_allocate_info(&info, typeIndex, blockWanted);
    if (grown != XUD_OK) {
        return grown;
    }

    grown = xud_result_from_vk(loader->deviceTable.vkAllocateMemory(device->device, &info, NULL, &memory));
    if (grown != XUD_OK) {
        return grown;
    }

    blockIndex = alloc->blockCount;
    alloc->blockCount = blockIndex + 1u;
    alloc->blocks[blockIndex].memory = memory;
    alloc->blocks[blockIndex].size = blockWanted;
    alloc->blocks[blockIndex].memoryTypeIndex = typeIndex;

    rangeRow = xud_alloc_take_range_row(alloc);
    alloc->ranges[rangeRow].blockIndex = blockIndex;
    alloc->ranges[rangeRow].offset = 0u;
    alloc->ranges[rangeRow].size = blockWanted;
    alloc->ranges[rangeRow].live = 0u;

    return xud_allocator_take_range(alloc, rangeRow, 0u, blockWanted, outRange);
}

XudResult xud_allocator_release(XudAllocator* alloc, const XudMemoryRange* range) {
    XudAllocRange* row = NULL;

    if ((alloc == NULL) || (range == NULL)) {
        return XUD_INVALID_ARG;
    }
    if ((range->rangeIndex >= alloc->rangeCount) || (range->blockIndex >= alloc->blockCount)) {
        return XUD_INVALID_ARG;
    }

    row = &alloc->ranges[range->rangeIndex];
    /* The three-part test, in the same order xud_handle_validate() states: bounds, then generation
       equality, then the live bit. A stale range (one kept across the release that freed it), a
       range from another allocator and a fabricated one are all refused here rather than double-
       releasing a row or coalescing over a live one.
     */
    if (row->generation != range->generation) {
        return XUD_INVALID_ARG;
    }
    if ((row->size == 0u) || (row->live == 0u)) {
        return XUD_INVALID_ARG;
    }
    if (row->blockIndex != range->blockIndex) {
        return XUD_INVALID_ARG;
    }

    row->live = 0u;
    row->generation = row->generation + 1u;
    alloc->rangeLiveCount = alloc->rangeLiveCount - 1u;
    alloc->bytesInUse = alloc->bytesInUse - row->size;

    xud_alloc_coalesce(alloc, range->rangeIndex);

    return XUD_OK;
}

XudResult xud_allocator_range_get(const XudAllocator* alloc, const XudMemoryRange* range, XudMemoryRange* outCurrent) {
    XudAllocRange* row = NULL;

    if ((alloc == NULL) || (range == NULL) || (outCurrent == NULL)) {
        return XUD_INVALID_ARG;
    }
    if ((range->rangeIndex >= alloc->rangeCount) || (range->blockIndex >= alloc->blockCount)) {
        return XUD_INVALID_ARG;
    }

    row = &alloc->ranges[range->rangeIndex];
    if (row->generation != range->generation) {
        return XUD_INVALID_ARG;
    }
    if ((row->size == 0u) || (row->live == 0u)) {
        return XUD_INVALID_ARG;
    }
    if (row->blockIndex != range->blockIndex) {
        return XUD_INVALID_ARG;
    }

    outCurrent->memory = alloc->blocks[row->blockIndex].memory;
    outCurrent->offset = row->offset;
    outCurrent->size = row->size;
    outCurrent->blockIndex = row->blockIndex;
    outCurrent->rangeIndex = range->rangeIndex;
    outCurrent->generation = row->generation;

    return XUD_OK;
}

XudResult xud_allocator_ring_acquire(XudMemoryRange* outRange, XudAllocator* alloc, XudLoader* loader, const XudDevice* device,
    const XudDeviceCaps* caps, VkDeviceSize size, VkDeviceSize alignment, uint32_t memoryTypeBits) {
    XudResult grown = XUD_OK;
    VkMemoryAllocateInfo info;
    VkMemoryRequirements ringAsk;
    VkDeviceSize aligned = 0u;
    uint32_t typeIndex = 0u;
    uint32_t blockIndex = 0u;
    uint32_t row = 0u;

    if ((outRange == NULL) || (alloc == NULL) || (loader == NULL) || (device == NULL) || (caps == NULL)) {
        return XUD_INVALID_ARG;
    }
    if (size == 0u) {
        return XUD_INVALID_ARG;
    }
    if ((alignment == 0u) || (alignment > (VkDeviceSize)XUD_ALLOCATOR_ALIGN_MAX)) {
        return XUD_INVALID_ARG;
    }
    if (memoryTypeBits == 0u) {
        return XUD_UNSUPPORTED;
    }
    /* A transient request larger than the whole ring is a caller bug, not a refusal worth growing
       for: a wrap would hand the same bytes out twice inside one frame, which is the one thing a
       ring may not do.
     */
    if (size > (VkDeviceSize)XUD_ALLOCATOR_RING_SIZE) {
        return XUD_OUT_OF_RESOURCES;
    }

    if (alloc->ringBlockIndex == XUD_ALLOCATOR_NO_BLOCK) {
        if (!xud_loader_device_bound(loader, device->device)) {
            return XUD_UNSUPPORTED;
        }
        if ((loader->deviceTable.vkAllocateMemory == NULL) || (loader->deviceTable.vkFreeMemory == NULL)) {
            return XUD_UNSUPPORTED;
        }

        /* The ring is one block of this layer, recorded in the block array like every other one, so
           xud_allocator_close() frees it with the rest and there is no second teardown path for it.
           The type rule is asked with the ring's own size, not the request's: the block is the whole
           ring, and the request's memoryTypeBits is what the caller above may bind it to.
         */
        ringAsk.size = (VkDeviceSize)XUD_ALLOCATOR_RING_SIZE;
        ringAsk.alignment = alignment;
        ringAsk.memoryTypeBits = memoryTypeBits;
        grown = xud_allocator_select_memory_type(caps, &ringAsk, &typeIndex);
        if (grown != XUD_OK) {
            return grown;
        }

        if (xud_alloc_reserve_blocks(alloc, 1u) != XUD_OK) {
            return XUD_OUT_OF_RESOURCES;
        }
        if (xud_alloc_reserve_ranges(alloc, 1u) != XUD_OK) {
            return XUD_OUT_OF_RESOURCES;
        }

        grown = xud_allocator_fill_allocate_info(&info, typeIndex, (VkDeviceSize)XUD_ALLOCATOR_RING_SIZE);
        if (grown != XUD_OK) {
            return grown;
        }

        grown = xud_result_from_vk(loader->deviceTable.vkAllocateMemory(device->device, &info, NULL, &alloc->ringMemory));
        if (grown != XUD_OK) {
            return grown;
        }

        blockIndex = alloc->blockCount;
        alloc->blockCount = blockIndex + 1u;
        alloc->blocks[blockIndex].memory = alloc->ringMemory;
        alloc->blocks[blockIndex].size = (VkDeviceSize)XUD_ALLOCATOR_RING_SIZE;
        alloc->blocks[blockIndex].memoryTypeIndex = typeIndex;
        alloc->ringSize = (VkDeviceSize)XUD_ALLOCATOR_RING_SIZE;
        alloc->ringMemoryTypeIndex = typeIndex;
        alloc->ringBlockIndex = blockIndex;
    }

    /* The cursor, and the wrap. A request that does not fit between the cursor and the end of the
       ring starts the next lap at 0, which is the whole contract of a ring: nothing it hands out
       lives past the frame it was asked for in, so overwriting what an earlier frame wrote is legal
       and is the point. The renderer step is what guarantees that contract; this layer's job is that
       the arithmetic is exact and that a request that does not fit in a whole lap is refused.
     */
    if (!xud_alloc_align_up(alloc->ringCursor, alignment, &aligned)) {
        return XUD_INVALID_ARG;
    }
    if (aligned > (VkDeviceSize)XUD_ALLOCATOR_RING_SIZE - size) {
        aligned = 0u;
    }
    if (aligned > (VkDeviceSize)XUD_ALLOCATOR_RING_SIZE - size) {
        return XUD_OUT_OF_RESOURCES;
    }

    row = xud_alloc_take_range_row(alloc);
    alloc->ranges[row].blockIndex = alloc->ringBlockIndex;
    alloc->ranges[row].offset = aligned;
    alloc->ranges[row].size = size;
    alloc->ranges[row].live = 1u;
    alloc->rangeLiveCount = alloc->rangeLiveCount + 1u;
    alloc->bytesInUse = alloc->bytesInUse + size;

    alloc->ringCursor = aligned + size;

    outRange->memory = alloc->blocks[alloc->ringBlockIndex].memory;
    outRange->offset = aligned;
    outRange->size = size;
    outRange->blockIndex = alloc->ringBlockIndex;
    outRange->rangeIndex = row;
    outRange->generation = alloc->ranges[row].generation;

    return XUD_OK;
}

XudResult xud_allocator_ring_reset(XudAllocator* alloc) {
    if (alloc == NULL) {
        return XUD_INVALID_ARG;
    }

    /* The cursor is the only state the ring has, so this is the whole reset: nothing is freed and
       nothing is marked free, because the ring is one block and its old contents are simply
       overwritten next frame. The range rows the ring handed out are still live and still counted -
       a caller above that releases them coalesces them back, and one that does not is refused at
       xud_allocator_close() by the block's own teardown.
     */
    alloc->ringCursor = 0u;
    return XUD_OK;
}

XudResult xud_allocator_stats(const XudAllocator* alloc, uint32_t* outBlockCount, uint32_t* outRangeLiveCount, VkDeviceSize* outBytesInUse,
    VkDeviceSize* outRingCursor) {
    if ((alloc == NULL) || (outBlockCount == NULL) || (outRangeLiveCount == NULL) || (outBytesInUse == NULL) || (outRingCursor == NULL)) {
        return XUD_INVALID_ARG;
    }

    *outBlockCount = alloc->blockCount;
    *outRangeLiveCount = alloc->rangeLiveCount;
    *outBytesInUse = alloc->bytesInUse;
    *outRingCursor = alloc->ringCursor;

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The teardown
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_allocator_close(XudAllocator** alloc, XudLoader* loader, const XudDevice* device) {
    XudAllocator* victim = NULL;
    XudResult guard = XUD_OK;
    XudResult first = XUD_OK;
    uint32_t row = 0u;

    if ((alloc == NULL) || (loader == NULL) || (device == NULL)) {
        return XUD_INVALID_ARG;
    }
    if ((*alloc) == NULL) {
        return XUD_ALREADY;
    }

    victim = *alloc;

    if (victim->blockCount == 0u) {
        /* Nothing was ever created on the GPU, so there is nothing to guard and nothing to free -
           but the host arrays are still here, and freeing them is what this rung is for. They are
           guarded rather than freed outright because an allocator that never allocated a block has
           never allocated an array either, and free(NULL) is undefined, not a no-op.
         */
        if (victim->ranges != NULL) {
            free(victim->ranges);
        }
        if (victim->blocks != NULL) {
            free(victim->blocks);
        }
        free(victim);
        *alloc = NULL;
        return XUD_OK;
    }

    /* The rows must be this device's rows, and the guard that bounds the frees is the same one
       xud_swapchain_close() states: a submission or a presentation may still be reading the very
       blocks being freed. A guard that cannot run is a teardown this rung must not start, because
       freeing the struct would leak every handle the driver still holds.
     */
    if (!xud_loader_device_bound(loader, device->device)) {
        return XUD_UNSUPPORTED;
    }
    if ((loader->deviceTable.vkDeviceWaitIdle == NULL) || (loader->deviceTable.vkFreeMemory == NULL)) {
        return XUD_UNSUPPORTED;
    }

    guard = xud_device_wait_idle(device, loader);
    if (guard == XUD_UNSUPPORTED) {
        return XUD_UNSUPPORTED;
    }
    if (guard != XUD_OK) {
        first = guard;
    }

    /* vkFreeMemory returns void, so there is no VkResult to map and nothing to check after the call
       - the zero-state-before-the-call rule this tree follows for every void entry point (vkFree
       Memory, vkDestroyDevice, vkGetDeviceQueue): the handle is read into a local, the row is cle
       ared, and only then is the handle handed over, so a teardown that runs twice cannot free the
       same handle twice and no reader can see a freed one.
     */
    for (row = 0u; row < victim->blockCount; row++) {
        VkDeviceMemory memory = victim->blocks[row].memory;
        victim->blocks[row].memory = VK_NULL_HANDLE;
        victim->blocks[row].size = 0u;
        if (memory != VK_NULL_HANDLE) {
            loader->deviceTable.vkFreeMemory(device->device, memory, NULL);
        }
    }
    victim->ringMemory = VK_NULL_HANDLE;
    victim->ringSize = 0u;
    victim->ringCursor = 0u;
    victim->ringBlockIndex = XUD_ALLOCATOR_NO_BLOCK;
    victim->ringMemoryTypeIndex = 0u;

    if (victim->ranges != NULL) {
        free(victim->ranges);
    }
    victim->ranges = NULL;
    victim->rangeCount = 0u;
    victim->rangeCapacity = 0u;
    victim->rangeLiveCount = 0u;
    victim->bytesInUse = 0u;
    if (victim->blocks != NULL) {
        free(victim->blocks);
    }
    victim->blocks = NULL;
    victim->blockCount = 0u;
    victim->blockCapacity = 0u;
    free(victim);
    *alloc = NULL;

    /* A lost device is still freed above, and the answer is still propagated - the same rule xud_de
       vice_close() and xud_swapchain_close() follow: XUD_LOST is a report, not a skip.
     */
    return first;
}
