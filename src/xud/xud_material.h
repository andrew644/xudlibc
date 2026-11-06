/***********************************************************************************************
 *   xud_material.h - the material rung: the record a MATERIAL-kind slot of the handle layer
 *                    holds, the pair that gives one a context a material of, and the pair
 *                    that puts it in the drawing state for the frames that follow
 *
 *   Owns : XudMaterialRecord - the object record the handle layer's slot `record` field has
 *          been waiting for - plus the rungs that fill and clear that field (xud_material_open(),
 *          xud_material_close(), xud_material_get(), xud_material_close_all()) and the two rungs t
 *          hat decide a record's two members before it is filled (xud_material_resolve_variant(), t
 *          he row a mapped material draws through, and xud_material_check_map(), the map's validati
 *          on) and the public surface xud.h declares above them: xud_create_material()/xud_close_ma
 *          terial() for the object's lifetime and xud_begin_material()/xud_end_material() for the d
 *          rawing state.
 *   Layer: above the handle table - it is the FIRST rung that fills a slot's record field with
 *          anything other than the sentinel, which is why the handle layer's own note says every
 *          slot is still a sentinel and this file's note does not - and below xud.h. It owns one
 *          host allocation per material and nothing of the driver's; it borrows the handle table
 *          (the slot, and the TEXTURE-kind array its diffuse map is validated against - one table
 *          holds every kind's array, so naming a texture of the same context needs no second hand
 *          le and no reach into the texture rung), the batch (a material change is the batch's row
 *          split) and the renderer (the wanted-material state lives there, beside the camera state
 *          the frame loop re-applies every frame).
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
 *   draw of this material uses, which is live the moment a material is, and the diffuse map its draws
 *   sample, which is a handle and not an image. The set-1 half the record names through the map's own
 *   samplerSlot is the one per-material set the descriptor-layout rung allocates
 *   (XUD_DESCRIPTOR_SET_MATERIAL), and the texture step is what writes a map's view into its entry of that
 *   set's sampler array - which is what makes a map's slot the record's to give back at the close rather
 *   than a fact only the free list knows. The variant is the material's because in Vulkan the blend attachment state is PIPELINE state, not
 *   draw state: raylib's Material is { shader, maps[], params[] } (reference/raylib/raylib.h 388-3
 *   92) and none of those three is a blend mode - raylib's blend mode is a global its BeginBlendMo
 *   de()/EndBlendMode() pair scopes - and the two things a material owns in this library are theref
 *   ore the pipeline row its draws bind and the set-1 bindings its draws name. The shader half of r
 *   aylib's Material is not a member here at all: this tree has one shader pair (shaders/solid.vert
 *   + solid.frag, landed S16) and no second pipeline to name, so a per-material shader choice is a
 *   n open ask, not a field. The maps half is the handle below rather than raylib's ten-row array,
 *   because one map has a pipeline row that reads it (XUD_BATCH_VARIANT_TEXTURED's frag module samp
 *   les set 1) and nine do not; naming a map moves the variant to that row, which is what makes "a
 *   material with a map draws textured" a fact of the record rather than a rule every draw call ha
 *   s to remember. The set-1 field is still VK_NULL_HANDLE: naming a map is not binding one, and t
 *   he write of a mapped texture into set 1 is the step after this one - the same "placeholder with
 *   a named owner" shape the descriptor rung states for the block members no shader reads.
 *
 *   Why the map is a handle the material names rather than an object the material owns: a texture
 *   is one image a dozen materials can sample, and an image with two owners is an image closed twi
 *   ce. The texture rung's slot is the owner (its close destroys the image, the view and the sample
 *   r), so the material's close frees its own record and calls nothing of the map's, and the one da
 *   ngling state that ordering allows - a texture closed while a material still names it - is caugh
 *   t where it would otherwise do damage: xud_begin_material() re-validates the map against the TEX
 *   TURE array and refuses, so no frame is ever recorded with a textured row whose image is gone. T
 *   he variant the renderer and the batch hold is read off the record after that check, which is wh
 *   y the frame loop never has to re-read a map.
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
   through a driver, the map it names belongs to the texture rung's slot and not to this one, and t
   he set-1 half that eventually will have is VK_NULL_HANDLE until the step that writes a mapped te
   xture into a set lands, so xud_material_close() frees memory and calls nothing.

   The three members, in the order the two rungs above fill them: the row, the map that decides th
   e row, and the set that map will eventually be bound into. The variant is the RESOLVED one, so n
   o rung below the create has to ask whether this material has a map to know which pipeline row it
   binds. The map is named and not owned: the texture rung's close destroys the image, the view and
   the sampler, and this record keeps only the handle that says which one its draws sample. The set
   is the sentinel, because naming a map is not binding one.
*/
typedef struct XudMaterialRecord {
    uint32_t variant; /* XUD_BATCH_VARIANT_*, and the textured row whenever the map below names a texture */
    XudHandle diffuseMap; /* the TEXTURE slot this material's draws sample; XUD_HANDLE_KIND_NONE when it has no map */
    VkDescriptorSet setOne; /* VK_NULL_HANDLE until this material's own map is written into a set of its own */
} XudMaterialRecord;

/*-----------------------------------------------------------------------------------------------
 * The layer's contract
 *-----------------------------------------------------------------------------------------------*/

/// Pure: the row of the pipeline list a material draws through, which is the one place the rule "a
/// material with a map draws textured" lives. The map's row is not a flag beside the blend mode's
/// row - the variant axis IS the pipeline list's variant axis, and the row that samples set 1 is a
/// different frag module - so the rule has to be applied once, at the record, rather than at every
/// draw that reads it. A mapped material keeps nothing of its blend mode but its name: the textured
/// row's blend attachment state is the transparent row's (xud_pipeline.c's two rows are the same f
/// actor pair), so the override moves which module runs and not how the result is blended. XUD_INV
/// ALID_ARG on a NULL out-variant or a blend mode at or past the axis; XUD_OK with the resolved row
/// , which is the textured one for any non-zero hasMap and the blend mode itself otherwise.
XudResult xud_material_resolve_variant(uint32_t* outVariant, uint32_t blendMode, uint32_t hasMap);

/// Pure: the map half of a material description - XUD_HANDLE_KIND_NONE is the no-map state and nee
/// ds no array to be checked against (so a description a caller zero-initialised passes without a t
/// able existing), and any other handle is validated against the TEXTURE kind of the SAME table th
/// e material's own slot lives in, which is what makes a map from another context, a map released b
/// efore the material was even created and a handle of a third kind all refusals rather than a mate
/// rial that names someone else's image. This rung validates and takes ownership of nothing: the t
/// exture rung's slot owns the image either way. XUD_INVALID_ARG on a NULL table or on a handle tha
/// t does not name a live TEXTURE slot; XUD_OK on the no-map handle and on a live map.
XudResult xud_material_check_map(const XudHandleTable* table, XudHandle diffuseMap);

/// Pure: the set-1 slot a material's draws sample through - the map's own record member rather
/// than a choice at the draw: XUD_BATCH_NO_TEXTURE when the record names no map (the slot 0 the
/// frame loop's per-frame bind leaves set 1 at, the white default), the map's samplerSlot when it
/// names one. The map is read through the texture rung's accessor against the SAME table the
/// material's own slot lives in, which is the begin call's re-validation from the texture's side:
/// a map closed after the material was created refuses here rather than leaving a textured row
/// whose image is gone, and a live map that occupies no slot of the array refuses rather than
/// handing out an index that names nothing. XUD_INVALID_ARG on a NULL record or out-index, or on
/// a named map that names no live TEXTURE slot of this table; XUD_OK with *outTextureIndex written
/// in either shape.
XudResult xud_material_resolve_texture(const XudHandleTable* table, const XudMaterialRecord* record, uint32_t* outTextureIndex);

/// Pure: allocate one MATERIAL-kind slot of this table AND the record that fills it, in the one
/// order that leaves nothing behind: the map validated first (so a description that names no textu
/// re spends nothing), then the slot (so the record has a slot to be found in), then the record, a
/// nd a record that cannot be allocated is freed alongside the slot it was about to fill before eit
/// her is left reachable. The record's variant is the resolved one rather than the blend mode as gi
/// ven: a material created with a map is a material of the textured row from the moment it exists, s
/// o the draws it is begun around and the model primitives that carry it read one number off the re
/// cord and cannot disagree about the row. On XUD_OK *outHandle names a live slot whose record is n
/// o longer the sentinel for the first time in this tree. The record itself is not handed out - a s
/// econd pointer to it would be a second owner - it is read back by the accessor below. XUD_INVALID
/// _ARG on a NULL out-handle or table, on a blend mode at or past the axis, or on a diffuse map tha
/// t names no live TEXTURE slot of this table; the handle layer's own XUD_OUT_OF_RESOURCES at its s
/// lot bound and XUD_OUT_OF_MEMORY when either allocation fails.
XudResult xud_material_open(XudHandle* outHandle, XudHandleTable* table, uint32_t blendMode, XudHandle diffuseMap);

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
