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
 *   The attachment set is ONE row today: the swapchain colour attachment. There is no depth
 *   attachment, and that is not an omission - a VkAttachmentDescription for a depth attach
 *   ment would need a VkImage and a VkDeviceMemory of its own, which is the allocator rung
 *   that IS in the tree now (S12) and holds no image at all - nothing above it has asked it
 *   for memory yet. The depth TEST and depth WRITE are both OFF in the pipeline for exactly
 *   that reason (xud_pipeline.h states it: a depth test against no depth buffer is implementa
 *   tion-dependent rather than a first triangle that draws every time), and it is why the first
 *   triangle has no hidden-surface behaviour. The depth attachment lands with the rung above
 *   the allocator that will ask it for an image, and this file says so rather than pretending
 *   the depth buffer exists.
 ***********************************************************************************************/

#ifndef XUD_RENDERPASS_H_
#define XUD_RENDERPASS_H_

#include "volk.h"
#include "xud_device.h"
#include "xud_loader.h"
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
    1u /* the swapchain colour attachment only - see the                                                                                   \
          note at the top of the file about why there is no                                                                                \
          depth attachment here yet */
#define XUD_RENDERPASS_SUBPASS_COUNT                                                                                                       \
    1u /* one subpass: the plan's subpass discipline is about                                                                              \
          ordering BETWEEN subpasses, and there is one subpass                                                                             \
          today, so the one dependency row is the external ->                                                                              \
          0 pair the spec requires for a pass that reads noth                                                                              \
          ing before it */
#define XUD_RENDERPASS_DEPENDENCY_COUNT 1u
#define XUD_RENDERPASS_COLOR_ATTACHMENT 0u /* which attachment the one subpass writes to */
#define XUD_RENDERPASS_FIRST_SUBPASS 0u /* the subpass the one dependency row targets */

/* The clear colour, in the sRGB non-linear space the swapchain's chosen format is in (the
   format selection rule in xud_swapchain.c only ever picks an SRGB_NONLINEAR row when one
   exists, see that file's note). A dark background rather than black so a run that drew
   nothing is distinguishable from a run that drew a black triangle. */
#define XUD_RENDERPASS_CLEAR_COLOR_R 0.02f
#define XUD_RENDERPASS_CLEAR_COLOR_G 0.02f
#define XUD_RENDERPASS_CLEAR_COLOR_B 0.05f
#define XUD_RENDERPASS_CLEAR_COLOR_A 1.0f

static_assert(XUD_RENDERPASS_ATTACHMENT_COUNT == (XUD_RENDERPASS_COLOR_ATTACHMENT + 1u),
    "the attachment rows are contiguous from the colour attachment up");
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
    VkSubpassDescription subpasses[XUD_RENDERPASS_SUBPASS_COUNT];
    VkSubpassDependency dependencies[XUD_RENDERPASS_DEPENDENCY_COUNT];
    VkClearValue clearValues[XUD_RENDERPASS_ATTACHMENT_COUNT]; /* the clear colour, in attach
                                                                  ment order, so VkRenderPas
                                                                  sBeginInfo's pClearValues
                                                                  index matches the attach
                                                                  ment index - the spec's own
                                                                  rule for that pair */
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
/// ment (MSAA is a gate this rung does not take, see AGENTS.md's render-pass note), and no
/// depth-stencil attachment (see the note at the top of the file). The dependency row is th
/// e spec's required external -> 0 pair with the colour-attachment-output stage on the sou
/// rce half and the vertex + colour-attachment-output stages on the destination half, and
/// the attachment-specific access bits rather than the generic READ/WRITE pair (these head
/// ers have no VK_ACCESS_CLEAR_AND_WRITE_BIT at all in 1.4.363, grepped, so the attachmen
/// t-specific names are the only ones that can be written) - a pass whose first subpass cl
/// ears an attachment has to say that the clear happens before the draw rather than rely o
/// n implicit ordering, which is the repo's subpass discipline rule read at the one place i
/// t can be read for one subpass. format is the swapchain's chosen format, written into th
/// e attachment row here rather than at the create-info step, because the image view the f
/// remebuffer below attaches has to name the SAME format and a mismatch is a validation er
/// ror the driver is entitled to refuse. XUD_INVALID_ARG on a NULL pass; XUD_UNSUPPORTED o
/// n VK_FORMAT_UNDEFINED (a swapchain that has no chosen format has no attachment descript
/// ion either).
XudResult xud_renderpass_fill_state(XudRenderPass* pass, VkFormat format);

/// Pure: the VkRenderPassCreateInfo the pass will be created with, assembled from the rows
/// above. Refuses XUD_UNSUPPORTED when any row of the struct in hand is not stamped - the
/// same rule xud_pipeline_fill_create_info() applies to its own rows. XUD_INVALID_ARG on a
/// NULL out-create-info or a NULL pass.
XudResult xud_renderpass_fill_create_info(const XudRenderPass* pass, VkRenderPassCreateInfo* outCreateInfo);

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
/// to match the pass's descriptions exactly), attachmentCount 1 with pAttachments the ONE
/// VIEW THE STRUCT HOLDS (&framebuffer->view, not a copy of a handle: the row has to point
/// at storage that outlives the call, and the only storage that does is the framebuffer's o
/// wn field - the same rule xud_swapchain.c states for a create-info that points at the str
/// ict's own record, one rung up), width/height the extent the swapchain was created at (co
/// pied, not re-queried: a framebuffer whose extent is not the image's is a framebuffer who
/// se viewport will be wrong, and the surface's currentExtent is the "undefined" answer her
/// e, see xud_swapchain.c), layers 1 (the pass's attachments all have one layer). XUD_INVALI
/// D_ARG on a NULL out-create-info, a NULL framebuffer, a framebuffer whose view is not lif
/// e yet, a handle-less render pass or a zero extent.
XudResult xud_framebuffer_fill_create_info(
    VkFramebufferCreateInfo* outCreateInfo, const XudFramebuffer* framebuffer, VkRenderPass renderPass, uint32_t width, uint32_t height);

/// Driver: vkCreateRenderPass through the device row. Before any call: the loader is open,
/// its device rows are this device's rows, and the destroy row is checked BEFORE the create
/// row (a pass this layer cannot destroy is a pass it must not create). Then the rows are
/// filled into the struct, the create-info is assembled from them, and the pass is created.
/// On XUD_OK *outPass owns a live pass whose rows say what it was created with; on any err
/// or it stays NULL. XUD_INVALID_ARG on a NULL out-pointer or one that already owns a pas
/// s, a NULL loader/device, or a handle-less device; XUD_UNSUPPORTED when the loader is c
/// losed, its rows are not this device's, a row is nil, or the format is VK_FORMAT_UNDEF
/// INED (a swapchain that has no chosen format has no attachment description either).
XudResult xud_renderpass_open(XudRenderPass** outPass, XudLoader* loader, const XudDevice* device, VkFormat format);

/// Driver: vkDestroyRenderPass through the device row, then free. XUD_ALREADY when the poin
/// ter owns no pass; XUD_INVALID_ARG on a NULL pointer-to-pointer, a NULL loader/device or
/// a handle-less device; XUD_UNSUPPORTED when the rows are not this device's or the destro
/// y row is nil while a handle is in hand (the struct is not freed, same rule as every oth
/// er teardown rung here).
XudResult xud_renderpass_close(XudRenderPass** ioPass, const XudDevice* device, XudLoader* loader);

/// Driver: one framebuffer for one swapchain image - vkCreateImageView, then vkCreateFrame
/// buffer, in that order, because the framebuffer's attachment list is the view the create
/// just made. The image is borrowed (it belongs to the swapchain, xud_swapchain.c's note a
/// bout vkDestroyImage and swapchain-owned images) and is never destroyed here. On any err
/// or *outFramebuffer stays NULL and a view that was created but whose framebuffer was no
/// t is destroyed before the rung returns, because a view nobody attaches is a view that w
/// ould otherwise leak. XUD_INVALID_ARG on a NULL out-pointer or one that already owns a f
/// reme... see below, a NULL loader/device, a handle-less device or render pass, a handle-
/// less image, or an extent of 0; XUD_UNSUPPORTED when the loader is closed, its rows are
/// not this device's, or a row this rung needs is nil.
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
