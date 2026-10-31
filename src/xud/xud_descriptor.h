/***********************************************************************************************
 *   xud_descriptor.h - the descriptor-layout rung: the two descriptor set layouts, the
 *                      push-constant range, the one pool they are allocated from, the
 *                      per-frame uniform buffer carved out of the allocator, and the
 *                      write that bound it into set 0
 *
 *   Owns : XudDescriptor (the two VkDescriptorSetLayouts, the one VkPushConstantRange row,
 *          the one VkDescriptorPool, the sets allocated from it, the one VkBuffer of set 0
 *          plus its host-visible staging arena, and the XudMemoryRange of the allocator the
 *          two sit in) and the contract of the rung
 *          - the caps gate, the create-info assembly of every row of this layout, the bring
 *          -up, the per-frame uniform block's byte layout, the upload that copies the arena
 *          into the buffer, and the one teardown order.
 *   Layer: above the allocator and below the renderer. This is the rung of the design sket
 *          ch's descriptor discipline - "one per-frame set (set 0, updated once per frame),
 *          one per-material set (set 1), push constants for the model matrix + material/tex
 *          ture indices" - and it is the rung that makes the pipeline's layout mean somethin
 *          g rather than being zero: xud_pipeline_fill_layout_info() used to answer zero set
 *          layouts and zero push-constant ranges for two shaders that read no binding, and i
 *          t now names the rows of this record.
 *
 *   Why this rung is context-owned rather than renderer-owned: it is the same position in t
 *          he ownership tree the allocator step settled - a VkBuffer is created FROM the all
 *          ocator's memory and the pipeline layout above is what names these layouts, so the
 *          order is allocator -> descriptor -> renderer on the way in and renderer -> descri
 *          ptor -> allocator on the way out. A buffer whose block is freed before the buffer
 *          is destroyed is a submission reading freed VkDeviceMemory, and xud_allocator_clos
 *          e() is exactly that call.
 *
 *   What this rung does NOT have, and why: no VkImage and no VkSampler anywhere, so set 1's
 *          layout is DECLARED and its set is never allocated - the texture step is what creates
 *          the images and allocates that set from this same pool (which is why the pool is sized
 *          for both sets today). The push row of this layout is DECLARED here and CONSUMED one ru
 *          ng over: the frame loop pushes it beside every draw it records and the vertex stage re
 *          ads the model matrix out of it, so the bytes this rung's range row describes are live e
 *          nd to end. What nothing reads today is the material / texture / flags trio of that same
 *          row - the channel the material, texture and variant steps above will fill, and a stage n
 *          eed not touch every member of a block its layout declares.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#ifndef XUD_XUD_DESCRIPTOR_H_
#define XUD_XUD_DESCRIPTOR_H_

#include "volk.h" /* Required for: VK_NO_PROTOTYPES - must precede <vulkan/vulkan.h>, see xud_loader.h */
#include "xud.h" /* Required for: XudResult - the descriptor layer's own error codes */
#include "xud_allocator.h" /* Required for: XudAllocator, XudMemoryRange - the GPU memory this rung's one VkBuffer sits in */
#include "xud_device.h" /* Required for: XudDevice - the handle every row of this rung is asked of, and the wait-idle guard */
#include "xud_loader.h" /* Required for: XudLoader - the Volk rows every call here is made through */
#include "xud_physical.h" /* Required for: XudDeviceCaps - the limits every gate below is read off */
#include "xud_vulkan.h" /* Required for: xud_result_from_vk - every VkResult here goes through that one funnel */
#include <vulkan/vulkan.h> /* Required for: VkDescriptorSetLayout, VkDescriptorPool, VkDescriptorSet, VkBuffer, VkPushConstantRange, the create-infos and their stamps - never linked */

#include <assert.h> /* Required for: static_assert, the compile-time checks below */
#include <stdint.h>

/*-----------------------------------------------------------------------------------------------
 * Why this rung is xud_descriptor and not a descriptor set inside the renderer
 *
 *   The design sketch's descriptor discipline is a LAYOUT decision, not a per-draw one: "one
 *   per-frame set (set 0, updated once per frame), one per-material set (set 1), push constants
 *   for the model matrix + material/texture indices". A layout is created once at init and is
 *   named by the pipeline layout, so it belongs to the same rung the pipeline layout belongs to
 *   - one below it - and not to the draw. The repo's "explicit pipelines, created once at init"
 *   rule is the same rule read from the other side: a layout built per draw would be a pipelin
 *   e rebuilt per draw.
 *
 *   The limits below are the core-1.0 ones only. Grepped in vulkan_core.h (1.4.363): maxDescri
 *   ptorSetSize, maxDescriptorPoolSize, maxDescriptorUpdatesSetCount, maxDescriptorRangeSizeItem
 *   s and bufferAlignmentGranularity are all ABSENT from these headers (grep each name across /u
 *   sr/include/vulkan/: zero hits), so the gates this rung can state are the ones the core prope
 *   rties have - maxBoundDescriptorSets, maxPerStageDescriptor*, maxDescriptorSet*, maxUniformBu
 *   fferRange and maxPushConstantsSize. That is the repo's minimum-core-version rule (the instan
 *   ce sits at the VK_API_VERSION_1_0 floor, see AGENTS.md) applied to the descriptor rung rathe
 *   r than a choice about it.
 *-----------------------------------------------------------------------------------------------*/

/* The two sets of the design sketch's layout, and which row of the array each is. The order is
   load-bearing: VkPipelineLayoutCreateInfo's pSetLayouts array IS the set numbering (the first
   setLayoutCount elements are the sets numbered 0..count-1), so set 0 is row 0 and set 1 is row
   1 by construction, and a swapped pair is a swapped layout, not a rename.
*/
#define XUD_DESCRIPTOR_SET_COUNT 2u
#define XUD_DESCRIPTOR_SET_PER_FRAME 0u /* set 0: the per-frame block, one uniform buffer */
#define XUD_DESCRIPTOR_SET_MATERIAL 1u /* set 1: the per-material sampler array */

static_assert(XUD_DESCRIPTOR_SET_PER_FRAME < XUD_DESCRIPTOR_SET_MATERIAL,
    "the two set rows are in set-number order, which is what makes pSetLayouts row 0 the set numbered 0");
static_assert(XUD_DESCRIPTOR_SET_COUNT == (XUD_DESCRIPTOR_SET_MATERIAL + 1u),
    "the set rows are contiguous from set 0 up, which is the rule VkPipelineLayoutCreateInfo states for pSetLayouts");

/* One binding per set today, and its number. The number is 0 in both sets, which is what makes
   the write's dstBinding a constant rather than a lookup, and what makes a binding a rung above
   wants a decision about the LAYOUT (recreate it at init) rather than a per-draw question.
*/
#define XUD_DESCRIPTOR_BINDINGS_PER_SET 1u
#define XUD_DESCRIPTOR_BINDING 0u

static_assert(XUD_DESCRIPTOR_BINDINGS_PER_SET == 1u,
    "one binding per set today, so the bindings array is indexed by the set alone and a second binding is a layout decision, not a slot "
    "that was left empty");
static_assert(
    XUD_DESCRIPTOR_BINDING == 0u, "the binding number is 0 in both sets, which is what makes the write's dstBinding the same constant");

/* The set-1 sampler array's size: the CEILING on how many combined image samplers one material's
 *  set may name. It is this layer's own bound for the same reason XUD_HANDLE_SLOT_MAX_CAPACITY is
 *  the handle layer's - nothing about Vulkan says how many textures a material may sample - and i
 *  t is a ceiling rather than a today-count because growing it is a recreate of the layout, of th
 *  e pipeline layout and of every pipeline that names it, which is exactly what the "created once
 *  at init" rule forbids at runtime. 16 is the smallest number that covers the plan's atlas-frien
 *  dly pool of one material's textures with room to spare; the caps gate below is what refuses a
 *  device that advertises less.
 */
#define XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE 16u

static_assert(XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE > 0u,
    "a sampler array of zero descriptors is a binding that names nothing, which is a layout that cannot be written to");
static_assert(XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE <= 1024u,
    "the array is one binding's descriptorCount, and an implementation need only guarantee 64 for the per-stage sampler total - a bigger "
    "request is a different decision, not a bigger constant");

/* The pool's rows: one per descriptor TYPE the sets allocated from it will be updated with. Both
   are declared today even though only one set is allocated today - the pool is sized for the set
   the texture step will allocate, so that step needs no second pool of its own.
*/
#define XUD_DESCRIPTOR_POOL_SIZE_COUNT 2u

static_assert(XUD_DESCRIPTOR_POOL_SIZE_COUNT == XUD_DESCRIPTOR_SET_COUNT,
    "one pool row per set of this layout, which is what makes the pool sized for both sets and not only for the one allocated today");

/*-----------------------------------------------------------------------------------------------
 * The two byte layouts this rung defines
 *
 *   These are the bytes the driver sees, so they are spelled here rather than remembered in a
 *   shader: the C record and the GLSL block are the same bytes only if the field order, the fiel
 *   d widths and the padding agree, and nothing checks that a GLSL block and a C struct agreed. T
 *   he spelling is the reason: in std140 a vec3 is rounded to 16 bytes and a mat4 is 64, so a bl
 *   ock spelled with vec3/vec4 members has holes a C struct does not have. Spelled as float arra
 *   ys and float scalars, an array of scalars is packed at 4-byte stride and the two layouts are
 *   identical. That is why every member below is a float array or a scalar and not a vec3.
 *-----------------------------------------------------------------------------------------------*/

/* set 0's one uniform buffer: the per-frame block of the design sketch - VP matrix, camera pos, t
 *  ime, fog, ambient, light count. Only viewProj is LIVE today: xud_descriptor_open() zeroes the
 *  whole block, xud_descriptor_fill_uniform_block() puts the identity in viewProj and the camera
 *  rung (xud_camera.h/.c, S28) overwrites it every frame with xud_descriptor_set_view_proj(), and
 *  solid.vert reads nothing else out of the block - cameraPos, time, fog, ambient and lightCount
 *  are the sketch's placeholders, zero forever until the step that wants one lands. The sketch's
 *  block also names a fixed-size light ARRAY, and the light element type (position, colour, inten
 *  sity, direction, type) is the light step's decision, not one this rung can make without invent
 *  ing that subsystem; XUD_DESCRIPTOR_UNIFORM_BLOCK_SIZE is the one place that decision has to
 *  grow, and growing it is a recreate at init, which is legal - it is the runtime recreate the sam
 *  e rule forbids that is at issue.
 */
typedef struct XudFrameUniformRow {
    float viewProj[16]; /* 64 B - the VP matrix, column-major, the same 64 B the push row carries */
    float cameraPos[3]; /* 12 B - the camera position, the sketch's fog input, zero today: no shader reads it */
    float time; /* 4 B - seconds since the context was brought up */
    float fog[4]; /* 16 B - rgb + density */
    float ambient[4]; /* 16 B - rgb + intensity */
    uint32_t lightCount; /* 4 B - how many of the light array the light step will fill are live */
    uint32_t reserved; /* 4 B - keeps the block a multiple of 4 with no hole anywhere */
} XudFrameUniformRow;

/* The push-constant range of the design sketch: the model matrix + material index + texture index
 *  + flags. The model matrix is the same 64 B as the VP matrix because both are a column-major 4x
 *  4 of floats, and the batch step is what will write it.
 */
typedef struct XudPushRow {
    float modelMatrix[16]; /* 64 B - the model matrix */
    uint32_t materialIndex; /* 4 B - which material slot this draw uses */
    uint32_t textureIndex; /* 4 B - which entry of set 1's sampler array this draw samples */
    uint32_t flags; /* 4 B - the draw's own bits (blend variant, unlit, ...) */
    uint32_t reserved; /* 4 B - keeps the row a multiple of 16 with no hole */
} XudPushRow;

#define XUD_DESCRIPTOR_PUSH_SIZE 80u /* sizeof(XudPushRow), asserted below */
#define XUD_DESCRIPTOR_UNIFORM_BLOCK_SIZE 120u /* sizeof(XudFrameUniformRow), asserted below */

static_assert(sizeof(XudFrameUniformRow) == XUD_DESCRIPTOR_UNIFORM_BLOCK_SIZE,
    "the per-frame block's C layout is the size this layer states, so the buffer's size and the GLSL block's bytes are one number and not "
    "two");
static_assert(sizeof(XudPushRow) == XUD_DESCRIPTOR_PUSH_SIZE,
    "the push row's C layout is the size this layer states, so the push-constant range's size and the GLSL block's bytes are one number "
    "and not two");
static_assert((XUD_DESCRIPTOR_PUSH_SIZE % 4u) == 0u,
    "a push-constant range's size must be a multiple of 4, which is what makes the driver's own offset arithmetic land on a float");
static_assert((XUD_DESCRIPTOR_UNIFORM_BLOCK_SIZE % 4u) == 0u,
    "a uniform block whose size is not a multiple of 4 is a block whose last float is read out of bounds by the shader");

/* The spec's guaranteed floor of maxPushConstantsSize (an implementation must support at least 12
 *  8 bytes). The push row fitting the FLOOR rather than the 256 this driver answers is the repo's
 *  "the library must still work at the floor" rule, and it is why the row is 80 bytes and not 128
 *  plus a light array.
 */
#define XUD_DESCRIPTOR_PUSH_MIN_GUARANTEE 128u

static_assert(XUD_DESCRIPTOR_PUSH_SIZE <= XUD_DESCRIPTOR_PUSH_MIN_GUARANTEE,
    "the push row must fit the spec's guaranteed floor of maxPushConstantsSize, not merely the number this driver happens to answer");
static_assert(XUD_DESCRIPTOR_PUSH_SIZE < XUD_DESCRIPTOR_UNIFORM_BLOCK_SIZE,
    "the push row and the per-frame block are two different sizes, and a layout that confused them would be one size written into two "
    "create-infos");

/*-----------------------------------------------------------------------------------------------
 * The stage flags of the two rows of this layout
 *-----------------------------------------------------------------------------------------------*/

/* The set-0 uniform buffer is read by BOTH stages of this library's only pipeline: the vertex sta
 *  ge reads the VP matrix to place a vertex and the fragment stage reads the fog/ambient to colour
 *   it. The set-1 sampler array is read by the fragment stage only - a vertex stage that samples i
 *  s a displacement decision this library does not take. The push row carries the same pair as th
 *  e buffer, because the model matrix is a vertex-stage read and the material/texture indices are
 *   a fragment-stage read, and a stage flag that is not a stage of the pipeline is an error agains
 *   t the layout rather than a no-op.
 */
#define XUD_DESCRIPTOR_BUFFER_STAGES (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
#define XUD_DESCRIPTOR_SAMPLER_STAGES VK_SHADER_STAGE_FRAGMENT_BIT
#define XUD_DESCRIPTOR_PUSH_STAGES (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)

static_assert(XUD_DESCRIPTOR_BUFFER_STAGES != XUD_DESCRIPTOR_SAMPLER_STAGES,
    "the uniform buffer is read by both stages and the sampler array by the fragment stage only, so a layout that gave them the same flags "
    "would be a layout that lost a decision");
static_assert((XUD_DESCRIPTOR_BUFFER_STAGES & XUD_DESCRIPTOR_SAMPLER_STAGES) == XUD_DESCRIPTOR_SAMPLER_STAGES,
    "the sampler stages are a subset of the buffer stages, which is what makes the two rows of this layout one stage set and not two "
    "unrelated ones");
static_assert(XUD_DESCRIPTOR_PUSH_STAGES == XUD_DESCRIPTOR_BUFFER_STAGES,
    "the push row and the per-frame buffer carry the same stage pair today, which is what makes one pipeline's two stages the whole stage "
    "set of this layout");

/*-----------------------------------------------------------------------------------------------
 * The buffer usage of set 0's one buffer
 *-----------------------------------------------------------------------------------------------*/

/* UNIFORM_BUFFER is the use this buffer has today. TRANSFER_DST is the use the design sketch's p
 *  er-frame update needs - "once per frame, one vkCmdCopyBuffer from the staging ring" - and a des
 *  tination whose usage does not include VK_BUFFER_USAGE_TRANSFER_DST_BIT is an error rather than
 *  something the driver forgives. Declaring it now is the same forward decision as the pool's seco
 *  nd row: the copy is the frame loop's step, the usage bit is this layout's.
 */
#define XUD_DESCRIPTOR_BUFFER_USAGE (VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)

static_assert((XUD_DESCRIPTOR_BUFFER_USAGE & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) == VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
    "set 0's binding is a UNIFORM_BUFFER descriptor, and a buffer whose usage does not include that bit is a descriptor that names a "
    "buffer it may not be read as");
static_assert((XUD_DESCRIPTOR_BUFFER_USAGE & VK_BUFFER_USAGE_TRANSFER_DST_BIT) == VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    "the per-frame update of the design sketch is a vkCmdCopyBuffer INTO this buffer, which the usage bits make legal rather than "
    "implementation-dependent");

/*-----------------------------------------------------------------------------------------------
 * The host-visible staging arena of set 0's block
 *
 *   The design sketch's "once per frame, one vkCmdCopyBuffer from the staging ring" is the whole
 *   of the per-frame update, and AGENTS.md's staging rule says the copy path is preferred over a
 *   mapped write for anything transient. So the block's bytes get a source buffer of their own, a
 *   host-visible one, mapped ONCE for the life of the descriptor rather than once a frame - the
 *   same shape the batch's arena is, one rung over, and the reason the batch's note gives: a map
 *   per frame would need a matching unmap plus an invalidate row, and a map for the whole life nee
 *   ds neither.
 *
 *   It is NOT carved from the allocator's transient ring, for the batch's own reason: the ring is a
 *   cursor that wraps, so a ring carve is bytes a later lap overwrites mid-frame, and these bytes
 *   have to survive from the moment the host wrote them to the moment the copy read them. A static
 *   carve is the one that lives that long.
 *
 *   The arena is exactly one block long rather than a doubling arena: the block's size is a fixed
 *   120 B and the only thing ever written into it is one XudFrameUniformRow, so there is nothing to
 *   grow. XUD_DESCRIPTOR_UPLOAD_REGION_COUNT is 1 because the block is one contiguous run of bytes
 *   and vkCmdCopyBuffer's regions are (srcOffset, dstOffset, extent) triples - one region carries
 *   the whole block, which is the opposite of the batch's five-call shape, where the five destinati
 *   ons are five buffers and this one destination is one buffer.
 *-----------------------------------------------------------------------------------------------*/

#define XUD_DESCRIPTOR_STAGING_USAGE VK_BUFFER_USAGE_TRANSFER_SRC_BIT
#define XUD_DESCRIPTOR_UPLOAD_REGION_COUNT 1u

static_assert(XUD_DESCRIPTOR_STAGING_USAGE == VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    "the staging arena is a source of the upload and never a destination of one, and no shader stage reads it");
static_assert((XUD_DESCRIPTOR_STAGING_USAGE & XUD_DESCRIPTOR_BUFFER_USAGE) == 0u,
    "the staging arena and set 0's uniform buffer claim disjoint usage: one is the copy's source and one is its destination, so a buffer "
    "that asked for both would be a buffer whose usage says nothing about which");
static_assert(XUD_DESCRIPTOR_UPLOAD_REGION_COUNT == 1u,
    "the block is one contiguous run of bytes, so one vkCmdCopyBuffer region carries the whole of it");
static_assert(XUD_DESCRIPTOR_UPLOAD_REGION_COUNT * XUD_DESCRIPTOR_UNIFORM_BLOCK_SIZE == XUD_DESCRIPTOR_UNIFORM_BLOCK_SIZE,
    "the one region's extent is the whole block, so the arena and the buffer hold the same number of uploadable bytes");

/*-----------------------------------------------------------------------------------------------
 * The pool's flags
 *-----------------------------------------------------------------------------------------------*/

/* VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT (0x1, core 1.0 - the only core pool flag; the
 *  UPDATE_AFTER_BIND and HOST_ONLY bits are extension-gated and this library enables no descripto
 *  r-pool extension). The bit makes vkFreeDescriptorSets legal, which is what lets a rung above re
 *  tire one set - a material's set 1 - without destroying the pool and every set in it. The set al
 *  located today is never freed: it is per-frame UPDATED, and an update is vkUpdateDescriptorSets,
 *  which needs no pool bit at all. The bit is therefore a policy of the pool rather than a call th
 *  is rung makes, and the reason it is on is the reason the texture step's set has to be freeable.
 */
#define XUD_DESCRIPTOR_POOL_FLAGS VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT

static_assert(XUD_DESCRIPTOR_POOL_FLAGS == VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
    "the one core-1.0 pool flag, written as the constant rather than as 0x1");

/*-----------------------------------------------------------------------------------------------
 * The one descriptor record of this rung, and every row it was assembled from
 *
 *   The create-infos are members rather than frame temporaries for the rule xud_swapchain.c and x
 *   ud_pipeline.c both state: the create-info and the record of what was created are the same byt
 *   es, so they cannot disagree, and the rows the create-infos point at have to outlive the call t
 *   hat read them.
 *-----------------------------------------------------------------------------------------------*/

typedef struct XudDescriptor XudDescriptor;
struct XudDescriptor {
    /* --- the layout: two set layouts + one push-constant range --- */
    VkDescriptorSetLayout setLayouts[XUD_DESCRIPTOR_SET_COUNT]; /* owned: VK_NULL_HANDLE until xud_descriptor_open() creates it, destroyed
                                                                   by xud_descriptor_close() in the reverse of creation order */
    VkDescriptorSetLayoutCreateInfo layoutCreateInfos[XUD_DESCRIPTOR_SET_COUNT]; /* what vkCreateDescriptorSetLayout was handed, per set */
    VkDescriptorSetLayoutBinding bindings[XUD_DESCRIPTOR_SET_COUNT]
                                         [XUD_DESCRIPTOR_BINDINGS_PER_SET]; /* the rows the create-infos above point at */
    VkPushConstantRange pushRange; /* the one range of the layout, recorded rather than discarded */

    /* --- the pool the sets are allocated from --- */
    VkDescriptorPool pool; /* owned: there is no per-set destroy, the sets die with it unless vkFreeDescriptorSets retires one first */
    VkDescriptorPoolCreateInfo poolInfo; /* what vkCreateDescriptorPool was handed */
    VkDescriptorPoolSize poolSizes[XUD_DESCRIPTOR_POOL_SIZE_COUNT]; /* the rows the create-info above points at */

    /* --- the sets allocated from it --- */
    VkDescriptorSet
        sets[XUD_DESCRIPTOR_SET_COUNT]; /* set 0 live from xud_descriptor_open() on; set 1 VK_NULL_HANDLE until the texture step allocates
                                           it - the "declared but not allocated" state, asserted rather than assumed */
    VkDescriptorSetAllocateInfo setAllocateInfo; /* what vkAllocateDescriptorSets was handed */

    /* --- set 0's one uniform buffer --- */
    VkBuffer uniformBuffer; /* owned: created here, destroyed here, its memory is the allocator's */
    VkBufferCreateInfo bufferInfo; /* what vkCreateBuffer was handed */
    XudMemoryRange uniformRange; /* BORROWED: the allocator owns the block this sits in and is the only place that frees it; VK_NULL_HANDLE
                                    memory is the "not carved yet" state */
    VkDeviceSize uniformBindOffset; /* the offset inside that block vkBindBufferMemory was handed, recorded so the teardown and the next
                                       bind read the same number the driver was given */

    /* --- the write that bound it --- */
    VkWriteDescriptorSet writeInfo; /* what vkUpdateDescriptorSets was handed */
    VkDescriptorBufferInfo bufferRow; /* the row the write above points at: (buffer, offset, range) */

    /* --- the host-side copy of set 0's block --- */
    XudFrameUniformRow uniformBlock; /* the bytes the GPU buffer is meant to hold. xud_descriptor_write_block() copies them into the arena
                                        below and xud_descriptor_upload() copies the arena into the GPU buffer, so today this is the layout
                                        of a block that has never been uploaded - see the honest-boundary note in todo_completed.md */

    /* --- set 0's host-visible staging arena, the copy's source --- */
    VkBuffer uniformStagingBuffer; /* owned: host-visible, created here, destroyed here */
    VkBufferCreateInfo uniformStagingInfo; /* what vkCreateBuffer was handed for it */
    XudMemoryRange
        uniformStagingRange; /* BORROWED: the allocator owns the block this sits in; VK_NULL_HANDLE memory is the "not carved yet" state */
    VkDeviceSize uniformStagingBindOffset; /* the offset inside that block vkBindBufferMemory was handed, recorded so the teardown and the
                                              map read the same number the driver was given */
    void* uniformStagingMapped; /* what vkMapMemory wrote, for the WHOLE arena, mapped once at open and unmapped once at close - the repo's
                                   "no vkMapMemory per frame without a matching vkUnmapMemory" rule, satisfied by mapping it once */
    uint32_t uniformStagingMappedLive; /* 1 while that mapping exists, 0 after xud_descriptor_close() unmapped it */

    /* --- the upload's create-info and its one region --- */
    VkBufferCopy
        uploadRegion; /* the one region of the frame loop's one vkCmdCopyBuffer call: (srcOffset 0, dstOffset 0, extent the block's size) */
};

/*-----------------------------------------------------------------------------------------------
 * The layer's contract
 *-----------------------------------------------------------------------------------------------*/

/// Pure: the caps gate for the whole layout, read entirely off the physical layer's recorded limi
/// ts and calling no driver function. Every number this rung writes is checked here rather than as
/// sumed, because the repo's rule is that anything unverified stays off and the answer above is XU
/// D_UNSUPPORTED, not a smaller layout: the two set layouts against maxBoundDescriptorSets (the ru
/// le that bounds a pipeline layout's setLayoutCount), the set-0 uniform buffer against maxPerStag
/// eDescriptorUniformBuffers and maxDescriptorSetUniformBuffers (the per-stage and per-set totals a
/// layout's descriptorCounts are checked against), the set-1 sampler array against maxPerStageDesc
/// riptorSamplers and maxDescriptorSetSamplers (the same pair for the COMBINED_IMAGE_SAMPLER type),
/// the push row against maxPushConstantsSize (the rule that bounds a layout's push-constant range)
/// and the uniform block against maxUniformBufferRange (the rule that bounds a UNIFORM_BUFFER desc
/// riptor's range - a block bigger than that limit is a refusal rather than a buffer the driver wil
/// l accept and the shader will not). XUD_INVALID_ARG on a NULL caps; XUD_UNSUPPORTED when any one
/// of them is too small; XUD_OK otherwise.
XudResult xud_descriptor_caps_support(const XudDeviceCaps* caps);

/// Pure: the two VkDescriptorSetLayoutCreateInfo rows and the binding rows they point at, stamped
/// and pNext-cleared, flags 0 and pImmutableSamplers NULL. Set 0 is one UNIFORM_BUFFER binding of
/// one descriptor read by both stages; set 1 is one COMBINED_IMAGE_SAMPLER binding of XUD_DESCRIPT
/// OR_SAMPLER_ARRAY_SIZE descriptors read by the fragment stage only. Both bindings are numbered XU
/// D_DESCRIPTOR_BINDING, which is what makes the binding list a contiguous list starting at 0, the
/// rule the create-info states for pBindings. An immutable sampler is a sampler handle this layer d
/// oes not own (the texture step's), so the pointer is NULL and the sampler is a descriptor the tex
/// ture step will write. The flags of a layout are the extension-gated VkDescriptorSetLayoutCreateF
/// lags bits and this rung writes none. The partially-bound, descriptor-indexed array the design
/// sketch wants is a gate stated rather than a row written: VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT
/// is here (:6459), VkDescriptorSetLayoutBindingFlagsCreateInfo is here at :6771 with its stamp at
/// :344 and VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME is here at :17467, but chaining that struct
/// into a layout's pNext would need the extension enabled on the instance and its answer recorded in
/// the caps, which is a CHAINED VkPhysicalDeviceDescriptorIndexingFeatures (:6778, descriptorBinding
/// PartiallyBound at :6798) - the same pNext cost the dynamic-rendering gate was refused for, since
/// it would grow XudDeviceCaps by a 4th chained struct, restate the physical layer's write-back check
/// and grow VkDeviceCreateInfo.pNext. So today every descriptor of set 1 has to be written before
/// that set is bound, and the chain is a written-down future gate. XUD_INVALID_ARG on a NULL
/// descriptor.
XudResult xud_descriptor_fill_set_layouts(XudDescriptor* descriptor);

/// Pure: the one VkPushConstantRange of this layout - stageFlags XUD_DESCRIPTOR_PUSH_STAGES, offse
/// t 0, size XUD_DESCRIPTOR_PUSH_SIZE - gated on the caps' maxPushConstantsSize. The offset is 0 be
/// cause a layout with one range has nothing to place it after, and a range's offset must be a mult
/// iple of 4, which is what makes the driver's own offset arithmetic land on a float. XUD_INVALID_AR
/// G on a NULL out-range or a NULL caps; XUD_UNSUPPORTED when the caps' floor is below this row's s
/// ize; XUD_OK otherwise.
XudResult xud_descriptor_fill_push_range(const XudDeviceCaps* caps, VkPushConstantRange* outRange);

/// Pure: the VkDescriptorPoolCreateInfo and the two VkDescriptorPoolSize rows it points at. maxSet
/// s is XUD_DESCRIPTOR_SET_COUNT, which is the rule that a pool's maxSets must be at least the numb
/// er of sets allocated from it read as a capacity rather than a today-count: the pool is sized for
/// the set the texture step will allocate so that step needs no second pool. poolSizes is one row pe
/// r descriptor TYPE (not per set) - one UNIFORM_BUFFER and XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE COMBI
/// NED_IMAGE_SAMPLER - because the rule the create-info states asks for one entry per type a set al
/// located from this pool will be updated with, and the counts are the ceiling of how many of each
/// type the pool's sets may hold in total. flags is XUD_DESCRIPTOR_POOL_FLAGS for the reason state
/// d beside that constant. XUD_INVALID_ARG on a NULL descriptor.
XudResult xud_descriptor_fill_pool_info(XudDescriptor* descriptor);

/// Pure: the VkDescriptorSetAllocateInfo that allocates THIS rung's set 0 - descriptorPool the pool
/// handle in the record, one set, pSetLayouts pointing at the rung's own setLayouts[XUD_DESCRIPTO
/// R_SET_PER_FRAME]. The array is a member rather than a temporary because the driver reads the po
/// inter during the call and the record of the call has to outlive it, the same reason xud_pipeline
/// _fill_create_info() points at the struct's own rows. XUD_INVALID_ARG on a NULL descriptor or one
/// whose pool handle is not live (an allocate-info that names no pool is a bad state of this layer'
/// s own record, not a bad argument from a caller - the rule xud_pipeline_fill_create_info() applies
/// to a pipeline that owns no layout).
XudResult xud_descriptor_fill_set_allocate_info(XudDescriptor* descriptor);

/// Pure: the VkBufferCreateInfo of set 0's one buffer - size XUD_DESCRIPTOR_UNIFORM_BLOCK_SIZE (the
/// block's own size, not a rounded-up guess: the buffer is exactly as big as the block, and the des
/// criptor's range is a read of a buffer that is exactly that big), usage XUD_DESCRIPTOR_BUFFER_USA
/// GE for the two reasons stated beside that constant, sharingMode EXCLUSIVE (this buffer is read b
/// y the draws of this one context and bound to no other queue family), flags 0 and pNext NULL (th
/// e dedicated-allocation chain entry the sketch's "one VkBuffer per stream" would want is VK_MEMOR
/// Y_DEDICATED_ALLOCATE_INFO, whose extension-name constant is not in these headers at all - the sa
/// me shape AGENTS.md keeps as a future gate for the allocator). queueFamilyIndexCount 0 with pQueue
/// FamilyIndices NULL is the spec's "the application does not care which queue families this buffer
/// may be used from" form, the same one xud_swapchain_fill_create_info() writes. XUD_INVALID_ARG on
/// a NULL descriptor.
XudResult xud_descriptor_fill_buffer_info(XudDescriptor* descriptor);

/// Pure: the VkWriteDescriptorSet that binds set 0's buffer into it - dstSet the set in the record,
/// dstBinding XUD_DESCRIPTOR_BINDING, dstArrayElement 0, descriptorCount 1, descriptorType UNIFORM_
/// BUFFER, pBufferInfo the rung's own bufferRow (buffer, offset 0, range the block's size) and pIma
/// geInfo and pTexelBufferView NULL, which is what the descriptor type makes the three pointer poin
/// ters mean: exactly one of the three may be non-NULL and it must match the type. The write is pe
/// er-frame in the sketch's words and once here in this rung's: the buffer handle never changes afte
/// r the bring-up below, so the set names the buffer from then on and only the buffer's CONTENTS chan
/// ge. XUD_INVALID_ARG on a NULL descriptor or one whose set or buffer handle is not live.
XudResult xud_descriptor_fill_write_info(XudDescriptor* descriptor);

/// Pure: the host-side bytes of set 0's block - the identity VP matrix and zeros everywhere else. T
/// his is the assembly the headless run can assert: the block's layout is a fact about this rung rat
/// her than a comment, and an identity matrix is the one matrix that is a fact rather than a choice (
/// a camera that is not identity is the camera step's, and this rung owns no camera). XUD_INVALID_AR
/// G on a NULL descriptor.
XudResult xud_descriptor_fill_uniform_block(XudDescriptor* descriptor);

/// Pure: set 0's block VP matrix, written from a matrix the rung above chooses. The block's viewProj
/// member is float[16] in column-major order (element index = column*4 + row, which is how the
/// block's viewProj member is read as a GLSL mat4) and this is the ONE place that member is written
/// from outside the rung's own identity default - the setter exists so the camera rung above hands
/// a matrix down rather than reaching into the block's field, which is the repo's accessor rule read
/// upward. XUD_INVALID_ARG on a NULL descriptor or a NULL matrix; the copy is element for element,
/// so a matrix that aliases the block's own member is a copy rather than an overwrite.
XudResult xud_descriptor_set_view_proj(XudDescriptor* descriptor, const float* viewProj);

/// Pure: the memory-type mask a staging arena needs - the AND of every caps memoryTypes row that
/// carries VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, so a buffer bound to a type in this mask is a buffe
/// r whose bytes the CPU can write. Zero means the driver has no host-visible type at all, which is
/// reported rather than worked around: an arena with nowhere to be written is an upload that cannot
/// happen, and the repo's rule is that an unverified capability stays off and the feature that neede
/// d it answers UNSUPPORTED. Same shape as xud_batch_host_visible_bits(), one rung over, and the sam
/// e reason: the mask is a PREFERENCE handed to the allocator, and the type the allocator actually
/// picked is re-read from xud_allocator_select_memory_type() before anything maps it.
XudResult xud_descriptor_host_visible_bits(const XudDeviceCaps* caps, uint32_t* outMemoryTypeBits);

/// Pure: the VkBufferCreateInfo of the staging arena - size XUD_DESCRIPTOR_UNIFORM_BLOCK_SIZE (one
/// block, no headroom, because the only thing ever written into it is one XudFrameUniformRow), usage
/// XUD_DESCRIPTOR_STAGING_USAGE for the reason stated beside that constant, sharingMode EXCLUSIVE, f
/// lags 0 and pNext NULL, queueFamilyIndexCount 0 with pQueueFamilyIndices NULL - the same "the
/// application does not care which queue families" form xud_swapchain_fill_create_info() and
/// xud_descriptor_fill_buffer_info() write. XUD_INVALID_ARG on a NULL descriptor.
XudResult xud_descriptor_fill_staging_info(XudDescriptor* descriptor);

/// Pure: the VkCopyBufferInfo of the per-frame upload and the one VkBufferCopy region it points at -
/// srcBuffer the arena, dstBuffer set 0's uniform buffer, one region at (srcOffset 0, dstOffset 0,
/// extent the block's size). Both offsets are 0 because both buffers are exactly one block long and
/// the block starts at the start of each, and the extent is the block's size rather than either buffe
/// r's, which is the rule vkCmdCopyBuffer states: a region must fit inside BOTH buffers, and here the
/// two fit and are equal. The rows are members rather than frame temporaries for the rule every other
/// create-info in this tree follows - the driver reads the pointers during the call, so what it reads
/// has to outlive the call. XUD_INVALID_ARG on a NULL descriptor or one whose arena or buffer handle
/// is not live.
XudResult xud_descriptor_fill_upload_info(XudDescriptor* descriptor);

/// Host write: copy the block's bytes into the mapped arena. This is the ONLY host write the per-fr
/// ame update makes, and the barrier that makes it visible to the transfer is the frame loop's, named
/// there - this call records nothing and calls no driver entry point, so it is testable with no device
/// present up to the one guard it runs: an arena that is not mapped is a state this rung refuses rat
/// her than a write into NULL. XUD_INVALID_ARG on a NULL descriptor; XUD_UNSUPPORTED when the arena
/// is not mapped live; XUD_OK once the bytes are in.
XudResult xud_descriptor_write_block(XudDescriptor* descriptor);

/// Driver: record the per-frame upload into a command buffer that has already been begun - ONE vkCmd
/// CopyBuffer call carrying the one region, from the mapped arena into set 0's uniform buffer. One
/// call rather than several because this upload has one source and one destination, which is the oppo
/// site of the batch's five-call shape where the five destinations are five buffers. The call is legal
/// only OUTSIDE a render pass instance, so the frame loop records it before vkCmdBeginRenderPass; the
/// two barriers that make the host write visible to the transfer and the transfer visible to the stag
/// es that read the block are the frame loop's too, and are named there. XUD_INVALID_ARG on a NULL
/// descriptor/loader/device/command-buffer-handle or on a descriptor whose arena or buffer handle is
/// not live; XUD_UNSUPPORTED when the arena is not mapped live, the upload's rows are not filled, or
/// the copy row is nil; XUD_OK only when the region's extent is non-zero, which xud_descriptor_fill_up
/// load_info() is the guard of.
XudResult xud_descriptor_upload(XudDescriptor* descriptor, XudLoader* loader, const XudDevice* device, VkCommandBuffer commandBuffer);

/// Driver: the whole rung in one call, in the one order that is correct, and every destroy row of ev
/// ery object checked before the create that needs it (a layout this layer cannot destroy is a layout
/// it must not create, the rule xud_window.c applies to a surface, xud_swapchain.c to a swapchain and
/// xud_allocator_acquire() to a block it cannot free). Order: the caps gate, then the two set layouts,
/// then the pool, then the buffer (vkCreateBuffer -> vkGetBufferMemoryRequirements -> xud_allocator_a
/// cquire() -> vkBindBufferMemory), then set 0 (vkAllocateDescriptorSets), then the write (vkUpdateDes
/// criptorSets), then the staging arena (vkCreateBuffer -> requirements -> acquire -> bind -> vkMapMem
/// ory, once, for the whole arena, for the whole life of the descriptor). The arena comes LAST because
/// it is the only object of the six that nothing else in this rung names, and putting it last is what
/// makes the teardown below its exact reverse rather than a second order to state. Four of the rows th
/// is rung needs return VOID - vkGetBufferMemoryRequirements, vkUpdateDescriptorSets and vkUnmapMemor
/// y - so there is no VkResult to map for them: the requirements row is cleared before the call and a
/// size of zero is a refusal, and the write's inputs are all checked live before the call. On any erro
/// r everything created so far is retired in reverse order and *outDescriptor stays NULL. XUD_INVALID_
/// ARG on a NULL out-pointer or one that already owns a descriptor, a NULL loader/device/caps/alloca
/// tor, or a handle-less device; XUD_UNSUPPORTED when the loader is closed, its device rows are not th
/// is device's, a row this rung needs is nil, the caps gate above refused, or no host-visible memory t
/// ype exists to bind the arena to; XUD_OUT_OF_MEMORY when a host allocation fails; XUD_OUT_OF_RESOUR
/// CES/XUD_LOST/XUD_VULKAN_ERROR when the driver refuses for its own reason.
XudResult xud_descriptor_open(
    XudDescriptor** outDescriptor, XudLoader* loader, const XudDevice* device, const XudDeviceCaps* caps, XudAllocator* alloc);

/// Driver: the teardown, in the reverse of the order above and for the reasons the order above gives.
/// The guard runs first and is not decoration: a command buffer of the frame loop above may still be r
/// eading this very buffer and this very set, so xud_device_wait_idle() is the same boundary xud_swapch
/// ain_close() and xud_allocator_close() state, and a guard that cannot run (XUD_UNSUPPORTED) is a tea
/// rdown this rung must not start while a LOST device is still torn down and still reported - the rule
/// xud_device_close() follows. Then, in the exact reverse of the order above: vkUnmapMemory for the st
/// aging arena FIRST (the repo's rule that a mapped range has a matching unmap, read at the one place a
/// descriptor dies, and the reason it is FIRST is that a destroy of a buffer whose range is still mapp
/// ed is a destroy over a mapping the driver still holds), then vkDestroyBuffer for the arena and xud_a
/// llocator_release() for its range, then vkDestroyDescriptorPool, which retires set 0 with it (there is
/// no vkDestroyDescriptorSet in these headers - grep it: zero hits - and the set is therefore the pool'
/// s to retire, which is why the pool is destroyed before the layouts: a set still allocated from a lay
/// out is an error against that layout's destroy), then vkDestroyBuffer for set 0's block, then xud_all
/// ocator_release() for its range (the buffer first, so a free range never names memory a live buffer s
/// its in, and the range after because the block itself is the allocator's to free, in xud_allocator_cl
/// ose(), which runs after this rung in the context's teardown order), then the two layout destroys in
/// reverse creation order, then free. XUD_ALREADY when the pointer owns no descriptor; XUD_INVALID_ARG
/// on a NULL pointer-to-pointer or a NULL loader/device/allocator; XUD_UNSUPPORTED when the device row
/// s are not this device's or a destroy row is nil while the object it would destroy is live.
/// ter or a NULL loader/device/allocator; XUD_UNSUPPORTED when the device rows are not this device's or
/// a destroy row is nil while the object it would destroy is live.
XudResult xud_descriptor_close(XudDescriptor** ioDescriptor, XudLoader* loader, const XudDevice* device, XudAllocator* alloc);

#endif /* XUD_XUD_DESCRIPTOR_H_ */
