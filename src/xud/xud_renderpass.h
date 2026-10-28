/***********************************************************************************************
 *   xud_renderpass.h - the render-pass + framebuffer rung: one VkRenderPass and one VkFrame
 *   buffer per swapchain image
 *
 *   Layer: above the pipeline, below the frame loop. This is the rung the repo's settled
 *   render-pass decision names - the explicit VkRenderPass + VkFramebuffer path, chosen over
 *   VK_KHR_dynamic_rendering and over gating both on caps (AGENTS.md, Settled decisions) - so
 *   the create-info assembly below is the pure half the headless-test rule leans on, and the
 *   driver half is what makes it real against lavvk.
 *
 *   The attachment set is TWO rows now: the swapchain colour attachment and a depth attach
 *   ment. The depth row is the first thing in this tree that needs an IMAGE rather than a buff
 *   er, and that is the point of the allocator's rules - a VkAttachmentDescription for a depth
 *   attachment would need a VkImage and a VkDeviceMemory of its own, so this rung creates the
 *   image, asks the driver what it needs, carves that out of the allocator's blocks and binds
 *   the two together. The depth TEST and depth WRITE are ON in the pipeline above for exactly
 *   that reason now (xud_pipeline.h states the pair: blend off + depth write is the OPAQUE
 *   variant of the plan's opaque/transparent pair, and it is only honest once a depth buffer
 *   exists), which is why the first triangle has hidden-surface behaviour. The transparent
 *   variant of the pair is the batch step's - the sort is what makes it mean anything.
 *
 *   A new swapchain extent forces a new depth buffer here: xud_renderpass_recreate_depth()
 *   recreates the attachment at the extent the swapchain was recreated to, because
 *   VUID-VkFramebufferCreateInfo-attachments-02702 makes every framebuffer attachment the
 *   framebuffer's own extent. The new image / view / carve is made BEFORE the old row is
 *   retired, so a recreate that fails leaves the pass exactly as it was rather than as a pass
 *   whose description names an image that was never made, and the rung waits for the device
 *   to be idle before it starts, because the frame loop above may still have a submission or
 *   a presentation reading the very depth buffer being replaced.
 *
 *   The depth image and its one view are owned HERE, not in the framebuffer array: the pass is
 *   what declares the attachment and every framebuffer row names the SAME view, so the view's
 *   owner is the pass and the rows only borrow it. That is the opposite of the colour half, whe
 *   re each swapchain image has its own view and that view belongs to the row that attaches it
 *   (there is no way to retire a framebuffer while the view that names its image is alive).
 ***********************************************************************************************/

#ifndef XUD_RENDERPASS_H_
#define XUD_RENDERPASS_H_

#include "volk.h"
#include "xud_allocator.h"
#include "xud_device.h"
#include "xud_loader.h"
#include "xud_physical.h"
#include "xud_swapchain.h"
#include "xud_vulkan.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

/*-----------------------------------------------------------------------------------------------
 * The rung's own constants
 *-----------------------------------------------------------------------------------------------*/

#define XUD_RENDERPASS_ATTACHMENT_COUNT                                                                                                    \
    2u /* the swapchain colour attachment AND a depth attachment: the                                                                      \
          plan's "one perspective 3D scene: one camera, one depth                                                                          \
          buffer, one render pass, one batch", read at the one place the                                                                   \
          depth buffer has to be named. The two rows are in attachment                                                                     \
          order, colour first, and that order is load-bearing - the subpass's                                                              \
          colour reference and the framebuffer's attachment list both index                                                                \
          it */
#define XUD_RENDERPASS_SUBPASS_COUNT                                                                                                       \
    1u /* one subpass: the plan's subpass discipline is about                                                                              \
          ordering BETWEEN subpasses, and there is one subpass                                                                             \
          today, so the one dependency row is the external ->                                                                              \
          0 pair the spec requires for a pass that reads noth                                                                              \
          ing before it */
#define XUD_RENDERPASS_DEPENDENCY_COUNT 1u
#define XUD_RENDERPASS_COLOR_ATTACHMENT 0u /* which attachment the one subpass writes to */
#define XUD_RENDERPASS_DEPTH_ATTACHMENT                                                                                                    \
    1u /* which attachment row the depth buffer is in: the row AFTER                                                                       \
          the colour one, and the index the subpass's pDepthStencilAttachment                                                              \
          carries and a framebuffer's second attachment row points at */
#define XUD_RENDERPASS_FIRST_SUBPASS 0u /* the subpass the one dependency row targets */

/* The clear colour, in the sRGB non-linear space the swapchain's chosen format is in (the
   format selection rule in xud_swapchain.c only ever picks an SRGB_NONLINEAR row when one
   exists, see that file's note). A dark background rather than black so a run that drew
   nothing is distinguishable from a run that drew a black triangle. */
#define XUD_RENDERPASS_CLEAR_COLOR_R 0.02f
#define XUD_RENDERPASS_CLEAR_COLOR_G 0.02f
#define XUD_RENDERPASS_CLEAR_COLOR_B 0.05f
#define XUD_RENDERPASS_CLEAR_COLOR_A 1.0f

/* The depth the pass clears every attachment to, and the far side of the projection the camera
   step will produce. 1.0 in a [0,1] depth buffer is "nothing has been drawn yet", which is what
   a CLEAR load op has to mean: a clear to 0 would be a buffer that already holds the NEAREST
   possible depth and would reject every fragment the draw produced, which is a pass that draws
   nothing rather than a pass that draws the triangle. */
#define XUD_RENDERPASS_CLEAR_DEPTH 1.0f

/* The depth attachment's format. VK_FORMAT_D32_SFLOAT rather than D24_UNORM_S8_UINT because the
   24-bit form is a PAIR of aspects and the 8-bit one drags stencil state (a stencil test, a
   stencil op row, a stencil clear value) into a library that has no stencil at all; D32_SFLOAT
   is one aspect, 4 bytes per texel, and the only cost is memory, which the allocator owns. The
   format is NOT assumed to be available: this rung asks the driver what an image of exactly th
   is (format, tiling, usage) tuple needs (vkGetImageMemoryRequirements) and creates it, and a
   driver that cannot make one answers at vkCreateImage, which maps through xud_result_from_vk()
   to XUD_UNSUPPORTED rather than to a pass whose attachment description names an image that wa
   s never made. There is no VK_FORMAT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL in these headers at all
   (grepped across /usr/include/vulkan/: zero hits), so the format is a name and not "whateve
   r the hardware prefers", and vkGetPhysicalDeviceFormatProperties is not called here because
   it is an INSTANCE-table row (it takes a VkPhysicalDevice) and this rung has no instance hand
   le - the layer above it would have to hand one down for a check that vkCreateImage already
   answers. */
#define XUD_RENDERPASS_DEPTH_FORMAT VK_FORMAT_D32_SFLOAT
#define XUD_RENDERPASS_DEPTH_USAGE VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT

static_assert(XUD_RENDERPASS_ATTACHMENT_COUNT == (XUD_RENDERPASS_DEPTH_ATTACHMENT + 1u),
    "the attachment rows are contiguous from the colour attachment up");
static_assert(XUD_RENDERPASS_DEPTH_FORMAT == VK_FORMAT_D32_SFLOAT, "one aspect, 4 bytes per texel, no stencil half to carry");
static_assert(XUD_RENDERPASS_DEPTH_USAGE == VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
    "the one usage bit a depth image is asked for - it is never sampled in this tree");
static_assert(VK_SUBPASS_EXTERNAL != XUD_RENDERPASS_FIRST_SUBPASS,
    "the external pseudo-subpass is not a real subpass index, which is what makes "
    "the one dependency row an external -> 0 pair rather than 0 -> 0");
static_assert(XUD_RENDERPASS_FIRST_SUBPASS == 0u, "the first subpass is index 0");

/*-----------------------------------------------------------------------------------------------
 * The render pass, and every row it was assembled from
 *-----------------------------------------------------------------------------------------------*/

typedef struct XudRenderPass XudRenderPass;
struct XudRenderPass {
    VkRenderPass renderPass; /* the one Vulkan object of this half: VK_NULL_HANDLE until
                                xud_renderpass_open() creates it, cleared and destroyed by
                                xud_renderpass_close() */
    VkRenderPassCreateInfo createInfo; /* what vkCreateRenderPass was handed */

    /* --- the rows below are the struct's OWN, and the create-info above points at them, so the
         create-info and the record of the pass are the same bytes and cannot disagree - the rule
         xud_swapchain.c states for its create-info and its record, one rung up --- */
    VkAttachmentDescription attachments[XUD_RENDERPASS_ATTACHMENT_COUNT];
    VkAttachmentReference colorReferences[XUD_RENDERPASS_SUBPASS_COUNT];
    VkAttachmentReference depthReferences[XUD_RENDERPASS_SUBPASS_COUNT];
    VkSubpassDescription subpasses[XUD_RENDERPASS_SUBPASS_COUNT];
    VkSubpassDependency dependencies[XUD_RENDERPASS_DEPENDENCY_COUNT];
    VkClearValue clearValues[XUD_RENDERPASS_ATTACHMENT_COUNT]; /* the clear values, in attach
                                                                  ment order, so VkRenderPas
                                                                  sBeginInfo's pClearValues
                                                                  index matches the attach
                                                                  ment index - the spec's own
                                                                  rule for that pair: row 0 is
                                                                  the colour float4 and row 1
                                                                  is the depth float */

    /* --- the depth attachment's own objects, owned here for the reason stated at the top of
       the header: the pass declares the attachment and every framebuffer row names the SAME
       view, so the view cannot belong to a row. The image is the first VkImage in this tree -
       the descriptor rung's uniform buffer is a VkBuffer, and a swapchain's images belong to
       the swapchain - and it is the first thing the allocator was ever asked for an IMAGE for
       (xud_descriptor.c asked for a buffer's memory at S13). --- */
    VkImage depthImage; /* owned: VK_NULL_HANDLE until xud_renderpass_open() creates it, and
                          vkDestroyImage retires it - unlike a swapchain image, which xud_swapchain.c
                          notes has no destroy call at all */
    VkImageView depthView; /* owned beside it: the ONE view every framebuffer row attaches. There
                              is no vkDestroyImageView while a framebuffer still names it, which is
                              why xud_renderpass_close() runs after every framebuffer row has been
                              closed - the renderer's teardown order */
    VkImageCreateInfo depthImageInfo; /* what vkCreateImage was handed */
    VkImageViewCreateInfo depthViewInfo; /* what vkCreateImageView was handed */
    XudMemoryRange depthRange; /* BORROWED from the allocator: (memory, offset, size) plus the
                                { blockIndex, rangeIndex, generation } triple that makes a stale
                                borrow a caught error. The allocator owns the VkDeviceMemory; this
                                rung owns the release of the range, which is the one thing that
                                makes the image's memory reachable from here */
};

/*-----------------------------------------------------------------------------------------------
 * One framebuffer, and the array of them
 *-----------------------------------------------------------------------------------------------*/

typedef struct XudFramebuffer XudFramebuffer;
struct XudFramebuffer {
    VkFramebuffer framebuffer; /* the one Vulkan object this row owns */
    VkImageView view; /* owned beside it: the view of the swapchain image the framebuffer
                         attaches. There is no way to retire the framebuffer while the view
                         that names its image is alive, and there is no vkDestroyImage for a
                         swapchain image (xud_swapchain.c's note), so the view is this row's
                         teardown half */
    VkImageView attachmentViews[XUD_RENDERPASS_ATTACHMENT_COUNT]; /* the row's OWN attachment
                     list, in attachment order: row 0 is a copy of the colour view above and
                     row 1 is the pass's depth view, BORROWED. The list is a member rather
                     than a local because the create-info below points at it and has to outlive
                     the call - the rule xud_swapchain.c states for a create-info that points
                     at the record it was assembled from, one rung up. It is a copy of the
                     handles rather than an array of pointers to them because the colour half
                     is this row's and the depth half is the pass's, and neither row may
                     retire the other's */
    VkImage image; /* BORROWED: the swapchain's own image, owned by XudSwapchain's images
                      array and never destroyed here */
    VkFramebufferCreateInfo createInfo; /* what vkCreateFramebuffer was handed */
    VkImageViewCreateInfo viewInfo; /* what vkCreateImageView was handed */
};

typedef struct XudFramebufferArray XudFramebufferArray;
struct XudFramebufferArray {
    XudFramebuffer** rows; /* the heap array of one framebuffer per swapchain image, in IMAGE
                              INDEX order - the frame loop picks a row by the index vkAcquireN
                              extImageKHR wrote, so the order is load-bearing, not a conveni
                              ence */
    uint32_t count; /* how many rows are live - always the swapchain's imageCount */
    uint32_t capacity; /* rows allocated for it. The count comes from a query that has an upper
                          limit (XUD_SWAPCHAIN_IMAGE_COUNT_MAX), so this is allocated exactly
                          rather than doubled: the doubling shape xud_swapchain.c's image query
                          uses is for a list whose length cannot be read without the array, and
                          here it can */
};

/*-----------------------------------------------------------------------------------------------
 * The layer's contract
 *-----------------------------------------------------------------------------------------------*/

/// Pure: the render pass's own rows, written into the struct above. No driver call, no handle
/// in hand, headless-testable. The attachment row is the swapchain's chosen format with
/// samples VK_SAMPLE_COUNT_1_BIT (which is the pipeline's rasterizationSamples, and the spec
/// requires the two to match), loadOp CLEAR (the pass clears the attachment rather than lo
/// ading whatever the previous frame left in it - the alternative would make a frame's out
/// put depend on the frame before it), storeOp STORE (the whole point: the image is present
/// ed), stencilLoadOp/stencilStoreOp the CORE values LOAD/STORE rather than NONE - both halv
/// es are ignored when there is no stencil attachment, and NONE is an extension-gated enum (
/// VK_KHR_load_store_op_none / VK_EXT_load_store_op_none / VK_QCOM_render_pass_store_ops / VK
/// _KHR_dynamic_rendering, none enabled here; the validation layer on this machine rejects a
/// NONE stencil op as a hard error, probed today), and initialLayout COLOR_ATTACHMENT_OPTIMA
/// L with finalLayout PRESENT_SRC_KHR - the textbook swapchain pattern, the pass transitioni
/// ng the image INTO the layout it is drawn in and BACK OUT to the one the compositor reads.
/// Both halves at PRESENT_SRC_KHR instead is a pass that draws into an image in the present
/// layout, which the spec forbids: VUID-VkAttachmentReference-layout-03077 names PRESENT_SR
/// C_KHR (with UNDEFINED, PREINITIALIZED and ZERO_INITIALIZED_EXT) as a layout a subpass ref
/// erence may never carry, and the validation layer on this machine fires exactly that VUID
/// (probed today). The subpass reference therefore carries initialLayout, not finalLayout. The sub
/// pass row is one colour attachment at index 0, no input attachments, no resolve attach
/// ment (MSAA is a gate this rung does not take, see AGENTS.md's render-pass note), and the
/// depth-stencil attachment the file-top note says now EXISTS (the subpass's pDepthStencilAtt
/// achment is the attachment index of the depth row, not a description: a VkSubpassDescripti
/// on names an attachment by INDEX into the pass's own attachment array, which is why the t
/// wo rows' order is load-bearing). The dependency row is the spec's required external -> 0
/// pair with the colour-attachment-output stage on the source half and the vertex + colour-a
/// ttachment-output stages on the destination half, and the attachment-specific access bits r
/// ather than the generic READ/WRITE pair (these headers have no VK_ACCESS_CLEAR_AND_WRITE_BI
/// T at all in 1.4.363, grepped, so the attachment-specific names are the only ones that can
/// be written) - a pass whose first subpass clears an attachment has to say that the clear h
/// appens before the draw rather than rely on implicit ordering, which is the repo's subpass
/// discipline rule read at the one place it can be read for one subpass. The DEPTH half of t
/// hat pair is the depth-attachment-output stage on the source and the vertex + fragment + d
/// epth-attachment-output stages on the destination: the pass clears the depth buffer to XUD_
/// RENDERPASS_CLEAR_DEPTH and the draw writes it, and a dependency that only names the colour
/// stages says nothing about the depth clear, which is a pass that may draw before its depth
/// buffer holds 1.0. format is the swapchain's chosen format, written into the colour attach
/// ment row here rather than at the create-info step, because the image view the framebuffer
/// below attaches has to name the SAME format and a mismatch is a validation error the drive
/// r is entitled to refuse; the depth row's format is XUD_RENDERPASS_DEPTH_FORMAT, which is
/// this layer's own choice and not the swapchain's. XUD_INVALID_ARG on a NULL pass; XUD_UNSU
/// PPORTED on VK_FORMAT_UNDEFINED (a swapchain that has no chosen format has no attachment d
/// escription either).
XudResult xud_renderpass_fill_state(XudRenderPass* pass, VkFormat format);

/// Pure: the VkRenderPassCreateInfo the pass will be created with, assembled from the rows
/// above. Refuses XUD_UNSUPPORTED when any row of the struct in hand is not stamped - the
/// same rule xud_pipeline_fill_create_info() applies to its own rows. XUD_INVALID_ARG on a
/// NULL out-create-info or a NULL pass.
XudResult xud_renderpass_fill_create_info(const XudRenderPass* pass, VkRenderPassCreateInfo* outCreateInfo);

/// Pure: the VkImageCreateInfo for the depth attachment - a 2D, OPTIMALLY-tiled image of
/// XUD_RENDERPASS_DEPTH_FORMAT at the swapchain's extent, one array layer, one mip, whose only
/// usage bit is VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT (the image is attached, never
/// sampled, never copied and never mapped in this tree, and a usage mask that names more than
/// the one use is a mask that asks the driver for a layout it does not need), whose subresource
/// range is the DEPTH aspect alone (VK_IMAGE_ASPECT_DEPTH_BIT, never the stencil half - the
/// format has one aspect and an aspect mask that names a second is a range that names an
/// aspect the image does not have), and whose initialLayout is UNDEFINED with finalLayout
/// DEPTH_STENCIL_ATTACHMENT_OPTIMAL: the image is created with no layout at all and the
/// frame loop's layout-transition barrier is what gives it one, which is the same rule
/// xud_frame.c states for a swapchain image. XUD_INVALID_ARG on a NULL out-create-info or a
/// zero extent.
XudResult xud_renderpass_fill_depth_info(VkImageCreateInfo* outCreateInfo, uint32_t width, uint32_t height);

/// Pure: the VkImageViewCreateInfo for the depth image - a 2D view of the image handle in hand,
/// in XUD_RENDERPASS_DEPTH_FORMAT, with an identity swizzle and a subresource range of the DEPTH
/// aspect alone, one mip, one layer, starting at 0. The aspect mask is the whole reason this is
/// a second view helper rather than xud_framebuffer_fill_view_info() with a different format:
/// that one names VK_IMAGE_ASPECT_COLOR_BIT, and a view of a depth image whose range names the
/// colour aspect is a view of nothing. XUD_INVALID_ARG on a NULL out-create-info.
XudResult xud_renderpass_fill_depth_view_info(VkImageViewCreateInfo* outCreateInfo, VkImage image);

/// Pure: the VkImageViewCreateInfo for one swapchain image - a 2D view of the image handle
/// in hand, in the swapchain's chosen format, with an identity swizzle (VK_COMPONENT_SWI
/// ZLE_IDENTITY on all four channels, the spec's "leave the components alone" form, writ
/// ten because the field is a value the driver reads rather than an optional one) and a
/// subresource range of one mip / one layer starting at 0 (the swapchain's imageArrayLay
/// ers is 1, xud_swapchain_fill_create_info() writes that). XUD_INVALID_ARG on a NULL ou
/// t-create-info; a handle-less image is refused by the DRIVER half, not here, so the he
/// adless run can exercise the assembly.
XudResult xud_framebuffer_fill_view_info(VkImageViewCreateInfo* outCreateInfo, VkImage image, VkFormat format);

/// Pure: the VkFramebufferCreateInfo for one image. renderPass is the handle in hand (the
/// pass this framebuffer was created FOR - the spec requires the framebuffer's attachments
/// to match the pass's descriptions exactly), attachmentCount XUD_RENDERPASS_ATTACHMENT_COUNT
/// with pAttachments the ONE VIEW LIST THE STRUCT HOLDS (&framebuffer->attachmentViews, not
/// a copy of a handle: the row has to point at storage that outlives the call, and the only
/// storage that does is the framebuffer's own field - the same rule xud_swapchain.c states
/// for a create-info that points at the record it was assembled from, one rung up), width/height
/// the extent the swapchain was created at (co
/// pied, not re-queried: a framebuffer whose extent is not the image's is a framebuffer who
/// se viewport will be wrong, and the surface's currentExtent is the "undefined" answer her
/// e, see xud_swapchain.c), layers 1 (the pass's attachments all have one layer). XUD_INVALI
/// D_ARG on a NULL out-create-info, a NULL framebuffer, a framebuffer whose colour view is not lif
/// e yet, a framebuffer whose depth view row is not filled yet (a row that attaches one attach
/// ment of two is a row the pass will refuse), a handle-less render pass or a zero extent.
XudResult xud_framebuffer_fill_create_info(
    VkFramebufferCreateInfo* outCreateInfo, const XudFramebuffer* framebuffer, VkRenderPass renderPass, uint32_t width, uint32_t height);

/// Driver: the whole rung - vkCreateImage + vkGetImageMemoryRequirements + vkBindImageMemor
/// y (the memory itself comes from the allocator, which is the only owner of GPU memory here)
/// + vkCreateImageView + vkCreateRenderPass, in that order, because the pass's attachment desc
/// ription is what says an image of that format and that extent exists, the image's memory req
/// uirements are what the allocator was asked for, and the framebuffer rows below attach the v
/// iew this creates. Before any call: the loader is open, its device rows are this device's ro
/// ws, and EVERY destroy row this teardown needs (image, view, pass) is checked BEFORE the crea
/// te rows it would otherwise leak (a pass this layer cannot destroy is a pass it must not crea
/// te, and so is an image or a view it cannot retire). Then the rows are filled into the struc
/// t, the create-info assembled from them, the depth image asked about and created, its memory
/// requirements carved out of the allocator's blocks and bound to it, its one view created, and
/// the pass created. On XUD_OK *outPass owns a live pass whose rows say what it was created wit
/// h AND a live depth image in allocator memory; on any error it stays NULL and everything crea
/// ted so far is retired in the one reverse order (xud_renderpass_retire(), the same shape xud
/// _descriptor.c's failure path uses). XUD_INVALID_ARG on a NULL out-pointer or one that alrea
/// dy owns a pass, a NULL loader/device/caps/allocator/swapchain, a handle-less device, a swap
/// chain with no handle or no images, or a zero extent; XUD_UNSUPPORTED when the loader is clo
/// sed, its rows are not this device's, a row is nil, the format is VK_FORMAT_UNDEFINED, or the
///  driver refuses this (format, tiling, usage) tuple at vkCreateImage; XUD_OUT_OF_RESOURCES wh
/// en the allocator cannot serve the image's requirements (reported, not grown past the block c
/// eiling - see xud_allocator.h).
XudResult xud_renderpass_open(XudRenderPass** outPass, XudLoader* loader, const XudDevice* device, const XudDeviceCaps* caps,
    XudAllocator* alloc, const XudSwapchain* swapchain);

/// Driver: vkDestroyRenderPass, then vkDestroyImageView, then vkDestroyImage, then the alloca
/// tor's release of the image's range, then free. That order is the only one that is correct
/// and every step of it is load-bearing: the pass's attachment description names the depth i
/// mage's layout, the framebuffer rows (closed above this rung, by xud_framebuffer_array_clo
/// se()) name the view, and the image's memory belongs to the allocator, so the image's ran
/// ge has to be released while the allocator is still open - the rule xud_descriptor_close()
/// states for its uniform buffer, one rung over. xud_device_wait_idle() guards the whole thi
/// ng first, and that guard is not decoration: the frame loop above may still have a submis
/// sion or a presentation in flight, and the submission is the one that reads this very dept
/// h buffer, so releasing the range under it is freeing memory a command buffer is still re
/// ading - the same boundary xud_swapchain_close(), xud_allocator_close() and xud_descriptor
/// _close() state, and a guard that cannot run (XUD_UNSUPPORTED) is a teardown this rung mus
/// t not start, because freeing the struct would leak every handle the driver still holds, w
/// hile a LOST device is still torn down and still reported, the rule xud_device_close() fol
/// lows. XUD_ALREADY when the pointer owns no pass; XUD_INVALID_ARG on a NULL pointer-to-po
/// inter; XUD_UNSUPPORTED on a NULL loader/device/allocator, a handle-less device, rows tha
/// t are not this device's, or a nil destroy row while the object it would destroy is live.
/// A refused step is reported, not hidden: the first non-OK answer is what the rung returns
/// and the steps after it still run.
XudResult xud_renderpass_close(XudRenderPass** ioPass, const XudDevice* device, XudLoader* loader, XudAllocator* alloc);

/// Driver: the depth attachment at a new extent, and nothing else - the one attachment a swapchain
///   recreate forces, because VUID-VkFramebufferCreateInfo-attachments-02702 requires every attach-
///   ment of one framebuffer to be at the framebuffer's own extent, so a depth buffer of the OLD
///   extent is a framebuffer the driver is entitled to refuse outright. The pass's own handle and
///   its attachment DESCRIPTIONS are untouched: only the image, its one view and the allocator's
///   range they back change. vkGetImageMemoryRequirements + vkCreateImage + vkBindImageMemory +
///   vkCreateImageView for the NEW row, then vkDestroyImageView + vkDestroyImage + the allocator's
///   release for the OLD one, in that order, so a recreate that fails leaves the pass exactly as it
///   was rather than as a pass whose description names an image that was never made; the new row is
///   retired through the one shared teardown order the close above uses, which is why that order is
///   a function of its own. Before any of it: the loader is open, its device rows are this device's,
///   and every destroy row the OLD row needs is checked before any create row the NEW one would
///   otherwise leak - the rule xud_renderpass_open() applies above, one rung down. Then xud_device_
///   wait_idle() guards the whole thing, for the reason the close above gives: the frame loop may
///   still have a submission in flight and that submission is the one reading this very depth buffe
///   r. A guard that cannot run (XUD_UNSUPPORTED) is a recreate this rung must not start. A LOST
///   device is NOT still recreated here, unlike the close's still-torn-down rule: a teardown on a
///   dying device has to run because the handles would otherwise leak, but a recreate on one has
///   nothing to create against, and half-swapping the attachment would leave a broken pass - and
///   nothing has been created at that point, so nothing leaks. The framebuffer rows above (which
///   name the OLD view) and the frame loop above (whose fences name the OLD images) are closed by
///   xud_renderer_resize() BEFORE this call, which is the only reason the old view can die here at
///   all. XUD_ALREADY when the depth image is already at the swapchain's extent (the state asked
///   for is the state present, the rule xud_swapchain_recreate() follows one rung up); XUD_INVALID
///   ARG on a NULL pointer-to-pointer or one that owns no pass, a NULL loader/device/caps/allocato
///   r/swapchain, a handle-less device, a swapchain with no handle or a zero extent, or a pass with
///   no depth attachment at all (xud_renderpass_open() never leaves that state, and the pass's desc
///   ription declares a depth row, so a pass that has none is a broken pass and xud_renderpass_open
///   () is the call for it); XUD_UNSUPPORTED when the loader is closed, its rows are not this devi
///   ce's, or a row this rung needs is nil.
XudResult xud_renderpass_recreate_depth(XudRenderPass** ioPass, XudLoader* loader, const XudDevice* device, const XudDeviceCaps* caps,
    XudAllocator* alloc, const XudSwapchain* swapchain);

/// Driver: one framebuffer for one swapchain image - vkCreateImageView, then vkCreateFrame
/// /// buffer, in that order, because the framebuffer's attachment list is the view the create
/// just made AND the second row of that list is the pass's OWN depth view, which this rung o
/// nly borrows (it is xud_renderpass_close() that retires it, after every row that names it
/// has died). The image is borrowed (it belongs to the swapchain, xud_swapchain.c's note ab
/// out vkDestroyImage and swapchain-owned images) and is never destroyed here. On any error
/// *outFramebuffer stays NULL and a view that was created but whose framebuffer was not is
/// destroyed before the rung returns, because a view nobody attaches is a view that would ot
/// herwise leak. XUD_INVALID_ARG on a NULL out-pointer or one that already owns a framebuff
/// er, a NULL loader/device/pass, a handle-less device, render pass or image, a pass whose d
/// epth view is not live yet (a row cannot attach an attachment that does not exist), or an
/// extent of 0; XUD_UNSUPPORTED when the loader is closed, its rows are not this device's, o
/// r a row this rung needs is nil.
XudResult xud_framebuffer_open(XudFramebuffer** outFramebuffer, XudLoader* loader, const XudDevice* device, const XudRenderPass* pass,
    VkImage image, VkFormat format, uint32_t width, uint32_t height);

/// Driver: vkDestroyFramebuffer, then vkDestroyImageView, then free. That order is the only
/// one that is correct: the framebuffer's attachment list names the view, so the view cann
/// ot die while something still names it. XUD_ALREADY when the pointer owns no fragment b
/// uffer; XUD_INVALID_ARG on a NULL pointer-to-pointer, a NULL loader/device or a handle-l
/// ess device; XUD_UNSUPPORTED when the rows are not this device's or a destroy row is nil
/// while the object it would destroy is live (the struct is not freed, because freeing it
/// would leak the handles silently).
XudResult xud_framebuffer_close(XudFramebuffer** ioFramebuffer, const XudDevice* device, XudLoader* loader);

/// Driver: the array - one framebuffer per swapchain image, in image-index order, read off
/// the swapchain through its own accessor (xud_swapchain_images_get(), which is the rule t
/// he repo records for a rung above the swapchain: a caller above holds an INDEX, never a
/// VkImage, because a recreate replaces the whole array). The extent and the format come f
/// rom the swapchain's own record (createInfo.imageExtent / imageFormat), not from a re-q
/// uery of the surface. On XUD_OK *outArray owns count rows, count == swapchain->imageCou
/// nt; on any error it stays NULL and every framebuffer created so far is closed in rever
/// se order. XUD_INVALID_ARG on a NULL out-pointer or one that already owns an array, a NUL
/// L loader/device/pass/swapchain, a swapchain with no handle or no images, a handle-less
/// device or render pass, or an image count above the swapchain layer's own bound; XUD_UN
/// SUPPORTED when the loader is closed, its rows are not this device's, a row is nil, or t
/// he swapchain has no chosen format.
XudResult xud_framebuffer_array_open(
    XudFramebufferArray** outArray, XudLoader* loader, const XudDevice* device, const XudRenderPass* pass, const XudSwapchain* swapchain);

/// Pure: one row of the array by image index. XUD_INVALID_ARG on a NULL out-row or array, o
/// r on an index at or above count - a stale index into an array that a recreate replaced i
/// s a caught error, not an out-of-range read, which is the same rule xud_swapchain_images_
/// get() states one rung below.
XudResult xud_framebuffer_array_get(const XudFramebufferArray* array, uint32_t index, XudFramebuffer** outFramebuffer);

/// Driver: vkDestroyFramebuffer + vkDestroyImageView for every row, in reverse creation ord
/// er, then the array itself. XUD_ALREADY when the pointer owns no array; XUD_INVALID_ARG o
/// n a NULL pointer-to-pointer, a NULL loader/device; XUD_UNSUPPORTED when the loader is c
/// losed, its rows are not this device's, or a destroy row is nil while an object it would
/// destroy is live (the array is not freed, because freeing it would leak the rows silentl
/// y). A refused row teardown is reported, not hidden - the first non-OK answer is what the
/// rung returns, and the rows after it are still torn down, the rule xud_close_context() s
/// tates for its own rungs.
XudResult xud_framebuffer_array_close(XudFramebufferArray** ioArray, const XudDevice* device, XudLoader* loader);

#endif /* XUD_RENDERPASS_H_ */
