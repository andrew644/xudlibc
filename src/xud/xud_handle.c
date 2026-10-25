/***********************************************************************************************
 *   xud_handle.c - the handle layer
 *   Owns: the per-kind slot arrays and everything that reads or writes them - allocation with a
 *         generation bump on reuse, release, the three-part validation, field-wise equality, the
 *         live count and teardown.
 *   Layer: above the swapchain, below the renderer. It calls no driver function and includes no
 *         Vulkan header: a handle is an index into this layer's own arrays, and the Vulkan handles
 *         a slot will eventually hold are created by the rungs ABOVE this one - the allocator is
 *         in the tree now (S12) and the renderer too (S11), but nothing above them has asked the
 *         allocator for a VkImage or a VkBuffer yet, so no slot's record field is filled. Every
 *         thing here is therefore pure and headless-testable by construction.
 ***********************************************************************************************/

#include "xud_handle.h"
#include "xud.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Internal: grow one kind's slot array by the doubling sequence of the header above. Called only
   when the high mark has reached the capacity, so a refusal here is a real "no more slots of this
   kind wanted" and not a formality. The old array is copied and freed, which is what keeps every
   index a handle carries valid across a growth step: an index is a position, not a pointer. */
static XudResult xud_handle_reserve(XudHandleTable* table, uint32_t kind) {
    XudHandleSlot* old = table->slots[kind];
    XudHandleSlot* grown = NULL;
    uint32_t oldCapacity = table->slotCapacity[kind];
    uint32_t newCapacity = XUD_HANDLE_SLOT_MIN_CAPACITY;

    if (oldCapacity >= XUD_HANDLE_SLOT_MAX_CAPACITY)
        return XUD_OUT_OF_RESOURCES;

    if (oldCapacity == 0u)
        newCapacity = XUD_HANDLE_SLOT_MIN_CAPACITY;
    else
        newCapacity = oldCapacity * 2u;
    if (newCapacity > XUD_HANDLE_SLOT_MAX_CAPACITY)
        newCapacity = XUD_HANDLE_SLOT_MAX_CAPACITY;

    grown = calloc(newCapacity, sizeof(XudHandleSlot));
    if (grown == NULL)
        return XUD_OUT_OF_MEMORY;

    /* A fresh array is already the zero state calloc gives (every slot free, generation 0, tag
       NONE), so the copy only happens on a growth step. */
    if (old != NULL) {
        memcpy(grown, old, (size_t)oldCapacity * sizeof(XudHandleSlot));
        free(old);
    }

    table->slots[kind] = grown;
    table->slotCapacity[kind] = newCapacity;
    return XUD_OK;
}

XudResult xud_handle_table_open(XudHandleTable** outTable) {
    XudHandleTable* table = NULL;

    if (outTable == NULL || *outTable != NULL)
        return XUD_INVALID_ARG;

    /* One allocation for the whole table: the per-kind arrays are NULL here and each grows on
       the first allocation of its own kind, so a context that never creates a font never pays
       for a font array. calloc is the empty state - no array, high mark 0, capacity 0. */
    table = calloc(1, sizeof(XudHandleTable));
    if (table == NULL)
        return XUD_OUT_OF_MEMORY;

    *outTable = table;
    return XUD_OK;
}

XudResult xud_handle_validate(const XudHandleTable* table, XudHandle handle, uint32_t expectedKind) {
    const XudHandleSlot* slot = NULL;

    if (table == NULL)
        return XUD_INVALID_ARG;

    /* The expected kind is the entry point's own argument, so its range is a precondition of the
       entry point rather than part of the three-part test - and it has to come first, because
       without it the array lookup below would index past the table's own rows. NONE is refused
       here for the same reason the tag compare further down refuses it: NONE is the tag of a
       handle that names nothing, and it must never select row 0 of a live table. */
    if (expectedKind == XUD_HANDLE_KIND_NONE || expectedKind >= XUD_HANDLE_KIND_COUNT)
        return XUD_INVALID_ARG;

    /* Part one: index bounds, against the high mark of the array of THAT kind - not against the
       handle's own tag. A kind whose array was never allocated has no slot at any index, which is
       the same answer as an out-of-range index and is reported as one. */
    if (table->slots[expectedKind] == NULL)
        return XUD_INVALID_ARG;
    if (handle.index >= table->slotCount[expectedKind])
        return XUD_INVALID_ARG;

    slot = &table->slots[expectedKind][handle.index];

    /* Part two: generation equality. A slot that has been released carries a generation one higher
       than the handle it was handed out under, so a handle kept across a release - or across any
       number of releases - fails here rather than naming a slot that has since been taken by
       another object. */
    if (slot->generation != handle.generation)
        return XUD_INVALID_ARG;

    /* Part three: tag equality, which is what makes the check exact rather than a layout
       coincidence: the (index, generation) pair of a texture handle can match a live model slot,
       and it is the tag that says it is not the same object. */
    if (slot->kind != handle.kind)
        return XUD_INVALID_ARG;

    /* The live bit, last: the three-part test above is what makes a STALE handle fail, and this is
       what makes a handle that is not stale but names a FREE slot fail too - a generation can
       match a free slot only for a handle that was never handed out, and refusing it is what keeps
       a decoded handle always a handle that names a live slot. */
    if (slot->occupied == 0u)
        return XUD_INVALID_ARG;

    return XUD_OK;
}

bool xud_handle_equal(XudHandle left, XudHandle right) {
    return (left.index == right.index) && (left.generation == right.generation) && (left.kind == right.kind);
}

XudResult xud_handle_alloc(XudHandle* outHandle, XudHandleTable* table, uint32_t kind) {
    XudHandleSlot* slot = NULL;
    XudResult grown = XUD_OK;
    uint32_t index = 0u;
    uint32_t highMark = 0u;

    if (outHandle == NULL || table == NULL)
        return XUD_INVALID_ARG;
    if (kind == XUD_HANDLE_KIND_NONE || kind >= XUD_HANDLE_KIND_COUNT)
        return XUD_INVALID_ARG;

    highMark = table->slotCount[kind];

    /* Reuse first: the first free slot below the high mark, so a released slot is taken again
       before the array grows and its generation is already one higher than the handle that was
       released (xud_handle_free bumped it). A never-used slot has generation 0 and is free, and a
       released slot's generation is at least 1, so the two states are never confused. */
    for (index = 0u; index < highMark; index++) {
        if (table->slots[kind][index].occupied == 0u)
            break;
    }

    if (index == highMark) {
        /* Nothing free below the high mark: take the high mark itself, which means growing this
           kind's array first when the high mark has reached the capacity. The growth answer
           travels unchanged - XUD_OUT_OF_MEMORY and XUD_OUT_OF_RESOURCES are different reasons,
           and the caller above reads them differently. */
        if (highMark >= table->slotCapacity[kind]) {
            grown = xud_handle_reserve(table, kind);
            if (grown != XUD_OK)
                return grown;
        }
        table->slotCount[kind] = highMark + 1u;
    }

    slot = &table->slots[kind][index];

    slot->occupied = 1u;
    slot->kind = kind;
    slot->record = NULL;

    outHandle->index = index;
    outHandle->generation = slot->generation;
    outHandle->kind = kind;

    return XUD_OK;
}

XudResult xud_handle_free(XudHandleTable* table, XudHandle handle) {
    XudHandleSlot* slot = NULL;

    if (table == NULL)
        return XUD_INVALID_ARG;

    /* Releasing is only ever about the array the handle's own tag names, so the expected kind here
       is the handle's tag - and validate() refuses a tag that is not a kind before it indexes, so
       a zero-initialised handle is refused rather than releasing row 0. A stale handle, a
       cross-kind handle and a fabricated one all fail here, which is what keeps a double release
       from bumping one generation twice and handing two handles out for one slot. */
    if (xud_handle_validate(table, handle, handle.kind) != XUD_OK)
        return XUD_INVALID_ARG;

    slot = &table->slots[handle.kind][handle.index];

    /* The bump is the whole stale-handle rule read at the one place a slot goes free: the handle
       in hand stops naming this slot, and so does every handle that named it before the release
       that freed it. The tag and the record are left as they were, so a free slot still says what
       it held last - the live bit is what distinguishes it. The wrap case (2^32 reuses of one
       index, where a bumped generation can come back to a value an older handle carries) is
       documented in xud.h rather than refused here - see the note there. */
    slot->occupied = 0u;
    slot->generation = slot->generation + 1u;

    return XUD_OK;
}

XudResult xud_handle_table_live_count(const XudHandleTable* table, uint32_t kind, uint32_t* outLiveCount) {
    uint32_t index = 0u;
    uint32_t live = 0u;

    if ((table == NULL) || (outLiveCount == NULL))
        return XUD_INVALID_ARG;
    if (kind == XUD_HANDLE_KIND_NONE || kind >= XUD_HANDLE_KIND_COUNT)
        return XUD_INVALID_ARG;

    if (table->slots[kind] != NULL) {
        for (index = 0u; index < table->slotCount[kind]; index++) {
            if (table->slots[kind][index].occupied != 0u)
                live++;
        }
    }

    *outLiveCount = live;
    return XUD_OK;
}

XudResult xud_handle_table_close(XudHandleTable** table) {
    XudHandleTable* victim = NULL;
    uint32_t kind = 0u;

    if (table == NULL)
        return XUD_INVALID_ARG;
    if (*table == NULL)
        return XUD_ALREADY;

    victim = *table;

    /* Cleared before freeing, so a table closed twice cannot double-free an array, and every kind's
       row is walked whether or not it was ever allocated - a NULL row is nothing to free. */
    for (kind = 0u; kind < XUD_HANDLE_KIND_COUNT; kind++) {
        if (victim->slots[kind] != NULL) {
            free(victim->slots[kind]);
            victim->slots[kind] = NULL;
        }
        victim->slotCount[kind] = 0u;
        victim->slotCapacity[kind] = 0u;
    }

    free(victim);
    *table = NULL;
    return XUD_OK;
}
