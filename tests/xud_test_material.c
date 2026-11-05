/***********************************************************************************************
 *
 *   xud_test_material.c - the material rung and the public material API above it
 *
 *   Owns : the object pair xud_create_material()/xud_close_material(), the scoped state pair xud
 *          _begin_material()/xud_end_material(), the record the MATERIAL kind's slots carry, the
 *          handle-table record accessors as the rung uses them, and the teardown rung xud_close_c
 *          ontext() runs above the handle table it closes - and nothing else: the row split a mate
 *          rial change owes is the batch area's (it checks xud_batch_set_material() there beside
 *          the topology axis it shares a mechanism with), the handle layer's own validation order
 *          is the handle area's, and the frame loop that binds a row's variant is the frame area's.
 *
 *   Layer: tests, above the handle and batch areas and below the core area. The pure half runs with
 *   no driver present - a handle table is one host allocation and the material record is one more,
 *   so the whole state machine, the validation of a handle and the teardown order are answerable wi
 *   th no device in sight - and the GPU half runs the same calls against a real context, which is wh
 *   ere the state the frame loop records a row FROM and the re-apply at a frame's opening are check
 *   ed.
 *
 *   What the checks are stated against: the record's three members and the two pure rungs that dec
 *   ide them, rather than a descriptor set, because naming a map is not binding one - the set-1 fiel
 *   d's owner is named and its value is the sentinel, which is the one state that is honest about an
 *   empty half. A material's effect on a frame is checked as the batch's state mid-frame and as the
 *   re-apply at the next frame's opening rather than as a pixel, because the row the frame loop reco
 *   rds is retired inside the frame it recorded it in and a pixel would be a check of the rasteriser
 *   .
 *
 *   Deterministic: fixed materials, fixed loop counts, no time, no randomness, no filesystem.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include <stdlib.h> /* Required for: calloc and free, the synthetic batch's row array this area allocates and frees */
#include <string.h> /* Required for: memset, the context and renderer shells this area builds itself */

#include "xud.h" /* Required for: XudResult, XudContext, XudMaterial, the XUD_BLEND_* tokens and the public material API */
#include "xud_batch.h" /* Required for: XudBatch, the batch's variant/material state the scoped pair moves */
#include "xud_core.h" /* Required for: struct XudContext, whose handleTable and renderer fields the shells fill */
#include "xud_handle.h" /* Required for: XudHandleTable, XudHandle and the MATERIAL kind's row */
#include "xud_material.h" /* Required for: XudMaterialRecord and the rung's own open/close/get/close_all */
#include "xud_renderer.h" /* Required for: XudRenderer, the record the scoped pair stores a material into */
#include "xud_test_harness.h" /* Required for: xud_check, xud_gpu_tests_enabled, the area entry points */

/*-----------------------------------------------------------------------------------------------
 * The material rung: the object a MATERIAL slot records, and the two public pairs above it. The
 * rung is host-only - one slot, one record, one state move - so the headless run covers it compl
 * etely, and the GPU run covers the wiring (the row the frame loop records, the teardown inside a
 * real context) rather than a second copy of the same facts.
 *-----------------------------------------------------------------------------------------------*/

/* The one description every "the material is fine" check below is the same material seen through a
   different refusal: the transparent variant, which is the row of the pipeline list whose blend
   attachment is enabled, and the one the opaque default is not.
*/
#define XUD_TEST_MATERIAL_TRANSPARENT                                                                                                      \
    (XudMaterial) { .blendMode = XUD_BLEND_TRANSPARENT }
#define XUD_TEST_MATERIAL_OPAQUE                                                                                                           \
    (XudMaterial) { .blendMode = XUD_BLEND_OPAQUE }

/*-----------------------------------------------------------------------------------------------
 * The pure half: the two pairs, the record and the teardown order, against shells
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_material_pure(void) {
    XudContext shell;
    XudRenderer renderer;
    XudBatch batch;
    XudContext empty;
    XudHandleTable* table = NULL;
    XudHandle handle = { 0u, 0u, 0u };
    XudHandle second = { 0u, 0u, 0u };
    XudHandle kept = { 0u, 0u, 0u };
    XudHandle foreign = { 0u, 0u, 0u };
    XudHandle map = { 0u, 0u, 0u };
    XudHandle mapped = { 0u, 0u, 0u };
    XudMaterialRecord const* record = NULL;
    XudMaterial description;
    XudMaterial broken;
    uint32_t variant = 0u;
    uint32_t closed = 0u;

    memset(&shell, 0, sizeof(shell));
    memset(&renderer, 0, sizeof(renderer));
    memset(&batch, 0, sizeof(batch));
    memset(&empty, 0, sizeof(empty));

    /* The shells the public pairs run against: a real handle table (xud_handle_table_open is one
       host allocation and calls no driver function, which is what makes it usable here) and a syn
       thetic batch at the state xud_batch_open opens one at - no pending run, TRIANGLES, opaque,
       no material. The batch's streams and buffers are left NULL because the scoped pair never to
       uches them: a material move reads the batch's state and writes the batch's state, and the on
       ly allocation it can do is a row in the row array, which the record call grows on demand.
    */
    batch.currentTopology = XUD_BATCH_TOPOLOGY_TRIANGLES;
    batch.currentVariant = XUD_BATCH_VARIANT_OPAQUE;
    batch.currentMaterial = XUD_BATCH_NO_MATERIAL;
    renderer.batch = &batch;
    shell.renderer = &renderer;

    xud_check(xud_handle_table_open(&table) == XUD_OK, "material/table-open", "the shell's table is a real one");
    shell.handleTable = table;

    xud_check(xud_begin_material(&empty, handle) == XUD_UNSUPPORTED, "material/begin-no-rungs-below",
        "and a context that owns no handle table and no renderer is refused as UNSUPPORTED rather th"
        "an INVALID_ARG, which is the split between a missing rung and a bad handle");
    xud_check(xud_end_material(&empty) == XUD_UNSUPPORTED, "material/end-no-rungs-below",
        "and the end pair splits it the same way, so the two pairs cannot disagree about what a cont"
        "ext with no renderer means");

    /*--- the refusals, and what each one leaves unspent ----------------------------------*/

    xud_check(xud_create_material(NULL, &shell, XUD_TEST_MATERIAL_TRANSPARENT) == XUD_INVALID_ARG, "material/create-null-out",
        "a handle with nowhere to be written is refused");
    xud_check(xud_create_material(&handle, NULL, XUD_TEST_MATERIAL_TRANSPARENT) == XUD_INVALID_ARG, "material/create-null-context",
        "a material with no context to own it is refused");
    xud_check(xud_create_material(&handle, &shell, (XudMaterial) { .blendMode = XUD_BLEND_COUNT }) == XUD_INVALID_ARG,
        "material/create-past-blend",
        "a blend mode at or past the axis is refused BEFORE anything is spent, so a bad request cann"
        "ot leave a slot that names a row of the pipeline list that does not exist");
    xud_check(xud_create_material(&handle, &shell, (XudMaterial) { .blendMode = XUD_BLEND_COUNT + 7u }) == XUD_INVALID_ARG,
        "material/create-past-blend-again", "and the check is a bound rather than a switch, so any past value refuses the same way");
    description = XUD_TEST_MATERIAL_OPAQUE;
    broken = description;
    broken.blendMode = 0xFFFFFFFFu;
    xud_check(xud_create_material(&handle, &shell, broken) == XUD_INVALID_ARG, "material/create-blend-max",
        "and the widest value is refused by the same test, which is why the member is unsigned and n"
        "ot a signed comparison against a bound");
    xud_check(handle.kind == 0u, "material/create-refusal-left-handle",
        "the out-handle is untouched on every refusal above, so a caller cannot read a half-filled h"
        "andle out of a refused create");

    /* A context that never completed bring-up has no table, and the answer is UNSUPPORTED rather
       than INVALID_ARG: the context is a real record and the rung it needs is the one that is mis
       sing, which is the same split the camera pair's public calls state.
    */
    xud_check(xud_begin_material(&shell, handle) == XUD_INVALID_ARG, "material/begin-uninitialised-handle",
        "a handle that names no kind of anything is refused as INVALID_ARG, which is the tag check r"
        "ather than a state query - the table and the batch are both in place at this point");
    xud_check(renderer.materialWanted == 0u, "material/begin-refusal-stored-nothing",
        "and the refusal stored nothing, so the next frame is drawn with no material rather than wit"
        "h a handle that was never validated");

    /*--- the object pair, and the record behind it --------------------------------------*/

    description = XUD_TEST_MATERIAL_TRANSPARENT;
    xud_check(xud_create_material(&handle, &shell, description) == XUD_OK, "material/create-ok", "the transparent material is created");
    xud_check(handle.kind == XUD_HANDLE_KIND_MATERIAL, "material/create-kind",
        "and the handle names the MATERIAL kind, which is what makes a cross-kind misuse a caught ru"
        "ntime error rather than a layout coincidence");
    xud_check(handle.generation == 0u, "material/create-generation",
        "and the first slot of a kind is still at generation 0, so the bump is what rejects a stale"
        "handle rather than the index being a fresh one");

    xud_check(xud_material_get(table, handle, &record) == XUD_OK, "material/get-ok",
        "the rung reads its own record back through the handle layer's accessor");
    xud_check(record != NULL, "material/get-record-live",
        "and the slot's record field is filled, which is the first time in this tree that a handle s"
        "lot's record is anything other than the sentinel");
    xud_check(record->variant == XUD_BLEND_TRANSPARENT, "material/get-variant",
        "and the variant is the blend mode the caller asked for, which is the mirror the header asse"
        "rts against the batch's variant axis rather than a conversion at the call");
    xud_check(record->setOne == VK_NULL_HANDLE, "material/get-set-one-sentinel",
        "and the set-1 field is the sentinel, because the texture step has not landed and set 1 is d"
        "eclared and never allocated - the field names its owner and the value says that owner has n"
        "ot run yet, which is the honest state of an empty half");

    description = XUD_TEST_MATERIAL_OPAQUE;
    xud_check(xud_create_material(&second, &shell, description) == XUD_OK, "material/create-second-ok",
        "a second material of the same kind takes the next slot");
    xud_check(second.index == handle.index + 1u, "material/create-second-index",
        "and the index moved, so the two materials are two records and not one record restated");
    xud_check(xud_material_get(table, second, &record) == XUD_OK, "material/get-second-ok", "the second record reads back");
    xud_check(record->variant == XUD_BLEND_OPAQUE, "material/get-second-variant",
        "and it carries its own variant, which is what makes the variant per material rather than a"
        "global the last create overwrote");
    xud_check(record->diffuseMap.kind == XUD_HANDLE_KIND_NONE, "material/get-second-unmapped",
        "and a description that named no map records no map, so the no-map state is a kind rather th"
        "an a slot a zero-initialised description happens to have aimed at slot zero");

    /*--- the diffuse map: the row it moves, and what it has to name -------------------*/

    /* The pure rung first, because it is arithmetic on two numbers and needs no table at all: the
       map's row is a rule about the variant axis, and the axis is the pipeline list's, so a materi
       al cannot draw through two rows at once.
    */
    xud_check(xud_material_resolve_variant(NULL, XUD_BLEND_OPAQUE, 0u) == XUD_INVALID_ARG, "material/resolve-null-out",
        "a variant with nowhere to be written is refused");
    xud_check(xud_material_resolve_variant(&variant, XUD_BLEND_COUNT, 0u) == XUD_INVALID_ARG, "material/resolve-past-blend",
        "and a blend mode at or past the axis is refused by the rung rather than by the create, so th"
        "e bound lives with the rule");
    variant = 0xFFFFFFFFu;
    xud_check(xud_material_resolve_variant(&variant, XUD_BLEND_TRANSPARENT, 0u) == XUD_OK, "material/resolve-unmapped",
        "an unmapped material answers");
    xud_check(variant == XUD_BATCH_VARIANT_TRANSPARENT, "material/resolve-unmapped-row",
        "and answers its own blend mode, so the no-map state costs no row change and the opaque defau"
        "lt is still the opaque default");
    variant = 0xFFFFFFFFu;
    xud_check(
        xud_material_resolve_variant(&variant, XUD_BLEND_OPAQUE, 1u) == XUD_OK, "material/resolve-mapped", "a mapped material answers");
    xud_check(variant == XUD_BATCH_VARIANT_TEXTURED, "material/resolve-mapped-row",
        "and the map moves the OPAQUE material to the textured row, because a map cannot be sampled b"
        "y the row whose frag module never reads set 1");
    xud_material_resolve_variant(&variant, XUD_BLEND_TRANSPARENT, 1u);
    xud_check(variant == XUD_BATCH_VARIANT_TEXTURED, "material/resolve-mapped-overrides-transparent",
        "and it moves the transparent one there too, which is the whole of the override: the textured"
        " row's blend attachment state is the transparent row's, so what is lost is the name of the b"
        "lend mode and not the blending");
    xud_material_resolve_variant(&variant, XUD_BLEND_OPAQUE, 7u);
    xud_check(variant == XUD_BATCH_VARIANT_TEXTURED, "material/resolve-any-nonzero-is-a-map",
        "and the map argument is a bound test rather than a switch, so any non-zero asks for the same"
        " row");

    /* The map's own check, before any material spends a slot on it. The slot is allocated through
       the handle layer rather than through xud_create_texture() because the material rung validates
       the HANDLE and reads no image: a TEXTURE-kind row of the same table is the whole of what a m
       ap has to be, and a real texture would put a driver between the pure rungs and the facts the
       y state.
    */
    xud_check(xud_material_check_map(NULL, XUD_HANDLE_NONE) == XUD_INVALID_ARG, "material/check-map-null-table",
        "a map with no table to be answered from is refused even when it names nothing");
    xud_check(xud_material_check_map(table, XUD_HANDLE_NONE) == XUD_OK, "material/check-map-none-ok",
        "and the no-map handle answers OK against a table that has no TEXTURE array at all, which is "
        "what keeps an unmapped material from needing one");
    xud_check(
        xud_material_check_map(table, (XudHandle) { .index = 0u, .generation = 0u, .kind = XUD_HANDLE_KIND_TEXTURE }) == XUD_INVALID_ARG,
        "material/check-map-no-array",
        "a texture handle against a table that never allocated the kind's array is refused, so the fi"
        "rst texture of a context cannot be named by a handle a caller made up");
    xud_check(xud_handle_alloc(&map, table, XUD_HANDLE_KIND_TEXTURE) == XUD_OK, "material/map-slot",
        "a TEXTURE slot of THIS table, allocated through the handle layer - the whole of what a map ha"
        "s to be to the material rung, which validates a handle and reads no image");
    xud_check(xud_material_check_map(table, map) == XUD_OK, "material/check-map-live", "and the live slot answers OK");
    xud_check(xud_material_check_map(table, handle) == XUD_INVALID_ARG, "material/check-map-cross-kind",
        "and a MATERIAL handle handed to the map check is refused by the tag, which is what makes a ma"
        "terial sampling itself a caught error rather than an index that happens to land");

    description = XUD_TEST_MATERIAL_OPAQUE;
    description.diffuseMap = map;
    xud_check(xud_create_material(&mapped, &shell, description) == XUD_OK, "material/create-mapped-ok",
        "the opaque description with a map on it is created");
    xud_check(xud_material_get(table, mapped, &record) == XUD_OK, "material/get-mapped-ok", "and its record reads back");
    xud_check(record->variant == XUD_BATCH_VARIANT_TEXTURED, "material/mapped-variant",
        "and the variant the record carries is the textured row rather than the opaque one the descri"
        "ption asked for - the rule is applied once, at the record, so no draw above has to remember i"
        "t");
    xud_check((record->diffuseMap.kind == XUD_HANDLE_KIND_TEXTURE) && (record->diffuseMap.index == map.index)
            && (record->diffuseMap.generation == map.generation),
        "material/mapped-record-names-the-map",
        "and the record names the map by the full handle, so the release of that slot is a state the m"
        "aterial can be checked against later rather than a number that silently still matches");
    xud_check(record->setOne == VK_NULL_HANDLE, "material/mapped-set-one-sentinel",
        "and naming a map is not binding one: the set-1 field is still the sentinel, which is the step"
        " after this one and the only honest value it has today");

    broken = description;
    broken.diffuseMap = handle;
    xud_check(xud_create_material(&foreign, &shell, broken) == XUD_INVALID_ARG, "material/create-map-cross-kind",
        "a map of another kind is refused at the create, before the slot is spent");
    broken.diffuseMap = (XudHandle) { .index = 999u, .generation = 0u, .kind = XUD_HANDLE_KIND_TEXTURE };
    xud_check(xud_create_material(&foreign, &shell, broken) == XUD_INVALID_ARG, "material/create-map-past-array",
        "and a map index past the array is refused by the same test, so a fabricated handle buys no ma"
        "terial");
    xud_check(foreign.kind == 0u, "material/create-map-refusal-left-handle",
        "and neither refusal wrote the out-handle, so a caller cannot read a half-material out of a r"
        "efused create");

    /*--- the scoped state pair, and the renderer's copy of the record -------------------*/

    xud_check(xud_begin_material(&shell, second) == XUD_OK, "material/begin-ok", "the second material is put in the drawing state");
    xud_check(renderer.materialWanted == 1u, "material/begin-sets-the-flag",
        "and the renderer says a material is live, which is the one state the frame loop reads at th"
        "e frame's opening");
    xud_check((renderer.material.kind == XUD_HANDLE_KIND_MATERIAL) && (renderer.material.index == second.index)
            && (renderer.material.generation == second.generation),
        "material/begin-copied-the-handle",
        "the renderer holds the handle BY VALUE rather than a pointer into the table, so the state th"
        "e frame loop reads cannot go stale behind a closed material");
    xud_check(renderer.materialVariant == XUD_BLEND_OPAQUE, "material/begin-copied-the-variant",
        "and the variant was copied off the record at the begin rather than read off the table at th"
        "e frame, which is what makes the renderer's state self-contained at the frame's opening");
    xud_check((batch.currentMaterial == second.index) && (batch.currentVariant == XUD_BATCH_VARIANT_OPAQUE),
        "material/begin-moved-the-batch",
        "and the batch moved at the call as well, so the draws that follow in the SAME frame are the"
        "material's - a scoped state that only took effect next frame would be a state the caller nev"
        "er sees");

    xud_check(xud_begin_material(&shell, handle) == XUD_OK, "material/begin-replaces",
        "a begin while one is live replaces it rather than stacking it, which is the same shape the"
        "camera pair's second begin states");
    xud_check(
        renderer.material.index == handle.index, "material/begin-replacement-landed", "and the handle that landed is the second call's");
    xud_check((batch.currentMaterial == handle.index) && (batch.currentVariant == XUD_BATCH_VARIANT_TRANSPARENT),
        "material/begin-replacement-moved-both-axes",
        "and both batch members moved together, so the row the frame loop records for these draws is"
        "the (topology, variant) pair this material draws through");

    foreign = handle;
    foreign.generation = 1u;
    xud_check(xud_begin_material(&shell, foreign) == XUD_INVALID_ARG, "material/begin-stale-handle",
        "a handle kept across the close of the material it names is refused - the generation is the"
        "bump that rejects it, not the index");
    xud_check(xud_begin_material(&shell, (XudHandle) { .index = 0u, .generation = 0u, .kind = XUD_HANDLE_KIND_TEXTURE }) == XUD_INVALID_ARG,
        "material/begin-cross-kind",
        "and a handle of another kind is refused, which is the tag check the handle layer's own asse"
        "rtion states and the reason the public call validates rather than trusting the index");
    xud_check(renderer.material.index == handle.index, "material/begin-refusal-left-the-live-material",
        "and the material that was live is still the one the renderer holds, so a refused begin is a"
        "frame drawn with the previous material rather than a frame drawn with nothing");

    xud_check(xud_end_material(&shell) == XUD_OK, "material/end-ok", "a live material is taken away");
    xud_check(renderer.materialWanted == 0u, "material/end-clears-the-flag",
        "and the flag is cleared, so the next frame's opening re-applies nothing rather than re-appl"
        "ying a material the caller already ended");
    xud_check((batch.currentMaterial == XUD_BATCH_NO_MATERIAL) && (batch.currentVariant == XUD_BATCH_VARIANT_OPAQUE),
        "material/end-moved-the-batch-back",
        "and the batch moved back to the default HERE rather than waiting for the next frame's reset,"
        "so the draws that follow in this frame are drawn with no material - which is what makes a m"
        "aterial that began and ended inside one frame a no-op rather than a leak into the next");
    xud_check(renderer.material.index == handle.index, "material/end-leaves-the-handle",
        "the handle itself is left as it was, because the flag is what governs and a begin after her"
        "e replaces the whole pair");
    xud_check(xud_end_material(&shell) == XUD_ALREADY, "material/end-no-material-live",
        "and ending a material that is not live is the repo's idempotent ask rather than a refusal");
    xud_check(renderer.materialWanted == 0u, "material/end-twice-changes-nothing",
        "the second end changed nothing, so the ask is idempotent rather than a refusal that left st"
        "ate behind");

    /*--- the object close, and the state it owes ----------------------------------------*/

    xud_check(xud_close_material(&shell, handle) == XUD_OK, "material/close-ok", "the live material is released");
    xud_check(renderer.materialWanted == 0u, "material/close-already-not-live",
        "the end above had already taken it away, so there was no renderer state for this close to c"
        "lear and the close is the object half alone");
    xud_check(xud_material_get(table, handle, &record) == XUD_INVALID_ARG, "material/close-stale-handle",
        "and the handle the close released is refused by the read after it - the generation bump is w"
        "hat rejects it, so a caller cannot read a record it just freed");

    xud_check(xud_close_material(&shell, second) == XUD_OK, "material/close-second-ok", "the second material is released");
    xud_check(xud_close_material(&shell, second) == XUD_INVALID_ARG, "material/close-twice-refused",
        "and a close run twice is refused rather than a double free, because the handle the first clo"
        "se released no longer names a live slot");
    xud_check(xud_close_material(NULL, second) == XUD_INVALID_ARG, "material/close-null-context", "a close with no context is refused");

    /* A material the renderer is drawing with, closed while it is live: the renderer's state has t
       o go with the object, or the next frame's opening would re-apply a handle whose record no lo
       nger exists and the frame loop would bind a row naming a slot that has been released.
    */
    description = XUD_TEST_MATERIAL_TRANSPARENT;
    xud_check(xud_create_material(&kept, &shell, description) == XUD_OK, "material/create-third-ok",
        "a third material, created to be begun and closed while live");
    xud_check(xud_begin_material(&shell, kept) == XUD_OK, "material/begin-third-ok", "and put in the drawing state");
    xud_check(renderer.materialWanted == 1u, "material/third-live", "and live");
    xud_check(xud_close_material(&shell, kept) == XUD_OK, "material/close-while-live-ok", "and released while the renderer drew with it");
    xud_check(renderer.materialWanted == 0u, "material/close-while-live-cleared-the-state",
        "and the renderer's state went with it, so the frames after here are drawn with no material"
        "rather than with rows naming a slot whose record no longer exists");
    xud_check((batch.currentMaterial == XUD_BATCH_NO_MATERIAL) && (batch.currentVariant == XUD_BATCH_VARIANT_OPAQUE),
        "material/close-while-live-moved-the-batch-back",
        "and the batch went back to the default with it, which is the same move the end pair applies"
        "and the reason the two pairs cannot disagree about the batch's state");

    /*--- the mapped material through the state pair, and the map closed behind it ------*/

    xud_check(xud_begin_material(&shell, mapped) == XUD_OK, "material/begin-mapped-ok", "the mapped material is put in the drawing state");
    xud_check(renderer.materialVariant == XUD_BATCH_VARIANT_TEXTURED, "material/begin-mapped-copied-the-resolved-variant",
        "and the variant the renderer stores is the record's resolved one, so the frame's opening rea"
        "ds the textured row without ever asking whether this material has a map");
    xud_check((batch.currentMaterial == mapped.index) && (batch.currentVariant == XUD_BATCH_VARIANT_TEXTURED),
        "material/begin-mapped-moved-the-row",
        "and the batch moved to the textured row at the call, which is the row the frame loop binds a"
        "nd the only one whose frag module reads the map this material names");
    xud_check(xud_end_material(&shell) == XUD_OK, "material/end-mapped-ok", "and it is taken away again");

    /* The map's slot released while the material still names it. The material never owned the tex
       ture - one image, many materials - so nothing above closed it for them, and the one dangling
       state that ordering allows is caught where it would otherwise become a draw: at the begin.
    */
    xud_check(xud_handle_free(table, map) == XUD_OK, "material/map-slot-released",
        "the texture behind the map is released through the handle layer, which is what a caller's xud"
        "_close_texture() does one rung up");
    xud_check(xud_begin_material(&shell, mapped) == XUD_INVALID_ARG, "material/begin-map-gone",
        "and the material that still names it is refused at the begin rather than beginning a frame t"
        "hat samples an image that is gone");
    xud_check(renderer.materialWanted == 0u, "material/begin-map-gone-stored-nothing",
        "and the refusal stored nothing, so the frames after here are drawn with no material rather t"
        "han with the last textured row this call would have moved in");
    xud_check((batch.currentMaterial == XUD_BATCH_NO_MATERIAL) && (batch.currentVariant == XUD_BATCH_VARIANT_OPAQUE),
        "material/begin-map-gone-left-the-batch",
        "and the batch is where the end left it, so a refused begin is a state move that did not happe"
        "n rather than one that half-happened");
    xud_check(xud_close_material(&shell, mapped) == XUD_OK, "material/close-mapped-ok",
        "and the mapped material still closes: naming a map is not owning one, so a dead map does not "
        "strand the material that named it");

    /*--- the teardown rung, and the order it owes ---------------------------------------*/

    /* Two more materials, one of them released through the public pair and one of them left live a
       t the table's teardown. The released one's record is NOT still in its slot: xud_close_materi
       al() cleared the field before it freed, which is the one-teardown-path-per-record rule this
       rung owns, and the count below is what says so - a rung that freed at the release and again a
       t the teardown would be a double free, and a rung that cleared the field but not the record
       would be a leak the sanitizer build reports. The walk over released slots is still the walk
       the rung takes (xud_handle_free() on its own leaves the field alone, which the handle area's
       own checks pin), it just finds the sentinel in this path.
    */
    xud_check(xud_create_material(&handle, &shell, XUD_TEST_MATERIAL_OPAQUE) == XUD_OK, "material/teardown-first-ok",
        "a material created to be released but not re-closed");
    xud_check(xud_create_material(&second, &shell, XUD_TEST_MATERIAL_TRANSPARENT) == XUD_OK, "material/teardown-second-ok",
        "and a second one, left live at the teardown");
    xud_check(xud_close_material(&shell, handle) == XUD_OK, "material/teardown-release-first",
        "the first is released, its record freed at that release");
    closed = 0u;
    xud_check(xud_material_close_all(table, &closed) == XUD_OK, "material/close-all-ok", "the teardown rung runs above the table");
    xud_check(closed == 1u, "material/close-all-count",
        "and freed one record - the live slot's - because the released slot's was already freed at i"
        "ts own release, which is the whole of the order: exactly one teardown path per record, and"
        "the table's close below frees the arrays only");
    xud_check(
        xud_material_close_all(NULL, &closed) == XUD_INVALID_ARG, "material/close-all-null", "the rung refuses a table with no owner");
    xud_check(xud_material_close_all(table, NULL) == XUD_INVALID_ARG, "material/close-all-null-count",
        "and refuses a count with nowhere to be written, so the count is not an optional out-param a"
        "caller can ignore");
    xud_check(xud_handle_table_close(&table) == XUD_OK, "material/table-close", "and the table's own teardown runs after it");
    xud_check(table == NULL, "material/table-close-clears", "and clears the pointer");

    free(batch.draws);
}

/*-----------------------------------------------------------------------------------------------
 * The GPU half: the same calls against a real context, and the frame's opening re-apply
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_material_gpu(void) {
    static const uint8_t texels2x2[4u * 4u] = { 220u, 40u, 40u, 255u, 40u, 220u, 40u, 255u, 40u, 40u, 220u, 255u, 220u, 220u, 40u, 255u };
    XudContext* context = NULL;
    XudResult result = XUD_OK;
    XudHandle handle = { 0u, 0u, 0u };
    XudHandle map = { 0u, 0u, 0u };
    XudHandle mapped = { 0u, 0u, 0u };
    XudHandle mapless = { 0u, 0u, 0u };
    XudHandle plain = { 0u, 0u, 0u };
    XudBatchDraw row;
    const XudTextureData mapData = {
        .width = 2u,
        .height = 2u,
        .rgba = texels2x2,
        .filter = XUD_TEXTURE_FILTER_NEAREST,
        .addressMode = XUD_TEXTURE_ADDRESS_CLAMP_TO_EDGE,
    };
    XudMaterialRecord const* record = NULL;
    uint32_t live = 0u;

    result = xud_init_context(&context, 64u, 64u);
    xud_check(result == XUD_OK, "gpu-material/bringup-completes",
        "this machine has a Vulkan loader and a window system, so the ladder must complete");
    if (context == NULL) {
        return;
    }

    xud_check(context->handleTable != NULL, "gpu-material/table-owned",
        "a context that completed owns the handle table the MATERIAL kind's row lives in");
    xud_check(xud_handle_table_live_count(context->handleTable, XUD_HANDLE_KIND_MATERIAL, &live) == XUD_OK,
        "gpu-material/live-at-open-query", "the live count of the kind answers");
    xud_check(
        live == 0u, "gpu-material/live-at-open", "and is zero, so a fresh context has no material and no slot of the kind has been taken");

    xud_check(xud_create_material(&handle, context, XUD_TEST_MATERIAL_TRANSPARENT) == XUD_OK, "gpu-material/create-ok",
        "the public create call hands a handle out of a real context");
    xud_check(xud_handle_table_live_count(context->handleTable, XUD_HANDLE_KIND_MATERIAL, &live) == XUD_OK, "gpu-material/live-after-query",
        "and the live count answers again");
    xud_check(live == 1u, "gpu-material/live-after", "and moved to one, which is the handle layer's count rather than this rung's");

    xud_check(xud_material_get(context->handleTable, handle, &record) == XUD_OK, "gpu-material/get-ok",
        "the record reads back through the accessor");
    xud_check((record != NULL) && (record->variant == XUD_BLEND_TRANSPARENT), "gpu-material/variant",
        "and the variant is the one the caller asked for, which is the row of the pipeline list the"
        "draws of this material bind");
    xud_check(record->setOne == VK_NULL_HANDLE, "gpu-material/set-one-sentinel",
        "and the set-1 field is still the sentinel against a real device, so a material created befo"
        "re the texture step landed owns no descriptor and the pipeline's set-1 rows are the only se"
        "t 1 this tree has ever declared");

    xud_check(xud_begin_material(context, handle) == XUD_OK, "gpu-material/begin-ok", "the material is put in the drawing state");
    xud_check((context->renderer->materialWanted == 1u) && (context->renderer->material.index == handle.index),
        "gpu-material/renderer-state", "and the renderer holds the pair the frame loop re-applies");
    xud_check(context->renderer->materialVariant == XUD_BLEND_TRANSPARENT, "gpu-material/renderer-variant",
        "and the variant was copied off the record at the begin, which is what makes the frame's ope"
        "ning re-apply a pair rather than a lookup");

    /* The frame, read at the two points the row's content IS observable at. The row itself is not
       observable after the frame: xud_frame.c's loop records the pending run, replays it and resets
       the batch at the end of a frame that completed, so the row list is empty by the time xud_end_
       drawing() returns. What is observable is the state the row is recorded FROM, mid-frame, and t
        he re-apply at the next frame's opening - which is the rule this rung owns and the reason a m
       aterial begun in one frame is drawn with in the ones that follow. The row's own fields are th
       e batch area's and the frame area's checks, beside the topology axis a material change shares
       a mechanism with.
    */
    xud_check(xud_begin_drawing(context) == XUD_OK, "gpu-material/begin-drawing-ok", "the frame opens");
    xud_check((context->renderer->batch->currentMaterial == handle.index)
            && (context->renderer->batch->currentVariant == XUD_BATCH_VARIANT_TRANSPARENT),
        "gpu-material/opening-reapplied-the-material",
        "and the opening moved the batch back to this material after its reset had moved it to the d"
        "efault, which is the renderer's copy of the pair read at the frame's opening rather than a "
        "lookup into the table");
    xud_check(xud_draw_cube(context, (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 0.0f }, 1.0f, 1.0f, 1.0f,
                  (XudColor) { .r = 200u, .g = 60u, .b = 60u, .a = 160u })
            == XUD_OK,
        "gpu-material/cube-appended", "a box with a half-alpha colour, drawn under the live material");
    xud_check((context->renderer->batch->pendingIndexCount > 0u) && (context->renderer->batch->drawCount == 0u),
        "gpu-material/mid-frame-the-run-is-pending",
        "mid-frame the cube's run is pending rather than a row, so the row the frame loop records is "
        "the one that closes THIS run and it is recorded from the state the opening moved above");
    result = xud_end_drawing(context);
    xud_check(result == XUD_OK, "gpu-material/end-drawing-ok", "and the frame closed");
    xud_check(context->renderer->batch->drawCount == 0u, "gpu-material/frame-end-row-list-empty",
        "and the row list is empty again at the frame's end, which is the reset a completed frame ru"
        "ns: the row was recorded, replayed and retired inside the frame rather than left for a next"
        "one");
    xud_check((context->renderer->batch->currentMaterial == XUD_BATCH_NO_MATERIAL)
            && (context->renderer->batch->currentVariant == XUD_BATCH_VARIANT_OPAQUE),
        "gpu-material/frame-end-at-default",
        "and the batch's state is at the default while the renderer still wants the material, which i"
        "s the half the next opening answers rather than a material left behind in a batch the frame"
        " closed");
    xud_check(context->renderer->materialWanted == 1u, "gpu-material/wanted-across-the-frame",
        "and the material is still wanted after the frame that drew it, which is what makes the next "
        "frame's opening re-apply it");

    /* A second frame with no begin call in it: the material has to be drawn with again, which is th
       e frame-opening rule read where a reset would otherwise have silently dropped it.
    */
    xud_check(xud_begin_drawing(context) == XUD_OK, "gpu-material/second-frame-opens", "a second frame opens with no begin call in it");
    xud_check((context->renderer->batch->currentMaterial == handle.index)
            && (context->renderer->batch->currentVariant == XUD_BATCH_VARIANT_TRANSPARENT),
        "gpu-material/second-frame-reapplied",
        "and the opening re-applied the material the renderer still wants, so a material begun in on"
        "e frame is drawn with in the ones that follow rather than only in the one it began in");
    xud_check(xud_draw_cube(context, (XudVector3) { .x = 0.0f, .y = 1.5f, .z = 0.0f }, 1.0f, 1.0f, 1.0f,
                  (XudColor) { .r = 60u, .g = 60u, .b = 200u, .a = 90u })
            == XUD_OK,
        "gpu-material/second-frame-cube-appended", "a box drawn with no begin call in this frame");
    result = xud_end_drawing(context);
    xud_check(result == XUD_OK, "gpu-material/second-frame-closes", "and the second frame closed");

    xud_check(xud_end_material(context) == XUD_OK, "gpu-material/end-ok", "the material is taken away");
    xud_check(context->renderer->materialWanted == 0u, "gpu-material/flag-cleared", "and the renderer says none is live");
    xud_check((context->renderer->batch->currentMaterial == XUD_BATCH_NO_MATERIAL)
            && (context->renderer->batch->currentVariant == XUD_BATCH_VARIANT_OPAQUE),
        "gpu-material/batch-back-at-default",
        "and the batch is back at the default, so the next frame's opening re-applies nothing and re"
        "cords a row at the default rather than at the material that just ended");

    xud_check(xud_close_material(context, handle) == XUD_OK, "gpu-material/close-ok", "the material is released");
    xud_check(xud_handle_table_live_count(context->handleTable, XUD_HANDLE_KIND_MATERIAL, &live) == XUD_OK,
        "gpu-material/live-after-close-query", "the live count answers again");
    xud_check(live == 0u, "gpu-material/live-after-close",
        "and is back to zero, so the object half released the slot rather than only the renderer's s"
        "tate");
    xud_check(xud_close_material(context, handle) == XUD_INVALID_ARG, "gpu-material/close-twice",
        "and a second close is refused, so the release is one free and not two");

    /*--- the diffuse map, against a real texture and a real device -------------------*/

    xud_check(xud_create_texture(&map, context, &mapData) == XUD_OK, "gpu-material/map-created",
        "a 2x2 texture is created, and it is the map half of the checks below rather than a texture ar"
        "ea's check repeated");
    xud_check(xud_create_material(&mapped, context, (XudMaterial) { .blendMode = XUD_BLEND_OPAQUE, .diffuseMap = map }) == XUD_OK,
        "gpu-material/create-mapped-ok", "the opaque description with a real texture on it is created against a real device");
    xud_check(xud_material_get(context->handleTable, mapped, &record) == XUD_OK, "gpu-material/get-mapped-ok", "and its record reads back");
    xud_check(record->variant == XUD_BATCH_VARIANT_TEXTURED, "gpu-material/mapped-variant",
        "and the record's row is the textured one, which against a real device is the row whose frag m"
        "odule is solid_textured.frag and samples set 1");
    xud_check((record->diffuseMap.kind == XUD_HANDLE_KIND_TEXTURE) && (record->diffuseMap.index == map.index),
        "gpu-material/mapped-names-the-map", "and the record names the texture that was handed to it");
    xud_check(
        xud_begin_material(context, mapped) == XUD_OK, "gpu-material/begin-mapped-ok", "the mapped material is put in the drawing state");
    xud_check(context->renderer->materialVariant == XUD_BATCH_VARIANT_TEXTURED, "gpu-material/mapped-renderer-variant",
        "and the renderer holds the resolved row, which is what the next frame's opening re-applies wi"
        "thout a lookup into the table");
    xud_check(xud_begin_drawing(context) == XUD_OK, "gpu-material/mapped-frame-opens", "a frame opens with the mapped material live");
    xud_check((context->renderer->batch->currentMaterial == mapped.index)
            && (context->renderer->batch->currentVariant == XUD_BATCH_VARIANT_TEXTURED),
        "gpu-material/opening-reapplied-the-textured-row",
        "and the frame's opening moved the batch to the textured row rather than to the opaque blend m"
        "ode the description asked for, so the row the frame loop records for these draws is the one t"
        "hat can sample the map");
    xud_check(xud_draw_cube(context, (XudVector3) { .x = 0.0f, .y = 0.0f, .z = 0.0f }, 1.0f, 1.0f, 1.0f,
                  (XudColor) { .r = 40u, .g = 200u, .b = 120u, .a = 255u })
            == XUD_OK,
        "gpu-material/mapped-cube-appended", "a box drawn under the mapped material");
    xud_check(xud_create_material(&plain, context, XUD_TEST_MATERIAL_OPAQUE) == XUD_OK, "gpu-material/create-plain-ok",
        "an unmapped opaque material, made only to move the state again below");
    xud_check(xud_begin_material(context, plain) == XUD_OK, "gpu-material/begin-plain-ok", "and begun, mid-frame, over the mapped run");
    xud_check(context->renderer->batch->drawCount == 1u, "gpu-material/map-change-split-the-run",
        "and the mapped run became a row of its own at that begin, which is the row split reading the "
        "map's row as an axis of its own");
    xud_check(
        xud_batch_draw_get(context->renderer->batch, 0u, &row) == XUD_OK, "gpu-material/row-read-back", "the recorded row reads back");
    xud_check(row.variant == XUD_BATCH_VARIANT_TEXTURED, "gpu-material/recorded-row-is-the-textured-row",
        "and the row the frame loop binds for the mapped draws names the textured variant, which is th"
        "e one fact a pixel would otherwise be standing in for: the pipeline those vertices ran under i"
        "s the one that samples set 1");
    xud_check(row.materialIndex == mapped.index, "gpu-material/recorded-row-names-the-mapped-material",
        "and the row names the mapped material's slot, so the row and the record agree about which mat"
        "erial the draws were made under");
    xud_check(xud_end_material(context) == XUD_OK, "gpu-material/end-plain-ok", "and the plain material is taken away again");
    result = xud_end_drawing(context);
    xud_check(result == XUD_OK, "gpu-material/mapped-frame-closes", "and the frame that drew it closed");

    /* The ownership rule, read at the two closes. The material's close closes no texture: a textu
       re is one image a dozen materials sample, and an image with two owners is an image destroyed
       twice. The texture's close then leaves the material that named it alive and its begin refuse
       d, which is the half that keeps the dangling state out of a frame.
    */
    xud_check(xud_close_material(context, mapped) == XUD_OK, "gpu-material/close-mapped-ok", "the mapped material is released");
    xud_check(xud_handle_table_live_count(context->handleTable, XUD_HANDLE_KIND_TEXTURE, &live) == XUD_OK, "gpu-material/map-live-query",
        "the texture live count answers");
    xud_check(live == 1u, "gpu-material/close-mapped-leaves-the-map",
        "and is still one, so the material's close closed the material and nothing of the map's");
    xud_check(xud_create_material(&mapless, context, (XudMaterial) { .blendMode = XUD_BLEND_OPAQUE, .diffuseMap = map }) == XUD_OK,
        "gpu-material/create-mapped-second-ok", "a second material may name the same texture");
    xud_check(xud_close_texture(context, map) == XUD_OK, "gpu-material/close-map-ok", "and the texture is closed while one still names it");
    xud_check(xud_begin_material(context, mapless) == XUD_INVALID_ARG, "gpu-material/begin-map-gone",
        "and that material is refused at its begin, which is the check the pure half states run agains"
        "t a real slot whose image has been destroyed");
    xud_check(context->renderer->materialWanted == 0u, "gpu-material/begin-map-gone-stored-nothing",
        "and nothing was stored, so no frame is recorded with a textured row whose image is gone");
    xud_check(xud_close_material(context, mapless) == XUD_OK, "gpu-material/close-mapless-material-ok",
        "and the material whose map went away still closes, because it never owned the map");
    xud_check(
        xud_close_material(context, plain) == XUD_OK, "gpu-material/close-plain-ok", "and so does the plain one the split above used");

    /* The teardown inside a real context: every material above was released through the public pair,
       so the rung above the table has the released slots' records to free before the arrays go. This
       is the path xud_close_context() runs, and the sanitizer build is what says it freed them.
    */
    result = xud_close_context(&context);
    xud_check(result == XUD_OK, "gpu-material/close-clean", "the whole ladder tears back down with nothing refused");
    xud_check(context == NULL, "gpu-material/close-clears", "and the out-param is cleared");
}

void xud_test_material(void) {
    xud_test_material_pure();
    if (xud_gpu_tests_enabled()) {
        xud_test_material_gpu();
    }
}
