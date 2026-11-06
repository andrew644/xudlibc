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
    uint64_t fakeImageHandle = 0x4600u;
    uint64_t fakeImageViewHandle = 0x4700u;
    uint64_t fakeSamplerHandle = 0x4800u;
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
    caps.properties.properties.limits.maxImageDimension2D = 8192u;

    xud_check(xud_descriptor_caps_support(NULL) == XUD_INVALID_ARG, "descriptor/caps-null",
        "is refused: a rung with no recorded caps has nothing to gate itself on");
    xud_check(xud_descriptor_caps_support(&caps) == XUD_OK, "descriptor/caps-can-have-it", "the layout as designed fits this caps row");

    /* Each refusal in turn, one limit at a time, so the gate's compares are seven observable rules
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

    caps.properties.properties.limits.maxImageDimension2D = 0u;
    xud_check(xud_descriptor_caps_support(&caps) == XUD_UNSUPPORTED, "descriptor/caps-no-image-room",
        "the white default every entry of set 1 names is an image this rung creates, so the one image bound the caps can refuse is refused "
        "on its own too");
    caps.properties.properties.limits.maxImageDimension2D = 8192u;

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

    /* The allocate-infos, which name the pool and the layout rather than a set. BOTH sets of this
       layout are allocated, so there are two rows of the array below rather than one, and the index
       that picks the row is the set number the pipeline layout's pSetLayouts array already fixed.
     */
    xud_check(xud_descriptor_fill_set_allocate_info(NULL, XUD_DESCRIPTOR_SET_PER_FRAME) == XUD_INVALID_ARG, "descriptor/fill-alloc-null",
        "is refused");
    xud_check(xud_descriptor_fill_set_allocate_info(&d, XUD_DESCRIPTOR_SET_COUNT) == XUD_INVALID_ARG, "descriptor/fill-alloc-bad-index",
        "a set index past this layout's own set count is refused rather than read off the end of the array");
    xud_check(xud_descriptor_fill_set_allocate_info(&d, XUD_DESCRIPTOR_SET_PER_FRAME) == XUD_INVALID_ARG, "descriptor/fill-alloc-no-pool",
        "a record whose pool was never created is a bad state of this rung, not a bad argument from "
        "a caller");
    d.pool = (VkDescriptorPool)&fakePoolHandle;
    xud_check(xud_descriptor_fill_set_allocate_info(&d, XUD_DESCRIPTOR_SET_PER_FRAME) == XUD_INVALID_ARG,
        "descriptor/fill-alloc-no-layout-0",
        "and the layout the set is allocated against is required too: an allocate-info that names a "
        "pool but no layout names nothing");
    d.setLayouts[XUD_DESCRIPTOR_SET_PER_FRAME] = (VkDescriptorSetLayout)&fakeLayoutHandle;
    xud_check(xud_descriptor_fill_set_allocate_info(&d, XUD_DESCRIPTOR_SET_PER_FRAME) == XUD_OK, "descriptor/fill-alloc-0-ok", "assembled");
    xud_check(d.setAllocateInfo[XUD_DESCRIPTOR_SET_PER_FRAME].sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        "descriptor/alloc-0-stamped", "stamped");
    xud_check((d.setAllocateInfo[XUD_DESCRIPTOR_SET_PER_FRAME].pNext == NULL)
            && (d.setAllocateInfo[XUD_DESCRIPTOR_SET_PER_FRAME].descriptorPool == d.pool),
        "descriptor/alloc-0-names-own-pool", "the pool this rung created");
    xud_check(d.setAllocateInfo[XUD_DESCRIPTOR_SET_PER_FRAME].descriptorSetCount == 1u, "descriptor/alloc-0-one-set",
        "one set per allocate call, which is what makes the two sets of this layout two calls rather than one call that names both");
    xud_check(d.setAllocateInfo[XUD_DESCRIPTOR_SET_PER_FRAME].pSetLayouts == &d.setLayouts[XUD_DESCRIPTOR_SET_PER_FRAME],
        "descriptor/alloc-0-names-set-0-layout",
        "set 0's layout specifically, and by pointer into the record's own array, so the set and the pipeline layout above cannot "
        "disagree about which layout set 0 is");

    /* Set 1's allocate-info, which needs its own layout live before it can be filled at all - the s
       econd half of the pair the layout declares.
     */
    xud_check(xud_descriptor_fill_set_allocate_info(&d, XUD_DESCRIPTOR_SET_MATERIAL) == XUD_INVALID_ARG,
        "descriptor/fill-alloc-no-layout-1",
        "set 1's layout is required too, so a half-filled layout is refused rather than half-allocated");
    d.setLayouts[XUD_DESCRIPTOR_SET_MATERIAL] = (VkDescriptorSetLayout)&fakeLayoutHandle;
    xud_check(xud_descriptor_fill_set_allocate_info(&d, XUD_DESCRIPTOR_SET_MATERIAL) == XUD_OK, "descriptor/fill-alloc-1-ok", "assembled");
    xud_check(d.setAllocateInfo[XUD_DESCRIPTOR_SET_MATERIAL].sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        "descriptor/alloc-1-stamped", "stamped");
    xud_check(d.setAllocateInfo[XUD_DESCRIPTOR_SET_MATERIAL].descriptorSetCount == 1u, "descriptor/alloc-1-one-set",
        "one set per allocate call, the same row as set 0's");
    xud_check(d.setAllocateInfo[XUD_DESCRIPTOR_SET_MATERIAL].pSetLayouts == &d.setLayouts[XUD_DESCRIPTOR_SET_MATERIAL],
        "descriptor/alloc-1-names-set-1-layout",
        "set 1's layout specifically, which is what makes the sampler array's set the one the pipeline layout numbers 1 rather than a "
        "second layout that happens to match");

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

    /* The white default every entry of set 1's sampler array names, and the two transitions around
       its fill. These are the rows of the ONE image this rung owns, and the reason it owns one at a
       ll is a rule about the set above rather than a rule about a shader: an allocated sampler arra
       y is only legal once every entry of it names a real view and a real sampler.
     */
    xud_check(xud_descriptor_fill_white_infos(NULL) == XUD_INVALID_ARG, "descriptor/fill-white-null", "is refused");
    xud_check(xud_descriptor_fill_white_infos(&d) == XUD_OK, "descriptor/fill-white-ok", "assembled");
    xud_check(d.whiteImageInfo.sType == VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, "descriptor/white-image-stamped", "stamped");
    xud_check((d.whiteImageInfo.pNext == NULL) && (d.whiteImageInfo.flags == 0u), "descriptor/white-image-chain-clear", "nothing chained");
    xud_check((d.whiteImageInfo.extent.width == 1u) && (d.whiteImageInfo.extent.height == 1u) && (d.whiteImageInfo.extent.depth == 1u),
        "descriptor/white-is-one-texel", "one texel, which is the smallest image that can hold the one colour a default has to hold");
    xud_check((d.whiteImageInfo.mipLevels == XUD_DESCRIPTOR_WHITE_MIP_LEVELS) && (d.whiteImageInfo.arrayLayers == 1u)
            && (d.whiteImageInfo.samples == VK_SAMPLE_COUNT_1_BIT),
        "descriptor/white-one-level", "one level, one layer, one sample: a default is not a chain and not an array and not multisampled");
    xud_check((d.whiteImageInfo.format == XUD_DESCRIPTOR_WHITE_FORMAT) && (d.whiteImageInfo.tiling == XUD_DESCRIPTOR_WHITE_TILING)
            && (d.whiteImageInfo.usage == XUD_DESCRIPTOR_WHITE_USAGE),
        "descriptor/white-format-usage",
        "the texture rung's format, OPTIMAL tiling and SAMPLED+TRANSFER_DST, which is what makes one image in this library have one format "
        "and one set of rules about it");
    xud_check(d.whiteImageInfo.initialLayout == XUD_DESCRIPTOR_WHITE_INITIAL_LAYOUT, "descriptor/white-initial-layout",
        "UNDEFINED at the create, the same shape the texture rung's images are created in");
    xud_check(d.whiteSamplerInfo.sType == VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, "descriptor/white-sampler-stamped", "stamped");
    xud_check((d.whiteSamplerInfo.magFilter == VK_FILTER_LINEAR) && (d.whiteSamplerInfo.minFilter == VK_FILTER_LINEAR)
            && (d.whiteSamplerInfo.mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR),
        "descriptor/white-sampler-filters",
        "linear on both axes and linear LOD selection, the same filter a texture of this library is sampled with");
    xud_check((d.whiteSamplerInfo.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE)
            && (d.whiteSamplerInfo.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE)
            && (d.whiteSamplerInfo.addressModeW == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE),
        "descriptor/white-sampler-address",
        "clamped on all three axes, so a UV outside the unit square reads the edge texel rather than wrapping");
    xud_check((d.whiteSamplerInfo.compareEnable == VK_FALSE) && (d.whiteSamplerInfo.compareOp == VK_COMPARE_OP_ALWAYS),
        "descriptor/white-sampler-no-compare",
        "no compare: a compareOp other than ALWAYS is a shadow-mapping decision this library does not take");
    xud_check(
        (d.whiteSamplerInfo.borderColor == VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE) && (d.whiteSamplerInfo.unnormalizedCoordinates == VK_FALSE),
        "descriptor/white-sampler-border",
        "an opaque-white border and normalised coordinates, so even a read past the edge of the default is the identity of the textured "
        "frag's multiply");
    xud_check(d.whiteViewInfo.sType == VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, "descriptor/white-view-stamped", "stamped");
    xud_check((d.whiteViewInfo.image == VK_NULL_HANDLE) && (d.whiteViewInfo.viewType == VK_IMAGE_VIEW_TYPE_2D)
            && (d.whiteViewInfo.format == XUD_DESCRIPTOR_WHITE_FORMAT),
        "descriptor/white-view-2d",
        "a 2D view of the default's own format, and no image yet: the handle is stamped into this row by the bring-up, which is the only "
        "place that has one");
    xud_check((d.whiteViewInfo.subresourceRange.aspectMask == XUD_DESCRIPTOR_WHITE_ASPECT_MASK)
            && (d.whiteViewInfo.subresourceRange.levelCount == XUD_DESCRIPTOR_WHITE_MIP_LEVELS)
            && (d.whiteViewInfo.subresourceRange.layerCount == 1u),
        "descriptor/white-view-range", "the colour plane of one level and one layer, which is the whole of a 1x1 image");
    xud_check((d.whiteClearColor.float32[0u] == 1.0f) && (d.whiteClearColor.float32[1u] == 1.0f) && (d.whiteClearColor.float32[2u] == 1.0f)
            && (d.whiteClearColor.float32[3u] == 1.0f),
        "descriptor/white-clear-is-white",
        "opaque white, which is the identity of the textured frag's multiply: a textured draw before any texture exists draws the vertex "
        "colour");
    xud_check((d.whiteSubresourceRange.aspectMask == XUD_DESCRIPTOR_WHITE_ASPECT_MASK)
            && (d.whiteSubresourceRange.levelCount == XUD_DESCRIPTOR_WHITE_MIP_LEVELS) && (d.whiteSubresourceRange.layerCount == 1u),
        "descriptor/white-range-is-one-row",
        "the clear and the two transitions share one subresource row rather than stamping three that could disagree");

    xud_check(xud_descriptor_fill_white_transitions(NULL) == XUD_INVALID_ARG, "descriptor/fill-white-barriers-null", "is refused");
    xud_check(xud_descriptor_fill_white_transitions(&d) == XUD_OK, "descriptor/fill-white-barriers-ok", "assembled");
    xud_check((d.whiteToUpload.sType == VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER)
            && (d.whiteToSampled.sType == VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER),
        "descriptor/white-barriers-stamped",
        "both rows are image barriers rather than memory barriers, because a layout transition is about an image");
    xud_check((d.whiteToUpload.oldLayout == XUD_DESCRIPTOR_WHITE_INITIAL_LAYOUT)
            && (d.whiteToUpload.newLayout == XUD_DESCRIPTOR_WHITE_UPLOAD_LAYOUT),
        "descriptor/white-into-upload-layout",
        "UNDEFINED into TRANSFER_DST: UNDEFINED is what UNDEFINED means to a barrier, namely whatever layout this subresource is in, which "
        "is true on the first frame and honest on every later one");
    xud_check((d.whiteToSampled.oldLayout == XUD_DESCRIPTOR_WHITE_UPLOAD_LAYOUT)
            && (d.whiteToSampled.newLayout == XUD_DESCRIPTOR_WHITE_SAMPLED_LAYOUT),
        "descriptor/white-into-sampled-layout", "TRANSFER_DST into SHADER_READ_ONLY, the one layout a sampled descriptor may read");
    xud_check((d.whiteToUpload.image == VK_NULL_HANDLE) && (d.whiteToSampled.image == VK_NULL_HANDLE),
        "descriptor/white-barriers-image-stamped-later",
        "the bring-up has not created the image yet, so the handle is stamped into these two rows by the upload rather than here");
    xud_check((d.whiteToUpload.subresourceRange.aspectMask == XUD_DESCRIPTOR_WHITE_ASPECT_MASK)
            && (d.whiteToSampled.subresourceRange.aspectMask == XUD_DESCRIPTOR_WHITE_ASPECT_MASK),
        "descriptor/white-barriers-share-the-range",
        "both barriers name the range the view names, so a barrier cannot be about an image the view does not look at");

    /* The write that bound set 1's sampler array.
     */
    xud_check(xud_descriptor_fill_sampler_write_info(NULL) == XUD_INVALID_ARG, "descriptor/fill-sampler-write-null", "is refused");
    xud_check(xud_descriptor_fill_sampler_write_info(&d) == XUD_INVALID_ARG, "descriptor/fill-sampler-write-no-default",
        "a set allocated against a sampler layout and no white view behind it is a write that names nothing, refused as a bad state of "
        "this rung");
    d.whiteImage = (VkImage)&fakeImageHandle;
    d.whiteImageView = (VkImageView)&fakeImageViewHandle;
    d.whiteSampler = (VkSampler)&fakeSamplerHandle;
    d.sets[XUD_DESCRIPTOR_SET_MATERIAL] = (VkDescriptorSet)&fakeSetHandle;
    xud_check(xud_descriptor_fill_sampler_write_info(&d) == XUD_OK, "descriptor/fill-sampler-write-ok", "assembled");
    xud_check(d.samplerWriteInfo.sType == VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, "descriptor/sampler-write-stamped", "stamped");
    xud_check(d.samplerWriteInfo.dstSet == d.sets[XUD_DESCRIPTOR_SET_MATERIAL], "descriptor/sampler-write-targets-set-1",
        "set 1, the sampler array's set, which is the set the pipeline layout numbers 1 rather than a second layout that happens to match");
    xud_check((d.samplerWriteInfo.dstBinding == XUD_DESCRIPTOR_BINDING) && (d.samplerWriteInfo.dstArrayElement == 0u),
        "descriptor/sampler-write-binding",
        "the one binding of that set, from element 0, which is the same numbering the layout's own binding row declares");
    xud_check((d.samplerWriteInfo.descriptorCount == XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE)
            && (d.samplerWriteInfo.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
        "descriptor/sampler-write-array",
        "every entry of the array and the type the layout declared for that binding, which is the rule that makes a write and a layout one "
        "agreement rather than two rows that happen to match");
    xud_check(d.samplerWriteInfo.pImageInfo == d.samplerRows, "descriptor/sampler-write-rows-are-the-records",
        "the image infos live inside the record, so the write read after the call still points at storage that outlived it");
    xud_check((d.samplerWriteInfo.pBufferInfo == NULL) && (d.samplerWriteInfo.pTexelBufferView == NULL),
        "descriptor/sampler-write-other-arrays-null",
        "the other two info arrays are NULL, which is what a COMBINED_IMAGE_SAMPLER write requires: exactly one of the three may be "
        "non-NULL for a given descriptorType");
    {
        uint32_t slot = 0u;
        for (slot = 0u; slot < XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE; slot++) {
            xud_check((d.samplerRows[slot].imageView == d.whiteImageView) && (d.samplerRows[slot].sampler == d.whiteSampler),
                "descriptor/sampler-write-every-slot-names-the-default",
                "every slot of the array names the same view and the same sampler, which is the point of the default: the array is a slot "
                "axis and the texture step is what puts different views into different rows of it");
            xud_check(d.samplerRows[slot].imageLayout == XUD_DESCRIPTOR_WHITE_SAMPLED_LAYOUT, "descriptor/sampler-write-slot-layout",
                "the layout the transition barrier above leaves the image in, which is what makes the descriptor's idea of the layout and "
                "the barrier's one one agreement");
        }
    }

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

    /* The free list of set 1's sampler array: the pick, the one-slot write and the two driver
       calls' own headless refusals.
     */
    {
        uint32_t slot = 99u;

        xud_check(xud_descriptor_pick_free_slot(NULL, &slot) == XUD_INVALID_ARG, "descriptor/pick-free-null", "is refused");
        xud_check(xud_descriptor_pick_free_slot(&d, NULL) == XUD_INVALID_ARG, "descriptor/pick-free-null-out",
            "and an out-slot with nowhere to be written is refused");
        xud_check(xud_descriptor_pick_free_slot(&d, &slot) == XUD_OK, "descriptor/pick-free-first", "an empty mask answers OK");
        xud_check(slot == 0u, "descriptor/pick-free-is-zero", "on the lowest free bit, which is slot 0 while the mask is empty");

        d.samplerSlotMask = XUD_DESCRIPTOR_SLOT_MASK_BIT(0u) | XUD_DESCRIPTOR_SLOT_MASK_BIT(1u);
        xud_check(xud_descriptor_pick_free_slot(&d, &slot) == XUD_OK, "descriptor/pick-free-after-two", "two taken slots still answer OK");
        xud_check(slot == 2u, "descriptor/pick-free-is-two", "on the lowest FREE bit rather than the lowest bit");

        {
            uint32_t full = 0u;
            slot = 99u;
            for (row = 0u; row < XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE; row++) {
                full |= XUD_DESCRIPTOR_SLOT_MASK_BIT(row);
            }
            d.samplerSlotMask = full;
            xud_check(xud_descriptor_pick_free_slot(&d, &slot) == XUD_OUT_OF_RESOURCES, "descriptor/pick-free-full",
                "and a mask with every bit set is the array's ceiling, which is a refusal rather than a slot that does not exist");
            xud_check(slot == 99u, "descriptor/pick-free-full-touched-nothing", "and the out-slot was not written on the refusal");
        }

        /* The one-slot write, and the refusals it states before the one it assembles. */
        d.samplerSlotMask = 0u;
        xud_check(xud_descriptor_fill_texture_slot_write(
                      NULL, 0u, (VkImageView)&fakeImageViewHandle, (VkSampler)&fakeSamplerHandle, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                == XUD_INVALID_ARG,
            "descriptor/slot-write-null", "is refused");
        xud_check(xud_descriptor_fill_texture_slot_write(
                      &d, 0u, (VkImageView)&fakeImageViewHandle, (VkSampler)&fakeSamplerHandle, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                == XUD_INVALID_ARG,
            "descriptor/slot-write-no-set",
            "and a record whose set-1 handle is not live is refused, because the write would name a set "
            "that is not there");
        xud_check(xud_descriptor_fill_texture_slot_write(&d, XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE, (VkImageView)&fakeImageViewHandle,
                      (VkSampler)&fakeSamplerHandle, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                == XUD_INVALID_ARG,
            "descriptor/slot-write-past-the-array", "and a slot at the array's size is an index into nothing");
        d.sets[XUD_DESCRIPTOR_SET_MATERIAL] = (VkDescriptorSet)&fakeSetHandle;
        xud_check(xud_descriptor_fill_texture_slot_write(
                      &d, 0u, VK_NULL_HANDLE, (VkSampler)&fakeSamplerHandle, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                == XUD_INVALID_ARG,
            "descriptor/slot-write-no-view", "and a write with no view names nothing, which is a COMBINED_IMAGE_SAMPLER that is not one");
        xud_check(xud_descriptor_fill_texture_slot_write(
                      &d, 3u, (VkImageView)&fakeImageViewHandle, (VkSampler)&fakeSamplerHandle, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                == XUD_OK,
            "descriptor/slot-write-ok", "and the same write with the pair and a slot inside the array is assembled");
        xud_check(d.textureSlotWriteInfo.sType == VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, "descriptor/slot-write-stamped", "stamped");
        xud_check((d.textureSlotWriteInfo.dstSet == d.sets[XUD_DESCRIPTOR_SET_MATERIAL])
                && (d.textureSlotWriteInfo.dstBinding == XUD_DESCRIPTOR_BINDING) && (d.textureSlotWriteInfo.dstArrayElement == 3u),
            "descriptor/slot-write-targets-the-slot",
            "set 1, its one binding, and the slot the caller named rather than element 0 - the element is what differs from the "
            "whole-array write, not the set or the binding");
        xud_check((d.textureSlotWriteInfo.descriptorCount == 1u)
                && (d.textureSlotWriteInfo.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            "descriptor/slot-write-one-element",
            "one element of the array's own type, which is what keeps the other entries of the array untouched");
        xud_check((d.textureSlotWriteInfo.pImageInfo == &d.textureSlotRow)
                && (d.textureSlotRow.imageView == (VkImageView)&fakeImageViewHandle)
                && (d.textureSlotRow.sampler == (VkSampler)&fakeSamplerHandle)
                && (d.textureSlotRow.imageLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            "descriptor/slot-write-row-is-the-record's",
            "and the row lives inside the record, so the write read after the call still points at storage that outlived it");
        xud_check((d.textureSlotWriteInfo.pBufferInfo == NULL) && (d.textureSlotWriteInfo.pTexelBufferView == NULL),
            "descriptor/slot-write-other-arrays-null",
            "and the other two info arrays are NULL for the reason the whole-array write states: exactly one of the three may be non-NULL "
            "for a given type");

        /* The two driver calls, headless: no loader at all, and a loader that is closed - the rung
           refuses before it would call.
         */
        xud_check(xud_descriptor_bind_texture_slot(NULL, NULL, NULL, (VkImageView)&fakeImageViewHandle, (VkSampler)&fakeSamplerHandle,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, NULL)
                == XUD_INVALID_ARG,
            "descriptor/bind-null", "is refused");
        xud_check(xud_descriptor_bind_texture_slot(&d, NULL, NULL, (VkImageView)&fakeImageViewHandle, (VkSampler)&fakeSamplerHandle,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &slot)
                == XUD_INVALID_ARG,
            "descriptor/bind-no-loader", "and a bind with no loader has no update row to make the write through");
        {
            XudLoader closedLoader;
            XudDevice noDevice;
            memset(&closedLoader, 0, sizeof(closedLoader));
            memset(&noDevice, 0, sizeof(noDevice));
            d.samplerSlotMask = XUD_DESCRIPTOR_SLOT_MASK_BIT(0u);
            xud_check(xud_descriptor_bind_texture_slot(&d, &closedLoader, &noDevice, (VkImageView)&fakeImageViewHandle,
                          (VkSampler)&fakeSamplerHandle, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &slot)
                    == XUD_UNSUPPORTED,
                "descriptor/bind-closed-loader", "and a closed loader is the same refusal the upload states one rung over");
            xud_check(xud_descriptor_release_texture_slot(&d, &closedLoader, &noDevice, 0u) == XUD_UNSUPPORTED,
                "descriptor/release-closed-loader", "and the release answers the same way, on a slot the mask says is taken");
        }
        d.samplerSlotMask = 0u;
        xud_check(xud_descriptor_release_texture_slot(NULL, NULL, NULL, 0u) == XUD_INVALID_ARG, "descriptor/release-null", "is refused");
        xud_check(xud_descriptor_release_texture_slot(&d, NULL, NULL, 0u) == XUD_INVALID_ARG, "descriptor/release-no-loader",
            "and a release with no loader is a bad argument for the reason the bind above gives");
        xud_check(xud_descriptor_release_texture_slot(&d, NULL, NULL, XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE) == XUD_INVALID_ARG,
            "descriptor/release-past-the-array", "and a slot at the array's size is an index into nothing, on either driver call");
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
    xud_check(context->descriptor->sets[XUD_DESCRIPTOR_SET_MATERIAL] != VK_NULL_HANDLE, "gpu-desc/set-1-allocated",
        "and set 1 is allocated too, from the same pool against the same layout: an allocated sampler array whose entries were never "
        "written is an invalid descriptor, so the set and the white default behind every entry of it are this rung's, and the texture step "
        "is what puts a different view into a different row of it");
    xud_check((context->descriptor->whiteImage != VK_NULL_HANDLE) && (context->descriptor->whiteImageView != VK_NULL_HANDLE)
            && (context->descriptor->whiteSampler != VK_NULL_HANDLE),
        "gpu-desc/white-default-live",
        "and the one image behind all 16 rows of that array is live: one view and one sampler rather than 16 of each, which is what makes "
        "the array a slot axis and not a texture axis");
    xud_check(context->descriptor->whiteRange.memory != VK_NULL_HANDLE, "gpu-desc/white-default-has-memory",
        "and it is bound to memory the allocator owns, the same way the uniform buffer is, because an image this rung creates is storage "
        "this rung must release");
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
       ck and one live range, its white default made a second block and a second live range, the rend
       er pass's depth image made a third of each, and the batch rung below the renderer made a fourt
       h block and six more live ranges - its arena and its five data buffers are six requests small
       enough to share blocks, which is the suballocation half of the repo's "allocate once, suballoca
       te always" rule seen from the other side of the ladder. Every number below is therefore read a
       s a delta on this baseline rather than as a constant.
     */
    xud_check(
        xud_allocator_stats(context->allocator, &blockCount, &liveCount, &bytes, &cursor) == XUD_OK, "gpu-desc/stats-answers", "answers");
    xud_check(blockCount == 5u, "gpu-desc/five-blocks",
        "five VkDeviceMemory: the descriptor's uniform buffer, the render pass's depth image, the white default's own image (the second "
        "thing in this tree that asked for an IMAGE's requirements, and its alignment does not fit a hole in the depth image's block "
        "either) and the two host-visible arenas, this rung's staging arena and the batch's, which are blocks of their own because a "
        "mapping is per-VkDeviceMemory rather than per-range");
    xud_check(liveCount == 10u, "gpu-desc/ten-live-ranges",
        "ten live ranges over them: the uniform buffer's carve, the white default's, the depth image's, the two arenas each of which is a "
        "whole block, and the batch's four streams plus its index buffer, which is what a completed context owns now");
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
                    + context->descriptor->whiteRange.size + context->renderer->renderPass->depthRange.size + batchCarves),
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
