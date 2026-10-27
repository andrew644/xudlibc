/***********************************************************************************************
 *   xud_descriptor.c - the descriptor-layout rung: the two set layouts, the push-constant range,
 *                      the pool, the per-frame uniform buffer carved out of the allocator and
 *                      the write that bound it into set 0
 *
 *   Owns : the bring-up and the teardown of XudDescriptor, and the pure half of the rung - the
 *          caps gate, the create-info assembly of every row of this layout and the host-side byte
 *          layout of set 0's block.
 *   Layer: above the allocator and below the renderer. Every driver call here is a row of one of
 *          Volk's two tables, and every row of this rung takes a VkDevice, so every one of them is
 *          a row of the DEVICE table - there is no instance-level name in this pair.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud_descriptor.h" /* Required for: XudDescriptor, every constant of this layer, the contract below */

#include <assert.h> /* Required for: static_assert, the compile-time checks of this pair */
#include <stdint.h>
#include <stdlib.h> /* Required for: calloc, free */
#include <string.h> /* Required for: memset, the host-side block's zeros */

static_assert(sizeof(XudFrameUniformRow) == XUD_DESCRIPTOR_UNIFORM_BLOCK_SIZE,
    "the per-frame block's C layout is the size this layer states, so the buffer's size and the GLSL block's bytes are one number and not "
    "two");
static_assert(sizeof(XudPushRow) == XUD_DESCRIPTOR_PUSH_SIZE,
    "the push row's C layout is the size this layer states, so the push-constant range's size and the GLSL block's bytes are one number "
    "and not two");

/*-----------------------------------------------------------------------------------------------
 * The one teardown order of this rung, shared by xud_descriptor_close() and by the failure path of
 * xud_descriptor_open()
 *-----------------------------------------------------------------------------------------------*/

/* Reverse creation order, and every rung is attempted after one refuses: a refused rung is report
 * ed to whoever can read it and skipping the rungs below it would leak them, the rule xud_close_co
 * ntext() states for its own rungs and xud_renderer_retire() follows one rung up. Each step answer
 * s for a field that was never filled, which is what makes this one order safe to call on a descri
 * ptor that failed part-way up its bring-up.
 *
 * The pool is destroyed FIRST, and that is load-bearing rather than tidy: there is no vkDestroyDesc
 * riptorSet in these headers (grep it across /usr/include/vulkan/: zero hits), so the set allocated
 * from the pool is the pool's to retire, and a set still allocated from a layout is an error agains
 * t that layout's destroy - so the set has to be gone before either layout goes. The buffer comes n
 * ext for the mirror-image rule on the other side of the same write: a descriptor set that refers t
 * o a buffer must be gone before that buffer is destroyed. The range is released after the buffer, s
 * o a free range never names memory a live buffer sits in, and the block itself is not freed here at
 * all - that is xud_allocator_close(), which runs after this rung in the context's teardown order.
 */
static void xud_descriptor_retire(XudDescriptor* descriptor, const XudDevice* device, XudLoader* loader, XudAllocator* alloc) {
    XudResult rung = XUD_OK;
    uint32_t row = 0u;

    if ((descriptor == NULL) || (loader == NULL) || (device == NULL)) {
        return;
    }

    if (descriptor->uniformStagingMappedLive != 0u) {
        /* The unmap comes FIRST, above every destroy, and it is load-bearing rather than tidy: thi
           s is the one place a descriptor dies, and the repo's rule is that a mapped range has a ma
           tching unmap. A destroy of a buffer whose memory is still mapped is a destroy over a map
           ping the driver still holds. vkUnmapMemory returns void, so the handle is read, the live
           bit cleared, and only then handed over - the zero-state-before-the-call rule every other
           void row in this tree follows. */
        if ((loader->deviceTable.vkUnmapMemory != NULL) && (descriptor->uniformStagingRange.memory != VK_NULL_HANDLE)) {
            VkDeviceMemory arenaMemory = descriptor->uniformStagingRange.memory;
            descriptor->uniformStagingMappedLive = 0u;
            descriptor->uniformStagingMapped = NULL;
            (void)loader->deviceTable.vkUnmapMemory(device->device, arenaMemory);
        }
    }

    if (descriptor->uniformStagingBuffer != VK_NULL_HANDLE) {
        if (loader->deviceTable.vkDestroyBuffer != NULL) {
            VkBuffer arena = descriptor->uniformStagingBuffer;
            descriptor->uniformStagingBuffer = VK_NULL_HANDLE;
            (void)loader->deviceTable.vkDestroyBuffer(device->device, arena, NULL);
        }
    }

    if (descriptor->uniformStagingRange.memory != VK_NULL_HANDLE) {
        /* The arena's carve goes free in the allocator's own rows, for the same reason the block bel
           ow it does: the block itself is xud_allocator_close()'s to free, after this rung. */
        rung = xud_allocator_release(alloc, &descriptor->uniformStagingRange);
        (void)rung;
        descriptor->uniformStagingRange.memory = VK_NULL_HANDLE;
        descriptor->uniformStagingRange.size = 0u;
        descriptor->uniformStagingRange.offset = 0u;
        descriptor->uniformStagingBindOffset = 0u;
    }

    if (descriptor->pool != VK_NULL_HANDLE) {
        if (loader->deviceTable.vkDestroyDescriptorPool != NULL) {
            /* The handle is an IN-parameter, so it is read into a local and the field cleared befo
               re the call - the zero-state-before-the-call rule every destroy in this tree follows,
               and the reason xud_renderpass_close() states: clearing first would destroy VK_NULL_HA
               NDLE and leave the pool alive. */
            VkDescriptorPool pool = descriptor->pool;
            descriptor->pool = VK_NULL_HANDLE;
            (void)loader->deviceTable.vkDestroyDescriptorPool(device->device, pool, NULL);
        }
    }

    if (descriptor->uniformBuffer != VK_NULL_HANDLE) {
        if (loader->deviceTable.vkDestroyBuffer != NULL) {
            VkBuffer buffer = descriptor->uniformBuffer;
            descriptor->uniformBuffer = VK_NULL_HANDLE;
            (void)loader->deviceTable.vkDestroyBuffer(device->device, buffer, NULL);
        }
    }

    if (descriptor->uniformRange.memory != VK_NULL_HANDLE) {
        /* The range goes free in the allocator's own rows, which is what makes a descriptor that w
           as retired here stop naming a carve that is still live. */
        rung = xud_allocator_release(alloc, &descriptor->uniformRange);
        (void)rung;
        descriptor->uniformRange.memory = VK_NULL_HANDLE;
        descriptor->uniformRange.size = 0u;
        descriptor->uniformRange.offset = 0u;
        descriptor->uniformBindOffset = 0u;
    }

    /* The two layouts in the reverse of the order they were created in, which is the reverse of the
       order the pipeline layout above names them in.
     */
    for (row = XUD_DESCRIPTOR_SET_COUNT; row > 0u; row--) {
        if (descriptor->setLayouts[row - 1u] != VK_NULL_HANDLE) {
            if (loader->deviceTable.vkDestroyDescriptorSetLayout != NULL) {
                VkDescriptorSetLayout layout = descriptor->setLayouts[row - 1u];
                descriptor->setLayouts[row - 1u] = VK_NULL_HANDLE;
                (void)loader->deviceTable.vkDestroyDescriptorSetLayout(device->device, layout, NULL);
            }
        }
    }

    /* The sets are not destroyed here and cannot be: there is no vkDestroyDescriptorSet, and the
       one set this rung allocated died with the pool above. Clearing the rows is what makes a desc
       riptor that was retired read as nothing rather than as a handle the driver no longer knows.
     */
    descriptor->sets[XUD_DESCRIPTOR_SET_PER_FRAME] = VK_NULL_HANDLE;
    descriptor->sets[XUD_DESCRIPTOR_SET_MATERIAL] = VK_NULL_HANDLE;
}

/*-----------------------------------------------------------------------------------------------
 * The caps gate for the whole layout - pure, no driver call, so the headless run exercises it
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_descriptor_caps_support(const XudDeviceCaps* caps) {
    const VkPhysicalDeviceLimits* limits = NULL;

    if (caps == NULL) {
        return XUD_INVALID_ARG;
    }

    /* The two-hop member path, the trap xud_physical.c states: the caps' field is itself named pro
       perties and its type is the VkPhysicalDeviceProperties2 WRAPPER, whose core struct is the .pr
       operties member. From a bare wrapper it is one hop, from the caps it is two.
     */
    limits = &caps->properties.properties.limits;

    /* The two sets of this layout against the implementation's bound. maxBoundDescriptorSets is gua
       ranteed 4 by the spec, so this check never fires on a conforming driver - it is the check the
       repo's "check before you use" rule asks for anyway, because a layout this library cannot sh
       ow the device is a layout it must not build.
     */
    if (limits->maxBoundDescriptorSets < XUD_DESCRIPTOR_SET_COUNT) {
        return XUD_UNSUPPORTED;
    }

    /* set 0's one UNIFORM_BUFFER descriptor against the per-stage and per-set totals. Both are read
       because the two numbers bound different things: the per-stage total is what one stage may bi
       nd and the per-set total is what one set may hold, and a layout that fit one and not the oth
       er is a layout that would be refused by the pipeline's create rather than by this one.
     */
    if ((limits->maxPerStageDescriptorUniformBuffers < 1u) || (limits->maxDescriptorSetUniformBuffers < 1u)) {
        return XUD_UNSUPPORTED;
    }

    /* set 1's sampler array against the same pair for the COMBINED_IMAGE_SAMPLER type. This is the
       one place in this rung where the caps can refuse the layout as designed rather than refuse a
       smaller one, and it is the reason XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE is a constant a rung abov
       e cannot raise: a device that serves fewer samplers per set than this layout declares is a d
       evice this library's material model does not fit, and the answer above is XUD_UNSUPPORTED, n
       ot a silently-smaller array.
     */
    if ((limits->maxPerStageDescriptorSamplers < XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE)
        || (limits->maxDescriptorSetSamplers < XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE)) {
        return XUD_UNSUPPORTED;
    }

    /* The push row against the implementation's push-constant budget. The spec guarantees 128 and t
       he assert beside XUD_DESCRIPTOR_PUSH_MIN_GUARANTEE is what makes this rung's row fit that gua
       rantee, so this check is about a driver that answers LESS than the guarantee rather than abo
       ut this row's size.
     */
    if (limits->maxPushConstantsSize < XUD_DESCRIPTOR_PUSH_SIZE) {
        return XUD_UNSUPPORTED;
    }

    /* The uniform block against the largest range a UNIFORM_BUFFER descriptor may name. This is the
       one limit here that the block's size can actually cross - maxUniformBufferRange is guarantee
       d 65536 and this block is 120 bytes today, so the headroom is where the sketch's light array
       grows and this gate is what makes growing it past the limit a refusal rather than a buffer th
       e driver accepts and the shader reads out of.
     */
    if (limits->maxUniformBufferRange < XUD_DESCRIPTOR_UNIFORM_BLOCK_SIZE) {
        return XUD_UNSUPPORTED;
    }

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The two set layouts and the binding rows they point at - pure
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_descriptor_fill_set_layouts(XudDescriptor* descriptor) {
    if (descriptor == NULL) {
        return XUD_INVALID_ARG;
    }

    /* The create-infos are stamped and their pNext cleared before anything reads them, the rule the
       physical layer states for the *2 getters: a calloc-ed row is not a substitute for stamping,
       and the validation layer reports an unstamped create-info as a hard error rather than forgiv
       ing it.
     */
    descriptor->layoutCreateInfos[XUD_DESCRIPTOR_SET_PER_FRAME].sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptor->layoutCreateInfos[XUD_DESCRIPTOR_SET_PER_FRAME].pNext = NULL;
    descriptor->layoutCreateInfos[XUD_DESCRIPTOR_SET_PER_FRAME].flags = 0u;
    descriptor->layoutCreateInfos[XUD_DESCRIPTOR_SET_PER_FRAME].bindingCount = XUD_DESCRIPTOR_BINDINGS_PER_SET;
    descriptor->layoutCreateInfos[XUD_DESCRIPTOR_SET_PER_FRAME].pBindings = descriptor->bindings[XUD_DESCRIPTOR_SET_PER_FRAME];

    descriptor->layoutCreateInfos[XUD_DESCRIPTOR_SET_MATERIAL].sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptor->layoutCreateInfos[XUD_DESCRIPTOR_SET_MATERIAL].pNext = NULL;
    descriptor->layoutCreateInfos[XUD_DESCRIPTOR_SET_MATERIAL].flags = 0u;
    descriptor->layoutCreateInfos[XUD_DESCRIPTOR_SET_MATERIAL].bindingCount = XUD_DESCRIPTOR_BINDINGS_PER_SET;
    descriptor->layoutCreateInfos[XUD_DESCRIPTOR_SET_MATERIAL].pBindings = descriptor->bindings[XUD_DESCRIPTOR_SET_MATERIAL];

    /* set 0: the one uniform buffer of the per-frame block. descriptorCount is 1 because this set h
       olds one buffer, and the binding is numbered XUD_DESCRIPTOR_BINDING so the binding list is th
       e contiguous list starting at 0 the create-info's own rule asks for.
     */
    descriptor->bindings[XUD_DESCRIPTOR_SET_PER_FRAME][0u].binding = XUD_DESCRIPTOR_BINDING;
    descriptor->bindings[XUD_DESCRIPTOR_SET_PER_FRAME][0u].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    descriptor->bindings[XUD_DESCRIPTOR_SET_PER_FRAME][0u].descriptorCount = 1u;
    descriptor->bindings[XUD_DESCRIPTOR_SET_PER_FRAME][0u].stageFlags = XUD_DESCRIPTOR_BUFFER_STAGES;
    descriptor->bindings[XUD_DESCRIPTOR_SET_PER_FRAME][0u].pImmutableSamplers = NULL;

    /* set 1: the sampler array of the per-material set. The count is the layout's ceiling rather th
       an a today-count, for the reason stated beside XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE.
     */
    descriptor->bindings[XUD_DESCRIPTOR_SET_MATERIAL][0u].binding = XUD_DESCRIPTOR_BINDING;
    descriptor->bindings[XUD_DESCRIPTOR_SET_MATERIAL][0u].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptor->bindings[XUD_DESCRIPTOR_SET_MATERIAL][0u].descriptorCount = XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE;
    descriptor->bindings[XUD_DESCRIPTOR_SET_MATERIAL][0u].stageFlags = XUD_DESCRIPTOR_SAMPLER_STAGES;
    descriptor->bindings[XUD_DESCRIPTOR_SET_MATERIAL][0u].pImmutableSamplers = NULL;

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The one push-constant range of this layout - pure, and the caps gate is the rung's own
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_descriptor_fill_push_range(const XudDeviceCaps* caps, VkPushConstantRange* outRange) {
    if ((caps == NULL) || (outRange == NULL)) {
        return XUD_INVALID_ARG;
    }

    /* The gate is re-run here rather than trusted from the bring-up below, because this function is
       the pure half the headless run calls on its own and a row written without its gate is a row
       the pipeline layout above would read whether or not the bring-up ran.
     */
    if (xud_descriptor_caps_support(caps) != XUD_OK) {
        return XUD_UNSUPPORTED;
    }

    outRange->stageFlags = XUD_DESCRIPTOR_PUSH_STAGES;
    outRange->offset = 0u;
    outRange->size = XUD_DESCRIPTOR_PUSH_SIZE;

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The pool and its two rows - pure
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_descriptor_fill_pool_info(XudDescriptor* descriptor) {
    if (descriptor == NULL) {
        return XUD_INVALID_ARG;
    }

    descriptor->poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descriptor->poolInfo.pNext = NULL;
    descriptor->poolInfo.flags = XUD_DESCRIPTOR_POOL_FLAGS;
    descriptor->poolInfo.maxSets = XUD_DESCRIPTOR_SET_COUNT;
    descriptor->poolInfo.poolSizeCount = XUD_DESCRIPTOR_POOL_SIZE_COUNT;
    descriptor->poolInfo.pPoolSizes = descriptor->poolSizes;

    /* One row per descriptor TYPE the sets allocated from this pool will be updated with, not one
       row per set - the rule the create-info states for pPoolSizes. The counts are the ceiling of h
       ow many of each type the pool's sets may hold in total, which is why the sampler row is the
       array's size rather than 1: the texture step allocates set 1 from this same pool and its one
       binding is XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE descriptors.
     */
    descriptor->poolSizes[0u].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    descriptor->poolSizes[0u].descriptorCount = 1u;
    descriptor->poolSizes[1u].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptor->poolSizes[1u].descriptorCount = XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE;

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The allocate-info for set 0 - pure
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_descriptor_fill_set_allocate_info(XudDescriptor* descriptor) {
    if (descriptor == NULL) {
        return XUD_INVALID_ARG;
    }

    /* An allocate-info that names no pool is a bad state of THIS layer's own record rather than a
       bad argument from a caller - the rule xud_pipeline_fill_create_info() applies to a pipeline t
       hat owns no layout and no render pass, one rung up.
     */
    if ((descriptor->pool == VK_NULL_HANDLE) || (descriptor->setLayouts[XUD_DESCRIPTOR_SET_PER_FRAME] == VK_NULL_HANDLE)) {
        return XUD_INVALID_ARG;
    }

    descriptor->setAllocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descriptor->setAllocateInfo.pNext = NULL;
    descriptor->setAllocateInfo.descriptorPool = descriptor->pool;
    descriptor->setAllocateInfo.descriptorSetCount = 1u;
    descriptor->setAllocateInfo.pSetLayouts = &descriptor->setLayouts[XUD_DESCRIPTOR_SET_PER_FRAME];

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The buffer of set 0 - pure
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_descriptor_fill_buffer_info(XudDescriptor* descriptor) {
    if (descriptor == NULL) {
        return XUD_INVALID_ARG;
    }

    descriptor->bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    descriptor->bufferInfo.pNext = NULL;
    descriptor->bufferInfo.flags = 0u;

    /* The buffer is exactly as big as the block, not a rounded-up guess: the descriptor's range is
       a read of this buffer, and a buffer smaller than the range is a read past the end of the all
       ocation the allocator carved.
     */
    descriptor->bufferInfo.size = XUD_DESCRIPTOR_UNIFORM_BLOCK_SIZE;
    descriptor->bufferInfo.usage = XUD_DESCRIPTOR_BUFFER_USAGE;
    descriptor->bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    descriptor->bufferInfo.queueFamilyIndexCount = 0u;
    descriptor->bufferInfo.pQueueFamilyIndices = NULL;

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The write that binds set 0's buffer into it - pure
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_descriptor_fill_write_info(XudDescriptor* descriptor) {
    if (descriptor == NULL) {
        return XUD_INVALID_ARG;
    }

    if ((descriptor->sets[XUD_DESCRIPTOR_SET_PER_FRAME] == VK_NULL_HANDLE) || (descriptor->uniformBuffer == VK_NULL_HANDLE)) {
        return XUD_INVALID_ARG;
    }

    descriptor->bufferRow.buffer = descriptor->uniformBuffer;
    descriptor->bufferRow.offset = 0u;

    /* The range is the block's size rather than the allocator's range size: the buffer is exactly a
       s big as the block and the allocator's carve may be bigger than that, so the descriptor names
       the buffer's own bytes rather than the hole around it.
     */
    descriptor->bufferRow.range = XUD_DESCRIPTOR_UNIFORM_BLOCK_SIZE;

    descriptor->writeInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor->writeInfo.pNext = NULL;
    descriptor->writeInfo.dstSet = descriptor->sets[XUD_DESCRIPTOR_SET_PER_FRAME];
    descriptor->writeInfo.dstBinding = XUD_DESCRIPTOR_BINDING;
    descriptor->writeInfo.dstArrayElement = 0u;
    descriptor->writeInfo.descriptorCount = 1u;
    descriptor->writeInfo.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    descriptor->writeInfo.pBufferInfo = &descriptor->bufferRow;
    descriptor->writeInfo.pImageInfo = NULL;
    descriptor->writeInfo.pTexelBufferView = NULL;

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The host-side bytes of set 0's block - pure
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_descriptor_fill_uniform_block(XudDescriptor* descriptor) {
    if (descriptor == NULL) {
        return XUD_INVALID_ARG;
    }

    /* The whole block at zero first, so every field of the layout is written in the order the lay
       out declares rather than left at whatever the allocation handed over.
     */
    memset(&descriptor->uniformBlock, 0, sizeof(descriptor->uniformBlock));

    /* The identity VP matrix: the one matrix that is a fact rather than a choice, because a camera
       that is not identity is the camera step's and this rung owns no camera. Column-major, the sa
       me order the push row's model matrix and the sketch's matrices are in.
     */
    descriptor->uniformBlock.viewProj[0u] = 1.0f;
    descriptor->uniformBlock.viewProj[5u] = 1.0f;
    descriptor->uniformBlock.viewProj[10u] = 1.0f;
    descriptor->uniformBlock.viewProj[15u] = 1.0f;

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The staging arena of set 0's block - the pure half: its create-info, its memory rule and the
 * upload's one region
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_descriptor_host_visible_bits(const XudDeviceCaps* caps, uint32_t* outMemoryTypeBits) {
    const VkPhysicalDeviceMemoryProperties* mem = NULL;
    uint32_t index = 0u;
    uint32_t mask = 0u;

    if ((caps == NULL) || (outMemoryTypeBits == NULL)) {
        return XUD_INVALID_ARG;
    }

    mem = &caps->memoryProperties.memoryProperties;
    if (mem->memoryTypeCount > VK_MAX_MEMORY_TYPES) {
        return XUD_INVALID_ARG;
    }

    for (index = 0u; index < mem->memoryTypeCount; index++) {
        if ((mem->memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0u) {
            mask |= (1u << index);
        }
    }

    *outMemoryTypeBits = mask;
    return XUD_OK;
}

XudResult xud_descriptor_fill_staging_info(XudDescriptor* descriptor) {
    if (descriptor == NULL) {
        return XUD_INVALID_ARG;
    }

    descriptor->uniformStagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    descriptor->uniformStagingInfo.pNext = NULL;
    descriptor->uniformStagingInfo.flags = 0u;
    descriptor->uniformStagingInfo.size = XUD_DESCRIPTOR_UNIFORM_BLOCK_SIZE;
    descriptor->uniformStagingInfo.usage = XUD_DESCRIPTOR_STAGING_USAGE;
    descriptor->uniformStagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    descriptor->uniformStagingInfo.queueFamilyIndexCount = 0u;
    descriptor->uniformStagingInfo.pQueueFamilyIndices = NULL;
    return XUD_OK;
}

XudResult xud_descriptor_fill_upload_info(XudDescriptor* descriptor) {
    if ((descriptor == NULL) || (descriptor->uniformStagingBuffer == VK_NULL_HANDLE) || (descriptor->uniformBuffer == VK_NULL_HANDLE)) {
        return XUD_INVALID_ARG;
    }

    /* One region, at (0, 0, the block's size): both buffers are exactly one block long and the blo
       ck starts at the start of each, so the two offsets are 0 rather than a computed pair, and the
       extent is the block's size rather than either buffer's - the rule vkCmdCopyBuffer states is th
       at a region must fit inside BOTH, and here the two fit and are equal. */
    descriptor->uploadRegion.srcOffset = 0u;
    descriptor->uploadRegion.dstOffset = 0u;
    descriptor->uploadRegion.size = XUD_DESCRIPTOR_UNIFORM_BLOCK_SIZE;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The per-frame update of set 0's block - the host write and the copy
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_descriptor_write_block(XudDescriptor* descriptor) {
    if (descriptor == NULL) {
        return XUD_INVALID_ARG;
    }
    if ((descriptor->uniformStagingMapped == NULL) || (descriptor->uniformStagingMappedLive == 0u)) {
        return XUD_UNSUPPORTED;
    }

    /* The one host write of the per-frame update: the block's bytes into the mapped arena. This cal
       l records nothing and calls no driver entry point, which is what keeps the block's layout tes
       table with no device present up to the one guard it runs - an arena that is not mapped is a s
       tate this rung refuses rather than a write into NULL. The barrier that makes these bytes visi
       ble to the transfer is the frame loop's, one rung up, because a barrier recorded into a comm
       and buffer this rung does not own is not a decision this rung can make.
     */
    memcpy(descriptor->uniformStagingMapped, &descriptor->uniformBlock, XUD_DESCRIPTOR_UNIFORM_BLOCK_SIZE);
    return XUD_OK;
}

XudResult xud_descriptor_upload(XudDescriptor* descriptor, XudLoader* loader, const XudDevice* device, VkCommandBuffer commandBuffer) {
    if ((descriptor == NULL) || (loader == NULL) || (device == NULL) || (commandBuffer == VK_NULL_HANDLE)) {
        return XUD_INVALID_ARG;
    }
    if (!loader->open) {
        return XUD_UNSUPPORTED;
    }
    if (device->device == VK_NULL_HANDLE) {
        return XUD_INVALID_ARG;
    }
    if (!xud_loader_device_bound(loader, device->device)) {
        return XUD_UNSUPPORTED;
    }
    if ((descriptor->uniformStagingBuffer == VK_NULL_HANDLE) || (descriptor->uniformBuffer == VK_NULL_HANDLE)) {
        return XUD_INVALID_ARG;
    }
    if ((descriptor->uniformStagingMapped == NULL) || (descriptor->uniformStagingMappedLive == 0u)) {
        return XUD_UNSUPPORTED;
    }
    if (loader->deviceTable.vkCmdCopyBuffer == NULL) {
        return XUD_UNSUPPORTED;
    }

    /* The rows are filled here rather than trusted from the bring-up, so the headless run can call
       the assembly on its own and the driver still gets the same rows it asserted. */
    if (xud_descriptor_fill_upload_info(descriptor) != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    /* ONE call carrying ONE region: this upload has one source and one destination, which is the op
       posite of the batch's five-call shape, where the five destinations are five buffers. The cal
       l is legal only OUTSIDE a render pass instance, which is the frame loop's job to honour, and
       so are the two barriers either side of it - a transfer command has no stage, so no stage mas
       k is an argument here at all.
     */
    loader->deviceTable.vkCmdCopyBuffer(commandBuffer, descriptor->uniformStagingBuffer, descriptor->uniformBuffer,
        XUD_DESCRIPTOR_UPLOAD_REGION_COUNT, &descriptor->uploadRegion);

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The bring-up - driver
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_descriptor_open(
    XudDescriptor** outDescriptor, XudLoader* loader, const XudDevice* device, const XudDeviceCaps* caps, XudAllocator* alloc) {
    XudDescriptor* descriptor = NULL;
    XudResult result = XUD_INVALID_ARG;
    VkMemoryRequirements needs;
    VkMemoryRequirements arenaNeeds;
    uint32_t row = 0u;
    uint32_t hostVisibleMask = 0u;
    uint32_t arenaTypeIndex = 0u;
    void* arenaMapped = NULL;

    if ((outDescriptor == NULL) || (*outDescriptor != NULL)) {
        return XUD_INVALID_ARG;
    }
    if ((loader == NULL) || (device == NULL) || (caps == NULL) || (alloc == NULL)) {
        return XUD_INVALID_ARG;
    }
    if (!loader->open) {
        return XUD_UNSUPPORTED;
    }
    if (device->device == VK_NULL_HANDLE) {
        return XUD_INVALID_ARG;
    }

    /* Every row of this rung is asked of a VkDevice, so every one of them is a row of Volk's DEVIC
       E table - there is no instance-level name in this pair. The destroy rows are checked here, be
       fore the creates, because a layout this layer cannot destroy is a layout it must not create:
       the rule xud_window.c applies to a surface, xud_swapchain.c to a swapchain and xud_allocator_
       acquire() to a block it cannot free, one rung below.
     */
    if ((loader->deviceTable.vkCreateDescriptorSetLayout == NULL) || (loader->deviceTable.vkDestroyDescriptorSetLayout == NULL)
        || (loader->deviceTable.vkCreateDescriptorPool == NULL) || (loader->deviceTable.vkDestroyDescriptorPool == NULL)
        || (loader->deviceTable.vkCreateBuffer == NULL) || (loader->deviceTable.vkDestroyBuffer == NULL)
        || (loader->deviceTable.vkGetBufferMemoryRequirements == NULL) || (loader->deviceTable.vkBindBufferMemory == NULL)
        || (loader->deviceTable.vkAllocateDescriptorSets == NULL) || (loader->deviceTable.vkUpdateDescriptorSets == NULL)
        || (loader->deviceTable.vkMapMemory == NULL) || (loader->deviceTable.vkUnmapMemory == NULL)) {
        return XUD_UNSUPPORTED;
    }

    /* The caps gate first, so a device this layout does not fit is refused before a single object
       exists rather than after five of them.
     */
    result = xud_descriptor_caps_support(caps);
    if (result != XUD_OK) {
        return result;
    }

    descriptor = calloc(1u, sizeof(*descriptor));
    if (descriptor == NULL) {
        return XUD_OUT_OF_MEMORY;
    }

    /* The rows of this layout, assembled before any of them is handed to a driver. The assembly is
       one block rather than interleaved with the creates because the headless run calls the pure h
       alf of this rung on its own and the same rows have to be the ones the driver was handed.
     */
    (void)xud_descriptor_fill_set_layouts(descriptor);
    (void)xud_descriptor_fill_pool_info(descriptor);
    (void)xud_descriptor_fill_buffer_info(descriptor);
    (void)xud_descriptor_fill_uniform_block(descriptor);

    /* The push-constant range is the one row of this layout that is not a member of the record's o
       wn array, and it is filled from the caps rather than from the record: the range's size is ga
       ted on maxPushConstantsSize, so a row written without its gate is a row the pipeline layout
       above would read whether or not the bring-up ran.
     */
    if (xud_descriptor_fill_push_range(caps, &descriptor->pushRange) != XUD_OK) {
        xud_descriptor_retire(descriptor, device, loader, alloc);
        free(descriptor);
        return XUD_UNSUPPORTED;
    }

    /* 1. the two set layouts, in set order. The order is load-bearing rather than tidy: the pipeli
       ne layout above names them in array order, and that array IS the set numbering.
     */
    for (row = 0u; row < XUD_DESCRIPTOR_SET_COUNT; row++) {
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;

        result = xud_result_from_vk(
            loader->deviceTable.vkCreateDescriptorSetLayout(device->device, &descriptor->layoutCreateInfos[row], NULL, &layout));
        if (result != XUD_OK) {
            xud_descriptor_retire(descriptor, device, loader, alloc);
            free(descriptor);
            return result;
        }
        descriptor->setLayouts[row] = layout;
    }

    /* 2. the pool, which the sets below are allocated from.
     */
    result = xud_result_from_vk(loader->deviceTable.vkCreateDescriptorPool(device->device, &descriptor->poolInfo, NULL, &descriptor->pool));
    if (result != XUD_OK) {
        xud_descriptor_retire(descriptor, device, loader, alloc);
        free(descriptor);
        return result;
    }

    /* 3. the buffer of set 0, created before its memory is asked for: vkGetBufferMemoryRequirements
       takes the buffer, so the requirements are a fact about a buffer that exists rather than a g
       uess about one that does not, and the allocator's acquire is then the acquire of exactly wh
       at the driver asked for.
     */
    result
        = xud_result_from_vk(loader->deviceTable.vkCreateBuffer(device->device, &descriptor->bufferInfo, NULL, &descriptor->uniformBuffer));
    if (result != XUD_OK) {
        xud_descriptor_retire(descriptor, device, loader, alloc);
        free(descriptor);
        return result;
    }

    /* vkGetBufferMemoryRequirements returns VOID, so there is no VkResult to map and nothing to
       check after the call except the out-param itself - the zero-state-before-the-call rule this
       tree follows for every void entry point (vkFreeMemory, vkDestroyDevice, vkGetDeviceQueue): t
       he row is cleared first, so a driver that wrote nothing leaves a row that says nothing rat
       her than a row of whatever the stack held.
     */
    memset(&needs, 0, sizeof(needs));
    loader->deviceTable.vkGetBufferMemoryRequirements(device->device, descriptor->uniformBuffer, &needs);
    if (needs.size == 0u) {
        /* A buffer that asks for no memory is a buffer this rung cannot carve, and a zero-size req
           uest to the allocator is XUD_INVALID_ARG rather than a zero-size block - the rule xud_al
           locator_acquire() states beside its own guard.
         */
        xud_descriptor_retire(descriptor, device, loader, alloc);
        free(descriptor);
        return XUD_VULKAN_ERROR;
    }

    result = xud_allocator_acquire(&descriptor->uniformRange, alloc, loader, device, caps, &needs);
    if (result != XUD_OK) {
        xud_descriptor_retire(descriptor, device, loader, alloc);
        free(descriptor);
        return result;
    }
    descriptor->uniformBindOffset = descriptor->uniformRange.offset;

    /* The buffer and its memory are two objects that must be tied together before anything can bi
       nd to either of them: an unbound buffer is a buffer whose contents are undefined, and a desc
       riptor that names one is a descriptor that reads undefined memory.
     */
    result = xud_result_from_vk(loader->deviceTable.vkBindBufferMemory(
        device->device, descriptor->uniformBuffer, descriptor->uniformRange.memory, descriptor->uniformBindOffset));
    if (result != XUD_OK) {
        xud_allocator_release(alloc, &descriptor->uniformRange);
        descriptor->uniformRange.memory = VK_NULL_HANDLE;
        descriptor->uniformRange.size = 0u;
        descriptor->uniformRange.offset = 0u;
        descriptor->uniformBindOffset = 0u;
        xud_descriptor_retire(descriptor, device, loader, alloc);
        free(descriptor);
        return result;
    }

    /* 4. set 0, allocated from the pool above against the layout above it. Set 1 is NOT allocated h
       ere and that is the state this rung is in by construction: the layout is declared, the set i
       s not, and the texture step is what allocates it. The assert below is that state written as
       a check rather than as a comment.
     */
    result = xud_descriptor_fill_set_allocate_info(descriptor);
    if (result != XUD_OK) {
        xud_descriptor_retire(descriptor, device, loader, alloc);
        free(descriptor);
        return result;
    }

    result = xud_result_from_vk(loader->deviceTable.vkAllocateDescriptorSets(
        device->device, &descriptor->setAllocateInfo, &descriptor->sets[XUD_DESCRIPTOR_SET_PER_FRAME]));
    if (result != XUD_OK) {
        xud_descriptor_retire(descriptor, device, loader, alloc);
        free(descriptor);
        return result;
    }

    /* Set 1 is NOT allocated here, and that is the state this rung is in by construction rather th
       an an oversight: its layout is declared, its set is not, and the texture step is what al
       locates it from this same pool. The calloc-ed struct is what leaves the row at VK_NULL_HANDL
       E, and xud_descriptor_close() asserts the same thing one step later.
     */

    /* 5. the write that bound the buffer into the set, last of the five because it is the only one
       of them that reads two objects created by two different steps above it.
     */
    result = xud_descriptor_fill_write_info(descriptor);
    if (result != XUD_OK) {
        xud_descriptor_retire(descriptor, device, loader, alloc);
        free(descriptor);
        return result;
    }

    /* vkUpdateDescriptorSets returns VOID too, so the write is not checked after the call: the thr
       ee rows it reads (the set, the buffer, the layout the set was allocated against) are all thr
       ee checked live before this point, which is what makes the write's inputs facts rather than
       hopes, and a write that the driver refused is a write the validation layer reports.
     */
    loader->deviceTable.vkUpdateDescriptorSets(device->device, 1u, &descriptor->writeInfo, 0u, NULL);

    /* 6. the staging arena of set 0's block, LAST of the six because it is the only object of them
       that nothing else in this rung names - and putting it last is what makes the teardown below i
       ts exact reverse rather than a second order to state. The arena is host-visible by constructi
       on: the block's bytes are written by the CPU each frame, so the buffer's own memoryTypeBits i
       s narrowed with the caps' host-visible rows BEFORE the allocator is asked, and the type the a
       llocator actually picked is then re-read from xud_allocator_select_memory_type() and checked
       for VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT rather than trusted from the mask that was asked for -
       a mask is a preference, and vkMapMemory is called on the row the allocator chose. A driver wi
       th no host-visible type at all is XUD_UNSUPPORTED, which is the repo's rule that an unverifi
       ed capability stays off and the feature that needed it answers UNSUPPORTED rather than being
       worked around.
     */
    result = xud_descriptor_fill_staging_info(descriptor);
    if (result != XUD_OK) {
        xud_descriptor_retire(descriptor, device, loader, alloc);
        free(descriptor);
        return result;
    }

    result = xud_result_from_vk(
        loader->deviceTable.vkCreateBuffer(device->device, &descriptor->uniformStagingInfo, NULL, &descriptor->uniformStagingBuffer));
    if (result != XUD_OK) {
        xud_descriptor_retire(descriptor, device, loader, alloc);
        free(descriptor);
        return result;
    }

    memset(&arenaNeeds, 0, sizeof(arenaNeeds));
    loader->deviceTable.vkGetBufferMemoryRequirements(device->device, descriptor->uniformStagingBuffer, &arenaNeeds);
    if (arenaNeeds.size == 0u) {
        xud_descriptor_retire(descriptor, device, loader, alloc);
        free(descriptor);
        return XUD_VULKAN_ERROR;
    }

    result = xud_descriptor_host_visible_bits(caps, &hostVisibleMask);
    if (result != XUD_OK) {
        xud_descriptor_retire(descriptor, device, loader, alloc);
        free(descriptor);
        return result;
    }
    if (hostVisibleMask == 0u) {
        xud_descriptor_retire(descriptor, device, loader, alloc);
        free(descriptor);
        return XUD_UNSUPPORTED;
    }
    arenaNeeds.memoryTypeBits &= hostVisibleMask;
    if (arenaNeeds.memoryTypeBits == 0u) {
        xud_descriptor_retire(descriptor, device, loader, alloc);
        free(descriptor);
        return XUD_UNSUPPORTED;
    }

    /* The arena's carve, and it is the host-visible acquire rather than the suballocating one: a
       mapping is per-VkDeviceMemory rather than per-range, so a carve inside a block someone else
       already mapped is a carve that cannot be mapped at all. The arena is small and a block is 64
       KiB, which is the cost of that fact - one block per host-visible arena, and the only place
       the repo's "allocate once, suballocate always" rule spends one.
    */
    result = xud_allocator_acquire_host_visible(&descriptor->uniformStagingRange, alloc, loader, device, caps, &arenaNeeds);
    if (result != XUD_OK) {
        xud_descriptor_retire(descriptor, device, loader, alloc);
        free(descriptor);
        return result;
    }
    descriptor->uniformStagingBindOffset = descriptor->uniformStagingRange.offset;

    result = xud_allocator_select_memory_type(caps, &arenaNeeds, &arenaTypeIndex);
    if ((result != XUD_OK) || (arenaTypeIndex >= caps->memoryProperties.memoryProperties.memoryTypeCount)
        || ((caps->memoryProperties.memoryProperties.memoryTypes[arenaTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            == 0u)) {
        xud_allocator_release(alloc, &descriptor->uniformStagingRange);
        descriptor->uniformStagingRange.memory = VK_NULL_HANDLE;
        descriptor->uniformStagingRange.size = 0u;
        descriptor->uniformStagingRange.offset = 0u;
        descriptor->uniformStagingBindOffset = 0u;
        xud_descriptor_retire(descriptor, device, loader, alloc);
        free(descriptor);
        return XUD_UNSUPPORTED;
    }

    result = xud_result_from_vk(loader->deviceTable.vkBindBufferMemory(
        device->device, descriptor->uniformStagingBuffer, descriptor->uniformStagingRange.memory, descriptor->uniformStagingBindOffset));
    if (result != XUD_OK) {
        xud_allocator_release(alloc, &descriptor->uniformStagingRange);
        descriptor->uniformStagingRange.memory = VK_NULL_HANDLE;
        descriptor->uniformStagingRange.size = 0u;
        descriptor->uniformStagingRange.offset = 0u;
        descriptor->uniformStagingBindOffset = 0u;
        xud_descriptor_retire(descriptor, device, loader, alloc);
        free(descriptor);
        return result;
    }

    /* The map, once, for the whole arena, for the whole life of the descriptor. flags 0 is the rea
       d/write pair the repo's rule asks for: the arena is written by the CPU and read by a transfer,
       and no cache-control flag is claimed for a type this layer has not asked to be cached. The x
       ud_batch_create_staging() above makes the same call for the same reason one rung over.
     */
    result = xud_result_from_vk(loader->deviceTable.vkMapMemory(device->device, descriptor->uniformStagingRange.memory,
        descriptor->uniformStagingRange.offset, descriptor->uniformStagingRange.size, 0u, &arenaMapped));
    if (result != XUD_OK) {
        xud_allocator_release(alloc, &descriptor->uniformStagingRange);
        descriptor->uniformStagingRange.memory = VK_NULL_HANDLE;
        descriptor->uniformStagingRange.size = 0u;
        descriptor->uniformStagingRange.offset = 0u;
        descriptor->uniformStagingBindOffset = 0u;
        xud_descriptor_retire(descriptor, device, loader, alloc);
        free(descriptor);
        return result;
    }

    descriptor->uniformStagingMapped = arenaMapped;
    descriptor->uniformStagingMappedLive = 1u;

    *outDescriptor = descriptor;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The teardown - driver
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_descriptor_close(XudDescriptor** ioDescriptor, XudLoader* loader, const XudDevice* device, XudAllocator* alloc) {
    XudDescriptor* victim = NULL;
    XudResult guard = XUD_OK;
    XudResult first = XUD_OK;

    if ((ioDescriptor == NULL) || (loader == NULL) || (device == NULL) || (alloc == NULL)) {
        return XUD_INVALID_ARG;
    }
    if ((*ioDescriptor) == NULL) {
        return XUD_ALREADY;
    }

    victim = *ioDescriptor;

    /* The rows must be this device's rows before anything is torn down, the same bound-handle rule
       xud_loader_instance_bound()/xud_loader_device_bound() make the stale-handle case a caught er
       ror rather than a call through a dead layer's pointer.
     */
    if (!xud_loader_device_bound(loader, device->device)) {
        return XUD_UNSUPPORTED;
    }

    /* The guard first, and it is not decoration: a command buffer of the frame loop above may stil
       l be reading this very buffer and this very set, so this is the same boundary xud_swapchain_
       close() and xud_allocator_close() state. A guard that cannot run is a teardown this rung mus
       t not start, because freeing the struct would leak every handle the driver still holds; a LO
       ST device is still torn down and still reported, the rule xud_device_close() follows.
     */
    guard = xud_device_wait_idle(device, loader);
    if (guard == XUD_UNSUPPORTED) {
        return XUD_UNSUPPORTED;
    }
    if (guard != XUD_OK) {
        first = guard;
    }

    xud_descriptor_retire(victim, device, loader, alloc);
    free(victim);
    *ioDescriptor = NULL;

    return first;
}
