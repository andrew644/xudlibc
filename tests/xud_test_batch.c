/***********************************************************************************************
 *
 *   xud_test_batch.c - the batch rung: the four SoA streams, the one index buffer, the mapped
 *          arena, the vertex-input rows, the flush decision and the capacity arithmetic
 *
 *   Owns : the arena's layout and the capacity doubling, the caps gate on the vertex-input limits,
 *          the two vertex-input rows per stream, the five copy rows, the invariant the settled SoA
 *          decision states, the flush-on-variant/material-change decision, the recorded draw rows,
 *          the two appends and the reset - and the GPU half that brings the whole rung up against a
 *          real device, grows it once and lets xud_close_context() tear it down. Layer: tests. See
 *          xud_batch.h and xud_physical.h: every gate below is read off the caps' vertex-input and
 *          memory rows, which is the record the physical layer owns.
 *
 *   What this area does NOT check, and why: the five vkCmdCopyBuffer calls of xud_batch_upload().
 *   The call needs a command buffer that is being recorded, and the only command buffer in this
 *   tree belongs to the frame loop, which records it itself - so the region ARITHMETIC the calls
 *   are built from is checked here (xud_batch_fill_copy_regions, headless, against live counts)
 *   and the calls themselves land with the frame loop's step that binds the batch. Saying so is
 *   the honest version of "the upload path is not exercised by a GPU test yet".
 *
 *   Deterministic: fixed loop counts, no time, no randomness, no filesystem.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud.h" /* Required for: XudResult, XUD_VERSION, the XUD_GPU_TESTS gate */
#include "xud_allocator.h" /* Required for: XudAllocator, XudMemoryRange, the ring block the GPU half reads back */
#include "xud_batch.h" /* Required for: XudBatch, the batch rung's own constants and every fill rung below */
#include "xud_core.h" /* Required for: XudContext, the owner the GPU half runs the ladder against */
#include "xud_physical.h" /* Required for: XudDeviceCaps, the vertex-input limits every gate is read off */
#include "xud_test_harness.h" /* Required for: xud_check, xud_gpu_tests_enabled, the area entry points */
#include <stdlib.h> /* Required for: free, the draw array this area's own record loop allocates */
#include <string.h> /* Required for: memset, memcmp, the synthetic rows this area zeroes and compares */

/* A host arena the headless appends write into. The append path's whole point is a memcpy at a
   computed offset, and the only thing that makes that testable with no driver present is real by
   tes to check - so the headless run hands the batch a synthetic arena backed by real memory, at
   the size the largest batch's layout asks for (the header's static_assert is what proves that s
   ize fits the allocator's ring, and therefore fits every capacity the doubling sequence reaches
   ). Nothing here is mapped: the batch's own mapping is a driver fact, checked in the GPU half.
*/
static uint8_t xudTestArena[(XUD_BATCH_MAX_VERTICES * XUD_BATCH_VERTEX_SIZE)
    + (XUD_BATCH_INDEX_CAPACITY(XUD_BATCH_MAX_VERTICES) * XUD_BATCH_INDEX_SIZE)];

/*-----------------------------------------------------------------------------------------------
 * The batch rung, headless: the layout, the doubling, the vertex-input rows, the copy rows, the
 * invariant, the flush decision and the draw rows, none of it ever handed to a driver. This is
 * the half the repo's headless-testable rule leans on - the whole arithmetic of the rung is func
 * tions that take structs and call nothing, so the capacity bounds, the arena's offsets and the
 * flush decision all run with no device present.
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_batch_pure(void) {
    XudBatch b;
    XudBatch broken;
    XudDeviceCaps caps;
    VkVertexInputBindingDescription binding = (VkVertexInputBindingDescription) { .inputRate = VK_VERTEX_INPUT_RATE_MAX_ENUM };
    VkVertexInputAttributeDescription attribute = (VkVertexInputAttributeDescription) { .offset = 0xFFFFFFFFu };
    uint32_t next = 0u;
    uint32_t flush = 0u;
    uint32_t vertexCount = 0u;
    uint32_t indexCount = 0u;
    uint32_t capacity = 0u;
    uint32_t drawCount = 0u;
    VkDeviceSize stagingBytes = 0u;
    uint32_t row = 0u;
    const float pos[3] = { 0.0f, 0.0f, 0.0f };
    const float uv[2] = { 0.0f, 0.0f };
    const uint8_t color[4] = { 0u, 0u, 0u, 0u };
    const float normal[3] = { 0.0f, 0.0f, 0.0f };

    memset(&b, 0, sizeof(b));
    memset(&caps, 0, sizeof(caps));

    /*--- the arena's layout, and the capacity bounds around it -------------------------------*/

    xud_check(xud_batch_layout(NULL, XUD_BATCH_INITIAL_VERTICES) == XUD_INVALID_ARG, "batch/layout-null-out",
        "a layout with nowhere to be written is refused");
    xud_check(xud_batch_layout(&b, 0u) == XUD_UNSUPPORTED, "batch/layout-zero-capacity",
        "a batch with no capacity is not a batch, it is a batch with no buffers to append into");
    xud_check(xud_batch_layout(&b, XUD_BATCH_MAX_VERTICES + 1u) == XUD_UNSUPPORTED, "batch/layout-past-bound",
        "past the bound the answer is a refusal rather than an arena the allocator's ring cannot hold - the bound is the ring's, and the "
        "static_assert in the header is the arithmetic that proves it");

    xud_check(xud_batch_layout(&b, XUD_BATCH_INITIAL_VERTICES) == XUD_OK, "batch/layout-initial", "the initial capacity lays out");
    xud_check(b.vertexCapacity == XUD_BATCH_INITIAL_VERTICES, "batch/capacity-is-the-request",
        "the capacity asked for is the capacity recorded, not one the rung rounded up to a page");
    xud_check(b.indexCapacity == XUD_BATCH_INDEX_CAPACITY(XUD_BATCH_INITIAL_VERTICES), "batch/index-capacity-is-3-over-2",
        "96 indices for 64 vertices, which is the Euler bound the header states rather than a guessed headroom");
    xud_check(b.streamOffset[XUD_BATCH_STREAM_POS] == 0u, "batch/offset-pos-first", "pos is the first window of the arena, at 0");
    xud_check(b.streamOffset[XUD_BATCH_STREAM_UV] == (VkDeviceSize)XUD_BATCH_INITIAL_VERTICES * XUD_BATCH_STREAM_POS_SIZE,
        "batch/offset-uv-after-pos", "uv starts where pos's window ends - 64 vertices at 12 B each");
    xud_check(b.streamOffset[XUD_BATCH_STREAM_COLOR]
            == (VkDeviceSize)XUD_BATCH_INITIAL_VERTICES * (XUD_BATCH_STREAM_POS_SIZE + XUD_BATCH_STREAM_UV_SIZE),
        "batch/offset-color-after-uv", "and colour after uv's window, so the offsets are a running sum and not four independent constants");
    xud_check(b.streamOffset[XUD_BATCH_STREAM_NORMAL]
            == (VkDeviceSize)XUD_BATCH_INITIAL_VERTICES
                * (XUD_BATCH_STREAM_POS_SIZE + XUD_BATCH_STREAM_UV_SIZE + XUD_BATCH_STREAM_COLOR_SIZE),
        "batch/offset-normal-last", "and normal last of the four, which is the stream order the pipeline's attribute locations use");
    xud_check(
        b.indexOffset == b.streamOffset[XUD_BATCH_STREAM_NORMAL] + (VkDeviceSize)XUD_BATCH_INITIAL_VERTICES * XUD_BATCH_STREAM_NORMAL_SIZE,
        "batch/index-offset-after-normal", "the index window follows the four streams rather than preceding them");
    xud_check(b.stagingBytes == b.indexOffset + (VkDeviceSize)b.indexCapacity * XUD_BATCH_INDEX_SIZE, "batch/arena-is-the-sum",
        "the arena's total is the last window's end, which is what makes an append that stays inside a window stay inside the arena");
    xud_check(b.stagingBytes
            == (VkDeviceSize)XUD_BATCH_INITIAL_VERTICES * XUD_BATCH_VERTEX_SIZE + (VkDeviceSize)b.indexCapacity * XUD_BATCH_INDEX_SIZE,
        "batch/arena-matches-the-interleaved-arithmetic",
        "the SoA arena is the same total an interleaved 36 B/vertex arena would be - SoA changes the unit of the copy and of the grow, not "
        "the size of a vertex");

    /* The layout writes the per-stream facts too, in the pure half: that is what lets the invari
       ant below be checked on a batch that never created a buffer. */
    xud_check((b.stream[XUD_BATCH_STREAM_POS].elementSize == XUD_BATCH_STREAM_POS_SIZE)
            && (b.stream[XUD_BATCH_STREAM_UV].elementSize == XUD_BATCH_STREAM_UV_SIZE)
            && (b.stream[XUD_BATCH_STREAM_COLOR].elementSize == XUD_BATCH_STREAM_COLOR_SIZE)
            && (b.stream[XUD_BATCH_STREAM_NORMAL].elementSize == XUD_BATCH_STREAM_NORMAL_SIZE),
        "batch/element-sizes-are-the-constants",
        "each stream's width is the header's constant, read through the chain rather than remembered");
    xud_check((b.stream[XUD_BATCH_STREAM_POS].format == XUD_BATCH_FORMAT_POS)
            && (b.stream[XUD_BATCH_STREAM_UV].format == XUD_BATCH_FORMAT_UV)
            && (b.stream[XUD_BATCH_STREAM_COLOR].format == XUD_BATCH_FORMAT_COLOR)
            && (b.stream[XUD_BATCH_STREAM_NORMAL].format == XUD_BATCH_FORMAT_NORMAL),
        "batch/formats-are-the-constants", "and each stream's VkFormat is the header's, in the same order");
    xud_check((b.stream[XUD_BATCH_STREAM_POS].capacityBytes == (VkDeviceSize)XUD_BATCH_INITIAL_VERTICES * XUD_BATCH_STREAM_POS_SIZE)
            && (b.stream[XUD_BATCH_STREAM_COLOR].capacityBytes == (VkDeviceSize)XUD_BATCH_INITIAL_VERTICES * XUD_BATCH_STREAM_COLOR_SIZE),
        "batch/capacity-bytes-are-per-stream",
        "the widest and the narrowest stream's capacity in bytes, both capacity x width - the two ends of the four");
    xud_check((b.vertexCount == 0u) && (b.indexCount == 0u) && (b.pendingIndexCount == 0u), "batch/layout-starts-empty",
        "a layout is a new set of buffers, so the live counts restart rather than surviving into it");

    /*--- the capacity doubling, and the bound it stops at ------------------------------------*/

    xud_check(xud_batch_next_capacity(0u, &next) == XUD_UNSUPPORTED, "batch/next-capacity-zero",
        "a batch with no capacity has no next capacity, and 0 is not a doubling of anything");
    xud_check(xud_batch_next_capacity(XUD_BATCH_INITIAL_VERTICES, NULL) == XUD_INVALID_ARG, "batch/next-capacity-null-out", "refused");
    xud_check((xud_batch_next_capacity(XUD_BATCH_INITIAL_VERTICES, &next) == XUD_OK) && (next == XUD_BATCH_INITIAL_VERTICES * 2u),
        "batch/next-capacity-doubles",
        "64 to 128: doubling is the whole policy, the same one the allocator's blocks and the handle table's slots use");
    xud_check((xud_batch_next_capacity(XUD_BATCH_MAX_VERTICES / 2u, &next) == XUD_OK) && (next == XUD_BATCH_MAX_VERTICES),
        "batch/next-capacity-clamps-at-the-bound", "the last doubling lands exactly on the bound rather than past it");
    xud_check((xud_batch_next_capacity(XUD_BATCH_MAX_VERTICES, &next) == XUD_OK) && (next == 0u), "batch/next-capacity-full-answers-zero",
        "at the bound the answer is 0, which is the batch being full - a refusal above, not a request for more than the ring has");

    /*--- the caps gate on the vertex-input limits --------------------------------------------*/

    /* A caps row spelled at this machine's answers (the GPU half reads the real ones back): 33 bi
       ndings, 29 attributes, a 4095 B stride. Well above the four this batch needs.
     */
    caps.properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    caps.properties.properties.limits.maxVertexInputBindings = 33u;
    caps.properties.properties.limits.maxVertexInputAttributes = 29u;
    caps.properties.properties.limits.maxVertexInputBindingStride = 4095u;

    xud_check(xud_batch_fill_binding(NULL, &caps, XUD_BATCH_STREAM_POS) == XUD_INVALID_ARG, "batch/binding-null-out",
        "a row with nowhere to be written is refused");
    xud_check(xud_batch_fill_binding(&binding, NULL, XUD_BATCH_STREAM_POS) == XUD_INVALID_ARG, "batch/binding-no-caps",
        "and a row with no caps to gate against - the repo's check-before-you-use rule has nothing to check");
    xud_check(xud_batch_fill_binding(&binding, &caps, XUD_BATCH_STREAM_COUNT) == XUD_INVALID_ARG, "batch/binding-past-four",
        "a stream index past the four is a bad argument, not a fifth binding");
    xud_check(xud_batch_fill_binding(&binding, &caps, XUD_BATCH_STREAM_COLOR) == XUD_OK, "batch/binding-ok",
        "the narrowest stream's binding row fills");
    xud_check(binding.binding == XUD_BATCH_STREAM_COLOR, "batch/binding-number-is-stream-index",
        "the binding number IS the stream index, which is what makes vkCmdBindVertexBuffers' binding argu"
        "ment and the pipeline's binding row one number");
    xud_check(binding.stride == XUD_BATCH_STREAM_COLOR_SIZE, "batch/stride-is-element-size",
        "the stride is that stream's element size, not the 36 B of a whole vertex - SoA means one buffer per stream, so a stride is a "
        "stream's element");
    xud_check(binding.inputRate == VK_VERTEX_INPUT_RATE_VERTEX, "batch/rate-per-vertex",
        "per-vertex, the only rate this batch uses - a per-instance rate is instancing, which is not here");

    /* Each refusal in turn, so the gate's compares are observable rules rather than one that hap
       pens to pass. */
    caps.properties.properties.limits.maxVertexInputBindings = XUD_BATCH_STREAM_COUNT - 1u;
    xud_check(xud_batch_fill_binding(&binding, &caps, XUD_BATCH_STREAM_NORMAL) == XUD_UNSUPPORTED, "batch/caps-too-few-bindings",
        "a device that binds three vertices' streams cannot hold this batch's four, and the answer is UNSUPPORTED rather than a "
        "three-stream batch");
    caps.properties.properties.limits.maxVertexInputBindings = 33u;
    caps.properties.properties.limits.maxVertexInputAttributes = XUD_BATCH_STREAM_COUNT - 1u;
    xud_check(xud_batch_fill_attribute(&attribute, &caps, XUD_BATCH_STREAM_NORMAL) == XUD_UNSUPPORTED, "batch/caps-too-few-attributes",
        "the attribute limit is a different number bounding a different thing, refused on its own too");
    caps.properties.properties.limits.maxVertexInputAttributes = 29u;
    caps.properties.properties.limits.maxVertexInputBindingStride = XUD_BATCH_STREAM_POS_SIZE - 1u;
    xud_check(xud_batch_fill_binding(&binding, &caps, XUD_BATCH_STREAM_POS) == XUD_UNSUPPORTED, "batch/caps-stride-too-small",
        "a stride the driver would reject is reported rather than a pipeline built with it - the check is after the row is written, so the "
        "row that comes back is the row that was checked against");
    caps.properties.properties.limits.maxVertexInputBindingStride = 4095u;

    xud_check(xud_batch_fill_attribute(&attribute, &caps, XUD_BATCH_STREAM_UV) == XUD_OK, "batch/attribute-ok",
        "the uv stream's attribute row fills");
    xud_check(attribute.location == XUD_BATCH_STREAM_UV, "batch/attribute-location-is-stream-index",
        "the attribute's location IS the stream index, which is the GLSL's in-location read from the other side");
    xud_check(attribute.binding == XUD_BATCH_STREAM_UV, "batch/attribute-binding-matches-location",
        "and the binding it reads from is the same number - one stream, one binding, one location");
    xud_check(attribute.format == XUD_BATCH_FORMAT_UV, "batch/attribute-format-is-the-stream's",
        "and the format is the stream's own, which is the pair the C record and the GLSL have to agree on");
    xud_check(attribute.offset == 0u, "batch/attribute-offset-zero",
        "offset 0 is what SoA buys: each stream is its own VkBuffer, so an attribute never sits at a non-zero offset inside one, and "
        "maxVertexInputAttributeOffset has nothing to be checked against");

    xud_check(xud_batch_fill_vertex_input(NULL, &caps) == XUD_INVALID_ARG, "batch/vertex-input-null-batch", "refused");
    xud_check(xud_batch_fill_vertex_input(&b, &caps) == XUD_OK, "batch/vertex-input-ok", "all four bindings and all four attributes fill");
    xud_check((b.bindings[XUD_BATCH_STREAM_POS].stride == XUD_BATCH_STREAM_POS_SIZE)
            && (b.bindings[XUD_BATCH_STREAM_NORMAL].stride == XUD_BATCH_STREAM_NORMAL_SIZE),
        "batch/vertex-input-bindings-filled", "the array's own rows, at both ends of the four");
    xud_check((b.attributes[XUD_BATCH_STREAM_POS].format == XUD_BATCH_FORMAT_POS)
            && (b.attributes[XUD_BATCH_STREAM_COLOR].format == XUD_BATCH_FORMAT_COLOR),
        "batch/vertex-input-attributes-filled", "and the attribute array's, at both the widest and the narrowest");
    xud_check((b.stream[XUD_BATCH_STREAM_UV].binding.binding == b.bindings[XUD_BATCH_STREAM_UV].binding)
            && (b.stream[XUD_BATCH_STREAM_UV].attribute.location == b.attributes[XUD_BATCH_STREAM_UV].location),
        "batch/stream-rows-are-the-same-bytes",
        "the row beside each buffer and the row in the array the pipeline is handed are one set of bytes, so the pipeline's vertex-input "
        "state and vkCmdBindVertexBuffers' argument cannot disagree");

    /*--- the invariant the settled SoA decision states ---------------------------------------*/

    xud_check(xud_batch_check_invariant(NULL) == XUD_UNSUPPORTED, "batch/invariant-null", "no batch is not an invariant holding");
    xud_check(xud_batch_check_invariant(&b) == XUD_OK, "batch/invariant-holds",
        "every stream's capacity divided by its own width is the batch's one vertex capacity, which is what makes one vertex index address "
        "all four buffers");

    broken = b;
    broken.stream[XUD_BATCH_STREAM_UV].capacityBytes += XUD_BATCH_STREAM_UV_SIZE;
    xud_check(xud_batch_check_invariant(&broken) == XUD_UNSUPPORTED, "batch/invariant-one-stream-longer",
        "one stream a vertex longer than the others is a broken state of this rung's own record, and the check catches it rather than an "
        "append writing past a shorter one");
    broken = b;
    broken.stream[XUD_BATCH_STREAM_UV].capacityBytes = XUD_BATCH_STREAM_UV_SIZE + 1u;
    xud_check(xud_batch_check_invariant(&broken) == XUD_UNSUPPORTED, "batch/invariant-not-whole-elements",
        "and a capacity that is not a whole number of elements is caught on the same compare - a stream whose last element is truncated is "
        "a stream an index would read half of");
    broken = b;
    broken.vertexCount = b.vertexCapacity + 1u;
    xud_check(xud_batch_check_invariant(&broken) == XUD_UNSUPPORTED, "batch/invariant-live-past-capacity",
        "the second half of the invariant: a live count past the capacity means an append wrapped rather than being refused");

    /*--- the five copy rows, against live counts ---------------------------------------------*/

    xud_check(xud_batch_fill_copy_regions(NULL) == XUD_INVALID_ARG, "batch/copy-null-batch", "refused");
    xud_check(xud_batch_fill_copy_regions(&b) == XUD_UNSUPPORTED, "batch/copy-nothing-live",
        "an empty batch has no region: a zero-size copy region is a hard error to vkCmdCopyBuffer, so the guard is here rather than in the "
        "driver call");

    b.vertexCount = 3u;
    b.indexCount = 3u;
    xud_check(xud_batch_fill_copy_regions(&b) == XUD_OK, "batch/copy-ok", "the five rows are filled");
    xud_check(b.copyRegions[XUD_BATCH_STREAM_POS].srcOffset == b.streamOffset[XUD_BATCH_STREAM_POS], "batch/copy-src-is-the-window",
        "each region starts at its own window's offset in the arena - the same offset the appends wrote at");
    xud_check(b.copyRegions[XUD_BATCH_STREAM_UV].srcOffset == b.streamOffset[XUD_BATCH_STREAM_UV], "batch/copy-src-uv-window",
        "and the next window's, for the next region");
    xud_check(b.copyRegions[XUD_BATCH_COPY_REGION_COUNT - 1u].srcOffset == b.indexOffset, "batch/copy-last-region-is-the-index-window",
        "the fifth region is the index buffer's window, which is why the region count is the stream count plus one");
    xud_check((b.copyRegions[XUD_BATCH_STREAM_POS].dstOffset == 0u) && (b.copyRegions[XUD_BATCH_COPY_REGION_COUNT - 1u].dstOffset == 0u),
        "batch/copy-dst-always-zero",
        "every destination is offset 0 of its own buffer, because the arena's content is rewritten from 0 every frame and the buffers are "
        "not a ring");
    xud_check((b.copyRegions[XUD_BATCH_STREAM_POS].size == 3u * XUD_BATCH_STREAM_POS_SIZE)
            && (b.copyRegions[XUD_BATCH_STREAM_COLOR].size == 3u * XUD_BATCH_STREAM_COLOR_SIZE),
        "batch/copy-size-is-live-not-capacity",
        "three live vertices at each stream's own width, so a frame that uses 3 of 64 vertices copies 3 and not 64");
    xud_check(b.copyRegions[XUD_BATCH_COPY_REGION_COUNT - 1u].size == 3u * XUD_BATCH_INDEX_SIZE, "batch/copy-index-size-is-live-indices",
        "and the index region is the live index count at 4 B each");

    /*--- the flush decision ------------------------------------------------------------------*/

    xud_check(xud_batch_should_flush(NULL, &b, XUD_BATCH_VARIANT_OPAQUE, XUD_BATCH_NO_MATERIAL, 0u, 0u) == XUD_INVALID_ARG,
        "batch/flush-null-out", "refused");
    xud_check(xud_batch_should_flush(&flush, NULL, XUD_BATCH_VARIANT_OPAQUE, XUD_BATCH_NO_MATERIAL, 0u, 0u) == XUD_INVALID_ARG,
        "batch/flush-null-batch", "refused");
    xud_check(xud_batch_should_flush(&flush, &b, XUD_BATCH_VARIANT_COUNT, XUD_BATCH_NO_MATERIAL, 0u, 0u) == XUD_INVALID_ARG,
        "batch/flush-bad-variant", "a variant past the two the plan names is a bad argument, not a third pipeline");

    /* The batch above has pendingIndexCount 0, so a variant change with nothing pending is not a
       batch to flush - the change still moves the current pair, one rung up. */
    flush = 0xDEADu;
    xud_check(xud_batch_should_flush(&flush, &b, XUD_BATCH_VARIANT_TRANSPARENT, XUD_BATCH_NO_MATERIAL, 0u, 0u) == XUD_OK,
        "batch/flush-change-with-nothing", "the decision itself answers OK");
    xud_check(flush == 0u, "batch/flush-nothing-pending-is-not-a-flush",
        "and it says do not flush: an empty pending batch is not a batch to submit, so a variant change with nothing under it is just a "
        "change");

    b.currentVariant = XUD_BATCH_VARIANT_OPAQUE;
    b.currentMaterial = XUD_BATCH_NO_MATERIAL;
    b.pendingIndexCount = 3u;
    flush = 0u;
    xud_check(xud_batch_should_flush(&flush, &b, XUD_BATCH_VARIANT_TRANSPARENT, XUD_BATCH_NO_MATERIAL, 0u, 0u) == XUD_OK,
        "batch/flush-variant-change", "the first half of the rule: a pipeline-variant change over a non-empty pending batch flushes");
    xud_check(flush == 1u, "batch/flush-variant-change-fires", "and it fires");
    flush = 0u;
    xud_check(xud_batch_should_flush(&flush, &b, XUD_BATCH_VARIANT_OPAQUE, 7u, 0u, 0u) == XUD_OK, "batch/flush-material-change",
        "the second half of the same rule: a material change flushes too, which is the plan's flush-on-pipeline/material-change read "
        "whole");
    xud_check(flush == 1u, "batch/flush-material-change-fires", "and it fires");
    flush = 0u;
    xud_check(xud_batch_should_flush(&flush, &b, XUD_BATCH_VARIANT_OPAQUE, XUD_BATCH_NO_MATERIAL, 1u, 0u) == XUD_OK,
        "batch/flush-same-pair-fits", "same variant, same material, one more vertex than the three already live");
    xud_check(flush == 0u, "batch/flush-same-pair-does-not-fire", "and nothing flushes - the append continues the pending batch");
    flush = 0u;
    xud_check(xud_batch_should_flush(&flush, &b, XUD_BATCH_VARIANT_OPAQUE, XUD_BATCH_NO_MATERIAL, b.vertexCapacity, 0u) == XUD_OK,
        "batch/flush-over-capacity",
        "the third half of the rule: an append that would pass the capacity flushes rather than wraps, because a stream that wrapped would "
        "be a draw whose index range names bytes the next frame overwrites");
    xud_check(flush == 1u, "batch/flush-over-capacity-fires", "and it fires");
    b.pendingIndexCount = 0u;
    flush = 0u;
    xud_check(xud_batch_should_flush(&flush, &b, XUD_BATCH_VARIANT_OPAQUE, XUD_BATCH_NO_MATERIAL, b.vertexCapacity, 0u) == XUD_OK,
        "batch/flush-over-capacity-empty", "the same over-capacity request with nothing pending");
    xud_check(flush == 0u, "batch/flush-over-capacity-empty-not-a-flush",
        "still not a flush - there is nothing to submit, so the answer is the caller above's refusal rather than an empty draw");

    /*--- the recorded draw rows --------------------------------------------------------------*/

    /* The pending pair, put back to a non-empty state: the row below records the pending batch,
       so the two numbers it writes are the pending pair's, not the live counts'.
    */
    b.pendingFirstIndex = 0u;
    b.pendingIndexCount = 3u;

    xud_check(xud_batch_record_draw(NULL, XUD_BATCH_VARIANT_OPAQUE, XUD_BATCH_NO_MATERIAL) == XUD_INVALID_ARG, "batch/draw-null-batch",
        "refused");
    xud_check(xud_batch_record_draw(&b, XUD_BATCH_VARIANT_COUNT, XUD_BATCH_NO_MATERIAL) == XUD_INVALID_ARG, "batch/draw-bad-variant",
        "a row that names no variant the plan names is a bad argument, not a row that would bind no pipeline");
    xud_check(xud_batch_record_draw(&b, XUD_BATCH_VARIANT_OPAQUE, XUD_BATCH_NO_MATERIAL) == XUD_OK, "batch/draw-recorded",
        "the first row records");
    xud_check(
        b.draws[0u].firstIndex == 0u, "batch/draw-first-index-is-pending-start", "the row's firstIndex is where the pending batch started");
    xud_check(b.draws[0u].indexCount == 3u, "batch/draw-index-count-is-pending-count",
        "and its indexCount is the pending count - the two arguments of vkCmdDrawIndexed");
    xud_check(b.draws[0u].variant == XUD_BATCH_VARIANT_OPAQUE, "batch/draw-variant", "the variant the row was accumulated under");
    xud_check(b.draws[0u].materialIndex == XUD_BATCH_NO_MATERIAL, "batch/draw-material-sentinel",
        "and the sentinel, because no material slot exists yet - the row names an INDEX into the variants rather than a VkPipeline, which "
        "is what keeps a recreated pipeline from making a stale row");
    xud_check((b.pendingFirstIndex == b.indexCount) && (b.pendingIndexCount == 0u), "batch/draw-pending-reset",
        "recording a row moves the pending pair to start after it, which is what makes the next row's range the next window of the index "
        "buffer");

    for (row = 0u; row < XUD_BATCH_DRAW_MIN; row++) {
        if (xud_batch_record_draw(&b, XUD_BATCH_VARIANT_OPAQUE, row) != XUD_OK) {
            xud_check(0, "batch/draw-array-grow-failed", "the doubling step failed on the host");
            break;
        }
    }
    xud_check(b.drawCount == (XUD_BATCH_DRAW_MIN + 1u), "batch/draw-array-grows",
        "the row array doubled once past its first capacity rather than refusing at a bound, so a caller above never has to know the "
        "bound");
    xud_check(
        b.drawCapacity >= b.drawCount, "batch/draw-capacity-covers-count", "and the capacity it allocated covers the count it recorded");

    /*--- the appends, the reset and the counters ---------------------------------------------*/

    xud_check(xud_batch_reset(NULL) == XUD_INVALID_ARG, "batch/reset-null", "refused");
    xud_check(xud_batch_reset(&b) == XUD_OK, "batch/reset-ok", "the reset completes");
    xud_check((b.vertexCount == 0u) && (b.indexCount == 0u) && (b.drawCount == 0u), "batch/reset-counts-zero",
        "the live counts and the rows go to 0, which is what a fresh frame's batch starts from");
    xud_check((b.vertexCapacity == XUD_BATCH_INITIAL_VERTICES) && (b.stagingBytes != 0u), "batch/reset-keeps-capacity",
        "the capacities and the buffers stay - the arena's content is overwritten next frame, the same property the allocator's ring note "
        "states for the ring itself");
    xud_check((b.currentVariant == XUD_BATCH_VARIANT_OPAQUE) && (b.currentMaterial == XUD_BATCH_NO_MATERIAL), "batch/reset-current-pair",
        "and the current pair restarts at the opaque variant with no material, so a reset batch and a fresh one make the same first "
        "decision");

    xud_check(xud_batch_stats(NULL, NULL, NULL, NULL, NULL, NULL) == XUD_INVALID_ARG, "batch/stats-null", "refused");
    xud_check(xud_batch_stats(&b, &vertexCount, &indexCount, &capacity, &drawCount, &stagingBytes) == XUD_OK, "batch/stats-ok",
        "the counters a caller above reads");
    xud_check((vertexCount == 0u) && (indexCount == 0u) && (capacity == XUD_BATCH_INITIAL_VERTICES) && (drawCount == 0u),
        "batch/stats-are-the-fields", "each out-param is the field it was declared to be, read rather than recomputed");
    xud_check(
        stagingBytes == b.stagingBytes, "batch/stats-arena-total", "and the arena's total is the one the staging buffer was created for");

    /* The appends, against the host arena above: this is the only place in the headless run the
       write offsets are checked, and the check is a memcmp at the offset the layout computed rath
       er than a count that would pass whatever the write went to.
    */
    b.stagingMapped = xudTestArena;
    b.stagingMappedLive = 1u;

    xud_check(xud_batch_append_vertex(&b, NULL, uv, color, normal) == XUD_INVALID_ARG, "batch/append-null-attributes",
        "an attribute array that cannot be read is a bad argument, refused before anything is written");
    xud_check(xud_batch_append_vertex(&b, pos, uv, color, normal) == XUD_OK, "batch/append-ok", "the first vertex appends");
    xud_check(b.vertexCount == 1u, "batch/append-bumps-count", "and the one live count moves for all four streams at once");

    for (row = 1u; row < 3u; row++) {
        if (xud_batch_append_vertex(&b, pos, uv, color, normal) != XUD_OK) {
            xud_check(0, "batch/append-refused", "a vertex the arena could not take");
            break;
        }
    }
    xud_check(b.vertexCount == 3u, "batch/append-three-live", "three live vertices");

    /* The fourth vertex carries values no other vertex has, so the memcmp below can only pass at
       exactly one offset in the pos window - a write at the wrong stride, or into the wrong wind
       ow, fails it.
    */
    {
        const float distinctive[3] = { 1.0f, 2.0f, 3.0f };
        xud_check(
            xud_batch_append_vertex(&b, distinctive, uv, color, normal) == XUD_OK, "batch/append-fourth", "the fourth vertex appends");
        xud_check(memcmp(xudTestArena + b.streamOffset[XUD_BATCH_STREAM_POS] + (3u * XUD_BATCH_STREAM_POS_SIZE), distinctive,
                      XUD_BATCH_STREAM_POS_SIZE)
                == 0,
            "batch/append-wrote-at-the-computed-offset",
            "the fourth vertex's pos bytes are exactly where the layout says they are: the stream's window offset plus three vertices at "
            "that stream's own width");
        xud_check(xudTestArena[b.streamOffset[XUD_BATCH_STREAM_POS] + (2u * XUD_BATCH_STREAM_POS_SIZE)] == 0u,
            "batch/append-stride-is-the-stream's-own",
            "and the third vertex's slot is still zero, so the stride moved by 12 B and not by the 36 B of a whole interleaved vertex");
        xud_check(xudTestArena[b.streamOffset[XUD_BATCH_STREAM_UV]] == 0u, "batch/append-stayed-in-its-window",
            "the first uv byte is still zero, so a pos write never ran into the next stream's window");
    }

    for (row = 0u; row < 3u; row++) {
        if (xud_batch_append_index(&b, row) != XUD_OK) {
            xud_check(0, "batch/append-index-refused", "an index the arena could not take");
            break;
        }
    }
    xud_check(b.indexCount == 3u, "batch/append-index-three-live", "three live indices");
    xud_check(xud_batch_append_index(&b, b.vertexCount) == XUD_INVALID_ARG, "batch/append-index-names-no-vertex",
        "an index at or above the live vertex count is a bad argument, the same rule vkCmdDrawIndexed's range enforces on the driver side, "
        "checked where it is testable");

    /* The index bound, filled rather than asserted: the capacity is 96 for 64 vertices, and the
       only way to know the bound is the one the layout computed rather than one this file remem
       bers is to run to it.
    */
    for (row = 3u; row < b.indexCapacity; row++) {
        if (xud_batch_append_index(&b, row % b.vertexCount) != XUD_OK) {
            xud_check(0, "batch/append-index-fill-refused", "an index before the capacity was refused");
            break;
        }
    }
    xud_check(b.indexCount == b.indexCapacity, "batch/append-index-at-capacity",
        "the index buffer fills at exactly the 3/2 capacity the layout computed");
    xud_check(xud_batch_append_index(&b, 0u) == XUD_OUT_OF_MEMORY, "batch/append-index-past-capacity",
        "and one more is refused rather than wrapped - the flush decision above is what tells a caller to grow instead");
    xud_check(
        b.indexCount == b.indexCapacity, "batch/append-index-refusal-did-not-move-count", "and the refusal left the count where it was");

    b.vertexCount = b.vertexCapacity;
    xud_check(xud_batch_append_vertex(&b, pos, uv, color, normal) == XUD_OUT_OF_MEMORY, "batch/append-at-capacity",
        "a vertex past the capacity is refused rather than written past the buffer - the guard is here, not only in the invariant check, "
        "because the write above has to be refused before it happens");
    xud_check(b.vertexCount == b.vertexCapacity, "batch/append-refusal-did-not-move-count", "and the refusal left the count where it was");

    xud_check(xud_batch_check_invariant(&b) == XUD_OK, "batch/invariant-after-appends",
        "the invariant still holds with both buffers at capacity, which is the half that says one vertex index addresses all four buffers "
        "legally");

    xud_check(xud_batch_stats(&b, &vertexCount, &indexCount, &capacity, &drawCount, &stagingBytes) == XUD_OK, "batch/stats-after-appends",
        "the counters read back");
    xud_check((vertexCount == capacity) && (indexCount == XUD_BATCH_INDEX_CAPACITY(capacity)), "batch/stats-count-the-appends",
        "both buffers exactly full, which is the two capacities the layout computed read at the live counts");

    /* The host arena is this area's, not the batch's: clearing the pointer says so rather than le
       aving a pointer to a static array in a struct that a driver half would otherwise write into.
    */
    b.stagingMapped = NULL;
    b.stagingMappedLive = 0u;

    free(b.draws);
    b.draws = NULL;
    b.drawCapacity = 0u;
}

/*-----------------------------------------------------------------------------------------------
 * The batch rung against a real device: the whole ladder up, the batch read back, one grow, and
 * the teardown that xud_close_context() runs. The validation layer is the signal here - the map,
 * the bind of six buffers into allocator-owned memory and the release/re-acquire of the grow all
 * have to be clean, not merely "did not crash".
 *-----------------------------------------------------------------------------------------------*/
static void xud_test_batch_gpu(void) {
    XudContext* context = NULL;
    XudBatch* batch = NULL;
    XudResult result = XUD_OK;
    uint32_t vertexCount = 0u;
    uint32_t indexCount = 0u;
    uint32_t capacity = 0u;
    uint32_t drawCount = 0u;
    VkDeviceSize stagingBytes = 0u;
    const float pos[3] = { 0.0f, 0.0f, 0.0f };
    const float uv[2] = { 0.0f, 0.0f };
    const uint8_t color[4] = { 255u, 255u, 255u, 255u };
    const float normal[3] = { 0.0f, 0.0f, 1.0f };
    uint32_t vertex = 0u;
    uint32_t stream = 0u;

    result = xud_init_context(&context, 256u, 256u);
    xud_check(result == XUD_OK, "gpu-batch/context-completes", "the ladder has to complete before the batch can exist");
    if (context == NULL) {
        return;
    }

    xud_check(context->renderer->batch != NULL, "gpu-batch/owned-by-renderer",
        "a completed context owns the batch rung through its renderer, so the rung is wired rather than a sentinel");
    batch = context->renderer->batch;
    if (batch == NULL) {
        (void)xud_close_context(&context);
        return;
    }

    xud_check((batch->stagingBuffer != VK_NULL_HANDLE) && (batch->indexBuffer != VK_NULL_HANDLE), "gpu-batch/two-single-buffers-live",
        "the arena and the index buffer are live handles");
    xud_check(
        (batch->stream[XUD_BATCH_STREAM_POS].buffer != VK_NULL_HANDLE) && (batch->stream[XUD_BATCH_STREAM_NORMAL].buffer != VK_NULL_HANDLE),
        "gpu-batch/four-streams-live", "and so are the four streams' own buffers, at both ends of the four");
    xud_check((batch->stagingMapped != NULL) && (batch->stagingMappedLive == 1u), "gpu-batch/arena-mapped-once",
        "the arena is mapped for the whole life of the batch, which is the repo's no-vkMapMemory-per-frame rule read as "
        "map-it-once-always");
    xud_check(batch->stagingRange.memory != VK_NULL_HANDLE, "gpu-batch/arena-in-allocator-memory",
        "and the arena's bytes live in a block the allocator owns - there is no vkMapMemory of a buffer of this rung's own");
    for (stream = 0u; stream < XUD_BATCH_STREAM_COUNT; stream++) {
        if (batch->stream[stream].range.memory == VK_NULL_HANDLE) {
            xud_check(0, "gpu-batch/a-stream-range-is-dead", "a stream bound to no memory is a half-bound batch");
        }
    }
    xud_check((batch->stagingRange.memory != VK_NULL_HANDLE) && (batch->indexRange.memory != VK_NULL_HANDLE),
        "gpu-batch/arena-and-index-ranges-live",
        "the arena's and the index buffer's ranges name memory too, so all six of this rung's ranges a"
        "re live together - a half-bound batch is not a state that comes back OK");
    xud_check(context->allocator->ringMemory == VK_NULL_HANDLE, "gpu-batch/ring-still-absent",
        "and the allocator's transient ring has NOT been created, which is the batch's own note read a"
        "s a fact: the arena is static, so it is carved from the blocks, and nothing in this rung has "
        "asked the ring for a byte - the frame loop is what will, one rung up");

    xud_check(xud_batch_check_invariant(batch) == XUD_OK, "gpu-batch/invariant-against-a-driver",
        "the settled decision's invariant holds against real VkBuffers, which is the half the headless check above could only assert about "
        "synthetic ones");
    xud_check((batch->vertexCapacity == XUD_BATCH_INITIAL_VERTICES) && (batch->growCount == 0u), "gpu-batch/starts-at-initial-capacity",
        "the batch opened at the initial capacity and has not grown");
    xud_check((batch->bindings[XUD_BATCH_STREAM_POS].stride == XUD_BATCH_STREAM_POS_SIZE)
            && (batch->attributes[XUD_BATCH_STREAM_NORMAL].offset == 0u),
        "gpu-batch/vertex-input-rows-against-a-driver",
        "the vertex-input rows the pipeline will be handed are filled against this device's caps, not against synthetic ones");

    /* The appends, against the real mapped arena: three vertices and the three indices of one tri
       angle, which is the only way to exercise the host writes without a command buffer to record
       the copy into.
     */
    for (vertex = 0u; vertex < 3u; vertex++) {
        if (xud_batch_append_vertex(batch, pos, uv, color, normal) != XUD_OK) {
            xud_check(0, "gpu-batch/append-refused", "a vertex the arena could not take");
        }
    }
    for (vertex = 0u; vertex < 3u; vertex++) {
        if (xud_batch_append_index(batch, vertex) != XUD_OK) {
            xud_check(0, "gpu-batch/append-index-refused", "an index the arena could not take");
        }
    }
    xud_check(xud_batch_append_index(batch, 3u) == XUD_INVALID_ARG, "gpu-batch/append-index-past-live-vertices",
        "and the refusal still fires against the real arena: an index naming no vertex is a bad argument");
    xud_check(xud_batch_stats(batch, &vertexCount, &indexCount, &capacity, &drawCount, &stagingBytes) == XUD_OK,
        "gpu-batch/stats-read-back", "the counters read back");
    xud_check((vertexCount == 3u) && (indexCount == 3u), "gpu-batch/three-vertices-three-indices",
        "three vertices and three indices are live in all four streams at once, which is the invariant's subject read at the counts");
    xud_check(xud_batch_fill_copy_regions(batch) == XUD_OK, "gpu-batch/copy-rows-against-a-driver",
        "the five copy rows fill against the live counts, which is the arithmetic the frame loop's five vkCmdCopyBuffer calls will be "
        "built from");

    /* The grow, against the real device: destroy six, release six ranges, lay out the doubled capa
       city, create and bind six again. This is the recreate-at-a-frame-boundary half of the repo's
       no-vkCreateBuffer-inside-a-frame rule, and it is the only place in this tree the doubling is
       ever run against a driver.
     */
    result = xud_batch_grow(batch, context->loader, context->device, &context->physicalDevice->caps, context->allocator);
    xud_check(result == XUD_OK, "gpu-batch/grow-completes", "the doubled capacity is created and bound");
    xud_check((batch->vertexCapacity == XUD_BATCH_INITIAL_VERTICES * 2u) && (batch->growCount == 1u), "gpu-batch/capacity-doubled-once",
        "128 vertices, and the counter says the growth path ran exactly once");
    xud_check((batch->stagingBuffer != VK_NULL_HANDLE) && (batch->indexBuffer != VK_NULL_HANDLE) && (batch->stagingMapped != NULL),
        "gpu-batch/grow-rebuilt-and-remapped",
        "the arena and the buffers are live again, and the arena was unmapped and remapped rather than left dangling");
    xud_check(xud_batch_check_invariant(batch) == XUD_OK, "gpu-batch/invariant-after-grow",
        "and the invariant holds again, which is what says all four new streams were laid out at the new capacity together");
    xud_check((batch->vertexCount == 0u) && (batch->indexCount == 0u), "gpu-batch/grow-restarts-counts",
        "the live counts restarted: a count naming vertices past the new buffers' contents would be a draw reading nothing");

    xud_check(xud_close_context(&context) == XUD_OK, "gpu-batch/context-closes",
        "the teardown runs xud_batch_close through the renderer, which unmaps the arena before it destroys, and the run stays "
        "validation-clean");
}

void xud_test_batch(void) {
    xud_test_batch_pure();
    if (xud_gpu_tests_enabled() == 1) {
        xud_test_batch_gpu();
    }
}
