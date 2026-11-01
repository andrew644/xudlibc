/***********************************************************************************************
 *   xud_material.c - the material rung: the record behind a MATERIAL-kind slot of the handle
 *                    layer, and the public create/close pair and the begin/end state pair
 *                    xud.h declares above it
 *
 *   Owns : the four rungs xud_material.h declares (xud_material_open(), xud_material_close(),
 *          xud_material_get(), xud_material_close_all()) and the four public calls xud.h
 *          declares above them (xud_create_material(), xud_close_material(), xud_begin_
 *          material(), xud_end_material()). The split is the point of the file: everything
 *          here is host-side - one malloc per material, freed at the slot's release and at the
 *          table's teardown - so the whole rung is checkable with no driver present, and the
 *          only GPU-adjacent thing a material does is name a row of the pipeline list, which
 *          it does by MOVING the batch's state through the batch's own row split rather than
 *          by touching a pipeline.
 *
 *   What the rung owns and what it borrows: it owns the record allocation and the rule about
 *   which of its two teardown paths frees it (xud_material_close() at a slot release, xud_
 *   material_close_all() at the table's teardown - and exactly one of the two ever runs per
 *   record, because close() clears the slot's field before it frees). It borrows the handle
 *   table (the slot, and the record field through the handle layer's own two accessors), the
 *   batch (xud_batch_set_material(), the row split) and the renderer (the wanted-material
 *   state, which lives there because the renderer is the owner of the frame loop and the
 *   state has to be re-applied at every frame opening, beside the camera state).
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud_material.h" /* Required for: XudMaterialRecord and the four rungs this file answers */
#include "xud.h" /* Required for: XudResult, XudHandle, XudMaterial, XUD_BLEND_*, XUD_HANDLE_KIND_MATERIAL - the public surface above */
#include "xud_batch.h" /* Required for: XUD_BATCH_VARIANT_*, XUD_BATCH_NO_MATERIAL, xud_batch_set_material() - the batch's variant axis and the row split a material change performs */
#include "xud_core.h" /* Required for: struct XudContext, whose handleTable and renderer the public calls reach */
#include "xud_handle.h" /* Required for: XudHandleTable, xud_handle_validate/free, xud_handle_record_get/set - the slot this rung's record lives in */
#include "xud_renderer.h" /* Required for: XudRenderer, the record the wanted-material state lives in */
#include <assert.h> /* Required for: static_assert, the public token mirror below */
#include <stdlib.h> /* Required for: malloc, free - the one allocation a material spends */

/*-----------------------------------------------------------------------------------------------
 * The public token mirror, and why it is an assert rather than a translation
 *-----------------------------------------------------------------------------------------------*/

/* XUD_BLEND_* are xud.h's tokens and XUD_BATCH_VARIANT_* are the batch's axis; the record
   carries the second, so a material created with the first has to draw through the row the
   second names. xud_material.h asserts the pairs equal; this is the count half, stated where
   the public value type is actually read. */
static_assert(XUD_BLEND_OPAQUE == XUD_BATCH_VARIANT_OPAQUE, "xud_material.c: the public opaque token must name the batch's opaque variant");
static_assert(XUD_BLEND_TRANSPARENT == XUD_BATCH_VARIANT_TRANSPARENT,
    "xud_material.c: the public transparent token must name the batch's transparent variant");
static_assert(
    XUD_BLEND_COUNT == XUD_BATCH_VARIANT_COUNT, "xud_material.c: the two axes must stay the same length, so growing one grows the other");

/*-----------------------------------------------------------------------------------------------
 * The record's lifetime - the three rungs that fill, read and clear one slot's record field
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_material_open(XudHandle* outHandle, XudHandleTable* table, uint32_t variant) {
    XudMaterialRecord* record = NULL;
    XudResult result = XUD_INVALID_ARG;

    if ((outHandle == NULL) || (table == NULL)) {
        return XUD_INVALID_ARG;
    }
    if (variant >= XUD_BATCH_VARIANT_COUNT) {
        /* The variant is the row of the pipeline list this material will ask the frame loop to
           bind, so an out-of-range variant is not a value the row split can name - refused
           before a slot is spent, so a refusal leaves no half-material behind.
        */
        return XUD_INVALID_ARG;
    }

    /* The slot first, then the record: the order is the one that leaves nothing reachable and
       unfilled. A slot allocated with no record is a slot whose record field is still the
       sentinel, which is a state the handle layer already documents as "nothing of that kind
       created yet", and it is only transient here - the rollback below frees the slot the
       record could not fill. The handle layer's own answers travel unchanged, because XUD_OUT_
       OF_RESOURCES (its slot bound) and XUD_OUT_OF_MEMORY (a growth step that failed) are
       different reasons and a caller above reads them differently.
    */
    result = xud_handle_alloc(outHandle, table, XUD_HANDLE_KIND_MATERIAL);
    if (result != XUD_OK) {
        return result;
    }

    record = malloc(sizeof(XudMaterialRecord));
    if (record == NULL) {
        (void)xud_handle_free(table, *outHandle);
        return XUD_OUT_OF_MEMORY;
    }

    record->variant = variant;
    record->setOne = VK_NULL_HANDLE;

    /* The set is written through the handle layer's own accessor rather than by indexing the
       slot array from here: the array is the handle layer's, it reallocs it as a unit, and the
       accessor is what bounds-checks (kind, index) against the array that exists. A set that
       refused means the slot this record was about to fill is not addressable, so both the
       record and the slot go back.
    */
    if (xud_handle_record_set(table, XUD_HANDLE_KIND_MATERIAL, outHandle->index, record) != XUD_OK) {
        free(record);
        (void)xud_handle_free(table, *outHandle);
        return XUD_INVALID_ARG;
    }

    return XUD_OK;
}

XudResult xud_material_get(const XudHandleTable* table, XudHandle handle, const XudMaterialRecord** outRecord) {
    void* record = NULL;

    if ((table == NULL) || (outRecord == NULL)) {
        return XUD_INVALID_ARG;
    }

    /* The three-part validation, against the MATERIAL kind this rung is about rather than
       against the handle's own tag: that is what makes a texture handle handed to a material
       call a refusal rather than a read of the wrong array (AGENTS.md's rule that the tag is
       what turns a cross-kind mix-up into a caught runtime error).
    */
    if (xud_handle_validate(table, handle, XUD_HANDLE_KIND_MATERIAL) != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    if (xud_handle_record_get(table, XUD_HANDLE_KIND_MATERIAL, handle.index, &record) != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    if (record == NULL) {
        /* A live MATERIAL slot whose record field is the sentinel is a slot this rung did not
           allocate - something above filled the slot through the handle layer directly. It is
           reported rather than handed back as a NULL record, because every caller of this
           accessor reads the record it points at.
        */
        return XUD_INVALID_ARG;
    }

    *outRecord = (const XudMaterialRecord*)record;
    return XUD_OK;
}

XudResult xud_material_close(XudHandleTable* table, XudHandle handle) {
    void* record = NULL;

    if (table == NULL) {
        return XUD_INVALID_ARG;
    }

    /* Validated against the MATERIAL kind, not against the handle's own tag: releasing is only
       ever about the array this rung's records live in, so a handle of another kind is refused
       rather than freeing a record of that other kind. xud_handle_free() re-validates below
       against the handle's own tag, which is the same test seen from the other side - a stale
       handle, a cross-kind handle and a fabricated one refuse here before any free happens.
    */
    if (xud_handle_validate(table, handle, XUD_HANDLE_KIND_MATERIAL) != XUD_OK) {
        return XUD_INVALID_ARG;
    }
    if (xud_handle_record_get(table, XUD_HANDLE_KIND_MATERIAL, handle.index, &record) != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    /* Cleared BEFORE the free, and before xud_handle_free() marks the slot free: the handle
       layer's own note says a released slot "leaves the record as it was", so a free slot can
       otherwise still own a heap record - and then two teardown paths (this one and the
       table's) would both free it. Clearing first makes the record owned by exactly one path
       at every moment, and makes a close run twice a close that frees once.
    */
    if (record != NULL) {
        if (xud_handle_record_set(table, XUD_HANDLE_KIND_MATERIAL, handle.index, NULL) != XUD_OK) {
            return XUD_INVALID_ARG;
        }
        free(record);
    }

    return xud_handle_free(table, handle);
}

/*-----------------------------------------------------------------------------------------------
 * The teardown rung above the handle table's own close
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_material_close_all(XudHandleTable* table, uint32_t* outClosed) {
    void* record = NULL;
    uint32_t index = 0u;

    if ((table == NULL) || (outClosed == NULL)) {
        return XUD_INVALID_ARG;
    }
    *outClosed = 0u;
    if (table->slots[XUD_HANDLE_KIND_MATERIAL] == NULL) {
        /* No MATERIAL array was ever allocated: nothing of this kind was ever created, which is
           a count of zero and not an error - the same answer xud_handle_table_live_count()
           gives for the same reason.
        */
        return XUD_OK;
    }

    /* Every row of the array is walked, live or free: xud_handle_free() leaves the record field
       as it was, so a slot a caller released still owns its record until this rung clears it,
       and a record left behind is a leak the table's own close cannot see (it frees the array,
       not what the array points at). The walk is bounded by the array's own high mark rather
       than by its capacity, so the rows past the last slot ever handed out - which have never
       held a record - are not read.
    */
    for (index = 0u; index < table->slotCount[XUD_HANDLE_KIND_MATERIAL]; index++) {
        if (xud_handle_record_get(table, XUD_HANDLE_KIND_MATERIAL, index, &record) != XUD_OK) {
            return XUD_INVALID_ARG;
        }
        if (record != NULL) {
            if (xud_handle_record_set(table, XUD_HANDLE_KIND_MATERIAL, index, NULL) != XUD_OK) {
                return XUD_INVALID_ARG;
            }
            free(record);
            (*outClosed)++;
        }
    }

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The public pair above the context - the object's lifetime
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_create_material(XudHandle* outHandle, XudContext* context, const XudMaterial material) {
    if ((outHandle == NULL) || (context == NULL)) {
        return XUD_INVALID_ARG;
    }
    if (context->handleTable == NULL) {
        /* A context with no handle table is a context that never completed bring-up: xud_init_
           context() hands back NULL for that case, so this is a caller holding on rather than
           a state the library leaves behind - the same answer xud_begin_drawing() gives for
           the same reason.
        */
        return XUD_UNSUPPORTED;
    }

    /* Validated before anything is spent: a blend mode outside the axis is a bad argument from
       the caller, and the answer is the same one every other public entry point gives for it.
       The token is translated here and nowhere else - the record carries the batch's variant,
       and this is the one place the public value type is read.
    */
    if (material.blendMode >= XUD_BLEND_COUNT) {
        return XUD_INVALID_ARG;
    }

    return xud_material_open(outHandle, context->handleTable, material.blendMode);
}

XudResult xud_close_material(XudContext* context, XudHandle handle) {
    XudRenderer* renderer = NULL;
    XudResult result = XUD_INVALID_ARG;

    if (context == NULL) {
        return XUD_INVALID_ARG;
    }
    if (context->handleTable == NULL) {
        return XUD_UNSUPPORTED;
    }

    /* Validated before anything above is touched: a refused release must be a no-op, and the
       renderer clear below is part of that release, so a handle that does not name a live MAT
       ERIAL slot of this context leaves the drawing state alone as well as freeing nothing. The
       test is the one xud_material_close() runs again against the same kind, stated here so th
       e order of the two halves (clear the state, then free the record) is guarded by it.
    */
    if (xud_handle_validate(context->handleTable, handle, XUD_HANDLE_KIND_MATERIAL) != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    /* The state is cleared only when the renderer names THIS handle (xud_handle_equal() is the
       all-three-field form, so a handle that was released and taken again - a different genera
       tion - does not clear a state that belongs to the newer material). The order matters: a r
       ecord freed while the renderer still says "draw with this material" is a next frame that r
       e-applies a slot index whose record no longer exists, which is a row naming a dead slot r
       ather than a frame drawn with the default. This is the camera rung's rule read at the mat
       erial: a state that began and ended inside one frame is a no-op, not a leak into the next
       frame.
    */
    renderer = context->renderer;
    if ((renderer != NULL) && (renderer->materialWanted != 0u) && (xud_handle_equal(renderer->material, handle) == true)) {
        renderer->materialWanted = 0u;
        renderer->material.index = 0u;
        renderer->material.generation = 0u;
        renderer->material.kind = XUD_HANDLE_KIND_NONE;
        renderer->materialVariant = XUD_BATCH_VARIANT_OPAQUE;
        if (renderer->batch != NULL) {
            (void)xud_batch_set_material(renderer->batch, XUD_BATCH_NO_MATERIAL, XUD_BATCH_VARIANT_OPAQUE);
        }
    }

    result = xud_material_close(context->handleTable, handle);
    if (result != XUD_OK) {
        /* A refused release means the handle did not name a live MATERIAL slot, so nothing was
           freed and nothing above was cleared - the state is unchanged on every refusal path
           here, which is what makes a refused close a no-op rather than a half-close.
        */
        return result;
    }

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The public pair above the context - the drawing state
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_begin_material(XudContext* context, XudHandle handle) {
    const XudMaterialRecord* record = NULL;
    XudRenderer* renderer = NULL;
    XudResult result = XUD_INVALID_ARG;

    if (context == NULL) {
        return XUD_INVALID_ARG;
    }
    if ((context->handleTable == NULL) || (context->renderer == NULL) || (context->renderer->batch == NULL)) {
        return XUD_UNSUPPORTED;
    }
    renderer = context->renderer;

    /* Validated at the call that asks, which is the half the pair buys against raylib's global
       state: the record is read here, once, and what is stored in the renderer is a COPY of
       the two numbers the batch's state move needs - the handle by value and the variant off
       the record - rather than a pointer into the table. That is the camera pair's rule read
       at the material: the owner of the frame loop holds the state, and a caller's local going
       out of scope at the end of a frame cannot leave the next frame reading a dead record.
    */
    result = xud_material_get(context->handleTable, handle, &record);
    if (result != XUD_OK) {
        return result;
    }

    renderer->material = handle;
    renderer->materialVariant = record->variant;
    renderer->materialWanted = 1u;

    /* The state moves at the call, not only at the next frame's opening: raylib's Begin/End
       pair is a scoped state, so the draws that follow in the SAME frame use the material, and
       a begin that only took effect next frame would be a begin whose first draws were drawn
       with the previous state. The move is the batch's own row split, so a begin mid-frame
       closes the run that was pending under the previous state into a row of its own and
       starts a new run - which is what makes a frame that mixes two materials more than one
       row, the fact the frame loop's multi-row replay was already built for.
    */
    return xud_batch_set_material(renderer->batch, handle.index, record->variant);
}

XudResult xud_end_material(XudContext* context) {
    XudRenderer* renderer = NULL;

    if (context == NULL) {
        return XUD_INVALID_ARG;
    }
    if ((context->handleTable == NULL) || (context->renderer == NULL) || (context->renderer->batch == NULL)) {
        return XUD_UNSUPPORTED;
    }
    renderer = context->renderer;

    if (renderer->materialWanted == 0u) {
        /* The state is already the one this call asks for - the same answer xud_end_camera_3d()
           gives for the same reason.
        */
        return XUD_ALREADY;
    }

    renderer->materialWanted = 0u;
    renderer->material.index = 0u;
    renderer->material.generation = 0u;
    renderer->material.kind = XUD_HANDLE_KIND_NONE;
    renderer->materialVariant = XUD_BATCH_VARIANT_OPAQUE;

    /* The batch moves back to the default HERE rather than at the next frame's reset, which is
       raylib's EndBlendMode() read literally ("reset to default: alpha blending", raylib.h
       1059): the draws that follow in this frame are drawn with no material, and a frame that
       began and ended a material inside itself records the runs either side of the End at
       their own rows rather than one row at whatever the begin moved in.
    */
    return xud_batch_set_material(renderer->batch, XUD_BATCH_NO_MATERIAL, XUD_BATCH_VARIANT_OPAQUE);
}
