/***********************************************************************************************
 *   xud_texture.h - the texture rung: the record behind a TEXTURE-kind slot of the handle
 *                   layer, the VkImage + VkImageView + VkSampler one texture owns, the mip chain
 *                   the CPU generates beside it, the staging upload that fills it, and the public
 *                   create/close pair xud.h declares above it
 *
 *   Owns : XudTextureRecord - the object record a TEXTURE-kind slot of the handle layer names -
 *          plus the rungs that fill and clear that field (xud_texture_open(), xud_texture_close(),
 *          xud_texture_get(), xud_texture_close_all()) and the pure halves above them: the
 *          argument check, the mip-count and mip-extent math, the staging arena's byte layout, the caps
 *          gate, the four create-infos this rung stamps, the guard that reads the staging buffer's own
 *          requirement back against the chain's total, and the write that fills the staging arena.
 *   Layer: above the handle table and the allocator, below xud.h - the third rung that fills a
 *          slot's record field (after the material's and the mesh's) and the second one whose
 *          record owns driver objects (a mesh's five buffers and one block, a texture's an image,
 *          a view, a sampler, a staging buffer and two blocks). It borrows the handle table (the
 *          slot and the record field through the handle layer's own two accessors), the allocator
 *          (both blocks), the loader's device table, the device and the frame rung's two pure
 *          create-infos (the upload pool and its one command buffer). It borrows nothing of the
 *          renderer's frame loop, nothing of the batch and nothing of the descriptor: a texture is
 *          not a frame object, it is an object a caller creates once and holds for as long as
 *          anything samples it.
 *
 *   Why a texture is an image plus a sampler rather than an image plus a view plus a sampler:
 *   raylib's Texture2D is { int id; int mipmaps; int width; int height; } (reference/raylib/
 *   raylib.h 273-279) - one GL texture name carrying texels, filtering and wrap mode inside it,
 *   because GL's texture object IS the sampling state. Vulkan splits the three: a VkImage is
 *   bytes with a format and no sampling state, a VkSampler is sampling state with no bytes, and
 *   a VkImageView is the one thing a descriptor may name (an image alone cannot go in a
 *   descriptor - VkDescriptorImageInfo::imageView, not .image). All three are therefore this
 *   rung's to own, and the pair above them hands out one handle for all three.
 *
 *   What the record does NOT have, and why: no descriptor set and no place in one. The per-
 *   material sampler array of the design sketch (set 1, whose LAYOUT the descriptor rung declares
 *   and whose SET it does not allocate) is the material's set-1 half, and that half is a change
 *   to a committed rung above this one - xud_material_open() would have to take the pool and the
 *   set-1 layout, and the material area's headless checks would move behind a driver. That half
 *   is recorded open in todo.md beside this step rather than done here, which is why a texture's
 *   bytes exist and nothing samples them yet: the sampler and the view are reachable through thi
 *   s record, and the step that binds them into a set is the one that changes the material.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#ifndef XUD_XUD_TEXTURE_H_
#define XUD_XUD_TEXTURE_H_

#include "volk.h" /* Required for: VK_NO_PROTOTYPES - must precede <vulkan/vulkan.h>, see xud_loader.h */
#include "xud.h" /* Required for: XudResult, XudHandle, XudTextureData, XUD_TEXTURE_* and XUD_HANDLE_KIND_TEXTURE - the public surface this rung is the half of */
#include "xud_allocator.h" /* Required for: XudAllocator, XudMemoryRange, XUD_ALLOCATOR_BLOCK_MAX_SIZE - the block a texture's bytes live in */
#include "xud_device.h" /* Required for: XudDevice, xud_device_wait_idle - the handle every row here is asked of, and the destroy guard */
#include "xud_handle.h" /* Required for: XudHandleTable, xud_handle_validate/alloc/free, xud_handle_record_get/set - the slot this record lives in */
#include "xud_loader.h" /* Required for: XudLoader, xud_loader_device_bound, xud_loader_is_open - the Volk rows every call here is made through */
#include "xud_physical.h" /* Required for: XudDeviceCaps - the image limits every gate below is read off */
#include "xud_vulkan.h" /* Required for: xud_result_from_vk - every VkResult here goes through that one funnel */
#include <vulkan/vulkan.h> /* Required for: VkImage, VkImageView, VkSampler, VkSubresourceLayout, the three create-infos and their stamps - never linked */

#include <assert.h> /* Required for: static_assert, the token mirrors and the layout's arithmetic below */
#include <stdint.h>

/*-----------------------------------------------------------------------------------------------
 * The public tokens, mirrored against Vulkan's own
 *
 *   Same shape as xud_material.h's mirror of XUD_BLEND_* against XUD_BATCH_VARIANT_*: the public
 *   tokens are what a caller writes, Vulkan's are what the sampler create-info is stamped with,
 *   and the two are the same numbers today - so the mirror is an assert rather than a translati
 *   on table someone has to keep in step. The numbering is Vulkan's own (vulkan_core.h 2381-238
 *   8 for the address modes, 2341-2343 for the filters), which is what makes a caller's token a
 *   name in this library's vocabulary and a fact about the driver at the same time.
 *-----------------------------------------------------------------------------------------------*/

#define XUD_TEXTURE_FILTER_NEAREST 0u /* VK_FILTER_NEAREST: nearest texel, no interpolation */
#define XUD_TEXTURE_FILTER_LINEAR 1u /* VK_FILTER_LINEAR: interpolated between texels */
#define XUD_TEXTURE_FILTER_COUNT 2u

#define XUD_TEXTURE_ADDRESS_REPEAT 0u /* VK_SAMPLER_ADDRESS_MODE_REPEAT: wrap, the atlas-unfriendly half of the tileable case */
#define XUD_TEXTURE_ADDRESS_MIRRORED_REPEAT 1u /* VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT: wrap with a mirrored neighbour */
#define XUD_TEXTURE_ADDRESS_CLAMP_TO_EDGE 2u /* VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE: the edge texel, raylib's CLAMP */
#define XUD_TEXTURE_ADDRESS_COUNT 3u

static_assert(XUD_TEXTURE_FILTER_NEAREST == VK_FILTER_NEAREST,
    "xud_texture.h: the public nearest token must be Vulkan's NEAREST, or a caller's filter is a different number from the sampler's");
static_assert(XUD_TEXTURE_FILTER_LINEAR == VK_FILTER_LINEAR,
    "xud_texture.h: the public linear token must be Vulkan's LINEAR, or a caller's filter is a different number from the sampler's");
static_assert(XUD_TEXTURE_FILTER_COUNT == (XUD_TEXTURE_FILTER_LINEAR + 1u),
    "xud_texture.h: the filter tokens are contiguous from nearest up, so the count is one past the highest and a range check is one "
    "compare");

static_assert(XUD_TEXTURE_ADDRESS_REPEAT == VK_SAMPLER_ADDRESS_MODE_REPEAT,
    "xud_texture.h: the public repeat token must be Vulkan's REPEAT, or a caller's wrap mode is a different number from the sampler's");
static_assert(XUD_TEXTURE_ADDRESS_MIRRORED_REPEAT == VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
    "xud_texture.h: the public mirrored token must be Vulkan's MIRRORED_REPEAT, or a caller's wrap mode is a different number from the "
    "sampler's");
static_assert(XUD_TEXTURE_ADDRESS_CLAMP_TO_EDGE == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    "xud_texture.h: the public clamp token must be Vulkan's CLAMP_TO_EDGE, or a caller's wrap mode is a different number from the "
    "sampler's");
static_assert(XUD_TEXTURE_ADDRESS_COUNT == (XUD_TEXTURE_ADDRESS_CLAMP_TO_EDGE + 1u),
    "xud_texture.h: the three address tokens are contiguous from repeat up, so the count is one past the highest and a range check is one "
    "compare");

/*-----------------------------------------------------------------------------------------------
 * The texel format, and the reason it is one format rather than an axis
 *
 *   R8G8B8A8_UNORM is the format raylib's ImageFromImage / LoadTexture produce for every raster
 *   it can read (raylib's own Texture is RGBA bytes in GL terms) and the one format that needs
 *   no decoder in the library: the caller's array is the texels, byte for byte, in the order the
 *   format names. A format axis (RGB, half-float, sRGB, a compressed one) is an open ask, not a
 *   field here: the compressed trio is deliberately NOT wanted by the physical layer's feature
 *   list (xud_physical.c: "no compressed-texture path yet"), and an sRGB format would be a dec
 *   isision about colour space that this tree has no other part of the chain made yet - no ot
 *   her stage converts, no swapchain composite is asked for one. The format is therefore a cons
 *   tant of this rung, and the four bytes per texel below are what the layout's arithmetic div
 *   ides by.
 *-----------------------------------------------------------------------------------------------*/

#define XUD_TEXTURE_FORMAT VK_FORMAT_R8G8B8A8_UNORM
#define XUD_TEXTURE_BYTES_PER_TEXEL 4u

static_assert(XUD_TEXTURE_BYTES_PER_TEXEL == 4u,
    "xud_texture.h: R8G8B8A8_UNORM is four bytes per texel, and the whole layout below is arithmetic on that number");
static_assert(XUD_TEXTURE_FORMAT == VK_FORMAT_R8G8B8A8_UNORM,
    "xud_texture.h: the format stamped into the image, the view and the texel count's divisor are one format, not three names");

/* OPTIMAL tiling, and the reason is a driver fact measured on this machine rather than a
 *   preference. LINEAR is the tiling whose interior a host mapping may write, and this machine's
 *   driver answers a ONE-level LINEAR image with exactly our arithmetic (an 8x8 one-level image
 *   requires 256 bytes at a row pitch of 32, which is 8 rows of 32) - but the same driver answers a
 *   FOUR-level LINEAR 8x8 with a requirement of 2048 bytes and per-level offsets of 0/1024/1088/1600
 *   at a row pitch of 128 for EVERY level, which is a layout whose level footprints overlap one
 *   another (level 1 at that pitch runs to 1424, past the 1088 where level 2 begins), so no write
 *   plan follows the driver's own numbers. A mip chain inside a LINEAR image is therefore not a
 *   path this library can take, and OPTIMAL plus a transfer is the path that asks the driver for no
 *   layout fact at all: the bytes are staged in our own compact arena (the arithmetic xud_texture_lay
 *   out computes, which is what the headless tests pin) and vkCmdCopyBufferToImage moves them into
 *   an interior the driver owns. That is the repo's staging rule read literally - "prefer the copy
 *   path over mapped writes" - and the mesh rung's mapped-write exception is an exception for BUFFERS
 *   , which have no interior to own; an image does. What this settles is that no texture in this
 *   library is host-written, and the cost is one staging buffer and one upload command buffer per
 *   texture, both retired at the create.
 */
#define XUD_TEXTURE_TILING VK_IMAGE_TILING_OPTIMAL

/* UNDEFINED at create, and the two layouts the image is used in after it. UNDEFINED is allowed by
 *   the create's own validation and says only "whatever these bytes happen to be", which is true
 *   because nothing host-writes this image any more and a transfer overwrites what it touches. The
 *   transition out of UNDEFINED is implicit at a command buffer's first use of the image, which is
 *   why the upload's first command is a copy that declares TRANSFER_DST_OPTIMAL rather than a barrier
 *   that names a layout the create never asked for. SHADER_READ_ONLY_OPTIMAL is what the transition
 *   at the END of the upload leaves the image in - the layout a sampled descriptor requires - and the
 *   record carries it (see XudTextureRecord::imageLayout), which is what makes the descriptor step's
 *   layout a fact read off the record rather than a guess about what the upload left behind.
 */
#define XUD_TEXTURE_INITIAL_LAYOUT VK_IMAGE_LAYOUT_UNDEFINED
#define XUD_TEXTURE_UPLOAD_LAYOUT VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
#define XUD_TEXTURE_SAMPLED_LAYOUT VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL

static_assert(XUD_TEXTURE_UPLOAD_LAYOUT != XUD_TEXTURE_INITIAL_LAYOUT,
    "xud_texture.h: the copies declare a layout and the create asked for another one, which is the transition the first use takes "
    "implicitly");
static_assert(XUD_TEXTURE_SAMPLED_LAYOUT != XUD_TEXTURE_UPLOAD_LAYOUT,
    "xud_texture.h: the upload ends in one layout and a sampler needs another, which is why the upload's last command is a transition");

/* SAMPLED is the use a texture has (the stage that colours a fragment reads it) and TRANSFER_DST is
 *   the use its upload has: vkCmdCopyBufferToImage's destination must have been created with it, and
 *   the validation layer answers a usage without it as a violation rather than a warning. No
 *   TRANSFER_SRC (nothing copies out of a texture), no DEPTH/STENCIL (a texture is never a render
 *   target here), no STORAGE (no shader writes one).
 */
#define XUD_TEXTURE_USAGE (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)

static_assert((XUD_TEXTURE_USAGE & VK_IMAGE_USAGE_SAMPLED_BIT) == VK_IMAGE_USAGE_SAMPLED_BIT,
    "xud_texture.h: the one use a texture has is a shader sampling it, which is the bit the image is created with");
static_assert((XUD_TEXTURE_USAGE & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    "xud_texture.h: the upload copies into the image, so the usage must claim the copy's destination use");
static_assert(
    (XUD_TEXTURE_USAGE & (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        == 0u,
    "xud_texture.h: a texture is never written by a shader, never an attachment and never a copy source");

/* The staging buffer's one use: it is a copy source and nothing else. The batch's staging arena
 *   states the same use, and a staging buffer that claimed UNIFORM_VERTEX or INDEX would be a buffer
 *   no binding ever makes.
 */
#define XUD_TEXTURE_STAGING_USAGE VK_BUFFER_USAGE_TRANSFER_SRC_BIT

static_assert(XUD_TEXTURE_STAGING_USAGE == VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    "xud_texture.h: the staging arena is a copy source, which is the only use the upload gives it");

/* The two barriers the upload records, and the pair of access types each one guards. The staging
 *   barrier is the frame's own batch barrier read at the same place it sits (HOST_WRITE to TRANSFER_
 *   READ, at the two stages the spec's supported-access-type table puts those access types at); the
 *   transition barrier is the image's, and its dstAccessMask is SHADER_READ at the two shader stages
 *   because the thing that reads this image is the fragment stage's sampler. A barrier is a scope,
 *   not a buffer, so the staging half's one call covers the whole arena.
 */
#define XUD_TEXTURE_STAGING_SRC_STAGE VK_PIPELINE_STAGE_HOST_BIT
#define XUD_TEXTURE_STAGING_DST_STAGE VK_PIPELINE_STAGE_TRANSFER_BIT
#define XUD_TEXTURE_STAGING_SRC_ACCESS VK_ACCESS_HOST_WRITE_BIT
#define XUD_TEXTURE_STAGING_DST_ACCESS VK_ACCESS_TRANSFER_READ_BIT
#define XUD_TEXTURE_TRANSITION_SRC_STAGE VK_PIPELINE_STAGE_TRANSFER_BIT
#define XUD_TEXTURE_TRANSITION_DST_STAGE (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)
#define XUD_TEXTURE_UPLOAD_DST_ACCESS                                                                                                      \
    VK_ACCESS_TRANSFER_WRITE_BIT /* the upload transition's own dst access: the copies below are the first thing that writes the image, so \
                                    the layout the transition moves into is the layout of a TRANSFER_WRITE - the constant the barrier      \
                                    before the copies stamps, and VUID-VkImageMemoryBarrier-dstAccessMask-0289 ("if the                    \
                                    VK_ACCESS_TRANSFER_WRITE_BIT bit is set in dstAccessMask, dstStageMask must include                    \
                                    VK_PIPELINE_STAGE_TRANSFER_BIT") is the rule that makes the same bit name the stage mask beside it */
#define XUD_TEXTURE_TRANSITION_SRC_ACCESS VK_ACCESS_TRANSFER_WRITE_BIT
#define XUD_TEXTURE_TRANSITION_DST_ACCESS VK_ACCESS_SHADER_READ_BIT

static_assert(XUD_TEXTURE_STAGING_SRC_ACCESS == VK_ACCESS_HOST_WRITE_BIT,
    "xud_texture.h: the CPU wrote the staging bytes, which is the one access type the spec puts at the HOST stage");
static_assert(XUD_TEXTURE_TRANSITION_DST_ACCESS == VK_ACCESS_SHADER_READ_BIT,
    "xud_texture.h: the image is read by a sampler, which is SHADER_READ rather than UNIFORM_READ - a buffer's access type against an "
    "image is refused by the validation layer rather than forgiven");
static_assert((XUD_TEXTURE_TRANSITION_DST_STAGE & (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)) != 0u,
    "xud_texture.h: a dstAccessMask of SHADER_READ needs a dstStageMask that names a shader stage, which is the rule the transition "
    "barrier is written against");

/* The wait-all the upload's fence wait takes: one fence, so ALL and ANY are the same question, and
 *   the frame loop's own wait states the same choice for the same reason.
 */
#define XUD_TEXTURE_WAIT_ALL VK_TRUE

/*-----------------------------------------------------------------------------------------------
 * The mip chain, and the reason it is generated rather than asked for
 *
 *   raylib generates one (raylib's Texture2D has an `int mipmaps` member and GenTextureMipmaps i
 *   s what fills it), so the caller's one mip-0 array is the whole description and the chain is
 *   the library's work. The count below is the largest the spec's own bound allows rather than a
 *   chosen quality level: the rule is mipLevels <= floor(log2(max(imageExtent[0], imageExtent[1]
 *   ))) + 1 (VUID-VkImageCreateInfo-imageMaxLevel), and taking the maximum is what makes every L
 *   OD a fragment-stage derivative can select have a level to select - with one level every LOD
 *   clamps to level 0 and a texture drawn from far away is the full-size one, which is the shimi
 *   est possible answer rather than a smaller chain chosen behind the caller's back.
 *
 *   The bound on the count is this layer's own, in the same shape XUD_MESH_MAX_VERTICES is: the
 *   arrays in the record are sized by it, and an extent that would need more levels than the arr
 *   ays have is refused before a slot is spent. 16 levels covers an extent of 32768, which is fo
 *   ur times the largest extent the spec guarantees a device must support (maxImageDimension2D i
 *   s at least 8192) and half of what the largest drivers advertise - so the caps gate below is
 *   what refuses a device, and this constant is what refuses an extent no array here can hold.
 *   Growing it grows the record, not the API.
 *-----------------------------------------------------------------------------------------------*/

#define XUD_TEXTURE_MIP_MAX_LEVELS 16u
#define XUD_TEXTURE_MAX_DIMENSION (1u << (XUD_TEXTURE_MIP_MAX_LEVELS - 1u))

static_assert(XUD_TEXTURE_MIP_MAX_LEVELS >= 14u,
    "xud_texture.h: the spec guarantees maxImageDimension2D of at least 8192, which needs 14 levels, so the arrays must at least cover "
    "what every device must accept");
static_assert(XUD_TEXTURE_MAX_DIMENSION == (1u << 15u),
    "xud_texture.h: the extent bound is one level per array row, so the largest extent the arrays can hold is 2 to the (levels - 1)");
static_assert(XUD_TEXTURE_MIP_MAX_LEVELS <= 32u,
    "xud_texture.h: an extent is a uint32_t, so no extent can ever ask for more than 32 levels - a bound above that would be an array no "
    "extent indexes");

/* The texel-count bound, derived rather than invented: the chain's total is at most 4/3 of mip 0'
 *  s area (every level is half the previous one per axis, so the areas form a quarter-ratio seri
 *  es), and the arena is one request the allocator's block-growth ceiling can serve - the same d
 *  ivision-out XUD_MESH_MAX_VERTICES does with XUD_ALLOCATOR_BLOCK_MAX_SIZE. A texture past it i
 *  s refused here, before a slot or a block is spent, rather than as a driver answer read after o
 *  ne. The 4/3 is an upper bound, not an equality: for a non-power-of-two extent the chain is sh
 *  orter than the series, which is why the assert below holds with room to spare.
 */
#define XUD_TEXTURE_MAX_TEXELS (XUD_ALLOCATOR_BLOCK_MAX_SIZE * 3u / (XUD_TEXTURE_BYTES_PER_TEXEL * 4u))

static_assert((XUD_TEXTURE_MAX_TEXELS * XUD_TEXTURE_BYTES_PER_TEXEL * 4u) <= (XUD_ALLOCATOR_BLOCK_MAX_SIZE * 3u),
    "xud_texture.h: the largest texture this rung names has a whole mip chain one request the allocator's block ceiling can serve");
static_assert(XUD_TEXTURE_MAX_TEXELS >= (1024u * 1024u),
    "xud_texture.h: the bound is at least a 1024x1024 texture with its whole chain, so a texture refused by the bound is not refused "
    "because the bound is smaller than an atlas-sized sheet");

/*-----------------------------------------------------------------------------------------------
 * The sampler's fixed rows, and the reason each one is fixed
 *
 *   Two of the sampler's members are a caller's choice (minFilter/magFilter and the three address
 *   modes' U/V pair) and the rest are this rung's. Each fixed one is fixed for a reason that is a
 *   fact about this library, not a default copied from a tutorial. The member names below are the
 *   ones these headers' VkSamplerCreateInfo actually has (2340-2358): mipmapMode rather than a bool
 *   enable, and minLod/maxLod beside mipLodBias - a sampler stamped from the other set of names is
 *   a sampler that does not compile, which is the same reason the image's create-info is stamped fr
 *   om what is there rather than from a tutorial:
 *-----------------------------------------------------------------------------------------------*/

#define XUD_TEXTURE_ADDRESS_MODE_MIP VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
#define XUD_TEXTURE_ANISOTROPY_MAX 1u
#define XUD_TEXTURE_MIPMAP_MODE VK_SAMPLER_MIPMAP_MODE_LINEAR
#define XUD_TEXTURE_LOD_BIAS 0.0f
#define XUD_TEXTURE_LOD_MIN 0.0f
#define XUD_TEXTURE_LOD_MAX VK_LOD_CLAMP_NONE
#define XUD_TEXTURE_BORDER_COLOR VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
#define XUD_TEXTURE_COMPARE_OP VK_COMPARE_OP_ALWAYS

static_assert(XUD_TEXTURE_ADDRESS_MODE_MIP == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    "xud_texture.h: addressModeW is the mip axis of a 2D texture, and the sampler clamps LOD to the image's mipLevelCount anyway, so the "
    "axis that has no beyond is clamped");
static_assert(XUD_TEXTURE_ANISOTROPY_MAX == 1u,
    "xud_texture.h: anisotropyEnable is VK_FALSE below, and one is the widest anisotropy that means no anisotropy - a bigger number beside "
    "a disabled bit is a row that says nothing about which");
static_assert(XUD_TEXTURE_MIPMAP_MODE == VK_SAMPLER_MIPMAP_MODE_LINEAR,
    "xud_texture.h: the chain is generated for every texture rather than asked for, so the LOD the derivative selects is interpolated "
    "between levels rather than snapped to one - a NEAREST mipmap mode beside a full chain is a texture that has the levels and never uses "
    "the ones between");
/* The two LOD bounds are not asserted: a static_assert needs an integer constant expression under
 *  -Wpedantic, and 0.0f and VK_LOD_CLAMP_NONE (vulkan_core.h 125, 1000.0F) are floats - the same
 *  reason xud_descriptor.h states for its float rows. They are stated at the stamp below instead.
 */

/*-----------------------------------------------------------------------------------------------
 * What a TEXTURE-kind slot records
 *
 *   Same shape as the mesh's record: heap-allocated by xud_texture_open(), stored in the slot's
 *   `record` field through the handle layer's own accessor, freed by this rung's teardown - and,
 *   age, a view, a sampler, a staging buffer and two blocks of the allocator's. The four
 *   create-infos are members rather than frame temporaries for the rule every other rung in this
 *   tree follows: the driver reads the pointers it is handed during the call, so what it reads has
 *   to outlive the call, and the create-info and the record of what was created are the same bytes
 *   and cannot disagree.
 *
 *   The three mip arrays are the STAGING arena's byte layout - offsets, sizes and row pitches,
 *   one row per mip level - and they are what the write uses and what the copy regions below
 *   are stamped from. One level's extent is not a member: the copies ask the pure mip-extent
 *   rung for it, so the extent and the pitch cannot be two numbers that drift apart.

 *   stagingMapped/stagingMappedLive are the mapping of the staging block: mapped ONCE, for the
 *   length of the upload only, and unmapped before this rung returns - a record that reached the
 *   write has its staging fields EMPTY, because the bytes the upload read are already inside the
 *   image and a record that kept them would be a record holding a second copy of every texture on
 *   the host. The one whole-block carve rule the mesh's note gives (vkMapMemory maps a VkDevice-
 *   Memory, not a range) is why the staging block is a host-visible acquire rather than a
 *   suballocating one; the image's own block is never mapped at all, because an optimal-tiled
 *   image's interior is the driver's and the upload reaches it through a transfer.
 *
 *   The sampler is a member of the record rather than one sampler shared by every texture, and tha
 *   t is a Vulkan fact rather than a preference: a COMBINED_IMAGE_SAMPLER descriptor names a sampl
 *   er and an image together, so two textures that want different filtering would need two sampler
 *   s, and one shared sampler would make the caller's filter token a token nothing obeys. The cost
 *   is one VkSampler per texture; the saving - a shared sampler per filtering mode - is an open as
 *   k, not a decision here.
 *-----------------------------------------------------------------------------------------------*/

typedef struct XudTextureRecord {
    uint32_t width; /* mip 0's extent, as the caller described it */
    uint32_t height;
    uint32_t mipLevels; /* the chain's length, the spec's maximum for that extent */

    VkDeviceSize mipOffset[XUD_TEXTURE_MIP_MAX_LEVELS]; /* where each level starts inside the staging arena */
    VkDeviceSize mipSize[XUD_TEXTURE_MIP_MAX_LEVELS]; /* how long each level is */
    VkDeviceSize mipRowPitch[XUD_TEXTURE_MIP_MAX_LEVELS]; /* the stride of one row inside one level */
    VkDeviceSize stagingBytes; /* the whole chain's total: the staging buffer's size and the staging block's request */

    XudMemoryRange imageRange; /* BORROWED: the allocator owns the block the image's memory sits in; VK_NULL_HANDLE memory is the "not
                                  carved yet" state */
    XudMemoryRange stagingRange; /* BORROWED: the same, for the staging buffer's whole host-visible block */
    VkDeviceSize imageBindOffset; /* the offset inside the image's block vkBindImageMemory2 was handed */
    VkBuffer stagingBuffer; /* owned: the copy source the arena above is staged into */
    void* stagingMapped; /* what vkMapMemory wrote, for the WHOLE staging block */
    uint32_t stagingMappedLive; /* 1 while that mapping exists, 0 after this rung's teardown unmapped it */

    VkImage image; /* owned */
    VkImageView view; /* owned: the only thing a descriptor may name of the two above */
    VkSampler sampler; /* owned: the sampling state that pairs with it */
    VkImageLayout imageLayout; /* the layout the upload left the image in, which is the layout a descriptor must name */

    VkImageCreateInfo imageInfo; /* what vkCreateImage was handed */
    VkImageViewCreateInfo viewInfo; /* what vkCreateImageView was handed */
    VkSamplerCreateInfo samplerInfo; /* what vkCreateSampler was handed */
    VkBufferCreateInfo stagingInfo; /* what vkCreateBuffer was handed for the staging buffer */
} XudTextureRecord;

/*-----------------------------------------------------------------------------------------------
 * The layer's contract
 *-----------------------------------------------------------------------------------------------*/

/// Pure: the argument check every entry point above runs before anything is spent. NULL descripti
/// on; an extent of zero (an image of no texels is an image this rung must not create, the same ze
/// ro-size rule xud_allocator_acquire() states for a block); an extent past XUD_TEXTURE_MAX_DIMENS
/// ION (no mip array here can hold it, see the constant's own note); a texel count past XUD_TEXTUR
/// E_MAX_TEXELS (the chain would not fit one request the allocator's block ceiling can serve); a fi
/// lter or address mode outside its axis (a token outside the axis is the caller's mistake, not a c
/// apability this device lacks); a NULL rgba array (a texture with no texels is not a texture with
/// a default - unlike a mesh's optional attribute streams, whose defaults are documented in xud.h,
/// the texel array is the one array a texture has and the only one with no neutral value that mean
/// s "draw this"); and the overflow guard on width*height*4, which is what makes the products below
///  a check rather than an arithmetic that wrapped past the bound. *outMipLevels, when not NULL, is
///  filled with the chain's length for that extent. XUD_INVALID_ARG on any of these or on a NULL ou
/// t-count that is asked for; XUD_OK otherwise.
XudResult xud_texture_check(const XudTextureData* data, uint32_t* outMipLevels);

/// Pure: the chain's length for one extent, which is the spec's own bound read as the maximum the
/// bound allows: 1 + floor(log2(max(width, height))). Stated as its own rung rather than folded in
/// to the check above because it is the one number the whole chain's arithmetic is built from and
/// the one number a test can pin against an extent without any other part of this rung present. XUD
/// _INVALID_ARG on a zero extent or a NULL out-count; XUD_OK otherwise.
XudResult xud_texture_mip_levels(uint32_t width, uint32_t height, uint32_t* outMipLevels);

/// Pure: one level's extent, which is the spec's mip-extent rule rather than a halving: an axis is
/// width >> level, or 1 once that shift has reached zero. The clamp matters for a non-power-of-two
/// extent - a 3x5 image's level 1 is 1x2 and its level 2 is 1x1, and an axis that halved with round
/// ing would be an axis the driver's own layout does not have, which is the reason the guard below
/// exists at all. XUD_INVALID_ARG on a NULL out-extent or a level at or past the chain's length fo
/// r that extent (asked for as the count rather than as a flag, so the level's bounds are one compa
/// re against a number the caller already has); XUD_OK otherwise.
XudResult xud_texture_mip_extent(
    uint32_t width, uint32_t height, uint32_t level, uint32_t mipLevels, uint32_t* outWidth, uint32_t* outHeight);

/// Pure: the arena's byte layout - one row per level, each level's extent from the rung above, eac
/// h level's rowPitch its width times the texel width, each level's size that pitch times its heig
/// ht, and the offsets running one level after another. There is no alignment parameter here and n
/// o padding at all, and that is the spec's own formula rather than a preference: for a LINEAR-ti
/// led, uncompressed, single-layer, single-sample image - exactly the shape the create-info below
/// stamps - a level's pitch is its width times the texel block size and the next level starts at t
/// he previous one's end rounded up to that same block size, and every size above is a multiple of
/// four, so the rounding is a no-op and the sum is exact. The mesh's layout takes an alignment bec
/// ause its numbers are a driver fact (the granularity at which separate resources are placed in m
/// emory); these numbers are a format fact (the texel block size of the one format this rung names
/// ). The driver's own answer is still read back against this layout below, and a driver that disag
/// rees is refused rather than written over. Fills the record's width/height/mipLevels/arenaBytes
/// and the three mip arrays; on any error nothing is written. XUD_INVALID_ARG on a NULL descriptio
/// n or record, or on an extent the check above would refuse; XUD_OK otherwise.
XudResult xud_texture_layout(const XudTextureData* data, XudTextureRecord* outRecord);

/// Pure: the caps gate for a texture, read entirely off the physical layer's recorded limits and ca
/// lling no driver function. maxImageDimension2D against each axis - the rule that bounds an image'
/// s extent, and the one limit these headers carry that does (grepped in vulkan_core.h: maxImageDim
/// ension1D/3D/Cube and maxImageArrayLayers are there too but describe other extents, and there is n
/// o maxImageLinearSize in these headers at all, so the linear-tiling bound the Vulkan 1.2 propertie
/// s struct would name is a number this tree's caps chain never records and a gate on it would be a
///  gate on a number never read). The texel-count bound above is this layer's own and is not a caps
///  fact, so it stays in the check rather than here. XUD_INVALID_ARG on a NULL caps or description;
///  XUD_UNSUPPORTED when the device's bound is below either axis; XUD_OK otherwise.
XudResult xud_texture_caps_support(const XudDeviceCaps* caps, const XudTextureData* data);

/// Pure: the VkImageCreateInfo this rung stamps - sType, pNext NULL, flags 0 (the sparse, the ycbc
/// r and the 3D-aliasing create flags are all extension-gated or describe an image this rung does n
/// ot have), imageType 2D, format XUD_TEXTURE_FORMAT, extent (width, height, 1) - the depth of a 2D
///  image is one, and the one is what makes the mip arithmetic above an axis pair rather than a trip
/// le -, mipLevels the chain's length, arrayLayers 1 (one image, not an array of them: an array lay
/// er is a second decision, the design sketch's atlas is a texture atlas in one image, not VK_IMAGE_
/// TYPE_2D with layers), samples 1 (no multisampling: the swapchain step's multisampling decision is
///  about the framebuffer, and a multisampled image this rung owns would need a resolve this rung do
/// es not have), tiling XUD_TEXTURE_TILING, usage XUD_TEXTURE_USAGE, sharingMode EXCLUSIVE, flags 0,
///  queueFamilyIndexCount 0 with pQueueFamilyIndices NULL (the same "the application does not care w
///  hich queue families" form xud_swapchain_fill_create_info() and xud_descriptor_fill_buffer_info()
///  write), initialLayout XUD_TEXTURE_INITIAL_LAYOUT - and nothing else. There is no size member t
/// o state here and there is no size member in these headers at all: grepped in /usr/include/vulka
/// n/vulkan_core.h 3898-3914 and in a second generated copy at 1.4.354, the struct ends at initial
/// Layout and the string "imageSize" appears nowhere in either file. The image's memory is therefo
/// re asked of below (vkGetImageMemoryRequirements2, core at this library's 1.3 floor and present i
/// n Volk's device table) rather than computed into the create-info, and the layout above is what b
/// ounds the texture's size bound and what the guard below compares the driver's per-level answer a
/// gainst. XUD_INVALID_ARG on a NULL description, record or out-info, or on a record whose extent a
/// nd description disagree.
XudResult xud_texture_fill_image_info(const XudTextureData* data, const XudTextureRecord* record, VkImageCreateInfo* outInfo);

/// Pure: the VkImageViewCreateInfo this rung stamps - image the image in the record, viewType 2D (t
/// he view type must match the image's dimensionality, which is what makes a 2D view of a 2D image a
///  fact rather than a choice), format the same format as the image's (a view whose format differs f
/// rom the image's is a view that reinterprets the texels, which is a different decision), component
/// s IDENTITY per channel (the header's own way of saying "use the channel as it is", and a view tha
/// t renamed channels would be a texture whose red and blue swap at every sample rather than a view
/// that failed), and the subresource range the view exposes: aspectMask COLOR (the only aspect a colo
/// ur image has - DEPTH and STENCIL are the aspects of an image this rung does not create), baseMipL
/// evel 0 with levelCount the chain's length (the view declares every level the sampler's LOD may sel
/// ect - a view that declared one level would make the chain this rung generated unreachable), baseA
/// rrayerLayer 0 with layerCount 1 (the image's one array layer, the same 1 the image's create-info s
/// tamps), flags 0, pNext NULL and sType stamped. XUD_INVALID_ARG on a NULL record or out-info, or on
///  a record whose image is still VK_NULL_HANDLE.
XudResult xud_texture_fill_view_info(const XudTextureRecord* record, VkImageViewCreateInfo* outInfo);

/// Pure: the VkSamplerCreateInfo this rung stamps, and every fixed row's reason is stated beside the
/// constant above rather than here. minFilter/magFilter are the caller's token in both slots (Vulkan
///  has two filter slots because a magnifying and a minifying filter can differ; raylib's SetTexture
/// Filter names one, so one token fills both - and the LOD behaviour of the pair is the note above, n
/// ot a guess). addressModeU/V are the caller's token, addressModeW is the mip axis. anisotropyEnabl
/// e is VK_FALSE with maxAnisotropy 1: anisotropic filtering is a device FEATURE (caps.features.featu
/// res.samplerAnisotropy, which the physical layer's wanted list asks for and which this machine repo
/// rts) and asking for it here would be choosing a quality level this library has no knob for, so th
/// e feature stays a fact about the device and the sampler stays isotropic - the open knob is recorde
/// d in todo.md. compareEnable is VK_FALSE with compareOp a valid-but-unused value: a comparison samp
/// ler is a shadow-map decision, and the rule the create-info states is that the comparison members a
/// re only meaningful when the bit is set. unnormalizedCoordinates is VK_FALSE: normalised [0,1] text
/// ure coordinates are what a texcoord attribute carries and what every shape this library draws carr
/// ies. mipmapMode is XUD_TEXTURE_MIPMAP_MODE rather than a bool enable (these headers' VkSamplerCre
/// ateInfo has no enable member - grepped at 2340-2358: the members are mipmapMode, mipLodBias, min
/// Lod and maxLod), and the mode is LINEAR because the chain is generated for every texture rather t
/// han asked for, so the LOD a derivative selects is interpolated between the levels that exist. min
/// Lod 0 with maxLod Vulkan's own clamp-none: the range covers the whole chain at every extent and t
/// he image's own mipLevelCount is what bounds it, so the sampler is never allowed to skip the sharp
/// est level the caller supplied. mipLodBias 0 is the sampler's only live LOD knob left at its neutr
/// al value. borderColor is a valid enum the sampler never reads, because no address mode above is a
///  BORDER one. XUD_INVALID_ARG on a NULL description or out-info, or on a filter or address token o
/// utside either axis.
XudResult xud_texture_fill_sampler_info(const XudTextureData* data, VkSamplerCreateInfo* outInfo);

/// Pure: the staging buffer's create-info, stamped from the layout's total. The buffer exists to hold
///   the compact chain the CPU writes, so its size is this rung's arithmetic and not the driver's: the
///   image's requirement is the driver's answer and the staging buffer's is ours, which is the split the
///   upload's two halves obey. XUD_INVALID_ARG on a NULL out-row; XUD_UNSUPPORTED on a total of zero (an
///   empty staging buffer is a buffer this rung must not create, the same zero-size rule the allocator
///   states for a block).
XudResult xud_texture_fill_staging_info(VkBufferCreateInfo* outInfo, VkDeviceSize totalBytes);

/// Pure: the guard that reads the driver's own buffer requirement back against the layout this rung
/// computed. It exists because the staging buffer is the one object whose bytes the CPU writes, and
/// a buffer's requirement is a driver fact (alignment and size) the staging block was acquired at: a
/// write that assumed its own offsets into a buffer the driver placed elsewhere would be a write into
/// bytes the copy then reads from the wrong place. The numbers compared are the requirement's size
/// against this rung's total and its alignment against the arena's floor; a mismatch is XUD_UNSUPPORTED
/// rather than a re-laid-out arena, because the buffer was already created with this rung's total by
/// the time the query answers. The image's own layout is never queried and never compared: an optimal-
/// tiled image's interior is implementation-defined by definition, which is what the tiling constant's
/// note settles, and the transfer is the one thing that does not need to know it. XUD_INVALID_ARG on a
/// NULL record or requirement, or on a level at or past the chain; XUD_UNSUPPORTED on a requirement
/// that cannot hold this rung's chain; XUD_OK on the two that fit.
XudResult xud_texture_verify_staging(const XudTextureRecord* record, const VkMemoryRequirements* requirements, VkDeviceSize totalBytes);

/// Pure: the write that fills the chain. Mip 0 is the caller's array, copied row by row at the recor
/// d's row pitch; every level above it is generated from the level below it AS IT SITS IN THE ARENA,
/// not from the caller's array - which is what makes the chain one set of bytes rather than a chain o
/// f temporaries that could disagree with the bytes the driver will read. The filter is a 2x2 box wi
/// th the odd edge clamped (a level whose axis is not a whole multiple of two repeats its last row/col
/// umn rather than reading past the level below), and each channel is averaged in integer arithmetic,
/// so the result is deterministic and no float rounding is involved - the repo's determinism rule rea
/// d at the one place this library generates bytes. The record's counts and the description's have to
/// name one texture (the write sizes its loops from the record and reads its arrays from the descripti
/// on, the mesh write's rule and reason), and the destination has to be at least the layout's total (t
/// he mesh write's rule and reason: the offsets reach into a destination this rung never sees). A recor
/// d whose arena was never laid out is XUD_UNSUPPORTED rather than a write at a run of zero offsets, th
/// e mesh's rule again. XUD_INVALID_ARG as above; XUD_OK once every byte is in.
XudResult xud_texture_write_mips(const XudTextureRecord* record, const XudTextureData* data, void* staging, VkDeviceSize stagingSize);

/// Pure: the accessor the rung above reaches instead of the slot array - validate against the TEXTUR
/// E kind, then hand back the record of a live slot. XUD_INVALID_ARG on a NULL out-record or table, o
/// n a handle that does not name a live TEXTURE slot (a stale handle, a cross-kind handle, a fabricat
/// ed one), or on a live slot whose record field is still the sentinel.
XudResult xud_texture_get(const XudHandleTable* table, XudHandle handle, const XudTextureRecord** outRecord);

/// Driver: the whole rung in one call, in the one order that is correct, and every destroy row checked
/// before the create that needs it (an image this layer cannot destroy is an image it must not create -
///  the rule xud_window.c applies to a surface, xud_swapchain.c to a swapchain and xud_allocator_acquire()
///  to a block it cannot free). Order: the loader/device guards, the create/query/destroy rows this rung
///  needs checked (the three creates, the three destroys, the bind, the buffer create/destroy, the two
///  requirement queries, the map/unmap, the command-buffer allocate/begin/end/free, the copy, the barrier
///  and the submit), the argument check and the caps gate (both before a slot is spent, so a refusal
///  leaves no half-texture), the slot, the record, the layout (this rung's own arithmetic, which gives
///  the per-level offsets and pitches the write below uses and the staging total), then the staging
///  buffer created at that total, then its requirement asked of and read back against the layout (the
///  guard above), then the one host-visible block acquired for the staging buffer, then that block
///  mapped, then the chain written into it, then the image created and its own requirement asked of
///  (these headers' VkImageCreateInfo has no size member, so the block's size is the driver's answer),
///  then the image's own block acquired at that requirement (NOT host-visible: the image is never
///  mapped, and the staging block above is the only mapping this rung takes), then the image bound into
///  it, then ONE command buffer recorded with the upload - a HOST_WRITE to TRANSFER_READ barrier over
///  the staging writes, one vkCmdCopyBufferToImage per mip level (the staging arena's compact bytes at
///  each level's offset into the image's own level, the destination layout XUD_TEXTURE_UPLOAD_LAYOUT),
///  then a barrier that transitions the image to XUD_TEXTURE_SAMPLED_LAYOUT - then submitted on the
///  graphics queue behind a fence this rung creates, waits on and destroys, then the command buffer
///  freed, then the staging mapping unmapped and the staging buffer destroyed (the copy has run by
///  then, which is what the wait is for: an unmapped-while-still-read staging buffer is a use after
///  the mapping that made it), then the view and the sampler created from the image that is now in
///  its sampled layout, then the record written into the slot through the handle layer's own accessor.
///  On any error everything created so far is retired in reverse order and *outHandle is left naming
///  nothing live. XUD_INVALID_ARG on a NULL out-handle, table, loader, device, caps or description, or
///  a handle-less device; XUD_UNSUPPORTED on a closed loader, on a loader whose rows this rung needs
///  are not there, on a device that is not this loader's, on a caps gate that refused, on a caps that
///  advertises no host-visible memory type for the staging block, or on a driver whose staging requirement
///  cannot hold this rung's chain; XUD_OUT_OF_RESOURCES at the handle layer's slot bound or when either
///  block cannot be carved; XUD_OUT_OF_MEMORY when the record cannot be allocated; XUD_VULKAN_ERROR or
///  the device's own code when the driver refuses for its own reason.

XudResult xud_texture_open(XudHandle* outHandle, XudHandleTable* table, XudLoader* loader, const XudDevice* device,
    const XudDeviceCaps* caps, XudAllocator* alloc, const XudTextureData* data);

/// Driver: release one live TEXTURE-kind slot and retire everything its record owns, in the reverse
///  of the order above: the wait-idle guard first (a texture's image is read by a submission the moment
///  anything samples it, and VUID-vkDestroyImage-image-00009 - "all uses of the image object must have
///  completed execution" - is the same rule the mesh's buffer destroy and the swapchain's recreate state
///  for their own objects; a guard that cannot run is a teardown this rung must not start, and a LOST
///  device is still torn down and still reported, the rule xud_device_close() follows), then the sampler
///  destroyed, the view destroyed, the image destroyed, the image's block released after the image that
///  names it is gone, the staging mapping unmapped BEFORE the staging buffer is destroyed and before
///  either block is released (a mapping is a promise about a VkDeviceMemory, and destroying an object
///  whose bytes live in a block while it is still mapped is the other order - the mesh's arena and the
///  descriptor's arena unmap in the same slot of their teardown for the same reason), then the staging
///  buffer destroyed and its block released, then the slot's record field cleared BEFORE the record is
///  freed (xud_handle_free() leaves the field as it was, so a free slot that still owns a record is a
///  record two teardown paths would both free). XUD_INVALID_ARG on a NULL table, loader, device or
///  allocator, or on a handle that does not name a live TEXTURE slot of this table; XUD_UNSUPPORTED on
///  a closed loader, on a device that is not this loader's, or on a guard that cannot run; the device's
///  own code when the guard reports one.

XudResult xud_texture_close(XudHandleTable* table, XudHandle handle, XudLoader* loader, const XudDevice* device, XudAllocator* alloc);

/// Driver: the teardown rung above the handle table's own close - retire every record any TEXTURE slot s
/// till names, live or released, and clear the field, so xud_handle_table_close() can free the arrays witho
/// ut leaking an image, a view, a sampler or a block behind them. This is the reason the handle layer's close
///  note says the records are closed BEFORE the table goes, and this rung is one of the three rungs that know
///  what a record is (xud_texture_close_all(), xud_mesh_close_all(), xud_material_close_all()) - the order in x
/// ud_close_context() is texture, mesh, material, table, texture first because a texture record owns an image, a
///  view, a sampler and a block and a mesh record owns five buffers and a block, and both have to be gone befor
/// e the arrays that name them are. *outClosed counts the records retired, the mesh's and the material's own coun
/// t and reason. XUD_INVALID_ARG on a NULL table, loader, device or allocator, or a handle-less device; XUD_OK wi
/// th 0 on a table with no TEXTURE array (nothing of this kind was ever created, which is a count of zero and not a
/// n error); XUD_UNSUPPORTED on a closed loader, on a device that is not this loader's, or on a guard that cannot r
/// un; the guard runs ONCE for the whole walk rather than once per texture, the mesh's reason.
XudResult xud_texture_close_all(
    XudHandleTable* table, XudLoader* loader, const XudDevice* device, XudAllocator* alloc, uint32_t* outClosed);

#endif /* XUD_XUD_TEXTURE_H_ */
