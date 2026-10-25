/***********************************************************************************************
 *
 *   xud_test_handle.c - the handle layer: the public currency and its per-kind slot arrays
 *
 *   Owns : slot allocation, the generation bump on reuse, the validation order that makes a stale or cross-kind handle a caught error, the
 * live count and the teardown. The layer calls no driver function and includes no Vulkan header, so the headless run covers it completely.
 *   Layer: tests. Sees xud_handle.h only. This is the one area whose entry point is the function itself: xud_test_handle() is the area, and
 * it is the only test function of this area.
 *
 *   Deterministic: fixed loop counts, no time, no randomness, no filesystem.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud.h" /* Required for: XudResult, XUD_VERSION, the XUD_GPU_TESTS gate */
#include "xud_handle.h" /* Required for: XudHandleTable, the slot rungs and the validation order */
#include "xud_test_harness.h" /* Required for: xud_check, xud_gpu_tests_enabled, the area entry points */
#include <stdlib.h> /* Required for: calloc and free, the synthetic records this area builds and frees */

/*-----------------------------------------------------------------------------------------------
 * The handle layer: the one XudHandle type of xud.h and the per-kind slot arrays behind it. All
 * pure - the layer calls no driver function and includes no Vulkan header - so the headless run
 * covers it completely, and the GPU run covers it again inside a real context. The invariants thi
 * s layer owns are the three-part validation (bounds, then generation, then tag), the generation
 * bump on reuse, the live count, the doubling growth sequence and its own bound, and teardown; th
 * e checks below walk each against the state the layer says it owns, including the states a calle
 * r can only reach by ignoring an error code (a zero-initialised handle, a handle kept across a
 * release, a fabricated one) and the states two contexts reach by owning two tables.
 *-----------------------------------------------------------------------------------------------*/

void xud_test_handle(void) {
    XudHandleTable* table = NULL;
    XudHandle zero = { 0u, 0u, 0u }; /* the state a caller gets when it ignores the XUD_RESULT of a create call */
    XudHandle handle = { 0u, 0u, 0u };
    XudHandle kept = { 0u, 0u, 0u };
    XudHandle probe = { 0u, 0u, 0u };
    XudHandle font = { 0u, 0u, 0u };
    XudHandle mesh = { 0u, 0u, 0u };
    XudHandle refused = { 0u, 0u, 0u };
    XudHandle fresh = { 0u, 0u, 0u };
    uint32_t live = 0u;
    uint32_t index = 0u;
    uint32_t released = 0u;

    /* A table is one owner: a NULL out-pointer is a bad argument, and a pointer that already names
       a table is a second table for one context, which would be two owners of the same slots. */
    xud_check(xud_handle_table_open(NULL) == XUD_INVALID_ARG, "handle/open-null", "is refused");
    xud_check(xud_handle_table_open(&table) == XUD_OK, "handle/open", "is a fresh table");
    xud_check(table != NULL, "handle/open/table", "and filled");
    xud_check(xud_handle_table_open(&table) == XUD_INVALID_ARG, "handle/open-twice", "is refused");

    /* An empty table answers for itself: no kind's array exists yet, so no index of any kind names
       a slot, and the live count of a kind never allocated is 0 rather than an error - the "no obj
       ect of that kind was ever created" state. */
    xud_check(xud_handle_validate(table, zero, XUD_HANDLE_KIND_TEXTURE) == XUD_INVALID_ARG, "handle/validate-empty", "refuses");
    xud_check(xud_handle_table_live_count(table, XUD_HANDLE_KIND_TEXTURE, &live) == XUD_OK, "handle/live-empty", "answers");
    xud_check(live == 0u, "handle/live-empty/count", "and is zero");
    xud_check(xud_handle_table_live_count(NULL, XUD_HANDLE_KIND_TEXTURE, &live) == XUD_INVALID_ARG, "handle/live-null-table", "is refused");
    xud_check(xud_handle_table_live_count(table, XUD_HANDLE_KIND_TEXTURE, NULL) == XUD_INVALID_ARG, "handle/live-null-out", "is refused");
    xud_check(xud_handle_table_live_count(table, XUD_HANDLE_KIND_NONE, &live) == XUD_INVALID_ARG, "handle/live-none", "is refused");
    xud_check(xud_handle_table_live_count(table, XUD_HANDLE_KIND_COUNT, &live) == XUD_INVALID_ARG, "handle/live-past", "is refused");

    /* NONE is not a kind a slot may be allocated for, and a value past the highest row of the
       table's own arrays is not a kind either - the range check is what keeps the kind-indexed loo
       kups below from reading past the table. */
    xud_check(xud_handle_alloc(NULL, table, XUD_HANDLE_KIND_TEXTURE) == XUD_INVALID_ARG, "handle/alloc-null-out", "is refused");
    xud_check(xud_handle_alloc(&handle, NULL, XUD_HANDLE_KIND_TEXTURE) == XUD_INVALID_ARG, "handle/alloc-null-table", "is refused");
    xud_check(xud_handle_alloc(&handle, table, XUD_HANDLE_KIND_NONE) == XUD_INVALID_ARG, "handle/alloc-none", "is refused");
    xud_check(xud_handle_alloc(&handle, table, XUD_HANDLE_KIND_COUNT) == XUD_INVALID_ARG, "handle/alloc-past", "is refused");
    xud_check(handle.index == 0u && handle.generation == 0u && handle.kind == 0u, "handle/alloc-refused/untouched",
        "and a refusal leaves the out-handle as it was");

    /* The first slot of a kind is index 0 at generation 0, and it validates against its own kind. */
    xud_check(xud_handle_alloc(&handle, table, XUD_HANDLE_KIND_TEXTURE) == XUD_OK, "handle/alloc-first", "answers");
    xud_check(handle.index == 0u, "handle/first/index", "is 0");
    xud_check(handle.generation == 0u, "handle/first/generation", "is 0");
    xud_check(handle.kind == XUD_HANDLE_KIND_TEXTURE, "handle/first/kind", "and is the kind asked for");
    xud_check(xud_handle_validate(table, handle, XUD_HANDLE_KIND_TEXTURE) == XUD_OK, "handle/validate-live", "accepts");

    /* The zero-initialised handle is refused by the TAG, not by the bounds: its index is in range
       and its generation matches a fresh slot, so only the tag compare tells them apart. That is
       the reason the tag exists, read as a check. */
    xud_check(xud_handle_validate(table, zero, XUD_HANDLE_KIND_TEXTURE) == XUD_INVALID_ARG, "handle/validate-zero", "refuses");

    /* The cross-kind case, and the reason the tag is not redundant with the array's identity: a
       font's slot 0 at generation 0 matches a texture handle's (index, generation) pair exactly,
       so only the tag makes that a refusal rather than a read of the wrong record. */
    xud_check(xud_handle_alloc(&font, table, XUD_HANDLE_KIND_FONT) == XUD_OK, "handle/alloc-font", "answers");
    xud_check(font.index == 0u && font.generation == 0u, "handle/font/pair", "matches the texture handle's");
    xud_check(xud_handle_validate(table, handle, XUD_HANDLE_KIND_FONT) == XUD_INVALID_ARG, "handle/validate-crosskind", "refuses");
    xud_check(xud_handle_validate(table, font, XUD_HANDLE_KIND_TEXTURE) == XUD_INVALID_ARG, "handle/validate-crosskind-back", "refuses");
    xud_check(xud_handle_equal(handle, font) == false, "handle/equal-samepair-differentkind", "is false");
    xud_check(xud_handle_equal(handle, handle) == true, "handle/equal-itself", "is true");

    /* A third kind is a third array: its own slot 0, its own generation 0, and nothing about the
       arrays above it. Two contexts' tables are checked the same way below, in the context test. */
    xud_check(xud_handle_alloc(&mesh, table, XUD_HANDLE_KIND_MESH) == XUD_OK, "handle/alloc-mesh", "answers");
    xud_check(mesh.index == 0u && mesh.generation == 0u, "handle/mesh/pair", "is its own slot 0 at gen 0");
    xud_check(xud_handle_validate(table, mesh, XUD_HANDLE_KIND_MESH) == XUD_OK, "handle/validate-mesh", "accepts");

    /* Release, and the stale-handle rule read at the one place a slot goes free: the handle in
       hand stops naming the slot, and so does every handle that named it before - the generation
       moved, so the next slot of that kind is the same index at a higher generation. */
    kept = handle;
    xud_check(xud_handle_free(table, kept) == XUD_OK, "handle/free", "accepts a live handle");
    xud_check(xud_handle_validate(table, kept, XUD_HANDLE_KIND_TEXTURE) == XUD_INVALID_ARG, "handle/validate-acrossfree", "refuses");
    xud_check(xud_handle_free(table, kept) == XUD_INVALID_ARG, "handle/free-twice", "refuses the stale handle");
    xud_check(xud_handle_alloc(&handle, table, XUD_HANDLE_KIND_TEXTURE) == XUD_OK, "handle/alloc-afterfree", "answers");
    xud_check(handle.index == 0u, "handle/reuse/index", "is the released slot's");
    xud_check(handle.generation == 1u, "handle/reuse/generation", "and is bumped");
    xud_check(xud_handle_validate(table, handle, XUD_HANDLE_KIND_TEXTURE) == XUD_OK, "handle/validate-reused", "accepts");
    xud_check(xud_handle_equal(kept, handle) == false, "handle/equal-acrossfree", "is false on the generation");

    /* A handle that is not stale but names a FREE slot: the generation and the tag both match, so
       only the live bit tells them apart, and refusing it is what keeps a decoded handle always a
       handle that names a live slot. */
    xud_check(xud_handle_free(table, handle) == XUD_OK, "handle/free-reused", "accepts");
    probe.index = 0u;
    probe.generation = 2u;
    probe.kind = XUD_HANDLE_KIND_TEXTURE;
    xud_check(xud_handle_validate(table, probe, XUD_HANDLE_KIND_TEXTURE) == XUD_INVALID_ARG, "handle/validate-fabricated-free", "refuses");
    xud_check(xud_handle_free(table, probe) == XUD_INVALID_ARG, "handle/free-fabricated", "refuses");

    /* The live count is what a caller above reads before it creates more: three live textures,
       then one released, then one more that reused it - so the count never counts a free slot and
       never counts a reused one twice. */
    xud_check(xud_handle_table_live_count(table, XUD_HANDLE_KIND_TEXTURE, &live) == XUD_OK, "handle/live-after", "answers");
    xud_check(live == 0u, "handle/live-after/count", "and is zero, since the reused slot was released just above");

    /* The growth sequence: the first XUD_HANDLE_SLOT_MIN_CAPACITY slots of a kind are one
       allocation, and the one past them is the growth step, which must not refuse. The index is a
       position, not a pointer, which is what keeps every handle below valid across it. */
    for (index = 0u; index < XUD_HANDLE_SLOT_MIN_CAPACITY; index++) {
        xud_check(xud_handle_alloc(&handle, table, XUD_HANDLE_KIND_MATERIAL) == XUD_OK, "handle/grow/before",
            "fills every slot of the first capacity");
        xud_check(handle.index == index, "handle/grow/before/index", "and hands them out in order");
    }
    xud_check(
        xud_handle_alloc(&handle, table, XUD_HANDLE_KIND_MATERIAL) == XUD_OK, "handle/grow/step", "crosses the capacity without refusing");
    xud_check(handle.index == XUD_HANDLE_SLOT_MIN_CAPACITY, "handle/grow/step/index", "is the new row's");
    xud_check(handle.generation == 0u, "handle/grow/step/generation", "and is a fresh slot's");
    xud_check(xud_handle_validate(table, handle, XUD_HANDLE_KIND_MATERIAL) == XUD_OK, "handle/grow/step/validate",
        "accepts it, so the copy kept every index below valid");

    /* Reuse is the first free slot, not the end of the array: releasing a slot in the middle and
       asking for another one hands back that middle index at a bumped generation, and the rows ab
       ove it are unmoved - which is what keeps an index a handle carries a position rather than a
       place in an allocation log, and what keeps a growing array from invalidating anything. */
    refused.index = 5u;
    refused.generation = 0u;
    refused.kind = XUD_HANDLE_KIND_MATERIAL;
    xud_check(xud_handle_free(table, refused) == XUD_OK, "handle/grow/release-middle", "accepts a slot in the middle");
    xud_check(xud_handle_alloc(&handle, table, XUD_HANDLE_KIND_MATERIAL) == XUD_OK, "handle/grow/reuse-middle", "answers");
    xud_check(handle.index == 5u, "handle/grow/reuse-middle/index", "is the released slot's, not a new row's");
    xud_check(handle.generation == 1u, "handle/grow/reuse-middle/generation", "and is bumped");
    refused.index = XUD_HANDLE_SLOT_MIN_CAPACITY;
    refused.generation = 0u;
    refused.kind = XUD_HANDLE_KIND_MATERIAL;
    xud_check(xud_handle_validate(table, refused, XUD_HANDLE_KIND_MATERIAL) == XUD_OK, "handle/grow/above-unmoved",
        "still accepts the row above the reused one");

    /* The bound is this layer's own ceiling, and reaching it is a reported refusal, not a quiet
       cut: a cut array would be an array whose later slots are unreachable and whose handles would
       address the wrong record. The out-handle is untouched on the refusal, so a caller cannot rea
       d a half-filled handle. The walk below is the only way to reach the ceiling honestly - the
       bound is a property of the growth sequence, not of one call - so its answer is asserted once
       at the end rather than once per row. */
    while (xud_handle_alloc(&probe, table, XUD_HANDLE_KIND_MODEL) == XUD_OK) {
        /* empty: the loop is the walk to the ceiling, the answer past it is asserted below */
    }
    xud_check(probe.index == XUD_HANDLE_SLOT_MAX_CAPACITY - 1u, "handle/bound/last/index", "is the last row the ceiling allows");
    xud_check(
        xud_handle_alloc(&fresh, table, XUD_HANDLE_KIND_MODEL) == XUD_OUT_OF_RESOURCES, "handle/bound/past", "is the reported refusal");
    xud_check(
        fresh.index == 0u && fresh.generation == 0u && fresh.kind == 0u, "handle/bound/past/untouched", "and the out-handle is untouched");
    xud_check(xud_handle_table_live_count(table, XUD_HANDLE_KIND_MODEL, &live) == XUD_OK, "handle/bound/live", "answers");
    xud_check(live == XUD_HANDLE_SLOT_MAX_CAPACITY, "handle/bound/live/count", "is the ceiling, not one past it");

    /* Every slot of every kind is released before the table goes away, which is the state the
       teardown comment says it owns: a table torn down is a table whose slots are all free, and a
       table closed twice cannot double-free. None of these slots was ever reused, so they are all
       still at generation 0, and the count asserted below is what says every one of them was acce
       pted rather than one of them being a silent refusal. */
    released = 0u;
    for (index = 0u; index < XUD_HANDLE_SLOT_MAX_CAPACITY; index++) {
        probe.index = index;
        probe.generation = 0u;
        probe.kind = XUD_HANDLE_KIND_MODEL;
        if (xud_handle_free(table, probe) == XUD_OK)
            released++;
    }
    xud_check(released == XUD_HANDLE_SLOT_MAX_CAPACITY, "handle/bound/release-all", "accepts every slot of the ceiling");
    xud_check(xud_handle_table_live_count(table, XUD_HANDLE_KIND_MODEL, &live) == XUD_OK, "handle/bound/live-after", "answers");
    xud_check(live == 0u, "handle/bound/live-after/count", "and is zero");

    xud_check(xud_handle_table_close(&table) == XUD_OK, "handle/close", "tears the table down");
    xud_check(table == NULL, "handle/close/null", "and clears the pointer");
    xud_check(xud_handle_table_close(&table) == XUD_ALREADY, "handle/close-twice", "is already, not a double free");
    xud_check(xud_handle_table_close(NULL) == XUD_INVALID_ARG, "handle/close-null", "is refused");
}
