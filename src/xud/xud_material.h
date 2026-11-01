/***********************************************************************************************
 *   xud_material.h - the material rung: the record a MATERIAL-kind slot of the handle layer
 *                    holds, the pair that gives one a context a material of, and the pair
 *                    that puts it in the drawing state for the frames that follow
 *
 *   Owns : XudMaterialRecord - the object record the handle layer's slot `record` field has
 *          been waiting for - plus the rungs that fill and clear that field (xud_material_open(),
 *          xud_material_close(), xud_material_get(), xud_material_close_all()) and the public
 *          surface xud.h declares above them: xud_create_material()/xud_close_material() for the
 *          object's lifetime and xud_begin_material()/xud_end_material() for the drawing state.
 *   Layer: above the handle table - it is the FIRST rung that fills a slot's record field with
 *          anything other than the sentinel, which is why the handle layer's own note says every
 *          slot is still a sentinel and this file's note does not - and below xud.h. It owns one
 *          host allocation per material and nothing of the driver's; it borrows the handle table
 *          (the slot), the batch (a material change is the batch's row split) and the renderer
 *          (the wanted-material state lives there, beside the camera state the frame loop
 *          re-applies every frame).
 *
 *   Why a material is a handle plus a state call rather than an argument on every draw: raylib
 *   names the material at the draw (BeginModelFrom takes the Material at the draw call) and holds
 *   it in a global, which is the shape that cannot validate at the call and cannot serve two
 *   contexts. The repo's currency is the handle (AGENTS.md: opaque { index, generation, kind }
 *   validated against a slot array, "the public API hands out this currency"), so the caller
 *   holds an XudHandle of the MATERIAL kind and the state call that puts it into the drawing
 *   state is the Begin/End shape xud.h already uses for the camera pair and raylib itself uses
 *   for its blend state (BeginBlendMode()/EndBlendMode(), raylib.h 1058-1059, whose End half is
 *   "reset to default: alpha blending"). The pair is the one raylib spelling that keeps a per-
 *   context state: raylib's blend state is a global its End call resets, and the reset-on-End
 *   half is what a frame's later draws read - which is why xud_end_material() moves the batch
 *   back to the default rather than waiting for the next frame's reset.
 *
 *   What the record holds, and why that is the whole of it today: the pipeline variant every
 *   draw of this material uses, which is live the moment a material is, and the set-1 half -
 *   the per-material VkDescriptorSet the descriptor-layout rung declares (XUD_DESCRIPTOR_SET_
 *   MATERIAL) and never allocates. The variant is the material's because in Vulkan the blend
 *   attachment state is PIPELINE state, not draw state: raylib's Material is { shader, maps[],
 *   params[] } (reference/raylib/raylib.h 388-392) and none of those three is a blend mode -
 *   raylib's blend mode is a global its BeginBlendMode()/EndBlendMode() pair scopes - and the
 *   two things a material owns in this library are therefore the pipeline row its draws bind
 *   and the set-1 bindings its draws name. The shader half of raylib's Material is not a member
 *   here at all: this tree has one shader pair (shaders/solid.vert + solid.frag, landed S16) and
 *   no second pipeline to name, so a per-material shader choice is an open ask, not a field.
 *   The maps half is the set-1 field below, and it is VK_NULL_HANDLE for as long as the texture
 *   step has not landed - the same "placeholder with a named owner" shape the descriptor rung
 *   states for the block members no shader reads.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#ifndef XUD_XUD_MATERIAL_H_
#define XUD_XUD_MATERIAL_H_

#include "volk.h" /* Required for: VK_NO_PROTOTYPES - must precede <vulkan/vulkan.h>, see xud_loader.h */
#include "xud.h" /* Required for: XudResult, XudHandle, XudMaterial, XUD_BLEND_* and XUD_HANDLE_KIND_MATERIAL - the public surface this rung is the half of */
#include "xud_batch.h" /* Required for: XUD_BATCH_VARIANT_*, XUD_BATCH_NO_MATERIAL - the batch's variant axis, which is what a material selects */
#include "xud_handle.h" /* Required for: XudHandleTable, XudHandleSlot, xud_handle_validate/alloc/free - the slot this rung's record lives in */
#include <vulkan/vulkan.h> /* Required for: VkDescriptorSet, VK_NULL_HANDLE - the set-1 half of the record, declared and not yet allocated */

#include <assert.h> /* Required for: static_assert, the mirror of the public tokens against the batch's axis */
#include <stdint.h>

/*-----------------------------------------------------------------------------------------------
 * The public tokens, mirrored against the batch's variant axis
 *-----------------------------------------------------------------------------------------------*/

/* The mirror is the whole point of these asserts: xud.h's XUD_BLEND_* are the tokens a caller
   writes, XUD_BATCH_VARIANT_* are the rows of the pipeline list the frame loop binds, and the
   material record carries the second and not the first. A mirror rather than a translation
   table, because the two axes have exactly the same members today - and the assert is what
   makes "exactly the same" a compile-time fact rather than a comment someone trusts. Growing
   the axis (a third variant, a second blend factor pair) moves both counts and the assert
   refuses a half-moved mirror. */
static_assert(XUD_BLEND_OPAQUE == XUD_BATCH_VARIANT_OPAQUE,
    "xud_material.h: the public opaque token must name the batch's opaque variant, or a material created with one draws through the other");
static_assert(XUD_BLEND_TRANSPARENT == XUD_BATCH_VARIANT_TRANSPARENT,
    "xud_material.h: the public transparent token must name the batch's transparent variant, or a material created with one draws through "
    "the other");
static_assert(XUD_BLEND_COUNT == XUD_BATCH_VARIANT_COUNT,
    "xud_material.h: the two axes must stay the same length, so growing one grows the other and a half-grown mirror is a compile error");

/*-----------------------------------------------------------------------------------------------
 * What a MATERIAL-kind slot records
 *-----------------------------------------------------------------------------------------------*/

/* The record is heap-allocated by xud_material_open() and stored in the slot's `record` field,
   which is the one place the library owns an object of a caller-addressable kind. It is a
   separate allocation rather than a field of the slot because the slot is a fixed-size row of
   an array the handle layer reallocs and frees as a unit, while the record is per-object state
   with a lifetime of its own - the handle layer's own note says the field "is the field they
   fill and the field their teardown rung has to close before the slot goes free", and this
   file is that teardown rung. Freeing a record is host-only: nothing in it has been created
   through a driver, and the set-1 half that eventually will have is VK_NULL_HANDLE until the
   texture step lands, so xud_material_close() frees memory and calls nothing.
*/
typedef struct XudMaterialRecord {
    uint32_t variant; /* which row of the pipeline list every draw of this material binds - XUD_BATCH_VARIANT_* */
    VkDescriptorSet setOne; /* this material's set 1: VK_NULL_HANDLE until the texture step allocates one, and the only Vulkan handle a slot
                               will ever own of its own */
} XudMaterialRecord;

/*-----------------------------------------------------------------------------------------------
 * The layer's contract
 *-----------------------------------------------------------------------------------------------*/

/// Pure: allocate one MATERIAL-kind slot of this table AND the record that fills it, in the one
/// order that leaves nothing behind: the slot first (so the record has a slot to be found in),
/// then the record, and a record that cannot be allocated is freed alongside the slot it was
/// about to fill before either is left reachable. On XUD_OK *outHandle names a live slot whose
/// record is no longer the sentinel for the first time in this tree. The record itself is not
/// handed out - a second pointer to it would be a second owner - it is read back by the access
/// or below. XUD_INVALID_ARG on a NULL out-handle or table, or on a
/// variant at or past the variant list; the handle layer's own XUD_OUT_OF_RESOURCES at its slot
/// bound and XUD_OUT_OF_MEMORY when either allocation fails.
XudResult xud_material_open(XudHandle* outHandle, XudHandleTable* table, uint32_t variant);

/// Pure: release one live MATERIAL-kind slot and free the record it names - the record is
/// cleared from the slot BEFORE the slot is marked free, so a slot that is free and a slot
/// that still points at freed memory are never both true at once, and a table closed twice
/// cannot double-free a record. The handle in hand is validated against the table first, so a
/// stale handle, a handle of another kind and a fabricated one refuse here rather than freeing
/// someone else's record. XUD_INVALID_ARG on a NULL table or on a handle that does not name a
/// live MATERIAL slot; XUD_OK otherwise.
XudResult xud_material_close(XudHandleTable* table, XudHandle handle);

/// Pure: the accessor one rung below reaches instead of the slot array - validate, then hand
/// back the record of a live MATERIAL slot. XUD_INVALID_ARG on a NULL out-record or on a handle
/// that does not name a live MATERIAL slot; XUD_OK with *outRecord pointing at the record the
/// slot names (never a copy: the record is the object, and a copy of it would be a second
/// answer to "what this material draws as").
XudResult xud_material_get(const XudHandleTable* table, XudHandle handle, const XudMaterialRecord** outRecord);

/// Pure: the teardown rung above the handle table's own close - free every record any MATERIAL
/// slot still names (live or released: xud_handle_free() leaves the record field as it was, so
/// a free slot can still own a record) and clear the field, so xud_handle_table_close() can
/// free the arrays without leaking a record behind them. This is the reason the handle layer's
/// close note says the records are closed BEFORE the table goes: this rung is the only one that
/// knows what a record is. *outClosed counts the records freed, which is the number a caller abo
/// ve can compare against the table's own counts - a count short of the array's high mark is a r
/// ecord this rung did not reach. XUD_INVALID_ARG on a NULL table or out-count; XUD_OK with 0 on
/// a table with no MATERIAL array (nothing was ever created, which is a count of zero and not an
/// error), and the rungs are idempotent because every record pointer is cleared here.
XudResult xud_material_close_all(XudHandleTable* table, uint32_t* outClosed);

#endif /* XUD_XUD_MATERIAL_H_ */
