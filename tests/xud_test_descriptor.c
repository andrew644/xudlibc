/***********************************************************************************************
 *
 *   xud_test_descriptor.c - the descriptor-layout rung: two set layouts, a pool, set 0, the uniform buffer
 *
 *   Owns : the caps gate that refuses rather than shrinks a layout, the two set layouts, the push range, the pool, set 0's allocate info,
 * the buffer, the write that binds it, the staging arena and the upload's one-region copy row, the uniform block's bytes - and the GPU
 * half that creates all of it against a real device, the arena mapped once and the block read back out of it. Layer: tests. Sees xud_
 * descriptor.h and xud_allocator.h: the uniform buffer and the staging arena are both created from memory the allocator owns, which is
 * the ownership order the teardown below makes this rung die in.
 *
 *   Deterministic: fixed loop counts, no time, no randomness, no filesystem.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud.h" /* Required for: XudResult, XUD_VERSION, the XUD_GPU_TESTS gate */
#include "xud_allocator.h" /* Required for: XudMemoryRange, the range the buffer is bound to, and the stats the GPU half reads back */
#include "xud_batch.h" /* Required for: XudBatch, the batch's carves and arena the stats below count */
#include "xud_core.h" /* Required for: XudContext, the owner the GPU half runs against */
#include "xud_descriptor.h" /* Required for: XudDescriptor, the caps gate, the eight fill rungs and the open/close pair */
#include "xud_physical.h" /* Required for: XudDeviceCaps, the limits every gate is read off */
#include "xud_test_harness.h" /* Required for: xud_check, xud_gpu_tests_enabled, the area entry points */
#include <string.h> /* Required for: memset, the synthetic rows this area zeroes and compares */

/*-----------------------------------------------------------------------------------------------
 * The descriptor-layout rung, headless: the caps gate, the two set layouts, the push-constant range
 *  the pool, the allocate-info, the buffer-info, the write and the host-side block, none of it ever
 *  handed to a driver. This is the half the repo's headless-testable rule asks for - every row of
 *  this layout is assembled by a function that takes structs and calls nothing, so the whole gate
 *  and the whole assembly run with no device present.
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_descriptor_pure(void) {
    XudDeviceCaps caps;
    XudDescriptor* desc = NULL;
    XudDescriptor d;
    VkPushConstantRange range;
    uint64_t fakePoolHandle = 0x4100u;
    uint64_t fakeLayoutHandle = 0x4200u;
    uint64_t fakeSetHandle = 0x4400u;
    uint64_t fakeBufferHandle = 0x4500u;
    uint64_t fakeStagingHandle = 0x4600u;
    uint32_t row = 0u;

    memset(&caps, 0, sizeof(caps));
    memset(&d, 0, sizeof(d));
    memset(&range, 0, sizeof(range));

    /* A caps row that can have this layout, spelled at the spec's guarantees rather than at this
       machine's answers (the GPU test reads the real ones back): 4 bound sets, 13 uniform buffers
       per stage, 64 per set, 16 samplers per stage and per set, 128 push bytes and a 64 KiB unifo
       rm range.
     */
    caps.properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    caps.properties.properties.limits.maxBoundDescriptorSets = 4u;
    caps.properties.properties.limits.maxPerStageDescriptorUniformBuffers = 13u;
    caps.properties.properties.limits.maxDescriptorSetUniformBuffers = 64u;
    caps.properties.properties.limits.maxPerStageDescriptorSamplers = XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE;
    caps.properties.properties.limits.maxDescriptorSetSamplers = 64u;
    caps.properties.properties.limits.maxPushConstantsSize = XUD_DESCRIPTOR_PUSH_MIN_GUARANTEE;
    caps.properties.properties.limits.maxUniformBufferRange = 65536u;

    xud_check(xud_descriptor_caps_support(NULL) == XUD_INVALID_ARG, "descriptor/caps-null",
        "is refused: a rung with no recorded caps has nothing to gate itself on");
    xud_check(xud_descriptor_caps_support(&caps) == XUD_OK, "descriptor/caps-can-have-it", "the layout as designed fits this caps row");

    /* Each refusal in turn, one limit at a time, so the gate's compares are five observable rules
       rather than one that happens to pass. Each is the repo's XUD_UNSUPPORTED rather than a smal
       ler layout: a device this material model does not fit is refused, not quietly reshaped.
     */
    caps.properties.properties.limits.maxBoundDescriptorSets = XUD_DESCRIPTOR_SET_COUNT - 1u;
    xud_check(xud_descriptor_caps_support(&caps) == XUD_UNSUPPORTED, "descriptor/caps-too-few-sets",
        "a device that binds fewer sets than this layout declares is refused");
    caps.properties.properties.limits.maxBoundDescriptorSets = 4u;

    caps.properties.properties.limits.maxPerStageDescriptorUniformBuffers = 0u;
    xud_check(xud_descriptor_caps_support(&caps) == XUD_UNSUPPORTED, "descriptor/caps-no-stage-uniform-buffer",
        "a stage that may bind no uniform buffer cannot read set 0 at all");
    caps.properties.properties.limits.maxPerStageDescriptorUniformBuffers = 13u;

    caps.properties.properties.limits.maxDescriptorSetUniformBuffers = 0u;
    xud_check(xud_descriptor_caps_support(&caps) == XUD_UNSUPPORTED, "descriptor/caps-no-set-uniform-buffer",
        "and the per-set total is a different number bounding a different thing, refused on its own"
        "too");
    caps.properties.properties.limits.maxDescriptorSetUniformBuffers = 64u;

    caps.properties.properties.limits.maxPerStageDescriptorSamplers = XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE - 1u;
    xud_check(xud_descriptor_caps_support(&caps) == XUD_UNSUPPORTED, "descriptor/caps-too-few-samplers",
        "the sampler array is refused as designed rather than silently shrunk, which is the reason "
        "XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE is a constant a rung above cannot raise");
    caps.properties.properties.limits.maxPerStageDescriptorSamplers = XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE;

    caps.properties.properties.limits.maxDescriptorSetSamplers = XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE - 1u;
    xud_check(xud_descriptor_caps_support(&caps) == XUD_UNSUPPORTED, "descriptor/caps-too-few-set-samplers",
        "the per-set half of the same pair, refused on its own as well");
    caps.properties.properties.limits.maxDescriptorSetSamplers = 64u;

    caps.properties.properties.limits.maxPushConstantsSize = XUD_DESCRIPTOR_PUSH_SIZE - 1u;
    xud_check(xud_descriptor_caps_support(&caps) == XUD_UNSUPPORTED, "descriptor/caps-too-few-push-bytes",
        "a driver that answers less than the spec's 128-byte push guarantee cannot carry this row");
    caps.properties.properties.limits.maxPushConstantsSize = XUD_DESCRIPTOR_PUSH_MIN_GUARANTEE;

    caps.properties.properties.limits.maxUniformBufferRange = XUD_DESCRIPTOR_UNIFORM_BLOCK_SIZE - 1u;
    xud_check(xud_descriptor_caps_support(&caps) == XUD_UNSUPPORTED, "descriptor/caps-uniform-range-too-small",
        "a UNIFORM_BUFFER descriptor may not name a range bigger than that limit, so a block bigger "
        "than it is a refusal rather than a buffer the driver accepts and the shader does not");
    caps.properties.properties.limits.maxUniformBufferRange = 65536u;

    xud_check(xud_descriptor_caps_support(&caps) == XUD_OK, "descriptor/caps-ok-again",
        "the gate is a compare, not a sticky flag: a caps row that fits answers OK every time");

    /* The two set layouts.
     */
    xud_check(xud_descriptor_fill_set_layouts(NULL) == XUD_INVALID_ARG, "descriptor/fill-layouts-null", "is refused");
    xud_check(xud_descriptor_fill_set_layouts(&d) == XUD_OK, "descriptor/fill-layouts-ok", "assembled");
    xud_check(d.layoutCreateInfos[XUD_DESCRIPTOR_SET_PER_FRAME].sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        "descriptor/layout-0-stamped", "stamped, the rule the physical layer states for a create-info");
    xud_check(
        (d.layoutCreateInfos[XUD_DESCRIPTOR_SET_MATERIAL].pNext == NULL) && (d.layoutCreateInfos[XUD_DESCRIPTOR_SET_MATERIAL].flags == 0u),
        "descriptor/layout-1-chain-clear",
        "nothing is chained into either layout: the binding-flags chain that would make a PARTIALLY"
        "_BOUND array legal is not in these headers at all");
    xud_check((d.layoutCreateInfos[XUD_DESCRIPTOR_SET_PER_FRAME].bindingCount == XUD_DESCRIPTOR_BINDINGS_PER_SET)
            && (d.layoutCreateInfos[XUD_DESCRIPTOR_SET_MATERIAL].bindingCount == XUD_DESCRIPTOR_BINDINGS_PER_SET),
        "descriptor/one-binding-each", "one binding per set, both counts paired with a real array");
    xud_check((d.layoutCreateInfos[XUD_DESCRIPTOR_SET_PER_FRAME].pBindings == d.bindings[XUD_DESCRIPTOR_SET_PER_FRAME])
            && (d.layoutCreateInfos[XUD_DESCRIPTOR_SET_MATERIAL].pBindings == d.bindings[XUD_DESCRIPTOR_SET_MATERIAL]),
        "descriptor/binding-arrays-are-the-records",
        "each create-info points at the record's own binding row rather than a copy, so the layout "
        "the driver was handed and the one this rung records are one row");
    xud_check((d.bindings[XUD_DESCRIPTOR_SET_PER_FRAME][0u].binding == XUD_DESCRIPTOR_BINDING)
            && (d.bindings[XUD_DESCRIPTOR_SET_MATERIAL][0u].binding == XUD_DESCRIPTOR_BINDING),
        "descriptor/both-bindings-zero", "both bindings are numbered 0, the contiguous list the create-info's own rule asks for");
    xud_check(d.bindings[XUD_DESCRIPTOR_SET_PER_FRAME][0u].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        "descriptor/set-0-is-a-uniform-buffer", "set 0 is the per-frame block, read as a UNIFORM_BUFFER");
    xud_check(d.bindings[XUD_DESCRIPTOR_SET_PER_FRAME][0u].descriptorCount == 1u, "descriptor/set-0-count-one", "one buffer in that set");
    xud_check(d.bindings[XUD_DESCRIPTOR_SET_PER_FRAME][0u].stageFlags == XUD_DESCRIPTOR_BUFFER_STAGES, "descriptor/set-0-stage-flags",
        "both stages read the per-frame block, which is the design "
        "sketch's VP matrix read as a stage pair");
    xud_check(d.bindings[XUD_DESCRIPTOR_SET_MATERIAL][0u].descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        "descriptor/set-1-is-a-sampler-array", "set 1 is the material's sampler array");
    xud_check(d.bindings[XUD_DESCRIPTOR_SET_MATERIAL][0u].descriptorCount == XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE,
        "descriptor/set-1-array-size",
        "the layout's ceiling rather than a today-count: a layout is created once at init, so the "
        "array's size cannot grow later without recreating the pipeline layout and every pipeline "
        "above it");
    xud_check(d.bindings[XUD_DESCRIPTOR_SET_MATERIAL][0u].stageFlags == XUD_DESCRIPTOR_SAMPLER_STAGES, "descriptor/set-1-stage-flags",
        "the fragment stage only, which is what makes the two sets "
        "' stage pairs differ and not one pair copied");
    xud_check((d.bindings[XUD_DESCRIPTOR_SET_PER_FRAME][0u].pImmutableSamplers == NULL)
            && (d.bindings[XUD_DESCRIPTOR_SET_MATERIAL][0u].pImmutableSamplers == NULL),
        "descriptor/no-immutable-samplers",
        "an immutable sampler is a VkSampler this rung does not own, so both lists are NULL and th"
        "e texture step writes the samplers instead");

    /* The push-constant range, gated on the caps rather than written unconditionally.
     */
    xud_check(xud_descriptor_fill_push_range(NULL, NULL) == XUD_INVALID_ARG, "descriptor/fill-push-null", "is refused");
    xud_check(xud_descriptor_fill_push_range(&caps, NULL) == XUD_INVALID_ARG, "descriptor/fill-push-no-out", "is refused");
    xud_check(xud_descriptor_fill_push_range(NULL, &range) == XUD_INVALID_ARG, "descriptor/fill-push-no-caps",
        "is refused: the row's size is gated, so a row written without its gate is a row that was "
        "not gated");
    caps.properties.properties.limits.maxPushConstantsSize = XUD_DESCRIPTOR_PUSH_SIZE - 1u;
    xud_check(xud_descriptor_fill_push_range(&caps, &range) == XUD_UNSUPPORTED, "descriptor/fill-push-gated",
        "the gate is re-run here rather than trusted from the bring-up below");
    xud_check(range.size == 0u, "descriptor/fill-push-refusal-untouched",
        "and a refusal leaves the out-row alone, so a caller cannot read a row that was not gated");
    caps.properties.properties.limits.maxPushConstantsSize = XUD_DESCRIPTOR_PUSH_MIN_GUARANTEE;
    xud_check(xud_descriptor_fill_push_range(&caps, &range) == XUD_OK, "descriptor/fill-push-ok", "written");
    xud_check(range.stageFlags == XUD_DESCRIPTOR_PUSH_STAGES, "descriptor/push-stage-flags",
        "both stages, the same pair the per-frame buffer carries");
    xud_check((range.offset == 0u) && (range.size == XUD_DESCRIPTOR_PUSH_SIZE), "descriptor/push-row",
        "offset 0 and this rung's own size, which is under the spec's 128-byte guarantee the assert "
        "beside XUD_DESCRIPTOR_PUSH_MIN_GUARANTEE checks");

    /* The pool and its two rows.
     */
    xud_check(xud_descriptor_fill_pool_info(NULL) == XUD_INVALID_ARG, "descriptor/fill-pool-null", "is refused");
    xud_check(xud_descriptor_fill_pool_info(&d) == XUD_OK, "descriptor/fill-pool-ok", "assembled");
    xud_check(d.poolInfo.sType == VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, "descriptor/pool-stamped", "stamped");
    xud_check((d.poolInfo.pNext == NULL) && (d.poolInfo.flags == XUD_DESCRIPTOR_POOL_FLAGS), "descriptor/pool-flags",
        "the one core pool flag, and nothing chained: the UPDATE_AFTER_BIND and HOST_ONLY bits are "
        "extension-gated and this library enables no descriptor-pool extension");
    xud_check(d.poolInfo.maxSets == XUD_DESCRIPTOR_SET_COUNT, "descriptor/pool-max-sets",
        "sized for both sets rather than the one allocated today, so the texture step needs no seco"
        "nd pool and no pool recreated at runtime");
    xud_check((d.poolInfo.poolSizeCount == XUD_DESCRIPTOR_POOL_SIZE_COUNT) && (d.poolInfo.pPoolSizes == d.poolSizes),
        "descriptor/pool-two-rows", "one row per descriptor TYPE, pointing at the record's own array");
    xud_check((d.poolSizes[0u].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) && (d.poolSizes[0u].descriptorCount == 1u),
        "descriptor/pool-uniform-row", "one uniform buffer");
    xud_check((d.poolSizes[1u].type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
            && (d.poolSizes[1u].descriptorCount == XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE),
        "descriptor/pool-sampler-row",
        "the array's size rather than 1, because the count is the ceiling of what the pool's sets m"
        "ay hold in total and the texture step's set 1 is this array");

    /* The allocate-info, which names the pool and the layout rather than a set.
     */
    xud_check(xud_descriptor_fill_set_allocate_info(NULL) == XUD_INVALID_ARG, "descriptor/fill-alloc-null", "is refused");
    xud_check(xud_descriptor_fill_set_allocate_info(&d) == XUD_INVALID_ARG, "descriptor/fill-alloc-no-pool",
        "a record whose pool was never created is a bad state of this rung, not a bad argument from "
        "a caller");
    d.pool = (VkDescriptorPool)&fakePoolHandle;
    xud_check(xud_descriptor_fill_set_allocate_info(&d) == XUD_INVALID_ARG, "descriptor/fill-alloc-no-layout-0",
        "and the layout the set is allocated against is required too: an allocate-info that names a "
        "pool but no layout names nothing");
    d.setLayouts[XUD_DESCRIPTOR_SET_PER_FRAME] = (VkDescriptorSetLayout)&fakeLayoutHandle;
    xud_check(xud_descriptor_fill_set_allocate_info(&d) == XUD_OK, "descriptor/fill-alloc-ok", "assembled");
    xud_check(d.setAllocateInfo.sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, "descriptor/alloc-stamped", "stamped");
    xud_check((d.setAllocateInfo.pNext == NULL) && (d.setAllocateInfo.descriptorPool == d.pool), "descriptor/alloc-names-own-pool",
        "the pool this rung created");
    xud_check(d.setAllocateInfo.descriptorSetCount == 1u, "descriptor/alloc-one-set",
        "one set per allocate call, which is what makes set 1's later allocation a call of the tex"
        "ture step rather than a second row here");
    xud_check(d.setAllocateInfo.pSetLayouts == &d.setLayouts[XUD_DESCRIPTOR_SET_PER_FRAME], "descriptor/alloc-names-set-0-layout",
        "set 0's layout specifically, and by pointer into the record's own array, so the set and th"
        "e pipeline layout above cannot disagree about which layout set 0 is");

    /* The buffer of set 0.
     */
    xud_check(xud_descriptor_fill_buffer_info(NULL) == XUD_INVALID_ARG, "descriptor/fill-buffer-null", "is refused");
    xud_check(xud_descriptor_fill_buffer_info(&d) == XUD_OK, "descriptor/fill-buffer-ok", "assembled");
    xud_check(d.bufferInfo.sType == VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, "descriptor/buffer-stamped", "stamped");
    xud_check((d.bufferInfo.pNext == NULL) && (d.bufferInfo.flags == 0u), "descriptor/buffer-chain-clear", "nothing chained");
    xud_check(d.bufferInfo.size == XUD_DESCRIPTOR_UNIFORM_BLOCK_SIZE, "descriptor/buffer-is-the-block",
        "exactly as big as the block, not a rounded-up guess: the descriptor's range is a read of t"
        "his buffer, and a buffer smaller than the range is a read past the allocation");
    xud_check(d.bufferInfo.usage == XUD_DESCRIPTOR_BUFFER_USAGE, "descriptor/buffer-usage",
        "UNIFORM_BUFFER plus TRANSFER_DST, the second bit being the design sketch's per-frame vkCmd"
        "CopyBuffer into this buffer made legal rather than implementation-dependent");
    xud_check((d.bufferInfo.sharingMode == VK_SHARING_MODE_EXCLUSIVE) && (d.bufferInfo.queueFamilyIndexCount == 0u)
            && (d.bufferInfo.pQueueFamilyIndices == NULL),
        "descriptor/buffer-exclusive",
        "exclusive, and no queue family named: an unshared buffer needs no family list, and the one"
        "family the physical layer chose is already the device's queue");

    /* The write that binds the buffer into set 0.
     */
    xud_check(xud_descriptor_fill_write_info(NULL) == XUD_INVALID_ARG, "descriptor/fill-write-null", "is refused");
    xud_check(xud_descriptor_fill_write_info(&d) == XUD_INVALID_ARG, "descriptor/fill-write-no-set",
        "a record with no set allocated and no buffer bound is a write that names nothing, refused "
        "as a bad state of this rung");
    d.sets[XUD_DESCRIPTOR_SET_PER_FRAME] = (VkDescriptorSet)&fakeSetHandle;
    xud_check(xud_descriptor_fill_write_info(&d) == XUD_INVALID_ARG, "descriptor/fill-write-no-buffer",
        "the set alone is still not a write: the buffer it binds is required too");
    d.uniformBuffer = (VkBuffer)&fakeBufferHandle;
    xud_check(xud_descriptor_fill_write_info(&d) == XUD_OK, "descriptor/fill-write-ok", "assembled");
    xud_check(d.writeInfo.sType == VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, "descriptor/write-stamped", "stamped");
    xud_check(d.writeInfo.dstSet == d.sets[XUD_DESCRIPTOR_SET_PER_FRAME], "descriptor/write-targets-set-0",
        "set 0, the one set this rung allocated");
    xud_check((d.writeInfo.dstBinding == XUD_DESCRIPTOR_BINDING) && (d.writeInfo.dstArrayElement == 0u), "descriptor/write-binding",
        "the one binding of that set, from element 0, which is the same numbering the layout's own "
        "binding row declares");
    xud_check((d.writeInfo.descriptorCount == 1u) && (d.writeInfo.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
        "descriptor/write-one-uniform",
        "one descriptor of the same type the layout declared for that binding, which is the rule th"
        "at makes a write and a layout one agreement rather than two rows that happen to match");
    xud_check(d.writeInfo.pBufferInfo == &d.bufferRow, "descriptor/write-buffer-row-is-the-records",
        "the buffer info lives inside the record, so the write read after the call still points at "
        "storage that outlived it");
    xud_check((d.writeInfo.pImageInfo == NULL) && (d.writeInfo.pTexelBufferView == NULL), "descriptor/write-other-arrays-null",
        "the other two info arrays are NULL, which is what a UNIFORM_BUFFER write requires: exactly"
        "one of the three may be non-NULL for a given descriptorType");
    xud_check((d.bufferRow.buffer == d.uniformBuffer) && (d.bufferRow.offset == 0u), "descriptor/write-binds-whole-buffer",
        "the buffer from its start, which is the offset the allocator's carve was asked to bind at");
    xud_check(d.bufferRow.range == XUD_DESCRIPTOR_UNIFORM_BLOCK_SIZE, "descriptor/write-range-is-the-buffer",
        "the buffer's own size rather than the allocator's carve, which may be bigger: the descript"
        "or names the buffer's bytes, not the hole around it");

    /* The host-side bytes of set 0's block.
     */
    xud_check(xud_descriptor_fill_uniform_block(NULL) == XUD_INVALID_ARG, "descriptor/fill-block-null", "is refused");
    xud_check(xud_descriptor_fill_uniform_block(&d) == XUD_OK, "descriptor/fill-block-ok", "written");
    xud_check((d.uniformBlock.viewProj[0u] == 1.0f) && (d.uniformBlock.viewProj[5u] == 1.0f) && (d.uniformBlock.viewProj[10u] == 1.0f)
            && (d.uniformBlock.viewProj[15u] == 1.0f),
        "descriptor/block-identity-matrix",
        "the identity VP matrix, the one matrix that is a fact rather than a choice: a camera that "
        "is not identity is the camera step's and this rung owns none");
    xud_check((d.uniformBlock.viewProj[1u] == 0.0f) && (d.uniformBlock.viewProj[4u] == 0.0f), "descriptor/block-off-diagonals-zero",
        "the off-diagonals are zero, so the identity is written rather than left");
    xud_check((d.uniformBlock.cameraPos[0u] == 0.0f) && (d.uniformBlock.time == 0.0f) && (d.uniformBlock.fog[3u] == 0.0f)
            && (d.uniformBlock.ambient[3u] == 0.0f),
        "descriptor/block-rest-zero",
        "the rest of the block is zeros rather than whatever the allocation handed over, which is "
        "what makes the block's bytes a fact");
    xud_check((d.uniformBlock.lightCount == 0u) && (d.uniformBlock.reserved == 0u), "descriptor/block-no-lights",
        "no lights, which is the one honest answer this rung can give: the light array is the step "
        "above's");

    /* The bring-up's guards, headless: no loader, no device, so the rung refuses before it calls.
     */
    xud_check(xud_descriptor_open(NULL, NULL, NULL, NULL, NULL) == XUD_INVALID_ARG, "descriptor/open-null", "is refused");
    xud_check(xud_descriptor_open(&desc, NULL, NULL, NULL, NULL) == XUD_INVALID_ARG, "descriptor/open-nothing-else",
        "is refused, because a bring-up with no loader, no device, no caps and no allocator has not"
        "hing to gate on");
    xud_check(desc == NULL, "descriptor/open-refusal-leaves-out", "and the out-pointer stays NULL");
    xud_check(xud_descriptor_close(NULL, NULL, NULL, NULL) == XUD_INVALID_ARG, "descriptor/close-null-args",
        "is refused: a teardown with no loader, no device and no allocator has no rows to tear down"
        " through, and XUD_ALREADY is the answer for a record in hand that is already NULL, which i"
        "s a case the GPU run's own close_context covers");

    /* The record's own empty state, which is what makes the retire order safe on a bring-up that
       failed part-way up: every handle is the NULL sentinel and every count is zero.
     */
    memset(&d, 0, sizeof(d));
    xud_check(d.pool == VK_NULL_HANDLE, "descriptor/empty-pool", "no pool");
    xud_check(d.sets[XUD_DESCRIPTOR_SET_PER_FRAME] == VK_NULL_HANDLE, "descriptor/empty-set-0", "no set");
    xud_check(d.uniformBuffer == VK_NULL_HANDLE, "descriptor/empty-buffer", "no buffer");
    xud_check(d.uniformRange.memory == VK_NULL_HANDLE, "descriptor/empty-range", "and no allocator range");
    for (row = 0u; row < XUD_DESCRIPTOR_SET_COUNT; row++) {
        xud_check(d.setLayouts[row] == VK_NULL_HANDLE, "descriptor/empty-layout", "and no set layout");
    }
    xud_check((d.uniformStagingBuffer == VK_NULL_HANDLE) && (d.uniformStagingMapped == NULL) && (d.uniformStagingMappedLive == 0u),
        "descriptor/empty-arena",
        "and no staging arena, mapped or unmapped - the arena is created last, so a bring-up that failed part-way up never has one");

    /* The staging arena's create-info and the upload's one region, assembled here with no device in
       sight. These two are the pair the GPU run's arena and copy are checked against, and the rule
       the repo settles is that the arena is NOT ring memory: a ring carve is a batch whose bytes a
       later lap overwrites mid-frame, and the block's bytes have to survive from the host write to
       the copy that reads them.
    */
    xud_descriptor_fill_staging_info(&d);
    xud_check(d.uniformStagingInfo.sType == VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, "descriptor/staging-stamped",
        "the arena's create-info is stamped");
    xud_check(d.uniformStagingInfo.size == XUD_DESCRIPTOR_UNIFORM_BLOCK_SIZE, "descriptor/staging-size-one-block",
        "the arena is exactly one block long, because that is all a frame ever writes into it");
    xud_check(d.uniformStagingInfo.usage == XUD_DESCRIPTOR_STAGING_USAGE, "descriptor/staging-usage-src-only",
        "and it is a transfer SOURCE and nothing else - the arena is read by the copy, never by the shader");
    xud_check((d.uniformStagingInfo.usage & XUD_DESCRIPTOR_BUFFER_USAGE) == 0u, "descriptor/staging-not-uniform",
        "so the arena never claims the usage the buffer it feeds claims, which is what lets both live in one allocator");

    /* The two buffers the upload reads between have to exist for its row to be assembled at all,
       so the synthetic record carries the pair of stand-ins this area uses everywhere else above
       the gate - and the row below is then the one the GPU run's copy is checked against.
    */
    d.uniformBuffer = (VkBuffer)&fakeBufferHandle;
    d.uniformStagingBuffer = (VkBuffer)&fakeStagingHandle;

    xud_descriptor_fill_upload_info(&d);
    xud_check(d.uploadRegion.srcOffset == 0u, "descriptor/upload-src-at-zero", "the region starts at the arena's start");
    xud_check(d.uploadRegion.dstOffset == 0u, "descriptor/upload-dst-at-zero", "and at the buffer's");
    xud_check(d.uploadRegion.size == XUD_DESCRIPTOR_UNIFORM_BLOCK_SIZE, "descriptor/upload-extent-one-block",
        "and is one block long, which is the only extent that fits inside both");

    /* The one host write, refused rather than performed: an arena that is not mapped is a state this
       rung reports, and the headless run has no arena at all, so this is the refusal the GPU run's
       success is the other half of.
    */
    xud_check(xud_descriptor_write_block(NULL) == XUD_INVALID_ARG, "descriptor/write-block-null", "is refused");
    xud_check(xud_descriptor_write_block(&d) == XUD_UNSUPPORTED, "descriptor/write-block-no-arena",
        "with no arena mapped - the struct above is the empty one, so the refusal is the unmapped arena rather than a missing argument");

    /* The host-visible preference, read off the caps rather than assumed. The mask is what narrows
       the allocator's choice; the chosen type is still checked for the bit before anything maps it,
       which is the half the GPU run reads.
    */
    {
        XudDeviceCaps caps;
        uint32_t bits = 0u;
        memset(&caps, 0, sizeof(caps));
        caps.memoryProperties.memoryProperties.memoryTypeCount = 2u;
        caps.memoryProperties.memoryProperties.memoryTypes[0u].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        caps.memoryProperties.memoryProperties.memoryTypes[1u].propertyFlags
            = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        xud_check(xud_descriptor_host_visible_bits(&caps, NULL) == XUD_INVALID_ARG, "descriptor/host-visible-null-out", "is refused");
        xud_check(xud_descriptor_host_visible_bits(NULL, &bits) == XUD_INVALID_ARG, "descriptor/host-visible-no-caps", "is refused");
        xud_check(xud_descriptor_host_visible_bits(&caps, &bits) == XUD_OK, "descriptor/host-visible-ok", "as queried");
        xud_check(bits == 0x00000002u, "descriptor/host-visible-mask",
            "and the mask names only the type the caps mark host-visible, which is a read of the caps rather than a constant");
        caps.memoryProperties.memoryProperties.memoryTypes[1u].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        xud_check(xud_descriptor_host_visible_bits(&caps, &bits) == XUD_OK, "descriptor/host-visible-none-is-empty",
            "and a device with no host-visible type gives an empty mask rather than a failure - the refusal belongs where the arena is "
            "acquired, because an empty mask narrows the choice to nothing");
        xud_check(bits == 0u, "descriptor/host-visible-mask-empty", "and the mask really is empty");
    }
}

/*-----------------------------------------------------------------------------------------------
 * The descriptor-layout rung against the driver: the two set layouts, the pool, set 0, the per-fra
 * me uniform buffer carved out of the allocator's first block and bound into that set, all of it cr
 * eated by xud_init_context() as part of the ladder rather than by this test. What the test adds on
 * top of the bring-up is the read-back: the handles that are live, the set that is NOT allocated, th
 * e range the allocator handed out, and the caps' own limits read as the gate's answer rather than a
 * s a constant.
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_gpu_descriptor(void) {
    XudContext* context = NULL;
    XudResult result = XUD_OK;
    uint32_t blockCount = 0u;
    uint32_t liveCount = 0u;
    uint32_t row = 0u;
    VkDeviceSize bytes = 0u;
    VkDeviceSize cursor = 0u;

    result = xud_init_context(&context, 64u, 64u);
    xud_check(result == XUD_OK, "gpu-desc/context-completes",
        "the ladder completes with this rung in it, which is the descriptor's bring-up not refusing "
        "a real device's caps");
    if (context == NULL) {
        return;
    }

    xud_check(context->descriptor != NULL, "gpu-desc/descriptor-owned",
        "and the context owns the descriptor rung, between the allocator and the renderer");
    if (context->descriptor == NULL) {
        (void)xud_close_context(&context);
        return;
    }

    /* The gate read against the REAL caps rather than the synthetic row of the headless test: this
       machine's limits can have this layout, and the answer is read rather than assumed.
     */
    xud_check(xud_descriptor_caps_support(&context->physicalDevice->caps) == XUD_OK, "gpu-desc/caps-answer-ok",
        "the recorded caps answer OK for the layout as designed, so the sampler array's size is a s"
        "ize this device serves rather than a size the gate would refuse");
    xud_check(context->physicalDevice->caps.properties.properties.limits.maxBoundDescriptorSets >= XUD_DESCRIPTOR_SET_COUNT,
        "gpu-desc/bound-sets", "the device binds at least as many sets as this layout declares");
    xud_check(context->physicalDevice->caps.properties.properties.limits.maxPerStageDescriptorSamplers >= XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE,
        "gpu-desc/sampler-headroom",
        "and at least the array's size per stage, which is the one limit this rung's design could h"
        "ave crossed");
    xud_check(context->physicalDevice->caps.properties.properties.limits.maxPushConstantsSize >= XUD_DESCRIPTOR_PUSH_SIZE,
        "gpu-desc/push-headroom",
        "and at least this rung's push row, which is the assert beside XUD_DESCRIPTOR_PUSH_MIN_GUAR"
        "ANTEE read against a driver instead of against the spec's floor");

    /* Every handle of the rung is live, and the one that is not is the one this rung does not crea
       te by construction.
     */
    for (row = 0u; row < XUD_DESCRIPTOR_SET_COUNT; row++) {
        xud_check(context->descriptor->setLayouts[row] != VK_NULL_HANDLE, "gpu-desc/layout-live",
            "both set layouts exist, so the pipeline layout above names two real handles rather than t"
            "wo sentinels");
    }
    xud_check(context->descriptor->pool != VK_NULL_HANDLE, "gpu-desc/pool-live", "the pool exists");
    xud_check(context->descriptor->sets[XUD_DESCRIPTOR_SET_PER_FRAME] != VK_NULL_HANDLE, "gpu-desc/set-0-live",
        "set 0 was allocated from that pool against that layout");
    xud_check(context->descriptor->sets[XUD_DESCRIPTOR_SET_MATERIAL] == VK_NULL_HANDLE, "gpu-desc/set-1-not-allocated",
        "and set 1 was not: the layout is declared, the set is not, and the texture step is what al"
        "locates it from this same pool");
    xud_check(context->descriptor->uniformBuffer != VK_NULL_HANDLE, "gpu-desc/buffer-live", "the per-frame uniform buffer exists");
    xud_check(context->descriptor->uniformRange.memory != VK_NULL_HANDLE, "gpu-desc/buffer-has-memory",
        "and it is bound to memory the allocator owns, which is the reason this rung sits above the"
        " allocator rather than beside it");
    xud_check(
        context->descriptor->uniformRange.blockIndex == 0u, "gpu-desc/range-first-block", "in the first block this allocator ever made");
    xud_check(context->descriptor->uniformRange.offset == 0u, "gpu-desc/range-at-start",
        "at that block's start, because it is the first carve the allocator was ever asked for");
    xud_check(context->descriptor->uniformBindOffset == context->descriptor->uniformRange.offset, "gpu-desc/bind-offset-is-the-range's",
        "the buffer was bound at the offset the range names rather than at the block's start read a"
        "second time");
    xud_check(context->descriptor->uniformRange.size == XUD_DESCRIPTOR_UNIFORM_BLOCK_SIZE, "gpu-desc/range-is-the-block",
        "the range is the block's size rather than the allocator's rounded-up carve, which is what "
        "makes the descriptor's range a read of the buffer rather than of the hole around it");

    /* The write that bound it, read back after the call rather than before it: the row inside the
       record still says what the driver was handed.
     */
    xud_check(context->descriptor->writeInfo.pBufferInfo == &context->descriptor->bufferRow, "gpu-desc/write-row-still-inside-record",
        "the write's buffer info still points at the record's own row, which is the rule that makes "
        "a create-info read after the call point at storage that outlived it");
    xud_check((context->descriptor->bufferRow.buffer == context->descriptor->uniformBuffer)
            && (context->descriptor->bufferRow.range == XUD_DESCRIPTOR_UNIFORM_BLOCK_SIZE),
        "gpu-desc/write-binds-the-buffer", "and the row names this rung's buffer for the block's own bytes");
    xud_check(context->descriptor->writeInfo.dstSet == context->descriptor->sets[XUD_DESCRIPTOR_SET_PER_FRAME],
        "gpu-desc/write-targets-set-0", "and the set it wrote is the set this rung allocated");

    /* The staging arena the upload path needs, read back after the bring-up rather than before it:
       a host-visible carve of its own, mapped once for the rung's whole life rather than per frame,
       which is the batch rung's rule seen from the block's side of the ladder - and the block's own
       bytes sitting in it, which is what the frame loop's copy has to read.
     */
    xud_check((context->descriptor->uniformStagingBuffer != VK_NULL_HANDLE)
            && (context->descriptor->uniformStagingRange.memory != VK_NULL_HANDLE),
        "gpu-desc/arena-live", "the staging buffer exists over its own carve");
    xud_check(context->descriptor->uniformStagingRange.size >= XUD_DESCRIPTOR_UNIFORM_BLOCK_SIZE, "gpu-desc/arena-owns-its-block",
        "and the carve is the whole block rather than the block's own size, which is the one place the "
        "repo's allocate-once rule spends a block of its own - a mapping is per-VkDeviceMemory, so a "
        "carve that is mapped cannot share a block with one that is mapped too");
    xud_check((context->descriptor->uniformStagingMapped != NULL) && (context->descriptor->uniformStagingMappedLive == 1u),
        "gpu-desc/arena-mapped-open",
        "and the arena is mapped exactly once for the rung's life, which is the rule that keeps a pe"
        "r-frame map out of the frame loop");
    xud_check((xud_descriptor_write_block(context->descriptor) == XUD_OK)
            && (memcmp(context->descriptor->uniformStagingMapped, &context->descriptor->uniformBlock, XUD_DESCRIPTOR_UNIFORM_BLOCK_SIZE)
                == 0),
        "gpu-desc/arena-holds-the-block",
        "and the one host write put the block's bytes in it, which is the only way a host-visible mapp"
        "ing can be read back at all - and the read-back is the copy's source bytes, which is what mak"
        "es the upload's one region a fact rather than an intent");

    /* The push row survived the bring-up rather than being left at zero, which is the shape the val
       idation layer reports as a hard error (stageFlags zero, size zero) rather than something the
       driver forgives.
     */
    xud_check((context->descriptor->pushRange.size == XUD_DESCRIPTOR_PUSH_SIZE)
            && (context->descriptor->pushRange.stageFlags == XUD_DESCRIPTOR_PUSH_STAGES) && (context->descriptor->pushRange.offset == 0u),
        "gpu-desc/push-row-written",
        "the pipeline layout above was handed a range with a size and a stage pair in it, not the z"
        "ero state calloc left");

    /* The renderer above was built against this rung: its pipeline layout is live, and the two set
       layouts it names are the two this rung created.
     */
    xud_check(context->renderer != NULL, "gpu-desc/renderer-owned", "the renderer sits above this rung");
    if (context->renderer != NULL) {
        xud_check(context->renderer->pipeline->layout != VK_NULL_HANDLE, "gpu-desc/pipeline-layout-live",
            "and the pipeline layout it built is live, which is a layout built from two real set ha"
            "ndles and one real push row rather than from the zero state this rung opens with");
    }

    /* The allocator below this rung is no longer storage-only: the descriptor's buffer made one blo
       ck and one live range, the render pass's depth image made a second of each, and the batch run
       g below the renderer made a third block and six more live ranges - its arena and its five dat
       a buffers are six requests small enough to share blocks, which is the suballocation half of t
       he repo's "allocate once, suballocate always" rule seen from the other side of the ladder. Ev
       ery number below is therefore read as a delta on this baseline rather than as a constant.
     */
    xud_check(
        xud_allocator_stats(context->allocator, &blockCount, &liveCount, &bytes, &cursor) == XUD_OK, "gpu-desc/stats-answers", "answers");
    xud_check(blockCount == 4u, "gpu-desc/four-blocks",
        "four VkDeviceMemory: the descriptor's uniform buffer, the render pass's depth image (the firs"
        "t thing in this tree that asked for an IMAGE's requirements, whose alignment is not a buffer's"
        ") and the two host-visible arenas, this rung's staging arena and the batch's, which are block"
        "s of their own because a mapping is per-VkDeviceMemory rather than per-range");
    xud_check(liveCount == 9u, "gpu-desc/nine-live-ranges",
        "nine live ranges over them: the uniform buffer's carve, the depth image's, the two arenas eac"
        "ch of which is a whole block, and the batch's four streams plus its index buffer, which is wh"
        "at a completed context owns now");
    {
        uint32_t stream = 0u;
        VkDeviceSize batchCarves = 0u;
        for (stream = 0u; stream < XUD_BATCH_STREAM_COUNT; stream++) {
            batchCarves += context->renderer->batch->stream[stream].range.size;
        }
        batchCarves += context->renderer->batch->stagingRange.size;
        batchCarves += context->renderer->batch->indexRange.size;
        xud_check(bytes
                == (context->descriptor->uniformRange.size + context->descriptor->uniformStagingRange.size
                    + context->renderer->renderPass->depthRange.size + batchCarves),
            "gpu-desc/bytes-are-every-carve",
            "the reserved bytes are exactly every carve added up, which is what makes the counter a fa"
            "ct rather than an estimate - and the batch's six are in that sum, so the rung below this o"
            "ne is being counted rather than assumed");
    }
    xud_check(cursor == 0u, "gpu-desc/cursor-untouched",
        "and the ring's cursor is still at the start: a per-frame uniform buffer is static, not transi"
        "ent, and so is the batch's arena - the batch's own note says why a ring is the wrong place fo"
        "r it, and the ring being untouched here is that note read as a fact");

    if (context->validate != 0u) {
        xud_check(context->diagnostics->errorMessages == 0u, "gpu-desc/validation-clean",
            "the layouts, the pool, the set, the buffer, the bind and the write are validation-clean"
            " rather than merely crash-free, which is the stamped create-infos, the pool's maxSets a"
            "nd the buffer's usage bits read as facts");
    }

    result = xud_close_context(&context);
    xud_check(result == XUD_OK, "gpu-desc/close-clean",
        "the whole ladder tears back down with nothing refused, which is the descriptor's teardown "
        "retiring the pool (and set 0 with it) before either layout goes and before the buffer does");
    xud_check(context == NULL, "gpu-desc/close-clears", "and the out-param is cleared");
}

void xud_test_descriptor(void) {
    xud_test_descriptor_pure();
    if (xud_gpu_tests_enabled()) {
        xud_test_gpu_descriptor();
    }
}
