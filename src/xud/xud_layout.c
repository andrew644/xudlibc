/***********************************************************************************************
 * xud_layout.c - the layout tracker's map and its one driver row
 *
 *   The map is open-addressed with linear probing, keyed by the VkImage handle itself. The hash
 * is multiply-shift: handles are allocator addresses whose low bits are heavily correlated (they
 * come out of the same pools), so the LOW bits of a plain product cluster and the HIGH bits of a
 * 64-bit product by the golden ratio are the ones that vary - the index is the product's top
 * bits scaled to the table. Probing is linear because the table is small and entries are read
 * once per barrier: cache beats cleverness at this size.
 *
 *   Growth rehashes every live key through the same index row, and forget leaves a delete
 * marker - XUD_LAYOUT_TOMBSTONE, a handle value no driver ever returns - in the cleared slot.
 * A probe walks OVER markers and stops only at a never-used slot, so retiring an image in the
 * middle of a cluster (a swapchain recreate and a render-pass recreate do exactly that, in the
 * middle of a live table) never orphans the keys behind it; a plain clear would. An insert
 * takes the first marker on its walk and growth drops every marker, so the table cannot fill
 * with corpses while the live count stays small - which is why the load factor counts markers
 * beside live entries. The consequence the callers feel: forget of a key that is not there is
 * refused rather than ignored, because the only way to get there is a rung that forgot twice or
 * forgot the wrong handle, and both are bugs this rung surfaces rather than absorbs.
 *
 *   No driver object is created, bound or destroyed here. vkCmdPipelineBarrier is the one driver
 * row named below, gated on xud_loader_device_bound() before it is called through, the same
 * check-before-you-use pair every rung that records commands applies.
 ***********************************************************************************************/

#include "xud_layout.h"

#include "volk.h" /* Required for: the Vk* types the header names - and it must come before
                     <vulkan/vulkan.h> (AGENTS.md, include-order trap) */

#include <stdlib.h> /* Required for: calloc, free */
#include <string.h> /* Required for: NULL */

#include "xud_loader.h" /* Required for: XudLoader, xud_loader_device_bound */

static_assert(sizeof(PFN_vkCmdPipelineBarrier) == sizeof(void*), "PFN row must be pointer-sized");

#define XUD_LAYOUT_LOAD_NUMERATOR 3u /* grow when (used+1)*DENOMINATOR > slots*NUMERATOR: the ta */
#define XUD_LAYOUT_LOAD_DENOMINATOR 4u /* ble stays at least a quarter free, the probe bound above */

#define XUD_LAYOUT_HASH_MULTIPLIER                                                                                                         \
    0x9E3779B97F4A7C15ull /* the golden ratio: the multiply-shift
                                                            row's other operand, from the note */

/*-----------------------------------------------------------------------------------------------
 * The index row and the probe walk - private to this file
 *-----------------------------------------------------------------------------------------------*/

static uint32_t xud_layout_index(const XudLayoutTracker* tracker, VkImage image) {
    uint64_t key = (uint64_t)(uintptr_t)image;

    key *= XUD_LAYOUT_HASH_MULTIPLIER;
    return (uint32_t)(key >> 32) % tracker->slotCount;
}

/* The slot that HOLDS image, or the first slot an insert may take on its walk when nothing
   holds it - a tombstoned slot or a never-used one, whichever comes first - the pair of answers
   an insert (take the holder, else take the reusable slot) and a lookup (take the holder, else
   refuse) both want from the one walk. Returns a slot index; *outFound answers which of the two
   it is. A walk is guaranteed to stop at a reusable slot because the load factor, counted over
   live entries AND tombstones, keeps the table non-full, which is the third thing the growth
   rule is for.
*/
static uint32_t xud_layout_probe(const XudLayoutTracker* tracker, VkImage image, bool* outFound) {
    uint32_t index = xud_layout_index(tracker, image);
    uint32_t firstTombstone = tracker->slotCount;
    uint32_t step = 0u;

    *outFound = false;
    for (step = 0u; step < tracker->slotCount; step++) {
        const XudImageSlot* slot = &tracker->slots[index];
        if (slot->image == XUD_LAYOUT_TOMBSTONE) {
            if (firstTombstone == tracker->slotCount) {
                firstTombstone = index;
            }
        } else if (slot->image == VK_NULL_HANDLE) {
            return (firstTombstone != tracker->slotCount) ? firstTombstone : index;
        } else if (slot->image == image) {
            *outFound = true;
            return index;
        }
        index = index + 1u;
        if (index == tracker->slotCount) {
            index = 0u;
        }
    }
    return firstTombstone; /* no never-used slot on the walk: only a marker can still carry the
                              insert, and slot [count] - which the caller turns into
                              XUD_INTERNAL_ERROR - answers a table with neither to spare */
}

/* Re-key every live entry into a fresh array of the given size. calloc answers NULL at OOM, and
   the caller's grow leaves the tracker entirely alone in that case - the old array, the old
   count and every entry stay valid, which is what makes growth a non-event for every caller
   that holds a pointer to this struct across a track.
*/
static XudResult xud_layout_grow(XudLayoutTracker* tracker, uint32_t newSlotCount) {
    XudImageSlot* oldSlots = tracker->slots;
    uint32_t oldSlotCount = tracker->slotCount;
    uint32_t index = 0u;
    uint32_t freshIndex = 0u;
    bool found = false;
    XudLayoutTracker fresh;

    fresh.slots = calloc(newSlotCount, sizeof(*fresh.slots));
    if (fresh.slots == NULL) {
        return XUD_OUT_OF_MEMORY;
    }
    fresh.slotCount = newSlotCount;
    fresh.usedCount = 0u;

    for (index = 0u; index < oldSlotCount; index++) {
        if ((oldSlots[index].image == VK_NULL_HANDLE) || (oldSlots[index].image == XUD_LAYOUT_TOMBSTONE)) {
            continue;
        }
        freshIndex = xud_layout_probe(&fresh, oldSlots[index].image, &found);
        if (freshIndex == newSlotCount) {
            free(fresh.slots);
            return XUD_INTERNAL_ERROR;
        }
        fresh.slots[freshIndex].image = oldSlots[index].image;
        fresh.slots[freshIndex].layout = oldSlots[index].layout;
        fresh.usedCount = fresh.usedCount + 1u;
    }

    free(oldSlots);
    tracker->slots = fresh.slots;
    tracker->slotCount = newSlotCount;
    tracker->tombCount = 0u;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * Bring-up and teardown
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_layout_tracker_open(XudLayoutTracker** outTracker, uint32_t capacityHint) {
    XudLayoutTracker* tracker = NULL;
    uint32_t slotCount = XUD_LAYOUT_MIN_SLOTS;

    if ((outTracker == NULL) || (*outTracker != NULL)) {
        return XUD_INVALID_ARG;
    }

    tracker = calloc(1u, sizeof(*tracker));
    if (tracker == NULL) {
        return XUD_OUT_OF_MEMORY;
    }
    while (capacityHint > slotCount) {
        slotCount = slotCount * 2u;
    }
    tracker->slots = calloc(slotCount, sizeof(*tracker->slots));
    if (tracker->slots == NULL) {
        free(tracker);
        return XUD_OUT_OF_MEMORY;
    }
    tracker->slotCount = slotCount;
    *outTracker = tracker;
    return XUD_OK;
}

XudResult xud_layout_tracker_close(XudLayoutTracker** ioTracker) {
    XudLayoutTracker* victim = NULL;

    if (ioTracker == NULL) {
        return XUD_INVALID_ARG;
    }
    victim = *ioTracker;
    if (victim == NULL) {
        return XUD_ALREADY;
    }

    free(victim->slots);
    victim->slots = NULL;
    victim->slotCount = 0u;
    victim->usedCount = 0u;
    free(victim);
    *ioTracker = NULL;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The map's three state changes and its one lookup
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_layout_track(XudLayoutTracker* tracker, VkImage image, VkImageLayout initialLayout) {
    bool found = false;
    uint32_t index = 0u;

    if ((tracker == NULL) || (image == VK_NULL_HANDLE)) {
        return XUD_INVALID_ARG;
    }
    index = xud_layout_probe(tracker, image, &found);
    if (index == tracker->slotCount) {
        return XUD_INTERNAL_ERROR;
    }
    if (found == true) {
        return XUD_INTERNAL_ERROR;
    }
    if (((tracker->usedCount + tracker->tombCount + 1u) * XUD_LAYOUT_LOAD_DENOMINATOR) > (tracker->slotCount * XUD_LAYOUT_LOAD_NUMERATOR)) {
        XudResult result = xud_layout_grow(tracker, tracker->slotCount * 2u);
        if (result != XUD_OK) {
            return result;
        }
        index = xud_layout_probe(tracker, image, &found);
        if ((index == tracker->slotCount) || (found == true)) {
            return XUD_INTERNAL_ERROR;
        }
    }

    if (tracker->slots[index].image == XUD_LAYOUT_TOMBSTONE) {
        tracker->tombCount = tracker->tombCount - 1u;
    }
    tracker->slots[index].image = image;
    tracker->slots[index].layout = initialLayout;
    tracker->usedCount = tracker->usedCount + 1u;
    return XUD_OK;
}

XudResult xud_layout_forget(XudLayoutTracker* tracker, VkImage image) {
    bool found = false;
    uint32_t index = 0u;

    if ((tracker == NULL) || (image == VK_NULL_HANDLE)) {
        return XUD_INVALID_ARG;
    }
    index = xud_layout_probe(tracker, image, &found);
    if ((index == tracker->slotCount) || (found == false)) {
        return XUD_NOT_FOUND;
    }

    tracker->slots[index].image = XUD_LAYOUT_TOMBSTONE;
    tracker->slots[index].layout = VK_IMAGE_LAYOUT_UNDEFINED;
    tracker->usedCount = tracker->usedCount - 1u;
    tracker->tombCount = tracker->tombCount + 1u;
    return XUD_OK;
}

XudResult xud_layout_get(VkImageLayout* outLayout, const XudLayoutTracker* tracker, VkImage image) {
    bool found = false;
    uint32_t index = 0u;

    if ((outLayout == NULL) || (tracker == NULL) || (image == VK_NULL_HANDLE)) {
        return XUD_INVALID_ARG;
    }
    index = xud_layout_probe(tracker, image, &found);
    if ((index == tracker->slotCount) || (found == false)) {
        return XUD_NOT_FOUND;
    }

    *outLayout = tracker->slots[index].layout;
    return XUD_OK;
}

XudResult xud_layout_set(XudLayoutTracker* tracker, VkImage image, VkImageLayout layout) {
    bool found = false;
    uint32_t index = 0u;

    if ((tracker == NULL) || (image == VK_NULL_HANDLE)) {
        return XUD_INVALID_ARG;
    }
    index = xud_layout_probe(tracker, image, &found);
    if ((index == tracker->slotCount) || (found == false)) {
        return XUD_NOT_FOUND;
    }

    tracker->slots[index].layout = layout;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The barrier: the pure fill and the recording call around it
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_layout_barrier_fill(
    VkImageMemoryBarrier* outBarrier, const XudLayoutTracker* tracker, VkImage image, const VkImageMemoryBarrier* request) {
    VkImageLayout tracked = VK_IMAGE_LAYOUT_MAX_ENUM;
    XudResult result = XUD_OK;

    if ((outBarrier == NULL) || (tracker == NULL) || (request == NULL) || (image == VK_NULL_HANDLE)) {
        return XUD_INVALID_ARG;
    }
    if (outBarrier == request) {
        return XUD_INVALID_ARG; /* the fill answers about the request without consuming it; an
                                   alias would make oldLayout's replacement read a row that is
                                   already half-overwritten */
    }
    if (request->newLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
        return XUD_INVALID_ARG; /* UNDEFINED is legal only as a source; a barrier INTO it has no
                                   defined meaning (Vulkan spec, 32.5) */
    }

    result = xud_layout_get(&tracked, tracker, image);
    if (result != XUD_OK) {
        return result;
    }

    *outBarrier = *request;
    outBarrier->image = image;
    outBarrier->oldLayout = tracked;
    return XUD_OK;
}

XudResult xud_layout_barrier(XudLayoutTracker* tracker, const XudLoader* loader, VkCommandBuffer commandBuffer,
    const VkImageMemoryBarrier* request, VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask) {
    VkImageMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .pNext = NULL };
    XudResult result = XUD_OK;

    if ((tracker == NULL) || (loader == NULL) || (commandBuffer == VK_NULL_HANDLE)) {
        return XUD_INVALID_ARG;
    }
    if (!xud_loader_is_open(loader)) {
        return XUD_UNSUPPORTED; /* the device table is filled at the device bind and cleared with
                                   the instance; past either edge the row below is not safe to
                                   call through - the bound-device check is the caller's, the one
                                   rung that named the command buffer knows whose device it is */
    }
    if (loader->deviceTable.vkCmdPipelineBarrier == NULL) {
        return XUD_UNSUPPORTED;
    }

    result = xud_layout_barrier_fill(&barrier, tracker, request->image, request);
    if (result != XUD_OK) {
        return result;
    }

    loader->deviceTable.vkCmdPipelineBarrier(commandBuffer, srcStageMask, dstStageMask, 0u, 0u, NULL, 0u, NULL, 1u, &barrier);
    return xud_layout_set(tracker, request->image, request->newLayout);
}
