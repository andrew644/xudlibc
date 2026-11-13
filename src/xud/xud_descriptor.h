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
 *   Layer: above the allocator and below the renderer. This is the rung of the bindless
 *          descriptor discipline - ONE per-frame set carrying the per-frame block (updated once
 *          per frame), the shadow sampler and the sampler array, with push constants for the mo
 *          del matrix + material/texture indices - and it is the rung that makes the layout mean somethin
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
 *   What this rung does NOT have, and why: no PER-TEXTURE VkImage and no per-texture VkSampler an
 *          ywhere, and no second set either: entry 0 of the sampler array names the one white de
 *          fault below at bring-up and the rest of the axis stays unbound until owners claim it
 *          - the texture rung creates the images, and the entry write below is what puts a
 *          texture's own view and sampler into its entry of the array (which is why the set
 *          is allocated here rather than there: a set whose layout this rung declares is a set this
 *          rung allocates, and a rung above that could not allocate a set it has no layout for would
 *          be a rung that bound a pipeline layout it did not build). The push row of this layout is
 *          DECLARED here and CONSUMED one rung over: the frame loop pushes it beside every draw it
 *          records and the vertex stage reads the model matrix out of it, so the bytes this rung's
 *          range row describes are live end to end. What nothing READS today is the material / f
 *          lags duo of that same row: the material channel is FILLED (the material rung above na
 *          mes a live MATERIAL slot of the handle table in it, and the variant half landed with
 *          the pipeline pair), the texture channel is the white entry's index until a texture own
 *          s one, and no stage touches the flags half yet - making a stage read them is the step
 *          that wants it, and a stage need not touch every member of a block its layout declares
 *          .
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#ifndef XUD_XUD_DESCRIPTOR_H_
#define XUD_XUD_DESCRIPTOR_H_

#include "volk.h" /* Required for: VK_NO_PROTOTYPES - must precede <vulkan/vulkan.h>, see xud_loader.h */
#include "xud.h" /* Required for: XudResult - the descriptor layer's own error codes */
#include "xud_allocator.h" /* Required for: XudAllocator, XudMemoryRange - the GPU memory this rung's one VkBuffer sits in */
#include "xud_device.h" /* Required for: XudDevice - the handle every row of this rung is asked of, and the wait-idle guard */
#include "xud_layout.h" /* Required for: XudLayoutTracker - the context's layout map the white image is registered with, and the rows the white pair of transitions records through */
#include "xud_loader.h" /* Required for: XudLoader - the Volk rows every call here is made through */
#include "xud_physical.h" /* Required for: XudDeviceCaps - the limits every gate below is read off */
#include "xud_vulkan.h" /* Required for: xud_result_from_vk - every VkResult here goes through that one funnel */
#include <vulkan/vulkan.h> /* Required for: VkDescriptorSetLayout, VkDescriptorPool, VkDescriptorSet, VkBuffer, VkPushConstantRange, the create-infos and their stamps - never linked */

#include <assert.h> /* Required for: static_assert, the compile-time checks below */
#include <stdint.h>

/*-----------------------------------------------------------------------------------------------
 * Why this rung is xud_descriptor and not a descriptor set inside the renderer
 *
 *   The design sketch's descriptor discipline is a LAYOUT decision, not a per-draw one, and the
 *   bindless form of it is what landed: one set with the block, the shadow and the sampler array,
 *   push constants choosing the array entry per draw instead of a second bind. A layout is created once at init and is
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
 *   ce sits at the VK_API_VERSION_1_3 floor, see AGENTS.md) applied to the descriptor rung rathe
 *   r than a choice about it.
 *-----------------------------------------------------------------------------------------------*/

/* The ONE SET LAYOUT of the pipeline. The pipeline layout names exactly this one row, and that
   array IS the set numbering, so set 0 is row 0 by construction. Set 0 carries everything the
   draws sample: the per-frame uniform block at binding 0, the shadow sampler at binding 1, and
   the sampler ARRAY at binding 2 - one binding whose elements are the textures, indexed per draw
   by the push row's textureIndex. There is one set and no per-draw bind at all: the frame binds
   set 0 once per pass and the array element a draw samples is a push, not a bind (the write-a-
   bound-set gate that the per-slot sets existed to dodge is lifted by the bindless knobs the
   device gate below insists on: partially-bound entries and update-after-bind).
*/
#define XUD_DESCRIPTOR_SET_COUNT 1u
#define XUD_DESCRIPTOR_SET_PER_FRAME 0u /* the one set: block at 0, shadow at 1, the sampler array at 2 */

static_assert(
    XUD_DESCRIPTOR_SET_PER_FRAME < XUD_DESCRIPTOR_SET_COUNT, "the one set is row 0 of pSetLayouts, which is what makes the set numbered 0");
static_assert(XUD_DESCRIPTOR_SET_COUNT == 1u,
    "one set is the bindless design: a second set would be a second bind per draw, which is what this rung used to pay");

/* Three bindings in the one set, a contiguous list from 0 as the create-info asks. The numbers
   are constants because the layout, like every other binding here, is a bring-up decision and
   not a per-draw question; the per-draw question is WHICH ELEMENT of binding 2, and that is a
   push.
*/
#define XUD_DESCRIPTOR_BINDINGS_PER_SET 3u
#define XUD_DESCRIPTOR_BINDING 0u /* the per-frame uniform block, read by both stages */
#define XUD_DESCRIPTOR_BINDINGS_PER_FRAME 3u
#define XUD_DESCRIPTOR_SHADOW_BINDING 1u /* the shadow map the fragment stage samples (S2.5) */
#define XUD_DESCRIPTOR_SHADOW_STAGE VK_SHADER_STAGE_FRAGMENT_BIT
#define XUD_DESCRIPTOR_SAMPLER_BINDING 2u /* the sampler array: the textures, indexed by push */

/* The bloom chain is LEVELS entries of the sampler array, not a binding of its own: the chain's
   views are written into the array at claim time (the renderer claims the block beside the HDR
   scratch) and the resolve indexes it with the constants below. The count is the chain's depth,
   stated once here and read by the renderer's array of levels and the shaders' index math.
*/
#define XUD_DESCRIPTOR_BLOOM_LEVELS 4u
#define XUD_DESCRIPTOR_BLOOM_STAGE VK_SHADER_STAGE_FRAGMENT_BIT

/* The sampler array's size: the CEILING on how many entries binding 2 may name. It is this
   layer's own bound for the same reason XUD_HANDLE_SLOT_MAX_CAPACITY is the handle layer's -
   nothing about Vulkan says how many textures a context may sample - and a runtime-sized array
   makes the entry count nearly free (an unbound entry costs the driver nothing it did not
   budget, and partially-bound is a claimed knob), so the ceiling is set where the bit-array
   bookkeeping stays one small row: 512 entries are eight mask words, and XUD_MAX_TEXTURES
   leaves 506 live textures after the permanent residents below. Growing it further is a layout
   and pipeline-layout recreate, which the "created once at init" rule forbids at runtime; the
   caps gate is what refuses a device whose sampled-image limits sit below the array.
*/
#define XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE 512u

/* The array's permanent residents, claimed at bring-up before any texture exists and never
   handed out. The shaders name the first two by literal constant (present.frag's HDR and BLOOM
   taps) and the asserts below pin the constants to these numbers: the array's numbering is one
   contract with two readers, and a silent reorder is a resolve sampling the wrong image.
*/
#define XUD_DESCRIPTOR_ENTRY_WHITE 0u /* the 1x1 white default an untextured draw samples */
#define XUD_DESCRIPTOR_ENTRY_HDR 1u /* the renderer's HDR scratch, the resolve's source */
#define XUD_DESCRIPTOR_ENTRY_BLOOM 2u /* + level: the bloom chain, the resolve's taps */

static_assert(XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE > 0u, "a sampler axis of zero entries has nothing for a draw's textureIndex to name");
static_assert((XUD_DESCRIPTOR_ENTRY_BLOOM + XUD_DESCRIPTOR_BLOOM_LEVELS) <= XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE,
    "the chain's block of entries fits the axis it is claimed from");
static_assert(XUD_MAX_TEXTURES == XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE - 6u,
    "the public ceiling of live textures is the array minus its six permanent residents - white, the HDR scratch and the four "
    "bloom-chain entries, all claimed before any texture exists - the two constants must not drift apart, because the free list "
    "enforces this rung's number while a caller reads xud.h's");

/* The free list of the sampler axis: one bit per entry, bit i set while an entry names a live
   owner's view and sampler and clear while nobody owns it (entry 0 is set at open and never
   cleared). The mask is the only state of which entry the next bind takes - xud_descriptor_b
   ind_texture_slot() takes the lowest clear bit, which is what makes a fixed sequence of creates
   and closes a fixed sequence of entry numbers, the repo's determinism rule read at the axis.
*/
#define XUD_DESCRIPTOR_SAMPLER_MASK_WORDS ((XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE + 63u) / 64u)
#define XUD_DESCRIPTOR_SLOT_MASK_WORD(slot) ((slot) / 64u)
#define XUD_DESCRIPTOR_SLOT_MASK_BIT(slot) (1ull << ((slot) % 64u))

static_assert(XUD_DESCRIPTOR_SAMPLER_MASK_WORDS == 8u,
    "the axis fits eight 64-bit words exactly, the row size the pick loop and the tests both read");

/* The pool's rows: one per descriptor TYPE the set is updated with - the uniform buffer, and
   COMBINED_IMAGE_SAMPLER at the whole axis plus the shadow sampler. The runtime-sized array is
   budgeted at its ceiling: the pool is sized so the axis can ever be fully written, which is
   the "sized for the capacity, not the today-count" rule the per-slot pool followed.
*/
#define XUD_DESCRIPTOR_POOL_SIZE_COUNT 2u

static_assert(XUD_DESCRIPTOR_POOL_SIZE_COUNT == 2u,
    "one pool row per descriptor TYPE of this layout, which is what makes the pool sized for both types and not only for one");

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

/* set 0's one uniform buffer: the per-frame block - VP matrix, camera pos, time, fog, ambient
 *  and the light table. The table is XUD_MAX_LIGHTS rows of XudLightRow, the light rung's
 *  (xud_light.h/.c) row element: position, cone axis, colour-and-intensity and range-and-cone
 *  are the four vec4 a sun, a point and a spot all fill, so ONE element serves the three kinds
 *  and the shader reads a flat array rather than three parallel ones. The rung WRITES them
 *  through xud_descriptor_set_lights() - the public xud_set_lights() is what calls it - and
 *  xud_descriptor_open() zeroes the whole block, xud_descriptor_fill_uniform_block() puts the
 *  identity in viewProj, and the camera rung (xud_camera.h/.c, S28) overwrites that every
 *  frame. cameraPos, time and fog are the sketch's remaining placeholders, zero forever until
 *  the step that wants one lands - the fragment stage reads ambient and the table, not them.
 *  The row is spelled with float arrays for the same reason every other member below is: a
 *  std140 struct and a C struct agree byte-for-byte only when both spell their own padding.
 */
/* One light's bytes: the table element the light rung builds and the fragment stage reads. The
 *  meaning of each member is the KIND's - params.w carries the kind (xud.h's XUD_LIGHT_KIND_*)
 *  and the other three floats say which of the members above mean attenuation and which mean a
 *  cone. A SUN has no position to light from (it is at infinity), so its position member holds
 *  the direction TOWARD the light, the one vector a sun does have.
*/
typedef struct {
    float position[4]; /* 16 B - the light's world position; a SUN stores its unit direction-THERE instead, w = 0 */
    float target[4]; /* 16 B - a SPOT's cone axis pointing AWAY from the light, w = 0; zero for the other kinds */
    float color[4]; /* 16 B - rgb 0..1 and w = the intensity the rgb is scaled by before the shade is summed */
    float params[4]; /* 16 B - x range (0 = no distance falloff), y inner cone cosine, z outer cone cosine, w kind */
} XudLightRow; /* 64 B: std140 aligns a struct to its strictest member (16) and rounds its size up to that, so the array stride is the same 64 both sides agree on */

static_assert(sizeof(XudLightRow) == 64u, "the light table's row is the 64 B the GLSL struct's std140 layout answers, on both sides");

typedef struct XudFrameUniformRow {
    float viewProj[16]; /* 64 B - the VP matrix, column-major, the same 64 B the push row carries */
    float cameraPos[3]; /* 12 B - the camera position, the sketch's fog input, zero today: no shader reads it */
    float time; /* 4 B - seconds since the context was brought up */
    float fog[4]; /* 16 B - rgb + density */
    float ambient[4]; /* 16 B - rgb + intensity */
    float lightViewProj[16]; /* 64 B - the shadow-casting SUN's own view-projection, column-major: the matrix
                                 the shadow pass's vertex stage maps world space through, and the matrix the
                                 fragment stage maps the shaded point through to find its own texel. Identity
                                 while no sun lives - the enable flag, not this matrix, is what tells the
                                 fragment stage to ignore it */
    float shadowParams[4]; /* 16 B - x and y the shadow map's texel size (1/size), z the depth compare's bias,
                               w 1.0 when a shadow-casting sun lives and 0.0 otherwise - the one flag that
                               folds the whole feature out of the shade's sum */
    uint32_t lightCount; /* 4 B - how many of the light array the light step will fill are live */
    uint32_t reserved; /* 4 B - keeps the scalars a multiple of 4 with no hole anywhere */
    float tablePad
        [2]; /* 8 B - std140 aligns a struct array to 16 and a C struct member lands wherever the scalars end: the pad IS the difference, written once so both sides put the table at 208 */
    XudLightRow lights[XUD_MAX_LIGHTS]; /* 512 B - the table; row i is live when i < lightCount */
} XudFrameUniformRow;

/* The push-constant range of the design sketch: the model matrix + material index + texture index
 *  + flags. The model matrix is the same 64 B as the VP matrix because both are a column-major 4x
 *  4 of floats, and the batch step is what will write it.
 */
typedef struct XudPushRow {
    float modelMatrix[16]; /* 64 B - the model matrix */
    uint32_t materialIndex; /* 4 B - which material slot this draw uses */
    uint32_t textureIndex; /* 4 B - which entry of set 0's sampler array this draw samples */
    uint32_t flags; /* 4 B - low two bits: the variant (XudBatchVariant); bits above: the flag macros below */
    uint32_t reserved; /* 4 B - keeps the row a multiple of 16 with no hole */
    float tint[4]; /* 16 B - the draw's own tint, 0..1 per channel: the per-draw colour the fragment stage multiplies by */
} XudPushRow;

/* The flags channel's bit above the variant bits: set when the draw's transform and tint come from
   the instance streams (bindings 4 and 5, xud_batch.h) and NOT from this row's modelMatrix/tint.
   The batch records the difference as XUD_BATCH_INSTANCE_FROM_PUSH in the draw row's firstInstance;
   xud_frame_fill_push_row is where the difference becomes a bit; the vertex stages branch on the
   bit rather than reading a matrix the row deliberately did not fill. The variant stays in bits 0..1
   (four variants, so a mask of 3 reads it back), which is what lets a UI-column instanced draw,
   were one ever recorded, keep both facts in one word. */
#define XUD_PUSH_FLAG_INSTANCED 8u
#define XUD_PUSH_VARIANT_MASK 3u

#define XUD_DESCRIPTOR_PUSH_SIZE 96u /* sizeof(XudPushRow), asserted below */
#define XUD_DESCRIPTOR_UNIFORM_BLOCK_SIZE 720u /* sizeof(XudFrameUniformRow) = 200 + the 8 B pad + 8 rows of 64 B, asserted below */

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
 *   it. The set-0 sampler array is read by the fragment stage only - a vertex stage that samples i
 *  s a displacement decision this library does not take. The push row carries the same pair as th
 *  e buffer, because the model matrix is a vertex-stage read and the material/texture indices are
 *   a fragment-stage read, and a stage flag that is not a stage of the pipeline is an error agains
 *   t the layout rather than a no-op.
 */
#define XUD_DESCRIPTOR_BUFFER_STAGES (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
#define XUD_DESCRIPTOR_SAMPLER_STAGES VK_SHADER_STAGE_FRAGMENT_BIT
#define XUD_DESCRIPTOR_WHITE_TRANSFER_STAGE VK_PIPELINE_STAGE_TRANSFER_BIT
#define XUD_DESCRIPTOR_WHITE_SAMPLED_STAGE VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
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
static_assert(XUD_DESCRIPTOR_WHITE_TRANSFER_STAGE != XUD_DESCRIPTOR_WHITE_SAMPLED_STAGE,
    "the two transitions around the white default's fill move between two layouts and the two stages that use them, so one stage mask for "
    "both "
    "would be a barrier about a stage that never touches the image");
static_assert((XUD_DESCRIPTOR_WHITE_SAMPLED_STAGE & VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT) == VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
    "the stage the sampled layout belongs to is the fragment stage, which is the same stage XUD_DESCRIPTOR_SAMPLER_STAGES names in the "
    "other "
    "enum family - the two families are two bit patterns and the layout's rows use one and the barriers use the other");

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
 * The one image this rung owns: the white default entry 0 of the sampler array names
 *
 *   Why the default has to exist at all is a rule about the ARRAY rather than about the shader:
 *   the entry every untextured draw names has to name a real VkImageView and a real VkSampler, a
 *   nd PARTIALLY_BOUND excuses the UNBOUND entries, not a bound one pointing nowhere, so
 *   this rung owns one image, one view and one sampler; entry 0 names them for the whole life
 *   of the rung and a texture's bind writes its own pair into the entry it claims. What the default's VALUE is follows from what the textured frag does with it:
 *   that stage multiplies the vertex colour by the texel, so white is the identity of the multiply a
 *   nd a textured draw before any texture exists draws the vertex colour rather than a black rectan
 *   gle or garbage. The extent is 1x1 because a default is sampled by every fragment of every draw
 *   that has no texture and one texel is the smallest image that can hold one; the format is the tex
 *   ture rung's own RGBA8 rather than a second format, which is what makes one image in this librar
 *   y have one format and one sampler filter rather than two sets of rules to keep in step.
 *
 *   The layout is the texture rung's UNDEFINED initial layout plus the two optimal layouts the
 *   texture rung already names, and the reason the default needs the same transition pair that an
 *   uploaded texture does is that it is WRITTEN every frame and READ in the same frame: a transfer
 *   into an image and a fragment sampling it out of it are two accesses of two layouts, and the one
 *   thing that orders them is the pair of barriers around the write. Clearing rather than copying is
 *   the other half: a default has no host bytes to stage, so vkCmdClearColorImage is the one command
 *   that writes a colour into an image with no source object and no staging buffer of its own.
 *-----------------------------------------------------------------------------------------------*/

#define XUD_DESCRIPTOR_WHITE_FORMAT VK_FORMAT_R8G8B8A8_UNORM
#define XUD_DESCRIPTOR_WHITE_TILING VK_IMAGE_TILING_OPTIMAL
#define XUD_DESCRIPTOR_WHITE_USAGE (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
#define XUD_DESCRIPTOR_WHITE_INITIAL_LAYOUT VK_IMAGE_LAYOUT_UNDEFINED
#define XUD_DESCRIPTOR_WHITE_UPLOAD_LAYOUT VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
#define XUD_DESCRIPTOR_WHITE_SAMPLED_LAYOUT VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
#define XUD_DESCRIPTOR_WHITE_MIP_LEVELS 1u
#define XUD_DESCRIPTOR_WHITE_ASPECT_MASK VK_IMAGE_ASPECT_COLOR_BIT
#define XUD_DESCRIPTOR_WHITE_CLEAR_COUNT 1u
#define XUD_DESCRIPTOR_WHITE_TRANSFER_WRITE_ACCESS VK_ACCESS_TRANSFER_WRITE_BIT
#define XUD_DESCRIPTOR_WHITE_SHADER_READ_ACCESS VK_ACCESS_SHADER_READ_BIT

static_assert(XUD_DESCRIPTOR_WHITE_FORMAT == VK_FORMAT_R8G8B8A8_UNORM,
    "the default's format is the texture rung's format, so one image in this library has one format and one set of rules about it");
static_assert((XUD_DESCRIPTOR_WHITE_USAGE & VK_IMAGE_USAGE_SAMPLED_BIT) == VK_IMAGE_USAGE_SAMPLED_BIT,
    "the default is what a COMBINED_IMAGE_SAMPLER descriptor samples, so its usage must claim the sampled use");
static_assert((XUD_DESCRIPTOR_WHITE_USAGE & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    "the default is written by vkCmdClearColorImage, whose image must have been created with the transfer-dst use");
static_assert((XUD_DESCRIPTOR_WHITE_USAGE
                  & (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        == 0u,
    "the default is never written by a shader, never an attachment and never a copy source");
static_assert(XUD_DESCRIPTOR_WHITE_UPLOAD_LAYOUT != XUD_DESCRIPTOR_WHITE_INITIAL_LAYOUT,
    "the create asks for UNDEFINED and the clear declares TRANSFER_DST, which is the transition the barrier above takes explicitly");
static_assert(XUD_DESCRIPTOR_WHITE_SAMPLED_LAYOUT != XUD_DESCRIPTOR_WHITE_UPLOAD_LAYOUT,
    "the clear ends in one layout and a sampler needs another, which is why the clear is followed by a transition");
static_assert(XUD_DESCRIPTOR_WHITE_MIP_LEVELS == 1u, "a 1x1 default has one level, so its subresource range is one level deep");
static_assert(XUD_DESCRIPTOR_WHITE_ASPECT_MASK == VK_IMAGE_ASPECT_COLOR_BIT,
    "an RGBA8 default is a colour image, so its subresource range names the colour plane and nothing else");
static_assert(XUD_DESCRIPTOR_WHITE_CLEAR_COUNT == 1u, "a 1x1 image is one layer, so the clear's layer count is one");
static_assert(XUD_DESCRIPTOR_WHITE_SHADER_READ_ACCESS == VK_ACCESS_SHADER_READ_BIT,
    "the layout a sampled descriptor's read lives in is the one whose access bit the transition barrier names");

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

/* VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT (core-promoted with the descriptor-indexing
 *  feature the device gate insists on). The bindless array takes the pair of per-binding flags
 *  beside it - PARTIALLY_BOUND, because the free list leaves entries unbound, and
 *  UPDATE_AFTER_BIND, because a texture created, closed, or re-filtered between frames rewrites
 *  an entry of the ONE set while frames are in flight and recorded command buffers name that
 *  set. The old per-slot-set design needed neither bit because it bound a different SET per
 *  texture; the bindless design trades the per-draw bind for these two flags, and the device
 *  gate (xud_device.c) is what proves the driver agreed to both before this pool exists. The
 *  FREE_DESCRIPTOR_SET bit is dropped: this rung allocates one set for its whole life and frees
 *  nothing, so the bit was a policy without a call and the call-gone design has no policy left.
 */
#define XUD_DESCRIPTOR_POOL_FLAGS VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT
#define XUD_DESCRIPTOR_SAMPLER_BINDING_FLAGS (VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT)

static_assert(XUD_DESCRIPTOR_POOL_FLAGS == VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
    "a pool that may hold a set with update-after-bind bindings must itself be created with the bit, the create-info's rule");
static_assert(
    (XUD_DESCRIPTOR_SAMPLER_BINDING_FLAGS & (VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT))
        == XUD_DESCRIPTOR_SAMPLER_BINDING_FLAGS,
    "the array's two flags are the pair the bindless design states: unbound holes and writes beside in-flight frames");

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
    /* --- the layout: one set layout + one push-constant range --- */
    VkDescriptorSetLayout setLayouts[XUD_DESCRIPTOR_SET_COUNT]; /* owned: VK_NULL_HANDLE until xud_descriptor_open() creates it, destroyed
                                                                   by xud_descriptor_close() in the reverse of creation order */
    VkDescriptorSetLayoutCreateInfo layoutCreateInfos[XUD_DESCRIPTOR_SET_COUNT]; /* what vkCreateDescriptorSetLayout was handed, per set */
    VkDescriptorSetLayoutBinding bindings[XUD_DESCRIPTOR_SET_COUNT]
                                         [XUD_DESCRIPTOR_BINDINGS_PER_FRAME]; /* the rows the create-infos above point at: block,
                                                                                 shadow, sampler array - a contiguous list from 0 */
    VkDescriptorBindingFlags samplerBindingFlags[XUD_DESCRIPTOR_BINDINGS_PER_FRAME]; /* per-binding rows: zeros for the block and the
                                                         shadow, the bindless pair for the sampler array */
    VkDescriptorSetLayoutBindingFlagsCreateInfo samplerBindingFlagsInfo; /* chained into the one layout create-info: the row above is
                                                         how binding 2 may name unbound entries and be
                                                         rewritten beside in-flight frames */
    VkPushConstantRange pushRange; /* the one range of the layout, recorded rather than discarded */

    /* --- the pool the sets are allocated from --- */
    VkDescriptorPool pool; /* owned: there is no per-set destroy, the sets die with it unless vkFreeDescriptorSets retires one first */
    VkDescriptorPoolCreateInfo poolInfo; /* what vkCreateDescriptorPool was handed */
    VkDescriptorPoolSize poolSizes[XUD_DESCRIPTOR_POOL_SIZE_COUNT]; /* the rows the create-info above points at */

    /* --- the one set allocated from it --- */
    VkDescriptorSet sets[XUD_DESCRIPTOR_SET_COUNT]; /* set 0, live from xud_descriptor_open() on: the block, the shadow
                                                       sampler and the whole sampler array. The frame binds this ONE set
                                                       once per pass; a draw's texture is a push indexing the array,
                                                       never a bind */
    VkDescriptorSetAllocateInfo setAllocateInfo[XUD_DESCRIPTOR_SET_COUNT]; /* what vkAllocateDescriptorSets
                                             was handed, one row per set - the array is the set numbering,
                                             the same reason pSetLayouts' array is */

    /* --- set 0's one uniform buffer --- */
    VkBuffer uniformBuffer; /* owned: created here, destroyed here, its memory is the allocator's */
    VkBufferCreateInfo bufferInfo; /* what vkCreateBuffer was handed */
    XudMemoryRange uniformRange; /* BORROWED: the allocator owns the block this sits in and is the only place that frees it; VK_NULL_HANDLE
                                    memory is the "not carved yet" state */
    VkDeviceSize uniformBindOffset; /* the offset inside that block vkBindBufferMemory was handed, recorded so the teardown and the next
                                       bind read the same number the driver was given */

    /* --- the write that bound it --- */
    VkWriteDescriptorSet writeInfo; /* what vkUpdateDescriptorSets was handed for set 0 */
    VkDescriptorBufferInfo bufferRow; /* the row the write above points at: (buffer, offset, range) */
    VkWriteDescriptorSet shadowWriteInfo; /* what vkUpdateDescriptorSets was handed for set 0's shadow row - the one write that puts the
                                             shadow view-and-sampler into binding 1, made once at renderer bring-up because the shadow
                                             map outlives every frame exactly as the block's buffer does */
    VkDescriptorImageInfo shadowImageRow; /* the (imageView, sampler, layout) triple that write names: written by the same call, read by
                                             nothing after it - the record-of-the-create rule one rung up, on a descriptor */
    VkWriteDescriptorSet bloomWriteInfo; /* what vkUpdateDescriptorSets was handed for the bloom chain - the one
                                            write that puts the chain's four view-and-sampler rows into the sampler
                                            array at its claimed block. Written at renderer bring-up AND at every
                                            resize re-open, because the chain's images are recreated with the
                                            scratch pair. */
    VkDescriptorImageInfo bloomImageRows[XUD_DESCRIPTOR_BLOOM_LEVELS]; /* the four (imageView, sampler, layout)
                                            triples that write names, in chain order: level 0 first. */

    /* --- the sampler array's writes: the white entry every untextured draw samples, and the bind/release/update pair --- */
    VkWriteDescriptorSet whiteWriteInfo; /* what vkUpdateDescriptorSets was handed to put the white pair into entry 0 at
                                             bring-up - the one write of the array that must happen: entries left unbound
                                             are legal only because the binding is PARTIALLY_BOUND, and entry 0 is the
                                             hole every untextured draw would otherwise sample */
    uint64_t samplerSlotMask[XUD_DESCRIPTOR_SAMPLER_MASK_WORDS]; /* the free list of the array: one bit per entry, set
                                             while an entry names a live owner's view and sampler. Entry 0's bit is set
                                             at open and never cleared, because entry 0 is the white default's own and
                                             a texture that took it would be sampled by every untextured draw */
    VkDescriptorImageInfo textureSlotRow; /* the row the array write below points at: a texture's view and sampler on a
                                             bind and an update, the white pair on a release */
    VkWriteDescriptorSet textureSlotWriteInfo; /* what vkUpdateDescriptorSets was handed for the one-entry array write a
                                             bind, release and update make: dstSet is the one set, dstBinding the
                                             sampler array and dstArrayElement the entry, the white pair on a release */
    VkDescriptorImageInfo whiteImageRow; /* the (imageView, sampler, layout) triple the white write names, and the row a
                                             release writes back over a texture's entry */
    VkImage whiteImage; /* owned: the 1x1 white default entry 0 of the sampler array names. It is this rung's
                                           rather than the texture rung's because it is what makes an
                                           ALLOCATED set legal at all: a bound set whose descriptors were
                                           never written is a violation, and the descriptors of this set
                                           are written by this rung */
    VkImageView whiteImageView; /* owned: the view whiteImageRow names */
    VkSampler whiteSampler; /* owned: the view they all sample with */
    VkImageCreateInfo whiteImageInfo; /* what vkCreateImage was handed for it */
    XudLayoutTracker* layout; /* NOT owned: the context's layout tracker, held here because the white image's pair of
                                 transitions is recorded by the upload step below - which is handed a command buffer, not
                                 a tracker - and because the registration at open and the forget at close are this rung's
                                 own bookkeeping. The tracker outlives this rung by the context's own order. */
    VkImageViewCreateInfo whiteViewInfo; /* what vkCreateImageView was handed for it */
    VkSamplerCreateInfo whiteSamplerInfo; /* what vkCreateSampler was handed for it */
    XudMemoryRange whiteRange; /* BORROWED: the allocator owns the block this sits in, the same rule a
                                   s uniformRange's */
    VkDeviceSize whiteBindOffset; /* the offset inside that block vkBindImageMemory was handed */
    VkClearColorValue whiteClearColor; /* what vkCmdClearColorImage was handed on the one frame that fills
                                           the image: opaque white, which is the identity of the textured frag's
                                           multiply */
    VkImageSubresourceRange whiteSubresourceRange; /* the one subresource of a one-level 1x1 image, and
                                           the range the clear and the two transitions above name */
    VkImageMemoryBarrier whiteToUpload; /* the transition the clear needs: the image's layout into
                                           TRANSFER_DST, the one layout a clear may write */
    VkImageMemoryBarrier whiteToSampled; /* the transition the clear leaves behind: TRANSFER_DST into
                                           SHADER_READ_ONLY, the one layout a sampled descriptor may read. The
                                           fill runs once, on the first frame, and leaves the image in exactly
                                           this layout, so a frame after the first records nothing for it */
    uint8_t whiteFilled; /* the one bit that decides whether the frame above records the fill's three
                           commands at all: set on the first frame to record them, and every frame after
                           the first skips them rather than re-clearing a 1x1 image that nothing else
                           writes */

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
/// D_UNSUPPORTED, not a smaller layout: the one set layout against maxBoundDescriptorSets, the se
/// t-0 uniform buffer against maxPerStageDescriptorUniformBuffers and maxDescriptorSetUniformBuffe
/// rs, the sampler array against maxPerStageDescriptorSampledImages and maxDescriptorSetSampledIma
/// ges - the array is 512 entries plus the shadow sampler, and a driver at the spec floor of 64 i
/// s a refusal, which is honest: a bindless device is a modern device and the same is true of its
/// four indexing knobs (the device gate, xud_device.c) - the push row against maxPushConstantsSize
/// and the uniform block against maxUniformBufferRange. XUD_INVALID_ARG on a NULL caps; XUD_UNSUPP
/// ORTED when any one of them is too small; XUD_OK otherwise.
XudResult xud_descriptor_caps_support(const XudDeviceCaps* caps);

/// Pure: the one VkDescriptorSetLayoutCreateInfo row and the three binding rows it points at, st
/// amped. Binding 0 is the UNIFORM_BUFFER of both stages; binding 1 the shadow COMBINED_IMAGE_SAMP
/// LER of the fragment stage; binding 2 the sampler array - descriptorCount XUD_DESCRIPTOR_SAMPLER
/// _ARRAY_SIZE with the bindless pair in its samplerBindingFlags row, chained through the rung's ow
/// n samplerBindingFlagsInfo, and the layout's flags carrying UPDATE_AFTER_BIND_POOL, the create-i
/// nfo's rule that a layout with update-after-bind bindings says so. descriptorCount on a runtime-
/// sized array is a BUDGET: the driver may hold fewer and the writes below never pass it. An immut
/// able sampler is a sampler handle this layer does not own (the texture step's), so the pointer s
/// tays NULL on every row. The knobs that make this legal are core-promoted on the 1.3 floor and t
/// he device gate has already refused a driver that answered any of them FALSE. XUD_INVALID_ARG on
/// a NULL descriptor.
XudResult xud_descriptor_fill_set_layouts(XudDescriptor* descriptor);

/// Pure: the one VkPushConstantRange of this layout - stageFlags XUD_DESCRIPTOR_PUSH_STAGES, offse
/// t 0, size XUD_DESCRIPTOR_PUSH_SIZE - gated on the caps' maxPushConstantsSize. The offset is 0 be
/// cause a layout with one range has nothing to place it after, and a range's offset must be a mult
/// iple of 4, which is what makes the driver's own offset arithmetic land on a float. XUD_INVALID_AR
/// G on a NULL out-range or a NULL caps; XUD_UNSUPPORTED when the caps' floor is below this row's s
/// ize; XUD_OK otherwise.
XudResult xud_descriptor_fill_push_range(const XudDeviceCaps* caps, VkPushConstantRange* outRange);

/// Pure: the VkDescriptorPoolCreateInfo and the two VkDescriptorPoolSize rows it points at. maxSet
/// s is XUD_DESCRIPTOR_SET_COUNT: the one bindless set, because the axis became descriptors inside
/// that set and not sets. poolSizes is one row per descriptor TYPE - one UNIFORM_BUFFER and SAMPLE
/// R_ARRAY_SIZE + 1 COMBINED_IMAGE_SAMPLER, the array at its ceiling plus the shadow sampler - the
/// pool sized so the axis can ever be fully written. flags is XUD_DESCRIPTOR_POOL_FLAGS for the re
/// ason stated beside that constant. XUD_INVALID_ARG on a NULL descriptor.
XudResult xud_descriptor_fill_pool_info(XudDescriptor* descriptor);

/// Pure: the VkDescriptorSetAllocateInfo that allocates THIS rung's one set - descriptorPool the
/// pool handle in the record, one set, pSetLayouts pointing at the rung's own setLayouts[0]. The
/// row is a member rather than a temporary because the driver reads the pointer during the call an
/// d the record of the call has to outlive it, the same reason xud_pipeline_fill_create_info() poi
/// nts at the struct's own rows. XUD_INVALID_ARG on a NULL descriptor, on a set index past the one
/// set of this layout, or on one whose pool handle is not live (an allocate-info that names no poo
/// l is a bad state of this layer's own record, not a bad argument from a caller - the rule xud_pi
/// peline_fill_create_info() applies to a pipeline that owns no layout).
XudResult xud_descriptor_fill_set_allocate_info(XudDescriptor* descriptor, uint32_t setIndex);

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

/// Pure: the create-infos of the white default this rung owns - the VkImageCreateInfo of a 1x1 one-
/// level RGBA8 OPTIMAL image whose usage is SAMPLED|TRANSFER_DST and whose initial layout is UNDEFIN
/// ED, the VkImageViewCreateInfo of a 2D view of the whole of it, and the VkSamplerCreateInfo of the
/// one sampler every array entry samples with: LINEAR on both axes, CLAMP_TO_EDGE on all six,
/// UNORM_FLOAT normalized coordinates, no LOD bias, no border clamp and no compare op (a compareOp
///  other than ALWAYS is a shadow-mapping decision this library does not take, and the sampler's bo
/// rderColor is white so even a border read is the identity of the textured frag's multiply). The vi
/// ew's and sampler's create-infos are filled BEFORE the image is created so a failed image create l
/// eaves nothing behind, the same order xud_texture.h's create uses. XUD_INVALID_ARG on a NULL descr
/// iptor.
XudResult xud_descriptor_fill_white_infos(XudDescriptor* descriptor);

/// Pure: the two VkImageMemoryBarrier rows around the white default's clear - the transition into
///  TRANSFER_DST (oldLayout UNDEFINED, which is what UNDEFINED means to a barrier: "whatever layou
///  t this subresource is in", true on the first frame and honest on every later one) and the transi
///  tion back into SHADER_READ_ONLY. Both rows carry the image as VK_NULL_HANDLE here because the im
///  age is the one object the barrier is about and the bring-up below has not created it yet; xud_de
///  scriptor_upload() stamps the live handle into the same two rows before it records them. XUD_INVA
///  LID_ARG on a NULL descriptor.
XudResult xud_descriptor_fill_white_transitions(XudDescriptor* descriptor);

/// Pure: the one write that puts the white pair into entry 0 of the sampler array - dstSet the
///  one set, dstBinding XUD_DESCRIPTOR_SAMPLER_BINDING, dstArrayElement XUD_DESCRIPTOR_ENTRY_WHITE
///  (both 0 is a coincidence of numbering, the rows say which), descriptorCount 1, descriptorType
///  COMBINED_IMAGE_SAMPLER, pImageInfo the rung's own whiteImageRow. It is the only entry the ope
///  n writes: the rest of the axis may stay unbound because the binding is PARTIALLY_BOUND, and a
///  draw that names an unbound entry samples the driver's white anyway - but entry 0 is the entry
///  every untextured draw names, so the rung writes the pair it owns rather than trusting that.
///  XUD_INVALID_ARG on a NULL descriptor or one whose set, white view or white sampler is not live
///  .
XudResult xud_descriptor_fill_white_write_info(XudDescriptor* descriptor);

/// Pure: the free list's own rule - the lowest-numbered entry of the sampler array whose bit of t
/// he mask is clear, scanned word by word, which is what makes the slot a caller draws against a
/// fact rather than a choice. XUD_OUT_OF_RESOURCES when every entry of the axis is taken (the arr
/// ay's ceiling is the ceiling, the rule stated beside XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE); XUD_IN
/// VALID_ARG on a NULL descriptor or out-slot.
XudResult xud_descriptor_pick_free_slot(const XudDescriptor* descriptor, uint32_t* outSlot);

/// Pure: the one-entry array write a bind, release and update make - dstSet the one set, dstBindi
/// ng XUD_DESCRIPTOR_SAMPLER_BINDING, dstArrayElement the entry, descriptorCount 1, descriptorType
/// COMBINED_IMAGE_SAMPLER and pImageInfo the rung's own textureSlotRow, whose (imageView, sampler
/// , imageLayout) triple is the caller's rows. One element rather than the array is what keeps ev
/// ery other entry untouched, and the write is legal beside in-flight frames because the binding
/// carries UPDATE_AFTER_BIND. XUD_INVALID_ARG on a NULL descriptor, on an entry past the axis's s
/// ize, on a NULL view or sampler, or on a record whose set is not live.
XudResult xud_descriptor_fill_texture_slot_write(
    XudDescriptor* descriptor, uint32_t slot, VkImageView view, VkSampler sampler, VkImageLayout imageLayout);

/// Driver: take the next free entry of the sampler axis and write the texture's own view and sampl
/// er into it - the free-list half of the texture's binding, one array element written rather than
/// one set replaced. The imageLayout is the layout the texture's upload left its image in, read of
/// f the texture record one rung over, and it is the layout the descriptor must name for the samp
/// ling to be legal. XUD_INVALID_ARG on a NULL descriptor/loader/device or out-slot, on a NULL vi
/// ew or sampler; XUD_UNSUPPORTED on a closed loader, on a loader whose device rows are not this
/// device's, or on a nil update row; XUD_OUT_OF_RESOURCES when every entry is taken; XUD_OK only
/// with *outSlot naming the entry that now holds the pair.
XudResult xud_descriptor_bind_texture_slot(XudDescriptor* descriptor, XudLoader* loader, const XudDevice* device, VkImageView view,
    VkSampler sampler, VkImageLayout imageLayout, uint32_t* outSlot);

/// Driver: give one entry back to the free list - the white pair is written into that entry and i
/// ts bit of the mask cleared, so a sampler array that still names a destroyed view and sampler i
/// s a state this rung leaves behind rather than one it makes. The rewrite rather than a bare cle
/// ar is what makes the clear honest: the entry keeps existing after the release, and while PARTI
/// ALLY_BOUND would tolerate unbinding, the white default is the pair this rung owns for its whole
/// life and the value an untextured draw expects. The write is legal even while a frame that drew
/// this texture is in flight: UPDATE_AFTER_BIND covers the entry, and a frame drawing a texture t
/// hat the app closes mid-flight is the app's bug, not this write's. XUD_INVALID_ARG on a NULL de
/// scriptor/loader/device or an entry past the axis's size; XUD_UNSUPPORTED on a closed loader, o
/// n a loader whose device rows are not this device's, or on a nil update row; XUD_INVALID_ARG on
/// a record whose white pair is not live; XUD_ALREADY when the entry's bit is already clear; XUD_
/// OK once the entry names the white default again.
XudResult xud_descriptor_release_texture_slot(XudDescriptor* descriptor, XudLoader* loader, const XudDevice* device, uint32_t slot);

/// Driver: rewrite one ALREADY-BOUND entry to name a new (view, sampler, imageLayout) triple - th
/// e same one-entry write a bind makes, but over an entry the mask already holds rather than one
/// it frees: the bit is left alone, so the entry stays the owner's and only the pair it names chan
/// ges. This is the rung the texture step reaches for a sampler it recreates on a live texture: t
/// he old sampler is destroyed only AFTER the entry names the new one, which is the order that le
/// aves no instant at which the entry names a destroyed sampler - and with UPDATE_AFTER_BIND it i
/// s legal even against an in-flight frame, because that frame samples the old pair, never the ne
/// w one, and dies before the old one is destroyed. XUD_INVALID_ARG on a NULL descriptor/loader/d
/// evice, on an entry past the axis's size, or on a NULL view or sampler; XUD_UNSUPPORTED on a cl
/// osed loader, on a loader whose device rows are not this device's, or on a nil update row; XUD_
/// ALREADY when the entry's bit is clear (an entry nobody bound has nothing to rewrite); XUD_OK o
/// nce the entry names the new triple.
XudResult xud_descriptor_update_texture_slot(XudDescriptor* descriptor, XudLoader* loader, const XudDevice* device, uint32_t slot,
    VkImageView view, VkSampler sampler, VkImageLayout imageLayout);

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

/// Pure: set 0's block ambient row - rgb and intensity, the four floats the fragment stage adds to
/// the lights' sum. The setter exists for the same reason viewProj's does: the ambient's MEANING is
/// the light rung's (xud_light.h) and the block's BYTES are this rung's, so the rung above hands
/// four floats down rather than reaching into the field. XUD_INVALID_ARG on a NULL descriptor or a
/// NULL rgba; the copy is element for element, so a row that aliases the block's own member is a
/// copy rather than an overwrite.
XudResult xud_descriptor_set_ambient(XudDescriptor* descriptor, const float* rgba);

/// Pure: the block's whole light table and its live count, written together because they are one
/// statement - a count that reaches past the rows just written would have the shader read stale
/// bytes, and a table written under an old count has the shader skip fresh rows. Rows live in order:
/// row i is read when i < count, and rows from count to XUD_MAX_LIGHTS keep whatever the last call
/// left in them - the shader never looks at them, and the light rung hands a freshly built array
/// every time rather than a patched one. XUD_INVALID_ARG on a NULL descriptor, a NULL rows when the
/// count is non-zero, or a count past XUD_MAX_LIGHTS - the count that would read past the table.
XudResult xud_descriptor_set_lights(XudDescriptor* descriptor, uint32_t count, const XudLightRow* rows);

/// The block's shadow rows, one write: the casting sun's view-projection and its four parameters
/// (texel size on both axes, the compare bias, the enable flag). A NULL descriptor or matrix is a
/// bad argument; the values are the block's bytes until the frame uploads, the rule every other
/// setter here states.
XudResult xud_descriptor_set_shadow_view(XudDescriptor* descriptor, const float* lightViewProj, float texelSize, float bias, float enabled);

/// Driver: the one write that names the shadow map in set 0's binding 1 - view and sampler handed
/// in, SHADER_READ_ONLY the layout both the render pass's finalLayout and the sampler's read agree
/// on. XUD_INVALID_ARG on a NULL descriptor/loader/device, a handle-less device, or a NULL view or
/// sampler; XUD_UNSUPPORTED on a closed or unbound loader or a nil write row; the write's own
/// XudResult otherwise. One call per shadow map, before the first frame binds set 0 - the same
/// once-at-bring-up rule the block's own binding follows.
XudResult xud_descriptor_set_shadow_texture(
    XudDescriptor* descriptor, const XudLoader* loader, const XudDevice* device, VkImageView view, VkSampler sampler);

/// Driver: the one write that names the bloom chain in the sampler array - four views, chain
/// order, written to the block of entries starting at `firstEntry` (the renderer claims the block
/// at bring-up and hands its base here; the write does not consult the mask, because the renderer
/// owns those four entries and the write is its bookkeeping, not the free list's). All sampled
/// through the ONE sampler handed in (linear/clamp: the chain's reads want the same filter the
/// scratch pair reads with, and a per-level sampler would be four objects agreeing on nothing
/// new). SHADER_READ_ONLY is the layout every level's pass finalLayout and the resolve's read
/// agree on. XUD_INVALID_ARG on a NULL descriptor/loader/device/views, a handle-less device, a
/// NULL sampler, any NULL view in the array, or a firstEntry past the axis; XUD_UNSUPPORTED on a
/// closed or unbound loader or a nil write row. Called at renderer bring-up AND again at every
/// resize re-open: the chain's images die and return with the scratch pair, and array entries
/// naming retired views are a resolve sampling handles the driver has freed.
XudResult xud_descriptor_set_bloom_textures(XudDescriptor* descriptor, const XudLoader* loader, const XudDevice* device,
    const VkImageView* views, VkSampler sampler, uint32_t firstEntry);

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
/// CopyBuffer call carrying the one region, from the mapped arena into set 0's uniform buffer, plus
///  ONE vkCmdClearColorImage writing white into the default every array entry samples. One call r
/// ather than several because this upload has one source and one destination, which is the opposite
///  of the batch's five-call shape where the five destinations are five buffers; the clear is a seco
/// nd command rather than a second call-shape because a default has no host bytes to stage and vkCmd
/// ClearImage is the one command that writes a colour with no source object. The clear is per frame r
/// ather than once because the alternative is a flag in this record that says "already filled", whic
/// h is state the frame loop would have to keep honest across a resize; a 1x1 clear is one command w
/// hose cost no frame can see. Both calls are legal only OUTSIDE a render pass instance, so the frame
///  loop records them before vkCmdBeginRenderPass; the barrier that makes the block's upload visible
///  to the stages that read it is the frame loop's, and the clear needs none of its own because the
///  image is used in VK_IMAGE_LAYOUT_OPTIMAL, the one layout the implementation transitions and sy
/// nchronizes by itself. XUD_INVALID_ARG on a NULL descriptor/loader/device/command-buffer-handle or
///  on a descriptor whose arena, buffer handle or white image handle is not live; XUD_UNSUPPORTED whe
/// n the arena is not mapped live, the upload's rows are not filled, the copy row is nil or the white
///  image's rows are not filled; XUD_OK only when the region's extent is non-zero, which xud_descrip
/// tor_fill_upload_info() is the guard of.
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
XudResult xud_descriptor_open(XudDescriptor** outDescriptor, XudLoader* loader, const XudDevice* device, const XudDeviceCaps* caps,
    XudAllocator* alloc, XudLayoutTracker* layout);

/// Driver: the teardown, in the reverse of the order above and for the reasons the order above gives.
/// The guard runs first and is not decoration: a command buffer of the frame loop above may still be r
/// eading this very buffer and this very set, so xud_device_wait_idle() is the same boundary xud_swapch
/// ain_close() and xud_allocator_close() state, and a guard that cannot run (XUD_UNSUPPORTED) is a tea
/// rdown this rung must not start while a LOST device is still torn down and still reported - the rule
/// xud_device_close() follows. Then, in the exact reverse of the order above: vkUnmapMemory for the st
/// aging arena FIRST (the repo's rule that a mapped range has a matching unmap, read at the one place a
/// descriptor dies, and the reason it is FIRST is that a destroy of a buffer whose range is still mapp
/// ed is a destroy over a mapping the driver still holds), then vkDestroyBuffer for the arena and xud_a
/// llocator_release() for its range, then vkDestroyDescriptorPool, which retires BOTH sets with it (th
/// e is no vkDestroyDescriptorSet in these headers - grep it: zero hits - and a set is therefore the po
/// ol's to retire, which is why the pool is destroyed before the layouts: a set still allocated from a
/// layout is an error against that layout's destroy, and destroying the pool before the white view is t
/// he same rule read from the other side: the array's descriptors name that view, so the set has to be g
/// one before the view is), then the white default in the reverse of how it was created - vkDestroySam
/// pler, vkDestroyImageView, vkDestroyImage and xud_allocator_release() for its range - then vkDestroyB
/// uffer for set 0's block, then xud_allocator_release() for its range (the buffer first, so a free rang
/// e never names memory a live buffer sits in, and the range after because the block itself is the alloc
/// ator's to free, in xud_allocator_close(), which runs after this rung in the context's teardown order)
/// , then the two layout destroys in reverse creation order, then free. XUD_ALREADY when the pointer own
/// s no descriptor; XUD_INVALID_ARG on a NULL pointer-to-pointer or a NULL loader/device/allocator; XUD
/// _UNSUPPORTED when the device rows are not this device's or a destroy row is nil while the object it
/// would destroy is live.
/// ter or a NULL loader/device/allocator; XUD_UNSUPPORTED when the device rows are not this device's or
/// a destroy row is nil while the object it would destroy is live.
XudResult xud_descriptor_close(XudDescriptor** ioDescriptor, XudLoader* loader, const XudDevice* device, XudAllocator* alloc);

#endif /* XUD_XUD_DESCRIPTOR_H_ */
