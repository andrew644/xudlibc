/***********************************************************************************************
 *   xud_handle.h - the handle layer: the per-kind slot arrays behind the one XudHandle type
 *                  that xud.h declares, and the only storage the library owns for the object
 *                  kinds a caller can name
 *
 *   Owns : XudHandleTable (one slot array per caller-addressable kind) and the contract of the
 *          rung - table bring-up, slot allocation with a generation bump on reuse, release, the
 *          validation every public entry point runs on a handle in hand, field-wise handle
 *          equality, the live count of one kind, and teardown.
 *   Layer: the public API's currency, and the storage rung for the object kinds a caller can
 *          name (textures, fonts, meshes, materials, models). It sits above the swapchain and
 *          below the renderer and the feature modules: a handle addresses an object that lives
 *          in GPU memory the allocator rung owns and that the renderer rung draws, and neither
 *          rung is in the tree yet. That is why nothing here calls a driver function and why
 *          every slot's record field is still the sentinel it opens with - the Vulkan handles
 *          the decision says a record holds land inside it when the object steps land, and
 *          until then there is no VkImage, VkBuffer or VkPipeline of any of these kinds to put
 *          one there. The type itself is public (xud.h declares it, because it is the one thing
 *          a caller holds); the table and these functions are internal like every layer below
 *          the public API: tests may include this header, user code must not.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#ifndef XUD_XUD_HANDLE_H_
#define XUD_XUD_HANDLE_H_

#include "xud.h" /* Required for: XudHandle, XUD_HANDLE_KIND_* and XudResult - the public currency this layer stores, and its own error codes */
#include <stdbool.h>
#include <stdint.h>

/*-----------------------------------------------------------------------------------------------
 * Why this rung is xud_handle and not a slot array on the context
 *-----------------------------------------------------------------------------------------------*/

/* A handle is not a field of the context because the thing a caller holds is not a pointer into
   the context: it is an index, a generation and a tag, and the arrays it indexes are owned by
   the context that allocated them. Owning them here is what makes the repo's "stale handles must
   be caught, not UB" rule a property of this layer rather than a property of whoever happens to
   read a handle - the three-part validation below is the one place a handle is decoded, and
   every public entry point that takes one runs it before it looks at anything the handle names.
   It is also what makes more than one context work: two contexts have two tables, and a handle
   from one is refused by the other rather than indexing the wrong array. */

/*-----------------------------------------------------------------------------------------------
 * The facts this layer's shape rests on
 *-----------------------------------------------------------------------------------------------*/

/* The handle is 12 bytes of three uint32_t and nothing else - asserted in xud.h, beside the type,
   because the type is public and the assert belongs where the type is. What is asserted HERE is
   the other half of the layout contract: the kind values are contiguous from NONE upward, so a
   kind value is an index into the table's own array of arrays and XUD_HANDLE_KIND_COUNT is one
   past the highest kind in use. Adding a kind (AUDIOSTREAM when audio lands, or any caller-
   addressable kind that lands after these five) means adding one value and moving XUD_HANDLE_KIND_
   MAX with it; forgetting the second half is a compile error here rather than a slot array that
   silently has no row for the new kind. */
static_assert(XUD_HANDLE_KIND_COUNT == XUD_HANDLE_KIND_MAX + 1u,
    "xud_handle.h: kind values are contiguous from NONE, so the array count must be one past the highest kind - a new kind must move "
    "XUD_HANDLE_KIND_MAX as well as its own value");
static_assert(XUD_HANDLE_KIND_NONE == 0u,
    "xud_handle.h: NONE is the 0th value, which is what makes a zero-initialised handle carry the tag no live slot ever has");
static_assert(XUD_HANDLE_KIND_COUNT > XUD_HANDLE_KIND_NONE,
    "xud_handle.h: the table must have at least the NONE row, or the kind-indexed arrays below have no row to index");

/* The two bounds of one kind's slot array. This is NOT a provable bound - nothing about Vulkan
   says how many textures or meshes an application may hold - so it is this layer's own ceiling,
   and a caller that reaches it is refused XUD_OUT_OF_RESOURCES rather than quietly cut, because a
   cut slot array is an array whose later slots are unreachable and whose handles would then
   address the wrong record. Growing it is not an option here either: an unbounded array is an
   unbounded host allocation, and the repo's rule is that a refusal is reported, not hidden. The
   doubling shape is the one the rest of this tree uses (xud_instance.c, xud_physical.c,
   xud_swapchain.c's image query); the two asserts below are what makes that sequence land on the
   bound exactly instead of stepping past it and refusing a slot one row too early. */
#define XUD_HANDLE_SLOT_MIN_CAPACITY 16u
#define XUD_HANDLE_SLOT_MAX_CAPACITY 4096u

static_assert(XUD_HANDLE_SLOT_MIN_CAPACITY > XUD_HANDLE_KIND_NONE,
    "xud_handle.h: a kind's array must start with room for at least one slot, or the first allocation is a growth step and growth is not a "
    "refusal path");
static_assert(XUD_HANDLE_SLOT_MIN_CAPACITY <= XUD_HANDLE_SLOT_MAX_CAPACITY,
    "xud_handle.h: the doubling sequence must start at or below the bound it stops at");
static_assert(XUD_HANDLE_SLOT_MAX_CAPACITY % XUD_HANDLE_SLOT_MIN_CAPACITY == 0u,
    "xud_handle.h: the doubling sequence must reach the bound exactly, so the loop that grows it cannot step past it and refuse a slot one "
    "row too early");

/*-----------------------------------------------------------------------------------------------
 * What a slot records
 *-----------------------------------------------------------------------------------------------*/

/* The slot is the record of one object, and today it records only its own identity: `occupied` is
   the live bit, `generation` is what a handle must carry to name this slot at all, and `kind` is
   the tag that makes the check exact (a texture handle's (index, generation) pair could otherwise
   match a live model slot by coincidence - see the note in xud.h). `record` is the sentinel of
   the rung that is not in the tree yet: the object record - the Vulkan handles and the caps-gated
   variant info the decision says a record holds - cannot exist before the allocator and the
   renderer rungs do, so this field is NULL on every live slot for as long as this layer is the
   top of the tree, and xud_handle_table_close() frees a table without walking it because there is
   nothing in it to close yet. When the object steps land, this is the field they fill and the
   field their teardown rung has to close before the slot goes free. */
typedef struct XudHandleSlot {
    uint32_t occupied; /* 0 = free (never used, or released), 1 = live                                  */
    uint32_t generation; /* bumped when a released slot is taken again, so a handle kept across a release stops naming it */
    uint32_t kind; /* the tag this slot was last allocated for, XUD_HANDLE_KIND_NONE until it was ever used */
    void* record; /* the object record: NULL until the allocator and renderer rungs land, and the only Vulkan state a slot will ever own */
} XudHandleSlot;

typedef struct XudHandleTable XudHandleTable;
struct XudHandleTable {
    /* One array per kind, indexed by the kind value itself, so row 0 (NONE) exists and stays
       NULL forever: NONE is the tag of a handle that names nothing, and the validation below
       refuses it before it indexes anything. An array is NULL until the first allocation of
       that kind, which is the "no object of that kind was ever created in this context" state
       and is what keeps a context that draws only meshes from paying for a font array. */
    XudHandleSlot* slots[XUD_HANDLE_KIND_COUNT];
    uint32_t slotCount[XUD_HANDLE_KIND_COUNT]; /* the high mark of each array above: the first index never handed out yet */
    uint32_t slotCapacity[XUD_HANDLE_KIND_COUNT]; /* rows allocated for each array above, a step of the doubling sequence */
};

/*-----------------------------------------------------------------------------------------------
 * The layer's contract
 *-----------------------------------------------------------------------------------------------*/

/// Pure: bring one table up for one context. Every kind's array starts NULL (no allocation of
/// that kind yet), every count and capacity starts 0, and the table itself is the only
/// allocation this call makes. XUD_INVALID_ARG on a NULL out-pointer or one that already owns a
/// table (a second table for one context would be two owners of the same slots); XUD_OUT_OF_
/// MEMORY when the table cannot be allocated.
XudResult xud_handle_table_open(XudHandleTable** outTable);

/// Pure: the three-part validation every public entry point runs on a handle in hand, in the
/// order the decision fixes - index bounds against the array of the kind the entry point is
/// about, then generation equality, then tag equality - and XUD_INVALID_ARG on any failure. The
/// expected kind is the ENTRY POINT's kind, not the handle's: that is what makes a handle of the
/// wrong kind a caught error rather than a read of the wrong array, and it is why the tag is not
/// redundant with the array's identity. A handle whose generation matches a slot that is free
/// (one a caller fabricated, rather than one it kept across a release) is refused too, by the
/// live bit checked last: the three-part test is what makes a stale handle fail, and the live
/// bit is what makes a handle that is not stale but not live either fail, so a decoded handle is
/// always a handle that names a live slot. XUD_INVALID_ARG on a NULL table, on an expected kind
/// that is not a kind (NONE, or above the highest row of the table), on an index at or above the
/// high mark of that kind's array, on a kind's array that does not exist (nothing of that kind
/// was ever allocated, so no index of any kind names it), on a generation that does not match,
/// on a tag that does not match, or on a slot that is free.
XudResult xud_handle_validate(const XudHandleTable* table, XudHandle handle, uint32_t expectedKind);

/// Pure: field-wise handle equality - all three fields must match, so this is the full identity
/// of an object across kinds. index and generation alone are the identity WITHIN one kind, and
/// across kinds they are not enough: two kinds' slots can share an (index, generation) pair,
/// which is exactly why the tag exists (see the note in xud.h). Neither handle is validated here
/// - this compares two values, it does not ask whether either names a live slot, and that
/// question belongs to xud_handle_validate().
bool xud_handle_equal(XudHandle left, XudHandle right);

/// Pure: allocate one slot of one kind and fill the handle that names it. The index chosen is
/// the first free slot below the high mark when there is one - which is the reuse case, and the
/// generation of a reused slot is bumped, so every handle that named it before the release that
/// freed it stops naming it - and the high mark itself when there is not, which grows that kind's
/// array by the doubling sequence above when the high mark has reached the capacity. XUD_INVALID_
/// ARG on a NULL out-handle or table, or on a kind that is not one (NONE, or above the highest
/// row); XUD_OUT_OF_RESOURCES when the array is at XUD_HANDLE_SLOT_MAX_CAPACITY and a slot is
/// still wanted (this layer's own ceiling, see the note above); XUD_OUT_OF_MEMORY when a growth
/// step cannot allocate. On XUD_OK the handle carries the index, the generation and the kind, and
/// the slot it names is live; on any error *outHandle is left untouched, so a caller cannot read
/// a half-filled handle.
XudResult xud_handle_alloc(XudHandle* outHandle, XudHandleTable* table, uint32_t kind);

/// Pure: release one live slot. The slot is marked free and its generation is bumped in the same
/// step, so the handle in hand stops naming it and every handle that named it before stops naming
/// it too - the stale-handle rule read at the one place a slot goes free. The wrap case (2^32
/// reuses of one index, where a bumped generation can come back to the value an older handle
/// carries) is documented in xud.h rather than refused here: it is a state this layer's own bound
/// makes unreachable in practice long before it is a state worth a counter for. The tag and the
/// record are left as they were, so a free slot still says what it held last; the live bit is
/// what distinguishes it. XUD_INVALID_ARG on a NULL table, or on a handle that does not name a
/// live slot of its own kind (xud_handle_validate() against the handle's own tag is the check, so
/// a stale handle, a cross-kind handle and a fabricated one are all refused here rather than
/// double-releasing a slot or bumping a generation twice).
XudResult xud_handle_free(XudHandleTable* table, XudHandle handle);

/// Pure: how many live slots one kind has, which is what a caller above reads to decide whether
/// it is at a limit of its own before creating more. XUD_INVALID_ARG on a NULL table or out-count,
/// or on an expected kind that is not one; XUD_OK with 0 for a kind whose array was never
/// allocated (the "no object of that kind was ever created" state is a count of zero, not an
/// error).
XudResult xud_handle_table_live_count(const XudHandleTable* table, uint32_t kind, uint32_t* outLiveCount);

/// Pure: the teardown rung - free every kind's array that exists and the table itself, clear the
/// pointers and counts before freeing (the tree's rule: a table torn down twice cannot double-
/// free), and set the out-pointer to NULL. A slot that was live at this moment is retired with
/// the table: the objects its record will name belong to the rungs below this one, which are
/// torn down AFTER it, so freeing the table frees the records and there is nothing of the driver's
/// still held here. XUD_ALREADY on a pointer-to-pointer that is NULL-in-hand; XUD_INVALID_ARG on
/// a NULL pointer-to-pointer.
XudResult xud_handle_table_close(XudHandleTable** table);

#endif /* XUD_XUD_HANDLE_H_ */
