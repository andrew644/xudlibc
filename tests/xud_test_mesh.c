/***********************************************************************************************
 *
 *   xud_test_mesh.c - the mesh rung and the public mesh API above it
 *
 *   Owns : the mesh's own byte layout (the five counts, the five offsets and the arena's total),
 *          its argument check, its caps gate, the five create-infos it stamps, the write that fil
 *          ls its bytes, the record a MESH-kind slot carries, and the public pair xud_upload_mesh
 *          ()/xud_close_mesh() - and nothing else: the handle layer's own validation order is the
 *          handle area's, the allocator's block discipline is the allocator area's, the vertex-in
 *          put binding rows are the batch area's, and the teardown rung xud_close_context() runs a
 *          bove the handle table it closes is the core area's.
 *
 *   Layer: tests, above the batch and shapes areas and below the core area. The pure half runs wi
 *   th no driver present at all - a mesh's arena is a range of bytes, so the write is handed a he
 *   ap buffer instead of one vkMapMemory wrote, and the layout, the offsets, the alignment padding
 *   and the defaults are all answerable with no device in sight. The GPU half runs the same public
 *   pair against a real context, which is where the block, the five binds and the mapping are obs
 *   ervable at all.
 *
 *   What the checks are stated against: the offsets rather than the bytes at them for the layout,
 *   and the bytes at the offsets for the write, so the two halves cannot both be "the same numbers
 *   copied twice" - and the record's live driver state (a non-null arena memory, a non-null mapp
 *   ing, five non-null buffers) rather than a pixel, because a mesh's bytes are its own and the
 *   frame's whole content can be one mesh row: the GPU half draws the same mesh twice through the
 *   public pair and checks the row list and the geometry list at the frame's end, which is where
 *   the rebind of a mesh's buffers beside the arena is observable.
 *
 *   Deterministic: fixed meshes, fixed alignments, fixed loop counts, no time, no randomness, no
 *   filesystem.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include <math.h> /* Required for: NAN, the non-finite element the matrix guard refuses */
#include <stdlib.h> /* Required for: malloc and free, the heap arena the write is handed instead of a mapping */
#include <string.h> /* Required for: memcmp and memset, the bytes the write is checked against */

#include "xud.h" /* Required for: XudResult, XudContext, XudHandle, XudMeshData and the public mesh pair */
#include "xud_allocator.h" /* Required for: XudMemoryRange, xud_allocator_acquire_host_visible - the readback's block */
#include "xud_batch.h" /* Required for: XUD_BATCH_STREAM_*, XUD_BATCH_VERTEX_SIZE, XUD_BATCH_INDEX_SIZE - the widths the layout is read against */
#include "xud_core.h" /* Required for: struct XudContext, whose handleTable/loader/device/allocator the GPU half reaches */
#include "xud_handle.h" /* Required for: XudHandleTable, xud_handle_alloc, xud_handle_table_live_count - the slot this record lives in */
#include "xud_material.h" /* Required for: xud_create_material, the OTHER kind's handle a cross-kind close is refused on */
#include "xud_mesh.h" /* Required for: XudMeshRecord and the rung's own check/layout/caps/write halves */
#include "xud_physical.h" /* Required for: XudDeviceCaps, the synthetic caps the pure half's gate is driven by */
#include "xud_test_harness.h" /* Required for: xud_check, xud_gpu_tests_enabled, the area entry points */

/*-----------------------------------------------------------------------------------------------
 * The one mesh every "the mesh is fine" check below is the same mesh seen through a different
 * refusal: three vertices, one triangle, all four streams brought. The second mesh the pure half
 * builds brings only pos and index, which is the half that tests the defaults.
 *-----------------------------------------------------------------------------------------------*/

static const float xudTestPos[9] = { -0.5f, -0.5f, 0.25f, 0.5f, -0.5f, 0.25f, 0.0f, 0.5f, 0.75f };
static const float xudTestUv[6] = { 0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f };
static const uint8_t xudTestColor[12] = { 200u, 20u, 20u, 255u, 20u, 200u, 20u, 255u, 20u, 20u, 200u, 128u };
static const float xudTestNormal[9] = { 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f };
static const uint32_t xudTestIndex[3] = { 0u, 1u, 2u };

/* The second mesh the pure half lays out: four vertices, two triangles. It is never written - it i
   s the record whose counts disagree with the one-triangle mesh's, which is the pairing check the w
   rite owns.
*/
static const float xudQuadPos[12] = { -0.5f, -0.5f, 0.0f, 0.5f, -0.5f, 0.0f, 0.5f, 0.5f, 0.0f, -0.5f, 0.5f, 0.0f };
static const uint32_t xudQuadIndex[6] = { 0u, 1u, 2u, 2u, 3u, 0u };

/*-----------------------------------------------------------------------------------------------
 * The pure half: the layout, the gate, the write and the refusals, against shells
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_mesh_pure(void) {
    XudMeshRecord laid;
    XudMeshRecord second;
    XudMeshRecord zeroed;
    XudMeshRecord quad;
    VkBufferCreateInfo info;
    XudDeviceCaps caps;
    XudHandleTable* table = NULL;
    XudHandle meshHandle = { 0u, 0u, 0u };
    XudHandle materialHandle = { 0u, 0u, 0u };
    XudHandle foreign = { 0u, 0u, 0u };
    const XudMeshRecord* record = NULL;
    void* rawRecord = NULL;
    uint32_t alignments[XUD_MESH_BUFFER_COUNT];
    uint32_t triangles = 0u;
    uint32_t closed = 0u;
    uint32_t live = 0u;
    uint8_t* arena = NULL;
    uint8_t expected[XUD_BATCH_VERTEX_SIZE * 3u + (XUD_BATCH_INDEX_SIZE * 3u)];
    size_t arenaBytes = 0u;
    size_t guardBytes = 0u;
    size_t index = 0u;
    size_t written = 0u;

    XudMeshData mesh = { .vertexCount = 3u,
        .indexCount = 3u,
        .pos = xudTestPos,
        .texcoords = xudTestUv,
        .color = xudTestColor,
        .normal = xudTestNormal,
        .index = xudTestIndex };
    XudMeshData bare = {
        .vertexCount = 3u, .indexCount = 3u, .pos = xudTestPos, .texcoords = NULL, .color = NULL, .normal = NULL, .index = xudTestIndex
    };
    XudMeshData broken = mesh;

    memset(&laid, 0, sizeof(laid));
    memset(&second, 0, sizeof(second));
    memset(&zeroed, 0, sizeof(zeroed));
    memset(&quad, 0, sizeof(quad));
    memset(&caps, 0, sizeof(caps));

    /*--- the argument check, and what each refusal means ------------------------------*/

    xud_check(xud_mesh_check(NULL, &triangles) == XUD_INVALID_ARG, "mesh/check-null", "no description is refused");
    xud_check(xud_mesh_check(&mesh, NULL) == XUD_OK, "mesh/check-no-out-count",
        "the out-count is optional, so a check that refuses nothing still answers without writing");

    broken = mesh;
    broken.vertexCount = 0u;
    xud_check(xud_mesh_check(&broken, &triangles) == XUD_INVALID_ARG, "mesh/check-no-vertices",
        "a mesh of no vertices is refused, because a zero-length stream is a buffer this rung must n"
        "ot create");
    broken = mesh;
    broken.vertexCount = XUD_MESH_MAX_VERTICES + 1u;
    xud_check(xud_mesh_check(&broken, &triangles) == XUD_INVALID_ARG, "mesh/check-past-bound",
        "one past this rung's own bound is refused here rather than at the block acquire, so the reas"
        "on is stated before a slot is spent");
    broken = mesh;
    broken.indexCount = 2u;
    xud_check(xud_mesh_check(&broken, &triangles) == XUD_INVALID_ARG, "mesh/check-two-indices",
        "two indices are not a triangle, and this rung has one topology");
    broken = mesh;
    broken.indexCount = 4u;
    xud_check(xud_mesh_check(&broken, &triangles) == XUD_INVALID_ARG, "mesh/check-not-a-multiple",
        "four indices are one and a half triangles, which is a value no row of the pipeline list can"
        "name");
    broken = mesh;
    broken.index = NULL;
    xud_check(xud_mesh_check(&broken, &triangles) == XUD_INVALID_ARG, "mesh/check-no-index-array",
        "the index array is the one source with no default: a mesh without indices is a mesh whose t"
        "opology is undefined");
    broken = mesh;
    broken.index = xudTestIndex;
    broken.vertexCount = 2u;
    xud_check(xud_mesh_check(&broken, &triangles) == XUD_INVALID_ARG, "mesh/check-index-out-of-range",
        "an index at or past the vertex count is caught here rather than handed down, because at the "
        "draw it is undefined behaviour rather than a refusal the driver would give");
    xud_check(xud_mesh_check(&mesh, &triangles) == XUD_OK, "mesh/check-ok", "the mesh of three vertices and one triangle is fine");
    xud_check(
        triangles == 1u, "mesh/check-triangle-count", "and the count the accessor fills is indexCount/3, the count raylib's Mesh names");

    /*--- the layout, at this layer's floor and at a driver's own alignments ----------*/

    xud_check(xud_mesh_layout(&mesh, NULL, &laid) == XUD_OK, "mesh/layout-floor", "the layout at the floor alignment answers");
    xud_check(laid.vertexCount == 3u, "mesh/layout-vertex-count", "the one count every stream is sized from");
    xud_check(laid.indexCount == 3u, "mesh/layout-index-count", "and the index count is the caller's, not a capacity");
    xud_check(laid.triangleCount == 1u, "mesh/layout-triangle-count",
        "and the triangle count is derived, so it cannot disagree with the index count");
    xud_check((laid.streamBytes[XUD_BATCH_STREAM_POS] == 36u) && (laid.streamBytes[XUD_BATCH_STREAM_UV] == 24u)
            && (laid.streamBytes[XUD_BATCH_STREAM_COLOR] == 12u) && (laid.streamBytes[XUD_BATCH_STREAM_NORMAL] == 36u),
        "mesh/layout-stream-sizes",
        "and every stream's length is the mesh's ONE vertex count times that stream's element width - "
        "the settled decision's invariant that every stream holds the same number of vertices, true "
        "here by construction rather than by a rule someone has to keep true later");
    xud_check(laid.indexBytes == 12u, "mesh/layout-index-size", "and the index buffer is indexCount at the batch's own index width");
    xud_check((laid.streamOffset[XUD_BATCH_STREAM_POS] == 0u) && (laid.streamOffset[XUD_BATCH_STREAM_UV] == 36u)
            && (laid.streamOffset[XUD_BATCH_STREAM_COLOR] == 60u) && (laid.streamOffset[XUD_BATCH_STREAM_NORMAL] == 72u)
            && (laid.indexOffset == 108u) && (laid.arenaBytes == 120u),
        "mesh/layout-floor-offsets",
        "back to back in slot order at the floor, so the five buffers are one block of 120 B and the "
        "offsets the binds will use and the write will use are the same numbers");

    alignments[XUD_BATCH_STREAM_POS] = 16u;
    alignments[XUD_BATCH_STREAM_UV] = 32u;
    alignments[XUD_BATCH_STREAM_COLOR] = 8u;
    alignments[XUD_BATCH_STREAM_NORMAL] = 16u;
    alignments[XUD_MESH_INDEX_SLOT] = 64u;
    xud_check(xud_mesh_layout(&mesh, alignments, &second) == XUD_OK, "mesh/layout-driver-alignments",
        "the same mesh laid out at the alignments a driver would report answers");
    xud_check(
        (second.streamBytes[XUD_BATCH_STREAM_POS] == laid.streamBytes[XUD_BATCH_STREAM_POS]) && (second.indexBytes == laid.indexBytes),
        "mesh/layout-sizes-alignment-independent",
        "the sizes did not move, which is why the creates run on the first pass and only the binds w"
        "ait for the driver's alignments");
    xud_check((second.streamOffset[XUD_BATCH_STREAM_POS] == 0u) && (second.streamOffset[XUD_BATCH_STREAM_UV] == 64u)
            && (second.streamOffset[XUD_BATCH_STREAM_COLOR] == 88u) && (second.streamOffset[XUD_BATCH_STREAM_NORMAL] == 112u)
            && (second.indexOffset == 192u) && (second.arenaBytes == 204u),
        "mesh/layout-padded-offsets",
        "and the offsets moved where a slot's own alignment demanded, so a layout that assumed an al"
        "ignment would be a layout that bound a buffer at an offset the driver then refused");
    xud_check(second.arenaBytes > laid.arenaBytes, "mesh/layout-padding-is-real",
        "and the padded arena is bigger than the tight one, which is the padding being a fact rather "
        "than a number that happens to match");

    alignments[XUD_MESH_INDEX_SLOT] = 0u;
    xud_check(xud_mesh_layout(&mesh, alignments, &second) == XUD_INVALID_ARG, "mesh/layout-zero-alignment",
        "an alignment of zero is not an alignment - every division below would fault, so the guard is "
        "the helper's own contract stated rather than a policy of the layout");
    alignments[XUD_MESH_INDEX_SLOT] = XUD_MESH_ALIGN_MAX + 1u;
    xud_check(xud_mesh_layout(&mesh, alignments, &second) == XUD_UNSUPPORTED, "mesh/layout-past-ceiling",
        "an alignment past the allocator's own ceiling is a request the block acquire would refuse, "
        "and refusing it here is that reason stated before a block is asked for");
    broken = mesh;
    broken.indexCount = 4u;
    xud_check(xud_mesh_layout(&broken, NULL, &second) == XUD_UNSUPPORTED, "mesh/layout-refuses-a-bad-mesh",
        "and a mesh the argument check refuses is refused by the layout too, so the two pure halves "
        "cannot disagree about what a mesh is");

    xud_check(xud_mesh_layout(&(XudMeshData) { .vertexCount = 4u,
                                  .indexCount = 6u,
                                  .pos = xudQuadPos,
                                  .texcoords = NULL,
                                  .color = NULL,
                                  .normal = NULL,
                                  .index = xudQuadIndex },
                  NULL, &quad)
            == XUD_OK,
        "mesh/layout-quad",
        "the two-triangle mesh is laid out too, so the record the write below refuses against is a re"
        "cord of a real mesh rather than a zeroed struct");
    xud_check(quad.triangleCount == 2u, "mesh/layout-quad-triangles",
        "and its triangle count is two, so the counts the write compares are derived rather than stored");

    /*--- the five create-infos -------------------------------------------------------*/

    xud_check(
        xud_mesh_fill_buffer_info(&laid, XUD_BATCH_STREAM_POS, &info) == XUD_OK, "mesh/info-pos", "the first stream's row is stamped");
    xud_check((info.size == 36u) && (info.usage == VK_BUFFER_USAGE_VERTEX_BUFFER_BIT), "mesh/info-pos-usage",
        "VERTEX_BUFFER alone and no TRANSFER_DST: the four streams are read by the pipeline's attribu"
        "te rows and nothing is copied into them, the write that fills them is the mapping above");
    xud_check(
        xud_mesh_fill_buffer_info(&laid, XUD_MESH_INDEX_SLOT, &info) == XUD_OK, "mesh/info-index", "the index buffer's row is stamped");
    xud_check((info.size == 12u) && (info.usage == VK_BUFFER_USAGE_INDEX_BUFFER_BIT), "mesh/info-index-usage",
        "and the index buffer names the entry point that names it, which is the fifth buffer being a "
        "different use rather than a fifth stream");
    xud_check(xud_mesh_fill_buffer_info(&laid, XUD_MESH_BUFFER_COUNT, &info) == XUD_INVALID_ARG, "mesh/info-past-slot",
        "and a slot past the five is refused, so the create loop and the destroy loop cannot disagree"
        "about how many buffers a mesh has");

    /*--- the caps gate ---------------------------------------------------------------*/

    caps.properties.properties.limits.maxVertexInputBindings = XUD_BATCH_STREAM_COUNT;
    caps.properties.properties.limits.maxVertexInputAttributes = XUD_BATCH_STREAM_COUNT;
    caps.properties.properties.limits.maxVertexInputBindingStride = XUD_BATCH_STREAM_POS_SIZE;
    xud_check(xud_mesh_caps_support(&caps, &mesh) == XUD_OK, "mesh/caps-exactly-enough",
        "a device that answers exactly the four bindings, four attributes and the widest stride is a "
        "device this mesh can be bound on");
    caps.properties.properties.limits.maxVertexInputBindings = XUD_BATCH_STREAM_COUNT - 1u;
    xud_check(xud_mesh_caps_support(&caps, &mesh) == XUD_UNSUPPORTED, "mesh/caps-three-bindings",
        "three bindings is a device a mesh's four streams cannot be bound on at all, and the answer i"
        "s UNSUPPORTED rather than a mesh drawn with one attribute missing");
    caps.properties.properties.limits.maxVertexInputBindings = XUD_BATCH_STREAM_COUNT;
    caps.properties.properties.limits.maxVertexInputBindingStride = XUD_BATCH_STREAM_COLOR_SIZE;
    xud_check(xud_mesh_caps_support(&caps, &mesh) == XUD_UNSUPPORTED, "mesh/caps-narrow-stride",
        "and a stride narrower than the widest stream is refused rather than a narrower format chosen "
        "behind the caller's back - the repo's rule that an unverified capability stays off");
    xud_check(xud_mesh_caps_support(NULL, &mesh) == XUD_INVALID_ARG, "mesh/caps-no-caps", "no caps is a bad argument");

    /*--- the write, into a heap arena rather than into a mapping ---------------------*/

    arenaBytes = (size_t)laid.arenaBytes;
    guardBytes = 64u;
    arena = malloc(arenaBytes + guardBytes);
    xud_check(arena != NULL, "mesh/write-arena-allocated", "the write is handed a heap buffer instead of one vkMapMemory wrote");
    if (arena != NULL) {
        memset(arena, 0xAAu, arenaBytes + guardBytes);
        xud_check(xud_mesh_write_arena(&laid, &mesh, arena, arenaBytes) == XUD_OK, "mesh/write-ok", "the write fills the arena");

        memcpy(expected, xudTestPos, sizeof(xudTestPos));
        memcpy(expected + 36u, xudTestUv, sizeof(xudTestUv));
        memcpy(expected + 60u, xudTestColor, sizeof(xudTestColor));
        memcpy(expected + 72u, xudTestNormal, sizeof(xudTestNormal));
        memcpy(expected + 108u, xudTestIndex, sizeof(xudTestIndex));
        xud_check(memcmp(arena, expected, arenaBytes) == 0, "mesh/write-bytes-match",
            "and every byte of the arena is the byte the caller brought at the offset the layout nam"
            "ed, so the write and the layout are one set of numbers rather than two");

        written = 0u;
        for (index = arenaBytes; index < (arenaBytes + guardBytes); index++) {
            if (arena[index] != 0xAAu) {
                written = 1u;
            }
        }
        xud_check(written == 0u, "mesh/write-stayed-inside",
            "and nothing was written past arenaBytes, which is the one thing a write into a mapping has t"
            "o guarantee about the block it was given");

        memset(arena, 0xAAu, arenaBytes + guardBytes);
        xud_check(xud_mesh_write_arena(&laid, &bare, arena, arenaBytes) == XUD_OK, "mesh/write-defaults-ok",
            "the same arena written from a mesh that brought only pos and index");
        xud_check(memcmp(arena, xudTestPos, sizeof(xudTestPos)) == 0, "mesh/write-defaults-pos",
            "the stream that WAS brought is still the caller's bytes");
        xud_check(memcmp(arena + 36u, xudTestUv, sizeof(xudTestUv)) != 0, "mesh/write-defaults-uv",
            "and the streams that were not are not the caller's - there is no caller's for them");
        xud_check((arena[36u] == 0u) && (arena[37u] == 0u) && (arena[38u] == 0u) && (arena[39u] == 0u), "mesh/write-defaults-uv-zero",
            "a missing uv stream is a zero uv stream, which is a mesh that draws flat-shaded rather than a mesh that does not exist");
        xud_check((arena[60u] == 255u) && (arena[61u] == 255u) && (arena[62u] == 255u) && (arena[63u] == 255u),
            "mesh/write-defaults-color-white",
            "and a missing colour stream is opaque white, the neutral value - the batch's per-draw col"
            "our has no equivalent at a mesh, so the neutral has to be in the stream");
        xud_check(memcmp(arena + 72u, xudTestNormal, sizeof(xudTestNormal)) != 0, "mesh/write-defaults-normal-not-callers",
            "and the normal stream is not the first mesh's, so the two writes did not share a source");
        xud_check((arena[80u] == 0u) && (arena[81u] == 0u) && (arena[82u] == 0x80u) && (arena[83u] == 0x3Fu),
            "mesh/write-defaults-normal-plus-z",
            "and a missing normal is +Z unit rather than zero: the frag stage renormalizes what it re"
            "ads and refuses to divide by zero on a zero vector, so a default normal has to be a unit "
            "one");

        xud_check(xud_mesh_write_arena(&laid, &mesh, NULL, arenaBytes) == XUD_INVALID_ARG, "mesh/write-no-arena",
            "an arena with nowhere to be written is refused, so the write cannot be a write into noth"
            "ing that answers OK");
        xud_check(xud_mesh_write_arena(&laid, &mesh, arena, arenaBytes - 1u) == XUD_INVALID_ARG, "mesh/write-destination-too-small",
            "and a destination one byte short of the layout's own total is refused rather than writin"
            "g past it, which is the one thing a write into a mapping has to guarantee about the block"
            " it was given");
        xud_check(xud_mesh_write_arena(&quad, &mesh, arena, arenaBytes) == XUD_INVALID_ARG, "mesh/write-counts-disagree",
            "and a record laid out for a two-triangle mesh used to write the one-triangle mesh's arr"
            "ays is refused, because the write sizes its loops from the record and reads its arrays f"
            "rom the description - the two have to name one mesh");
        xud_check(xud_mesh_write_arena(&zeroed, &mesh, arena, arenaBytes) == XUD_UNSUPPORTED, "mesh/write-unlaid-record",
            "and a record whose arena was never laid out is refused rather than writing five streams "
            "at the same offset");
    }
    if (arena != NULL) {
        free(arena);
    }

    /*--- the slot, the accessor and the cross-kind refusal ---------------------------*/

    xud_check(xud_handle_table_open(&table) == XUD_OK, "mesh/table-opened", "a table with no MESH array yet is opened");
    xud_check(xud_mesh_get(table, meshHandle, &record) == XUD_INVALID_ARG, "mesh/get-no-array",
        "a kind whose array was never allocated names nothing, so the accessor refuses rather than r"
        "eading a NULL array");

    xud_check(xud_handle_alloc(&meshHandle, table, XUD_HANDLE_KIND_MESH) == XUD_OK, "mesh/slot-allocated",
        "a MESH slot is taken - the slot half of this rung's own bring-up, which is host-side and so "
        "runs with no driver present");
    xud_check(meshHandle.kind == XUD_HANDLE_KIND_MESH, "mesh/slot-kind", "and the handle names the MESH kind");
    xud_check(xud_handle_record_get(table, XUD_HANDLE_KIND_MESH, meshHandle.index, &rawRecord) == XUD_OK, "mesh/slot-record-sentinel",
        "and the slot's record field is still the sentinel, because the record is the driver half's a"
        "llocation and nothing above has run");
    xud_check(xud_mesh_get(table, meshHandle, &record) == XUD_INVALID_ARG, "mesh/get-sentinel-record",
        "and the accessor refuses a live slot whose record is the sentinel rather than handing back a"
        "NULL record every caller reads");

    xud_check(xud_handle_alloc(&materialHandle, table, XUD_HANDLE_KIND_MATERIAL) == XUD_OK, "mesh/foreign-slot",
        "a MATERIAL slot in the SAME table, so the two handles share an index space and only the tag "
        "separates them");
    xud_check(xud_mesh_get(table, materialHandle, &record) == XUD_INVALID_ARG, "mesh/get-cross-kind",
        "and a handle of another kind is refused, which is the tag check the handle layer's own assert"
        "ion states and the reason the public call validates rather than trusting the index");
    foreign = meshHandle;
    foreign.generation = 1u;
    xud_check(xud_mesh_get(table, foreign, &record) == XUD_INVALID_ARG, "mesh/get-stale",
        "and a handle kept across the release that freed the slot it names is refused by the generat"
        "ion bump rather than the index");

    xud_check(xud_handle_table_live_count(table, XUD_HANDLE_KIND_MESH, &live) == XUD_OK, "mesh/live-count-query", "the live count answers");
    xud_check(live == 1u, "mesh/live-count-one", "and one MESH slot is live, which is the handle layer's count rather than this rung's");

    /* A closed loader is the reason a driver half cannot run, and it is reported rather than a mesh
       being half-torn-down: the loader in hand here is a shell, so every driver half of this rung
       answers UNSUPPORTED before it touches the table.
    */
    xud_check(xud_mesh_close(table, meshHandle, NULL, NULL, NULL) == XUD_INVALID_ARG, "mesh/close-null-rungs",
        "a close with no loader, device or allocator is a bad argument");
    xud_check(xud_mesh_close_all(table, NULL, NULL, NULL, &closed) == XUD_INVALID_ARG, "mesh/close-all-null-rungs",
        "and the teardown rung refuses the same way");
    xud_check(xud_handle_record_get(table, XUD_HANDLE_KIND_MESH, meshHandle.index, &rawRecord) == XUD_OK,
        "mesh/close-refusal-left-the-record",
        "and a refused close freed nothing, so the slot still names what it named before - a refused "
        "release is a no-op rather than a half-release");
    xud_check(closed == 0u, "mesh/close-all-refused-count",
        "and the out-count is zero on a refusal, so a caller cannot read a count of records that are still live");

    (void)xud_handle_table_close(&table);
}

/*-----------------------------------------------------------------------------------------------
 * The GPU half: the public pair against a real context
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_mesh_gpu(void) {
    XudContext* context = NULL;
    XudResult result = XUD_OK;
    XudHandle handle = { 0u, 0u, 0u };
    XudHandle second = { 0u, 0u, 0u };
    XudHandle material = { 0u, 0u, 0u };
    XudHandle foreign = { 0u, 0u, 0u };
    const XudMeshRecord* record = NULL;
    VkDeviceSize firstArenaBytes = 0u;
    uint32_t live = 0u;
    uint32_t triangles = 0u;
    float poseMatrix[XUD_BATCH_MODEL_MATRIX_COUNT];
    float nanMatrix[XUD_BATCH_MODEL_MATRIX_COUNT];
    XudColor tint;
    uint32_t element = 0u;
    uint32_t mismatched = 0u;

    result = xud_init_context(&context, 64u, 64u, "xud_test");
    xud_check(result == XUD_OK, "gpu-mesh/bringup-completes",
        "this machine has a Vulkan loader and a window system, so the ladder must complete");
    if (context == NULL) {
        return;
    }

    xud_check(context->handleTable != NULL, "gpu-mesh/table-owned",
        "a context that completed owns the handle table the MESH kind's row lives in");
    xud_check(xud_handle_table_live_count(context->handleTable, XUD_HANDLE_KIND_MESH, &live) == XUD_OK, "gpu-mesh/live-at-open-query",
        "the live count answers");
    xud_check(live == 0u, "gpu-mesh/live-at-open", "and is zero, so a fresh context has no mesh and no slot of the kind has been taken");

    /* The refusals against a real context, before any block is spent. */
    xud_check(
        xud_upload_mesh(NULL, context, &(XudMeshData) { .vertexCount = 3u, .indexCount = 3u, .pos = xudTestPos, .index = xudTestIndex })
            == XUD_INVALID_ARG,
        "gpu-mesh/upload-null-out", "a handle with nowhere to be written is refused");
    xud_check(xud_upload_mesh(&handle, context, NULL) == XUD_INVALID_ARG, "gpu-mesh/upload-null-mesh", "no description is refused");
    xud_check(
        xud_upload_mesh(&handle, context, &(XudMeshData) { .vertexCount = 3u, .indexCount = 2u, .pos = xudTestPos, .index = xudTestIndex })
            == XUD_INVALID_ARG,
        "gpu-mesh/upload-two-indices", "and a mesh that is not a whole number of triangles is refused BEFORE a slot or a block is spent");
    xud_check(
        xud_upload_mesh(&handle, context, &(XudMeshData) { .vertexCount = 2u, .indexCount = 3u, .pos = xudTestPos, .index = xudTestIndex })
            == XUD_INVALID_ARG,
        "gpu-mesh/upload-index-out-of-range", "and an index at or past the vertex count is refused before the same spend");
    xud_check(xud_handle_table_live_count(context->handleTable, XUD_HANDLE_KIND_MESH, &live) == XUD_OK,
        "gpu-mesh/live-after-refusals-query", "the live count answers again");
    xud_check(live == 0u, "gpu-mesh/live-after-refusals", "and is still zero, so every refusal above left no half-mesh behind");

    xud_check(xud_upload_mesh(&handle, context,
                  &(XudMeshData) { .vertexCount = 3u,
                      .indexCount = 3u,
                      .pos = xudTestPos,
                      .texcoords = xudTestUv,
                      .color = xudTestColor,
                      .normal = xudTestNormal,
                      .index = xudTestIndex })
            == XUD_OK,
        "gpu-mesh/upload-ok", "the full mesh is uploaded against a real device");
    xud_check(handle.kind == XUD_HANDLE_KIND_MESH, "gpu-mesh/upload-kind",
        "and the handle names the MESH kind, which is what makes a cross-kind close a caught runtime error rather than a layout "
        "coincidence");
    xud_check(
        xud_mesh_get(context->handleTable, handle, &record) == XUD_OK, "gpu-mesh/get-ok", "the record reads back through the accessor");
    xud_check((record != NULL) && (record->vertexCount == 3u) && (record->indexCount == 3u) && (record->triangleCount == 1u),
        "gpu-mesh/record-counts", "and the counts are the caller's, so the record says what was uploaded rather than what was asked for");
    firstArenaBytes = (record != NULL) ? record->arenaBytes : 0u;
    xud_check(record->arenaRange.memory != VK_NULL_HANDLE, "gpu-mesh/arena-memory",
        "and the arena's block is a real VkDeviceMemory, which is the one block this mesh's five buf"
        "fers live in - the host-visible acquire rather than the suballocating one, because a mapping "
        "is per-VkDeviceMemory rather than per-range");
    xud_check(record->arenaMappedLive == 1u, "gpu-mesh/arena-mapped-live",
        "and the mapping is live, which is the state that says the mesh's bytes were written once, he"
        "re, and that they will never be written again");
    xud_check((record->buffer[XUD_BATCH_STREAM_POS] != VK_NULL_HANDLE) && (record->buffer[XUD_MESH_INDEX_SLOT] != VK_NULL_HANDLE),
        "gpu-mesh/buffers-created",
        "and the streams and the index buffer are real Vulkan objects, so a mesh created today owns t"
        "he driver objects a material's record does not");
    xud_check(record->bindOffset[XUD_BATCH_STREAM_POS] == record->arenaRange.offset, "gpu-mesh/first-bind-at-block-base",
        "the first stream is bound at the block's own base rather than at zero, which is what makes a "
        "block that does not start at zero still a block whose bytes are addressed correctly");
    xud_check(record->bindOffset[XUD_MESH_INDEX_SLOT] == (record->arenaRange.offset + record->indexOffset), "gpu-mesh/index-bind-offset",
        "and the index buffer is bound at the block's base plus the layout's own offset, so the bind "
        "and the write are reading one set of numbers");
    xud_check(xud_handle_table_live_count(context->handleTable, XUD_HANDLE_KIND_MESH, &live) == XUD_OK, "gpu-mesh/live-after-query",
        "the live count answers");
    xud_check(live == 1u, "gpu-mesh/live-after", "and moved to one");

    xud_check(
        xud_upload_mesh(&second, context, &(XudMeshData) { .vertexCount = 3u, .indexCount = 3u, .pos = xudTestPos, .index = xudTestIndex })
            == XUD_OK,
        "gpu-mesh/upload-second-ok",
        "a second mesh is uploaded, bringing only pos and index so the defaults run against a real device too");
    xud_check(second.index == handle.index + 1u, "gpu-mesh/second-index",
        "and the index moved, so the two meshes are two records and not one record restated");
    xud_check(xud_mesh_get(context->handleTable, second, &record) == XUD_OK, "gpu-mesh/second-get-ok", "the second record reads back");
    xud_check((record != NULL) && (record->arenaBytes == firstArenaBytes), "gpu-mesh/second-arena",
        "and its arena is the same size as the first mesh's, so the defaults cost no extra bytes and t"
        "he two meshes of the same counts, on the same driver, got the same layout - the size is the "
        "driver's alignments' rather than this layer's floor, which is why it is compared rather than "
        "stated");
    xud_check(xud_handle_table_live_count(context->handleTable, XUD_HANDLE_KIND_MESH, &live) == XUD_OK, "gpu-mesh/live-two-query",
        "the live count answers");
    xud_check(live == 2u, "gpu-mesh/live-two", "and two MESH slots are live");

    /* The draw half, against a real device: the second mesh is still live in a slot here, so the
       public draw has a handle to name, and nothing was appended to the batch's arena, so this is
       the frame whose whole content is a mesh row - the case an empty arena used to be. The mesh is
       drawn twice, which is the dedup's own case: two rows over one geometry entry.
    */
    xud_check(xud_begin_drawing(context) == XUD_OK, "gpu-mesh/frame-opens", "the frame opens");
    xud_check(xud_draw_mesh(context, second) == XUD_OK, "gpu-mesh/draw-mesh-ok",
        "the first public draw that names a handle records a row that replays through the mesh's own five buffers");
    xud_check(context->renderer->batch->drawCount == 1u, "gpu-mesh/draw-recorded-mid-frame",
        "and the row is already a row mid-frame rather than a pending run, because a mesh has no append "
        "state to accumulate into");
    xud_check(context->renderer->batch->geometryCount == 1u, "gpu-mesh/draw-one-geometry-entry", "one geometry entry for the one mesh");
    xud_check(xud_draw_mesh(context, second) == XUD_OK, "gpu-mesh/draw-mesh-twice", "the same mesh drawn again records a row again");
    xud_check(context->renderer->batch->drawCount == 2u, "gpu-mesh/draw-two-rows", "and two rows");
    xud_check(context->renderer->batch->geometryCount == 1u, "gpu-mesh/draw-deduped-geometry",
        "but still one geometry entry, so the frame loop rebinds once rather than twice - which is what "
        "a frame that draws one model a hundred times pays for");

    /* The posed pair: the guards are the matrix's rather than the mesh's, so they are checked
       against the same live handle, and the draw that passes them stamps the row rather than the
       identity, which is what the frame loop's push row reads out. */
    memset(poseMatrix, 0, sizeof(poseMatrix));
    poseMatrix[0] = 1.0f;
    poseMatrix[5] = 1.0f;
    poseMatrix[10] = 1.0f;
    poseMatrix[12] = 0.5f;
    poseMatrix[13] = -1.0f;
    poseMatrix[15] = 1.0f;
    tint = (XudColor) { .r = 255u, .g = 0u, .b = 0u, .a = 128u };
    xud_check(xud_draw_mesh_ex(NULL, second, poseMatrix, tint) == XUD_INVALID_ARG, "gpu-mesh/draw-ex-null-context",
        "a posed draw with no context is refused");
    xud_check(xud_draw_mesh_ex(context, second, NULL, tint) == XUD_INVALID_ARG, "gpu-mesh/draw-ex-null-matrix",
        "and a matrix with no elements behind it is refused before the handle is read");
    memcpy(nanMatrix, poseMatrix, sizeof(nanMatrix));
    nanMatrix[14] = NAN;
    xud_check(xud_draw_mesh_ex(context, second, nanMatrix, tint) == XUD_INVALID_ARG, "gpu-mesh/draw-ex-nan-matrix",
        "and a non-finite element is the same refusal, because a matrix that is not a number is not a transform a vertex stage could "
        "apply");
    xud_check(xud_draw_mesh_ex(context, second, poseMatrix, tint) == XUD_OK, "gpu-mesh/draw-ex-ok",
        "the posed draw records a row the same way the plain one does");
    xud_check(context->renderer->batch->drawCount == 3u, "gpu-mesh/draw-ex-third-row", "and it is the frame's third row");
    for (element = 0u; element < XUD_BATCH_MODEL_MATRIX_COUNT; element++) {
        if (context->renderer->batch->draws[context->renderer->batch->drawCount - 1u].modelMatrix[element] != poseMatrix[element]) {
            mismatched++;
        }
    }
    xud_check(mismatched == 0u, "gpu-mesh/draw-ex-row-stamped-with-the-matrix",
        "and the row carries the caller's matrix element for element, which is the stamp the frame loop's push row reads out");
    xud_check((context->renderer->batch->draws[context->renderer->batch->drawCount - 1u].tint[0] == (255.0f / 255.0f))
            && (context->renderer->batch->draws[context->renderer->batch->drawCount - 1u].tint[1] == 0.0f)
            && (context->renderer->batch->draws[context->renderer->batch->drawCount - 1u].tint[2] == 0.0f)
            && (context->renderer->batch->draws[context->renderer->batch->drawCount - 1u].tint[3] == (128.0f / 255.0f)),
        "gpu-mesh/draw-ex-row-stamped-with-the-tint",
        "and the row carries the caller's tint the same way, the four channels the fragment stage will spend");
    result = xud_end_drawing(context);
    xud_check(result == XUD_OK, "gpu-mesh/frame-presents",
        "and the frame presents, so the rebind of a mesh's buffers beside the arena is a command buffer "
        "the validation layer accepted");
    xud_check(context->renderer->batch->drawCount == 0u, "gpu-mesh/frame-end-rows-empty", "the row list is empty at the frame's end");
    xud_check(context->renderer->batch->geometryCount == 0u, "gpu-mesh/frame-end-geometry-retired",
        "and the geometry list is retired with it, because it is indexed by rows recorded after the reset");

    /* The cross-kind close, refused before anything is destroyed. */
    xud_check(xud_create_material(&material, context, (XudMaterial) { .blendMode = XUD_BLEND_OPAQUE }) == XUD_OK,
        "gpu-mesh/material-created", "a MATERIAL slot in the same table");
    xud_check(xud_close_mesh(context, material) == XUD_INVALID_ARG, "gpu-mesh/close-cross-kind",
        "and closing it as a mesh is refused, so a handle of another kind cannot destroy a record of "
        "that other kind");
    foreign = handle;
    foreign.generation = handle.generation + 1u;
    xud_check(xud_close_mesh(context, foreign) == XUD_INVALID_ARG, "gpu-mesh/close-stale",
        "and a handle kept across the close of the mesh it names is refused by the generation bump");
    xud_check(xud_handle_table_live_count(context->handleTable, XUD_HANDLE_KIND_MESH, &live) == XUD_OK, "gpu-mesh/live-still-two-query",
        "the live count answers");
    xud_check(live == 2u, "gpu-mesh/live-still-two",
        "and both refusals destroyed nothing, so a refused release is a no-op rather than a half-release");

    xud_check(xud_close_mesh(context, handle) == XUD_OK, "gpu-mesh/close-ok", "the first mesh is released");
    xud_check(xud_mesh_get(context->handleTable, handle, &record) == XUD_INVALID_ARG, "gpu-mesh/close-freed-the-slot",
        "and the handle no longer names a live slot, so a mesh kept past its close is refused rather "
        "than dangling");
    xud_check(xud_handle_table_live_count(context->handleTable, XUD_HANDLE_KIND_MESH, &live) == XUD_OK, "gpu-mesh/live-after-close-query",
        "the live count answers");
    xud_check(live == 1u, "gpu-mesh/live-after-close", "and one MESH slot is live, the second mesh still in hand");

    /* The teardown rung above the handle table's own close, run by xud_close_context(): the second
       mesh is still live in a slot at this point, so the rung has one record to destroy and the all
       ocator's close afterwards has one block to free.
    */
    xud_check(xud_mesh_close_all(context->handleTable, context->loader, context->device, context->allocator, &live) == XUD_OK,
        "gpu-mesh/close-all-explicit", "the rung runs and closes the one MESH record a slot still names");
    xud_check(live == 1u, "gpu-mesh/close-all-count", "and the count it reports is the one record that was still reachable");
    xud_check(xud_mesh_close_all(context->handleTable, context->loader, context->device, context->allocator, &triangles) == XUD_OK,
        "gpu-mesh/close-all-idempotent",
        "and running it again closes nothing, because every record pointer was cleared by the run abo"
        "ve - the rung is idempotent rather than a double destroy");
    xud_check(triangles == 0u, "gpu-mesh/close-all-second-count", "and the second count is zero");

    result = xud_close_context(&context);
    xud_check((result == XUD_OK) || (result == XUD_ALREADY), "gpu-mesh/close-context",
        "the context closes with the mesh rung's teardown inside it");
    xud_check(context == NULL, "gpu-mesh/context-cleared", "and the context pointer is cleared, so a close run twice cannot double-free");
}

/*-----------------------------------------------------------------------------------------------
 * The instanced GPU half: ONE draw row whose two instance rows place the same quad at two screen
 * columns, read back as pixels. This is the whole claim of instancing seen from outside: one
 * vkCmdDrawIndexed, two copies, each at its own matrix and its own tint. The probes are three
 * columns of the middle row - the left copy's centre, the gap between them, the right copy's
 * centre - so a draw that ignored the instance matrices and drew both at the pushed identity
 * fails the side probes, a draw that misindexed the matrices puts the wrong colour on one side,
 * and a draw that read the matrices but multiplied in the STALE push row lands the quads
 * elsewhere or nowhere. The byte arithmetic is the rendertarget area's: ACES at the resolve, the
 * sRGB encode on top when the swapchain is SRGB.
 *-----------------------------------------------------------------------------------------------*/

#define XUD_TEST_INST_WINDOW 192u /* the square window; the three probe columns are 60/96/132 */
#define XUD_TEST_INST_TOLERANCE 8u /* the compositor's own drift, the same order the rendertarget probes carry */

static float xud_test_inst_aces(float c) {
    const float t = (c * (2.51f * c + 0.03f)) / (c * (2.43f * c + 0.59f) + 0.14f);

    return (t < 0.0f) ? 0.0f : ((t > 1.0f) ? 1.0f : t);
}

static uint32_t xud_test_inst_srgb_byte(uint8_t value) {
    const float c = xud_test_inst_aces((float)value / 255.0f);
    const float e = (c <= 0.0031308f) ? (12.92f * c) : (1.055f * powf(c, 1.0f / 2.4f) - 0.055f);

    return (uint32_t)(e * 255.0f + 0.5f);
}

static uint32_t xud_test_inst_aces_byte(uint8_t value) { return (uint32_t)(xud_test_inst_aces((float)value / 255.0f) * 255.0f + 0.5f); }

/* One frame of the probe scene: the identity-of-the-camera pair looking down -Z from (0,0,4) with
   a 90-degree aperture, so the z=0 plane maps linearly to NDC - x=±1.5 world is x=±0.375 NDC -
   and the whole content is the one instanced row. */
static void xudTestInstFrame(XudContext* context, XudHandle quad, const float* matrices, const XudColor* tints) {
    (void)xud_begin_drawing(context);
    (void)xud_clear_background(context, (XudColor) { .r = 0u, .g = 0u, .b = 0u, .a = 255u });
    (void)xud_begin_camera_3d(context,
        (XudCamera3D) { .position = { 0.0f, 0.0f, 4.0f },
            .target = { 0.0f, 0.0f, 0.0f },
            .up = { 0.0f, 1.0f, 0.0f },
            .fovy = 90.0f,
            .near = 0.1f,
            .far = 100.0f });
    (void)xud_draw_mesh_instanced(context, quad, matrices, tints, 2u);
    (void)xud_end_camera_3d(context);
    (void)xud_end_drawing(context);
}

static void xud_test_mesh_instanced_gpu(void) {
    XudContext* context = NULL;
    XudResult result = XUD_OK;
    XudHandle quad = { 0u, 0u, 0u };
    VkResult vk = VK_SUCCESS;
    float matrices[2u * XUD_BATCH_MODEL_MATRIX_COUNT];
    XudColor tints[2];
    uint32_t midRows = 0u;
    uint32_t midInstances = 0u;
    uint32_t frame = 0u;
    uint32_t acquireTry = 0u;
    VkBuffer readback = VK_NULL_HANDLE;
    XudMemoryRange readbackRange;
    VkMemoryRequirements readbackReq;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkFence acquireFence = VK_NULL_HANDLE;
    uint8_t* pixels = NULL;
    uint32_t acquired = 0u;

    result = xud_init_context(&context, XUD_TEST_INST_WINDOW, XUD_TEST_INST_WINDOW, "xud_test");
    xud_check(result == XUD_OK, "mesh-instanced/bringup", "the probe scene needs a completed context");
    if (context == NULL) {
        return;
    }

    /* The two instance rows: identity rotations translated to x = -1.5 (tint red) and x = +1.5
       (tint blue). With the camera below the z=0 plane maps xNDC = x / (4 * aspect), so the two
       centres are NDC -0.375 and +0.375 on a square window - pixel columns 60 and 132 of 192. */
    memset(matrices, 0, sizeof matrices);
    matrices[0] = 1.0f;
    matrices[5] = 1.0f;
    matrices[10] = 1.0f;
    matrices[15] = 1.0f;
    matrices[12] = -1.5f;
    matrices[16] = 1.0f;
    matrices[21] = 1.0f;
    matrices[26] = 1.0f;
    matrices[31] = 1.0f;
    matrices[16 + 12] = 1.5f;
    tints[0] = (XudColor) { .r = 255u, .g = 0u, .b = 0u, .a = 255u };
    tints[1] = (XudColor) { .r = 0u, .g = 0u, .b = 255u, .a = 255u };

    result
        = xud_upload_mesh(&quad, context, &(XudMeshData) { .vertexCount = 4u, .indexCount = 6u, .pos = xudQuadPos, .index = xudQuadIndex });
    xud_check(
        result == XUD_OK, "mesh-instanced/quad-uploaded", "the twice-instanced quad is uploaded, white colour and +Z normal by default");

    /* The row's own shape mid-frame: ONE row, TWO instance rows, ONE geometry entry. A public call
       that degraded into two plain draws would double the row count here, which is the very cost
       instancing exists to remove. */
    xud_check(xud_begin_drawing(context) == XUD_OK, "mesh-instanced/frame-opens", "the frame opens");
    xud_check(xud_draw_mesh_instanced(context, quad, matrices, tints, 2u) == XUD_OK, "mesh-instanced/draw-recorded",
        "the public instanced draw records");
    midRows = context->renderer->batch->drawCount;
    midInstances = context->renderer->batch->instanceCount;
    xud_check(midRows == 1u, "mesh-instanced/one-row", "and it is ONE draw row, not two");
    xud_check(midInstances == 2u, "mesh-instanced/two-instance-rows",
        "with two instance rows behind it - the cursor the frame loop's firstInstance reads for the row");
    xud_check(context->renderer->batch->geometryCount == 1u, "mesh-instanced/one-geometry-entry",
        "and one geometry entry, the quad's five buffers deduped as for the plain mesh draw");
    xud_check(xud_end_drawing(context) == XUD_OK, "mesh-instanced/frame-presents",
        "the frame with the instanced row presents, so the instance binds and the instance-flagged push row are commands "
        "the validation layer accepted");

    /* The window's late tile: the rendertarget area's spin, so no probe reads an image of an extent
       that moved after it was drawn. */
    {
        uint32_t stableFrames = 0u;
        uint32_t spinFrames = 0u;
        uint32_t lastWidth = context->swapchain->createInfo.imageExtent.width;
        uint32_t lastHeight = context->swapchain->createInfo.imageExtent.height;
        while ((stableFrames < 12u) && (spinFrames < 600u)) {
            xudTestInstFrame(context, quad, matrices, tints);
            if ((context->swapchain->createInfo.imageExtent.width == lastWidth)
                && (context->swapchain->createInfo.imageExtent.height == lastHeight)) {
                stableFrames++;
            } else {
                stableFrames = 0u;
                lastWidth = context->swapchain->createInfo.imageExtent.width;
                lastHeight = context->swapchain->createInfo.imageExtent.height;
            }
            spinFrames++;
        }
    }
    for (frame = 0u; frame < (context->swapchain->imageCount + 1u); frame++) {
        xudTestInstFrame(context, quad, matrices, tints);
    }

    const VkFenceCreateInfo acquireFenceInfo
        = (VkFenceCreateInfo) { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = NULL, .flags = 0u };
    (void)context->loader->deviceTable.vkCreateFence(context->device->device, &acquireFenceInfo, NULL, &acquireFence);
    for (acquireTry = 0u; acquireTry < 8u; acquireTry++) {
        (void)context->loader->deviceTable.vkResetFences(context->device->device, 1u, &acquireFence);
        vk = context->loader->deviceTable.vkAcquireNextImageKHR(
            context->device->device, context->swapchain->swapchain, UINT64_MAX, VK_NULL_HANDLE, acquireFence, &acquired);
        if ((vk != VK_ERROR_OUT_OF_DATE_KHR) && (vk != VK_SUBOPTIMAL_KHR)) {
            break;
        }
        for (frame = 0u; frame < (context->swapchain->imageCount + 1u); frame++) {
            xudTestInstFrame(context, quad, matrices, tints);
        }
    }
    if (vk != VK_SUCCESS) {
        (void)context->loader->deviceTable.vkDestroyFence(context->device->device, acquireFence, NULL);
        (void)xud_close_context(&context);
        return;
    }
    (void)context->loader->deviceTable.vkWaitForFences(context->device->device, 1u, &acquireFence, VK_TRUE, UINT64_MAX);
    (void)context->loader->deviceTable.vkDestroyFence(context->device->device, acquireFence, NULL);
    {
        const uint32_t width = context->swapchain->createInfo.imageExtent.width;
        const uint32_t height = context->swapchain->createInfo.imageExtent.height;
        const VkFormat format = context->swapchain->createInfo.imageFormat;
        const uint32_t blueIndex = (format == VK_FORMAT_B8G8R8A8_SRGB) || (format == VK_FORMAT_B8G8R8A8_UNORM) ? 0u : 2u;
        const uint32_t redIndex = (blueIndex == 0u) ? 2u : 0u;
        const uint32_t fullByte = ((format == VK_FORMAT_R8G8B8A8_SRGB) || (format == VK_FORMAT_B8G8R8A8_SRGB))
            ? xud_test_inst_srgb_byte(255u)
            : xud_test_inst_aces_byte(255u);
        const uint32_t zeroByte = xud_test_inst_aces_byte(0u);
        const float aspect = (float)width / (float)height;
        const uint32_t row = height / 2u;
        const uint32_t leftCol = (uint32_t)((((1.0f + (-1.5f / (4.0f * aspect))) * 0.5f) * (float)width) + 0.5f);
        const uint32_t rightCol = (uint32_t)((((1.0f + (1.5f / (4.0f * aspect))) * 0.5f) * (float)width) + 0.5f);
        const uint32_t middleCol = width / 2u;
        VkImage image = context->swapchain->images[acquired];
        VkBufferCreateInfo bufferInfo;
        VkCommandPoolCreateInfo poolInfo;
        VkCommandBufferAllocateInfo allocationInfo;
        VkCommandBufferBeginInfo beginInfo;
        VkImageMemoryBarrier imageBarrier;
        VkImageSubresourceLayers subresource;
        VkBufferImageCopy region;
        VkSubmitInfo submitInfo;
        VkFenceCreateInfo fenceInfo;

        xud_check((width >= XUD_TEST_INST_WINDOW) && (height >= XUD_TEST_INST_WINDOW), "mesh-instanced/extent",
            "the presented extent keeps all three probe columns in view");
        xud_check((format == VK_FORMAT_R8G8B8A8_SRGB) || (format == VK_FORMAT_B8G8R8A8_SRGB) || (format == VK_FORMAT_R8G8B8A8_UNORM)
                || (format == VK_FORMAT_B8G8R8A8_UNORM),
            "mesh-instanced/format", "and the format is one of the four 8888s the byte arithmetic below reads");

        memset(&bufferInfo, 0, sizeof bufferInfo);
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = (VkDeviceSize)width * (VkDeviceSize)height * 4u;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vk = context->loader->deviceTable.vkCreateBuffer(context->device->device, &bufferInfo, NULL, &readback);
        xud_check(vk == VK_SUCCESS, "mesh-instanced/buffer", "the destination buffer is created");
        context->loader->deviceTable.vkGetBufferMemoryRequirements(context->device->device, readback, &readbackReq);
        {
            const VkMemoryType* memTypes = context->physicalDevice->caps.memoryProperties.memoryProperties.memoryTypes;
            const uint32_t memTypeCount = context->physicalDevice->caps.memoryProperties.memoryProperties.memoryTypeCount;
            uint32_t hostVisibleMask = 0u;
            uint32_t type = 0u;

            for (type = 0u; type < memTypeCount; type++) {
                if ((memTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0u) {
                    hostVisibleMask |= (1u << type);
                }
            }
            readbackReq.memoryTypeBits &= hostVisibleMask;
        }
        result = xud_allocator_acquire_host_visible(
            &readbackRange, context->allocator, context->loader, context->device, &context->physicalDevice->caps, &readbackReq);
        xud_check(result == XUD_OK, "mesh-instanced/block", "a host-visible block is carved for it");
        vk = context->loader->deviceTable.vkBindBufferMemory(context->device->device, readback, readbackRange.memory, readbackRange.offset);
        xud_check(vk == VK_SUCCESS, "mesh-instanced/bound", "and the buffer binds into it");
        vk = context->loader->deviceTable.vkMapMemory(
            context->device->device, readbackRange.memory, readbackRange.offset, readbackRange.size, 0u, (void**)&pixels);
        xud_check((vk == VK_SUCCESS) && (pixels != NULL), "mesh-instanced/mapped", "and the mapping is live");

        memset(&poolInfo, 0, sizeof poolInfo);
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = context->device->graphicsQueue.queueFamilyIndex;
        vk = context->loader->deviceTable.vkCreateCommandPool(context->device->device, &poolInfo, NULL, &pool);
        memset(&allocationInfo, 0, sizeof allocationInfo);
        allocationInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocationInfo.commandPool = pool;
        allocationInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocationInfo.commandBufferCount = 1u;
        vk = context->loader->deviceTable.vkAllocateCommandBuffers(context->device->device, &allocationInfo, &command);
        memset(&beginInfo, 0, sizeof beginInfo);
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vk = context->loader->deviceTable.vkBeginCommandBuffer(command, &beginInfo);

        memset(&imageBarrier, 0, sizeof imageBarrier);
        imageBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        imageBarrier.srcAccessMask = 0u;
        imageBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        imageBarrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        imageBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        imageBarrier.image = image;
        imageBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imageBarrier.subresourceRange.levelCount = 1u;
        imageBarrier.subresourceRange.layerCount = 1u;
        context->loader->deviceTable.vkCmdPipelineBarrier(
            command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, NULL, 0u, NULL, 1u, &imageBarrier);
        memset(&subresource, 0, sizeof subresource);
        subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        subresource.layerCount = 1u;
        memset(&region, 0, sizeof region);
        region.bufferImageHeight = height;
        region.imageSubresource = subresource;
        region.imageExtent.width = width;
        region.imageExtent.height = height;
        region.imageExtent.depth = 1u;
        context->loader->deviceTable.vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1u, &region);
        imageBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        imageBarrier.dstAccessMask = 0u;
        imageBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        imageBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        context->loader->deviceTable.vkCmdPipelineBarrier(
            command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0u, 0u, NULL, 0u, NULL, 1u, &imageBarrier);
        vk = context->loader->deviceTable.vkEndCommandBuffer(command);
        memset(&fenceInfo, 0, sizeof fenceInfo);
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        vk = context->loader->deviceTable.vkCreateFence(context->device->device, &fenceInfo, NULL, &fence);
        memset(&submitInfo, 0, sizeof submitInfo);
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1u;
        submitInfo.pCommandBuffers = &command;
        vk = context->loader->deviceTable.vkQueueSubmit(context->device->graphicsQueue.queue, 1u, &submitInfo, fence);
        vk = context->loader->deviceTable.vkWaitForFences(context->device->device, 1u, &fence, VK_TRUE, UINT64_MAX);
        xud_check(vk == VK_SUCCESS, "mesh-instanced/copied", "the copy runs, so the buffer holds the presented image");

        if ((vk == VK_SUCCESS) && (pixels != NULL)) {
            const size_t leftAt = (((size_t)row * (size_t)width) + (size_t)leftCol) * 4u;
            const size_t middleAt = (((size_t)row * (size_t)width) + (size_t)middleCol) * 4u;
            const size_t rightAt = (((size_t)row * (size_t)width) + (size_t)rightCol) * 4u;

            xud_check((pixels[leftAt + redIndex] >= (fullByte - XUD_TEST_INST_TOLERANCE))
                    && (pixels[leftAt + redIndex] <= (fullByte + XUD_TEST_INST_TOLERANCE))
                    && (pixels[leftAt + blueIndex] <= (zeroByte + XUD_TEST_INST_TOLERANCE)),
                "mesh-instanced/left-copy-is-the-first-instance",
                "the left probe is RED - the first instance row's matrix (x=-1.5) and the first row's tint, both read from "
                "bindings 4 and 5 rather than from the push row");
            xud_check((pixels[rightAt + blueIndex] >= (fullByte - XUD_TEST_INST_TOLERANCE))
                    && (pixels[rightAt + blueIndex] <= (fullByte + XUD_TEST_INST_TOLERANCE))
                    && (pixels[rightAt + redIndex] <= (zeroByte + XUD_TEST_INST_TOLERANCE)),
                "mesh-instanced/right-copy-is-the-second-instance",
                "the right probe is BLUE - the SECOND row's matrix and tint, so the instance index advanced between the copies "
                "and neither copy fell back to the pushed identity");
            xud_check((pixels[middleAt + redIndex] <= (zeroByte + XUD_TEST_INST_TOLERANCE))
                    && (pixels[middleAt + blueIndex] <= (zeroByte + XUD_TEST_INST_TOLERANCE)),
                "mesh-instanced/gap-stays-clear",
                "and the column between them is the cleared black: the two copies are 72 pixels apart, which only the per-instance "
                "translations can produce - both quads at the pushed identity would have covered it, and both at one instance's "
                "matrix would have left one side probe empty");
        }

        if (fence != VK_NULL_HANDLE) {
            context->loader->deviceTable.vkDestroyFence(context->device->device, fence, NULL);
        }
        if (pool != VK_NULL_HANDLE) {
            context->loader->deviceTable.vkFreeCommandBuffers(context->device->device, pool, 1u, &command);
            context->loader->deviceTable.vkDestroyCommandPool(context->device->device, pool, NULL);
        }
        if (readback != VK_NULL_HANDLE) {
            context->loader->deviceTable.vkDestroyBuffer(context->device->device, readback, NULL);
        }
        (void)xud_allocator_release(context->allocator, &readbackRange);
    }

    xud_check(xud_close_mesh(context, quad) == XUD_OK, "mesh-instanced/quad-closed", "the quad's slot closes before the context does");
    (void)xud_close_context(&context);
}

void xud_test_mesh(void) {
    xud_test_mesh_pure();
    if (xud_gpu_tests_enabled()) {
        xud_test_mesh_gpu();
        xud_test_mesh_instanced_gpu();
    }
}
