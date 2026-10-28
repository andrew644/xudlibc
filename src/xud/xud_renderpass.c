/***********************************************************************************************
 *
 *   xud_renderpass.c - the render-pass + framebuffer layer: one explicit VkRenderPass, and
 *          one VkFramebuffer (with its own VkImageView) per swapchain image
 *
 *   Owns : the render-pass rows and their create-info assembly (pure), the framebuffer's view
 *          and create-info assembly (pure), then vkCreateRenderPass / vkCreateImageView /
 *          vkCreateFramebuffer and the teardown that hands every one of them back, plus
 *          xud_renderpass_recreate_depth(): the depth attachment recreated at a new extent, the
 *          new image / view / carve made before the old row is retired so a failed recreate
 *          leaves the pass exactly as it was rather than as a pass whose description names an
 *          image that was never made. The teardown split is one owner's - xud_renderpass_retire_
 *          depth() (view before image before release, handles cleared before the destroy calls)
 *          is used by the whole-pass teardown and by the recreate's failure paths alike.
 *   Layer: above the pipeline, below the frame loop. Nothing in this file calls a Vulkan entry
 *          point directly - every one is reached through a row of XudLoader's Volk device ta
 *          ble, so the archive holds no vk* symbol and the link line has no -lvulkan anywher
 *          e (AGENTS.md).
 *          The attachment set is TWO rows now: the swapchain colour attachment and a depth atta
 *          chment this layer creates and owns, which is the first thing above the allocator that
 *          needed an image rather than a buffer. The image and its one view belong to this rung;
 *          the memory they sit in belongs to the allocator, which is why the pass's open and clo
 *          se take an XudAllocator* and why vkGetImageMemoryRequirements / vkBindImageMemory are
 *          rows here at all. The validation layer's rules for these create-infos (attachment cou
 *          nt equal between the pass and every framebuffer created from it, the framebuffer's ex
 *          tent equal to the images', every attachment of a framebuffer the same extent, the init
 *          ial/final layout pair of a swapchain image) are enforced by this file's own assembly
 *          rather than by a driver that says no.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud_renderpass.h"
#include "xud.h"
#include "xud_device.h"
#include "xud_loader.h"
#include "xud_swapchain.h"
#include "xud_vulkan.h"

#include <assert.h> /* Required for: static_assert, the row and array-size checks */
#include <stdlib.h> /* Required for: calloc, free */
#include <string.h> /* Required for: memset, the VkMemoryRequirements zeros */

/* The rows this layer calls through must be pointer-sized, the same rule xud_loader.h checks
   for the table pair as a whole. */
static_assert(sizeof(PFN_vkCreateRenderPass) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkDestroyRenderPass) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkCreateImageView) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkDestroyImageView) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkCreateFramebuffer) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkDestroyFramebuffer) == sizeof(void*), "PFN row must be pointer-sized");

/* The rows this layer creates an IMAGE from must be pointer-sized too - the depth attachment
   is the first VkImage in this tree, and the allocator's rows are the ones it carves its
   memory out of (xud_allocator.c's own asserts cover the allocate/free pair; the bind and the
   requirements rows are this layer's to check). */
static_assert(sizeof(PFN_vkCreateImage) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkDestroyImage) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkGetImageMemoryRequirements) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkBindImageMemory) == sizeof(void*), "PFN row must be pointer-sized");

/* The create-info's pAttachments and pSubpasses point at the struct's OWN rows, so the rows
   must exist and be the counts the create-info claims - the same rule xud_device.c applies to
   a queue row pointing at the struct's own priorities array. The member access inside sizeof
   is not evaluated. */
static_assert(sizeof(((XudRenderPass*)0)->attachments) / sizeof(VkAttachmentDescription) == XUD_RENDERPASS_ATTACHMENT_COUNT,
    "the attachment array must be exactly the count the create-info claims");
static_assert(sizeof(((XudRenderPass*)0)->colorReferences) / sizeof(VkAttachmentReference) == XUD_RENDERPASS_SUBPASS_COUNT,
    "every subpass needs one colour reference row");
static_assert(sizeof(((XudRenderPass*)0)->depthReferences) / sizeof(VkAttachmentReference) == XUD_RENDERPASS_SUBPASS_COUNT,
    "every subpass needs one depth reference row - pDepthStencilAttachment is a pointer to one, not a bare index");
static_assert(sizeof(((XudRenderPass*)0)->subpasses) / sizeof(VkSubpassDescription) == XUD_RENDERPASS_SUBPASS_COUNT,
    "the subpass array must be exactly the count the create-info claims");
static_assert(sizeof(((XudRenderPass*)0)->dependencies) / sizeof(VkSubpassDependency) == XUD_RENDERPASS_DEPENDENCY_COUNT,
    "the dependency array must be exactly the count the create-info claims");
static_assert(sizeof(((XudRenderPass*)0)->clearValues) / sizeof(VkClearValue) == XUD_RENDERPASS_ATTACHMENT_COUNT,
    "one clear value per attachment, in attachment order - the spec's own rule for the pair");
static_assert(sizeof(((XudFramebuffer*)0)->attachmentViews) / sizeof(VkImageView) == XUD_RENDERPASS_ATTACHMENT_COUNT,
    "a framebuffer's attachment list must be exactly the pass's attachment count - VUID-VkFramebufferCreateInfo-attachmentCount-00656 says "
    "the framebuffer's attachments must match the pass's descriptions exactly");

/*-----------------------------------------------------------------------------------------------
 * The render pass's rows - pure, no driver call
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_renderpass_fill_state(XudRenderPass* pass, VkFormat format) {
    if (pass == NULL) {
        return XUD_INVALID_ARG;
    }
    if (format == VK_FORMAT_UNDEFINED) {
        return XUD_UNSUPPORTED;
    }

    /* --- the one attachment: the swapchain's own image. format is filled in by the create-info
       step from the swapchain's chosen format (see the note there about why the framebuffer's
       view has to agree with it), so the row's format field is left at its zero state until a
       caller with a swapchain in hand fills it - xud_renderpass_fill_create_info() refuses a
       row that still says nothing. samples is VK_SAMPLE_COUNT_1_BIT, and the spec requires the
       attachment's sample count to equal the pipeline's rasterizationSamples, which is the value
       xud_pipeline_fill_state() writes. loadOp CLEAR is the pass clearing the attachment rather
       than loading whatever the previous frame left in it - the alternative would make a frame's
       output depend on the frame before it. storeOp STORE is the whole point: the image is pres
       ented. stencilLoadOp/stencilStoreOp are the CORE values LOAD/STORE rather than NONE: the sp
       ec ignores both halves when there is no stencil attachment, and NONE is an extension-gated
       enum (VK_KHR_load_store_op_none / VK_EXT_load_store_op_none / VK_QCOM_render_pass_store_ops
       / VK_KHR_dynamic_rendering, none of which is enabled here - probed today: the validation la
       yer rejects a NONE stencil op as a hard error against both stencil-op VUIDs). initialLayout
       is COLOR_ATTACHMENT_OPTIMAL and finalLayout PRESENT_SRC_KHR - the textbook swapchain patte
       rn: the pass transitions the image INTO the layout it is drawn in and BACK OUT to the one t
       he compositor reads, so the image is in the present layout on every frame that does not ren
       der into it and in the attachment layout on the one that does. Leaving both at PRESENT_SRC_
       KHR instead is a pass that draws into an image in the present layout, which the spec forbid
       s outright (probed today: VUID-VkAttachmentReference-layout-03077 fires on the subpass refe
       rence and names PRESENT_SRC_KHR as one of the four layouts a subpass may never use). --- */
    pass->attachments[XUD_RENDERPASS_COLOR_ATTACHMENT].flags = 0u;
    pass->attachments[XUD_RENDERPASS_COLOR_ATTACHMENT].format = format;
    pass->attachments[XUD_RENDERPASS_COLOR_ATTACHMENT].samples = VK_SAMPLE_COUNT_1_BIT;
    pass->attachments[XUD_RENDERPASS_COLOR_ATTACHMENT].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    pass->attachments[XUD_RENDERPASS_COLOR_ATTACHMENT].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    pass->attachments[XUD_RENDERPASS_COLOR_ATTACHMENT].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    pass->attachments[XUD_RENDERPASS_COLOR_ATTACHMENT].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    pass->attachments[XUD_RENDERPASS_COLOR_ATTACHMENT].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    pass->attachments[XUD_RENDERPASS_COLOR_ATTACHMENT].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    /* --- the SECOND attachment: the depth buffer, and the reason the pipeline above can now turn
       depth test and depth write on. format is XUD_RENDERPASS_DEPTH_FORMAT rather than the swap
       chain's, because this image is not a swapchain image - it is an image this layer creates a
       nd owns, which is why its half of the struct has an image, a view, an image create-info an
       d a memory range in it while the colour half has none. samples is the same VK_SAMPLE_COUNT_1
       _BIT as the colour row for the same reason: the spec requires every attachment of a pass to
       carry the same sample count as the pipeline's rasterizationSamples, and xud_pipeline_fill_s
       tate() writes XUD_PIPELINE_SAMPLE_COUNT, which is 1. loadOp CLEAR is the half that makes h
       idden-surface behaviour deterministic at all: without it the depth buffer holds whatever th
       e frame before left in it, and a fragment that should have been rejected is accepted or rej
       ected by chance rather than by depth. storeOp STORE is the whole point of a depth buffer ex
       isting: the draw's depth output has to survive the pass, and the only reason it survives fo
       r is that the NEXT pass reads it - a pass that stored nothing would need no depth attachmen
       t at all. stencilLoadOp/stencilStoreOp are the CORE LOAD/STORE pair for the same reason the
       colour row gives them: the spec ignores both halves when no attachment of the pass has a st
       encil aspect (and none does - XUD_RENDERPASS_DEPTH_FORMAT is one aspect and its view's range
       names VK_IMAGE_ASPECT_DEPTH_BIT alone), and VK_ATTACHMENT_LOAD_OP_NONE / VK_ATTACHMENT_STOR
       E_OP_NONE are extension-gated enum values (VK_KHR_load_store_op_none / VK_EXT_load_store_op
       _none / VK_QCOM_render_pass_store_ops / VK_KHR_dynamic_rendering, none enabled here) that th
       e validation layer refuses as hard errors against both stencil-op VUIDs. initialLayout is D
       EPTH_STENCIL_ATTACHMENT_OPTIMAL and finalLayout the SAME: a depth attachment is never pres
       ented, so unlike the colour row it has no PRESENT_SRC_KHR half, and the pair being equal is
       what makes the frame loop's layout-transition barrier for this image a NO-OP once the firs
       t frame has given it a layout - the first frame's barrier still has to move it out of VK_IMA
       GE_LAYOUT_UNDEFINED, which is the layout xud_renderpass_fill_depth_info() declares the image
       created in. --- */
    pass->attachments[XUD_RENDERPASS_DEPTH_ATTACHMENT].flags = 0u;
    pass->attachments[XUD_RENDERPASS_DEPTH_ATTACHMENT].format = XUD_RENDERPASS_DEPTH_FORMAT;
    pass->attachments[XUD_RENDERPASS_DEPTH_ATTACHMENT].samples = VK_SAMPLE_COUNT_1_BIT;
    pass->attachments[XUD_RENDERPASS_DEPTH_ATTACHMENT].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    pass->attachments[XUD_RENDERPASS_DEPTH_ATTACHMENT].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    pass->attachments[XUD_RENDERPASS_DEPTH_ATTACHMENT].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    pass->attachments[XUD_RENDERPASS_DEPTH_ATTACHMENT].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    pass->attachments[XUD_RENDERPASS_DEPTH_ATTACHMENT].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    pass->attachments[XUD_RENDERPASS_DEPTH_ATTACHMENT].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    /* --- the one colour reference: which attachment the one subpass writes to. layout is the la
       yout the attachment is in AT THE SUBPASS, which is the description's initialLayout and not
       its finalLayout - the spec's own rule, and VUID-VkAttachmentReference-layout-03077, which
       names UNDEFINED, PREINITIALIZED, ZERO_INITIALIZED_EXT and PRESENT_SRC_KHR as the four layo
       uts a subpass reference may never carry (probed today: the layer fires exactly this VUID w
       hen the reference says PRESENT_SRC_KHR). --- */
    pass->colorReferences[XUD_RENDERPASS_FIRST_SUBPASS].attachment = XUD_RENDERPASS_COLOR_ATTACHMENT;
    pass->colorReferences[XUD_RENDERPASS_FIRST_SUBPASS].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    /* --- the one depth reference: which attachment row the depth buffer is in. The reference's
       layout is the attachment's initialLayout for the same reason the colour one's is, and the
       reason a reference exists at all rather than a bare index is that the spec's rule for the
       pair is that the reference's layout must equal the description's initialLayout (VUID-VkSub
       passDescription-pDepthStencilAttachment-01440) - a subpass that names an attachment row wi
       thout saying what layout it expects there is a subpass that will be handed an image in a l
       ayout nothing asked for. --- */
    pass->depthReferences[XUD_RENDERPASS_FIRST_SUBPASS].attachment = XUD_RENDERPASS_DEPTH_ATTACHMENT;
    pass->depthReferences[XUD_RENDERPASS_FIRST_SUBPASS].layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    /* --- the one subpass: one colour attachment AND one depth-stencil attachment. No input att
       achments (no subpass reads a previous one), no resolve attachment (MSAA is a framebuffer*S
       ampleCounts gate this rung does not take, see AGENTS.md's render-pass note), no preserved a
       ttachments (nothing is read and left alone). pDepthStencilAttachment is a POINTER to one Vk
       AttachmentReference, not a bare index, so it points at this struct's own row - the same re
       ason the colour half points at its own row, and the same reason the framebuffer's attachme
       nt LIST below is a member rather than a local: a create-info that points at storage which i
       s gone when the driver reads it is a create-info whose contents are whatever was on the sta
       ck. --- */
    pass->subpasses[XUD_RENDERPASS_FIRST_SUBPASS].flags = 0u;
    pass->subpasses[XUD_RENDERPASS_FIRST_SUBPASS].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    pass->subpasses[XUD_RENDERPASS_FIRST_SUBPASS].inputAttachmentCount = 0u;
    pass->subpasses[XUD_RENDERPASS_FIRST_SUBPASS].pInputAttachments = NULL;
    pass->subpasses[XUD_RENDERPASS_FIRST_SUBPASS].colorAttachmentCount = 1u;
    pass->subpasses[XUD_RENDERPASS_FIRST_SUBPASS].pColorAttachments = &pass->colorReferences[XUD_RENDERPASS_FIRST_SUBPASS];
    pass->subpasses[XUD_RENDERPASS_FIRST_SUBPASS].pResolveAttachments = NULL;
    pass->subpasses[XUD_RENDERPASS_FIRST_SUBPASS].pDepthStencilAttachment = &pass->depthReferences[XUD_RENDERPASS_FIRST_SUBPASS];
    pass->subpasses[XUD_RENDERPASS_FIRST_SUBPASS].preserveAttachmentCount = 0u;
    pass->subpasses[XUD_RENDERPASS_FIRST_SUBPASS].pPreserveAttachments = NULL;

    /* --- the one dependency: the spec's required external -> 0 pair. A pass whose first subpass
       clears an attachment has to say that the clear happens before the draw rather than rely on
       implicit ordering, which is the repo's subpass-discipline rule read at the one place it can
       be read for one subpass. The stage masks are the two the pair actually spans, for BOTH att
       achments now: the colour-attachment-output stage AND the two fragment-test stages on the sou
       rce half (both clears) and the vertex, fragment, colour-attachment-output and fragment-test
       stages on the destination half (the draw's own output). There is NO depth-attachment-output
       stage to name (grepped across vulkan_core.h 1.4.363's VkPipelineStageFlagBits block: the co
       re bits stop at TOP_OF_PIPE, DRAW_INDIRECT, VERTEX_INPUT, VERTEX_SHADER, the tessellation p
       air, GEOMETRY, FRAGMENT_SHADER, EARLY_FRAGMENT_TESTS, LATE_FRAGMENT_TESTS, COLOR_ATTACHMENT_
       OUTPUT, COMPUTE, TRANSFER, BOTTOM_OF_PIPE, HOST, ALL_GRAPHICS, ALL, and the rest are extensi
       on bits this library enables none of) - depth writes happen in the fragment-test stages, so
       the pair that names the depth clear is the fragment-test pair, and VK_PIPELINE_STAGE_DEPTH_A
       TTACHMENT_OUTPUT_BIT does not exist at all. A dependency that names only the colour stages,
       as it did while the pass had one attachment, says nothing about the depth clear: the depth b
       uffer could be cleared after the fragments that test against it ran, which is a pass that re
       jects or accepts by chance rather than by depth. The access masks
       are the attachment-specific pair for each aspect rather than the generic READ/WRITE pair:
       the clear writes the attachment and the draw writes it, and these headers have no VK_ACCESS
       _CLEAR_AND_WRITE_BIT at all (grepped, 1.4.363), so the attachment-specific names are the on
       es that can be written. dependencyFlags 0 is the default "no export to or from an external
       subpass" answer, which is what a single-subpass pass wants. --- */
    pass->dependencies[0u].srcSubpass = VK_SUBPASS_EXTERNAL;
    pass->dependencies[0u].dstSubpass = XUD_RENDERPASS_FIRST_SUBPASS;
    pass->dependencies[0u].srcStageMask = (VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
        | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
    pass->dependencies[0u].dstStageMask
        = (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
            | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
    pass->dependencies[0u].srcAccessMask = (VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    pass->dependencies[0u].dstAccessMask = (VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    pass->dependencies[0u].dependencyFlags = 0u;

    /* --- the clear values, in attachment order: VkRenderPassBeginInfo's pClearValues is indexed
       by attachment index, which is the spec's own rule for the pair, so two attachments mean tw
       o clear values and the order below is the order the frame loop hands them in. The colour h
       alf is the float4; the depth half is the float in the union's depthStencil member, and the
       union is why one VkClearValue can describe a colour attachment and a depth attachment at a
       ll - the spec's own answer to "what does clearing mean for a float aspect". --- */
    pass->clearValues[XUD_RENDERPASS_COLOR_ATTACHMENT].color.float32[0] = XUD_RENDERPASS_CLEAR_COLOR_R;
    pass->clearValues[XUD_RENDERPASS_COLOR_ATTACHMENT].color.float32[1] = XUD_RENDERPASS_CLEAR_COLOR_G;
    pass->clearValues[XUD_RENDERPASS_COLOR_ATTACHMENT].color.float32[2] = XUD_RENDERPASS_CLEAR_COLOR_B;
    pass->clearValues[XUD_RENDERPASS_COLOR_ATTACHMENT].color.float32[3] = XUD_RENDERPASS_CLEAR_COLOR_A;
    pass->clearValues[XUD_RENDERPASS_DEPTH_ATTACHMENT].depthStencil.depth = XUD_RENDERPASS_CLEAR_DEPTH;

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The render-pass create-info - pure, assembled from the rows above
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_renderpass_fill_create_info(const XudRenderPass* pass, VkRenderPassCreateInfo* outCreateInfo) {
    if (outCreateInfo == NULL) {
        return XUD_INVALID_ARG;
    }
    if (pass == NULL) {
        return XUD_INVALID_ARG;
    }

    /* Every row must be stamped: the assembly below is only ever a step from xud_renderpass_fill_
       state(), so a struct whose rows were never filled is a bad state of this layer's own record,
       not a bad argument from a caller. */
    if ((pass->attachments[XUD_RENDERPASS_COLOR_ATTACHMENT].format == VK_FORMAT_UNDEFINED)
        || (pass->attachments[XUD_RENDERPASS_DEPTH_ATTACHMENT].format == VK_FORMAT_UNDEFINED)
        || (pass->subpasses[XUD_RENDERPASS_FIRST_SUBPASS].pipelineBindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS)
        || (pass->subpasses[XUD_RENDERPASS_FIRST_SUBPASS].pDepthStencilAttachment == NULL)) {
        return XUD_UNSUPPORTED;
    }

    outCreateInfo->sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    outCreateInfo->pNext = NULL;
    outCreateInfo->flags = 0u;
    outCreateInfo->attachmentCount = XUD_RENDERPASS_ATTACHMENT_COUNT;
    outCreateInfo->pAttachments = pass->attachments;
    outCreateInfo->subpassCount = XUD_RENDERPASS_SUBPASS_COUNT;
    outCreateInfo->pSubpasses = pass->subpasses;
    outCreateInfo->dependencyCount = XUD_RENDERPASS_DEPENDENCY_COUNT;
    outCreateInfo->pDependencies = pass->dependencies;

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The depth image and its one view - pure
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_renderpass_fill_depth_info(VkImageCreateInfo* outCreateInfo, uint32_t width, uint32_t height) {
    if (outCreateInfo == NULL) {
        return XUD_INVALID_ARG;
    }
    if ((width == 0u) || (height == 0u)) {
        return XUD_INVALID_ARG;
    }

    outCreateInfo->sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    outCreateInfo->pNext = NULL;
    outCreateInfo->flags = 0u;
    outCreateInfo->imageType = VK_IMAGE_TYPE_2D;
    outCreateInfo->format = XUD_RENDERPASS_DEPTH_FORMAT;

    /* The extent is the swapchain's, read rather than re-queried, and it has to be the SAME extent
       as the colour attachment's: VUID-VkFramebufferCreateInfo-attachments-02702 (and its pNext
       sibling) require every attachment of one framebuffer to have the same extent, so a depth bu
       ffer of a different size is a framebuffer the driver is entitled to refuse outright. The sw
       apchain's own currentExtent is the "undefined" answer here (xud_swapchain.c's note), so the
       extent this layer asked the swapchain for is the only extent that is a fact. A 2D image's th
       ird dimension is 1, written rather than left at zero, because VkExtent3D is one struct and a
       depth of 0 is an image of no texels at all. */
    outCreateInfo->extent.width = width;
    outCreateInfo->extent.height = height;
    outCreateInfo->extent.depth = 1u;

    /* One mip level and one sample, and both are facts about this image rather than defaults: the
       pass's depth attachment row says samples = VK_SAMPLE_COUNT_1_BIT, and VUID-VkFramebufferCrea
       teInfo-attachments-02695 requires the framebuffer's attachment's samples to match the pipeli
       ne's rasterizationSamples, which is XUD_PIPELINE_SAMPLE_COUNT, also 1. mipLevels 1 is the f
       orm this tree uses everywhere (the view below names levelCount 1 too), and a mip chain of a
       depth attachment is never read by anything. */
    outCreateInfo->mipLevels = 1u;
    outCreateInfo->samples = VK_SAMPLE_COUNT_1_BIT;

    /* One array layer, matching xud_swapchain_fill_create_info()'s imageArrayLayers 1 and the pass
       's attachment rows, which have no layer dimension of their own - a depth image of two layers
       against a pass whose subpass has layerCount 1 is a pass drawing into half an image. */
    outCreateInfo->arrayLayers = 1u;

    /* OPTIMAL tiling rather than LINEAR: linear is the layout a texture that will be sampled in a
       specific order asks for, and this image is only ever attached. OPTIMAL lets the driver pick
       the swizzled tiled form it prefers for a depth attachment, which is the form the fixed-form
       depth hardware reads. */
    outCreateInfo->tiling = VK_IMAGE_TILING_OPTIMAL;

    /* The one usage bit this image has: it is attached and nothing else. It is never sampled (no
       descriptor in xud_descriptor.c's set-1 layout names a depth image), never copied and never
       mapped, and a usage mask that names more than the one use is a mask that asks the driver for
       a layout it does not need - and vkCreateImage's answer for an unsupported (format, tiling,
       usage) tuple is exactly what this bit is a fact about. */
    outCreateInfo->usage = XUD_RENDERPASS_DEPTH_USAGE;

    /* EXCLUSIVE sharing and no queue-family row: the image belongs to one device and is never tra
       nsferred or copied to another, which is the same pair xud_swapchain_fill_create_info() writ
       es for the swapchain's images. queueFamilyIndexCount 0 with pQueueFamilyIndices NULL is the
       spec's "this image is not shared across families" form, not an unset pair. */
    outCreateInfo->sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    outCreateInfo->queueFamilyIndexCount = 0u;
    outCreateInfo->pQueueFamilyIndices = NULL;

    /* UNDEFINED, not DEPTH_STENCIL_ATTACHMENT_OPTIMAL: the image is created with no contents at a
       ll, and the frame loop's layout-transition barrier is what moves it into the pass's initial
       layout before the first attachment write. Declaring the optimal layout here would be a promi
       se about bytes that do not exist yet, which is the same reason xud_frame.c starts its image
       layouts at UNDEFINED. There is no finalLayout field of VkImageCreateInfo at all in these hea
       ders (grepped: the row ends at initialLayout) - the layout an image ends a render pass in is
       stated by the pass's attachment row, not by the image. */
    outCreateInfo->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    return XUD_OK;
}

XudResult xud_renderpass_fill_depth_view_info(VkImageViewCreateInfo* outCreateInfo, VkImage image) {
    if (outCreateInfo == NULL) {
        return XUD_INVALID_ARG;
    }

    outCreateInfo->sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    outCreateInfo->pNext = NULL;
    outCreateInfo->flags = 0u;
    outCreateInfo->image = image;
    outCreateInfo->viewType = VK_IMAGE_VIEW_TYPE_2D;
    outCreateInfo->format = XUD_RENDERPASS_DEPTH_FORMAT;

    /* The identity swizzle is written for the same reason xud_framebuffer_fill_view_info() writes
       it: the field is a value the driver reads rather than an optional one. */
    outCreateInfo->components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    outCreateInfo->components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    outCreateInfo->components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    outCreateInfo->components.a = VK_COMPONENT_SWIZZLE_IDENTITY;

    /* The DEPTH aspect, and the reason this is a second view helper rather than xud_framebuffer_fi
       ll_view_info() with a different format: that one names VK_IMAGE_ASPECT_COLOR_BIT, and a view
       of a depth image whose range names the colour aspect is a view of nothing - the validation l
       ayer answers it as VUID-VkImageViewCreateInfo-subresourceRange-0959 (the aspect must be one t
       he image's format has) and the driver is entitled to hand back a view the framebuffer will th
       en refuse. One mip, one layer, starting at 0, matching the image create-info above. */
    outCreateInfo->subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    outCreateInfo->subresourceRange.baseMipLevel = 0u;
    outCreateInfo->subresourceRange.levelCount = 1u;
    outCreateInfo->subresourceRange.baseArrayLayer = 0u;
    outCreateInfo->subresourceRange.layerCount = 1u;

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The image view - pure
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_framebuffer_fill_view_info(VkImageViewCreateInfo* outCreateInfo, VkImage image, VkFormat format) {
    if (outCreateInfo == NULL) {
        return XUD_INVALID_ARG;
    }

    outCreateInfo->sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    outCreateInfo->pNext = NULL;
    outCreateInfo->flags = 0u;
    outCreateInfo->image = image;
    outCreateInfo->viewType = VK_IMAGE_VIEW_TYPE_2D;
    outCreateInfo->format = format;

    /* An identity swizzle on all four channels, written rather than left at zero: the field is a
       value the driver reads, not an optional one, and VK_COMPONENT_SWIZZLE_IDENTITY is the spec's
       "leave the components alone" form (it happens to be 0, and writing the name is what says so
       to a reader). */
    outCreateInfo->components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    outCreateInfo->components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    outCreateInfo->components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    outCreateInfo->components.a = VK_COMPONENT_SWIZZLE_IDENTITY;

    /* One mip, one layer, starting at 0: xud_swapchain_fill_create_info() writes imageArrayLayers
       1, and vkGetSwapchainImagesKHR hands back one image per swapchain image, so there is nothing
       else this view could name. */
    outCreateInfo->subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    outCreateInfo->subresourceRange.baseMipLevel = 0u;
    outCreateInfo->subresourceRange.levelCount = 1u;
    outCreateInfo->subresourceRange.baseArrayLayer = 0u;
    outCreateInfo->subresourceRange.layerCount = 1u;

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The framebuffer create-info - pure
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_framebuffer_fill_create_info(
    VkFramebufferCreateInfo* outCreateInfo, const XudFramebuffer* framebuffer, VkRenderPass renderPass, uint32_t width, uint32_t height) {
    if ((outCreateInfo == NULL) || (framebuffer == NULL)) {
        return XUD_INVALID_ARG;
    }
    if (framebuffer->view == VK_NULL_HANDLE) {
        return XUD_INVALID_ARG;
    }
    if ((renderPass == VK_NULL_HANDLE) || (width == 0u) || (height == 0u)) {
        return XUD_INVALID_ARG;
    }
    if (framebuffer->attachmentViews[XUD_RENDERPASS_DEPTH_ATTACHMENT] == VK_NULL_HANDLE) {
        return XUD_INVALID_ARG;
    }

    outCreateInfo->sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    outCreateInfo->pNext = NULL;
    outCreateInfo->flags = 0u;
    outCreateInfo->renderPass = renderPass;
    outCreateInfo->attachmentCount = XUD_RENDERPASS_ATTACHMENT_COUNT;

    /* The row points at the framebuffer's OWN attachment list, not at a copy of a handle: the crea
       te-info is handed to vkCreateFramebuffer AFTER this call returns, and the only storage that i
       s still alive then is the struct's. The same rule xud_swapchain.c states for a create-info th
       at points at the record it was assembled from, one rung up - and it is why this function take
       s the struct rather than a VkImageView. The list is TWO rows now, in the pass's attachment ord
       er: row 0 is this row's own colour view and row 1 is the pass's depth view, which this row on
       ly borrows. attachmentCount is the pass's own constant rather than a literal 1 because VUID-V
       kFramebufferCreateInfo-attachmentCount-00656 requires the framebuffer's attachments to match t
       he pass's descriptions exactly, and a row that attaches one of two is a row the pass above wil
       l refuse. */
    outCreateInfo->pAttachments = framebuffer->attachmentViews;
    outCreateInfo->width = width;
    outCreateInfo->height = height;
    outCreateInfo->layers = 1u;

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The render pass - the driver half
 *-----------------------------------------------------------------------------------------------*/

/* Internal: the depth attachment's own teardown - view before image before release. Shared by
   the pass's whole teardown below AND by xud_renderpass_recreate_depth()'s failure path, which
   is why it is a function of its own: a recreate that created a new image and then failed has to
   retire exactly that half, and the only way to say that once is to have one order rather than a
   second copy of it in the failure path. The order is the exact reverse of the order the objects
   were created in, and every step of it is load-bearing: the VIEW before the IMAGE, because the
   view names the image and there is no way to retire a view while its image is gone; the IMAGE
   before the RELEASE, because an image bound to memory the allocator has just freed is an image
   naming freed memory - the rule xud_descriptor.c's uniform buffer follows one rung over. Every
   destroy handle is an IN-parameter, read into a local and the field cleared BEFORE the call,
   which is the shape xud_renderpass_close() states below: clearing after it would destroy VK_NUL
   L_HANDLE and leave the object alive, which the validation layer reports as VUID-vkDestroyDevic
   e-device-05137 at the following vkDestroyDevice. */
static void xud_renderpass_retire_depth(XudRenderPass* pass, const XudDevice* device, XudLoader* loader, XudAllocator* alloc) {
    if ((pass == NULL) || (loader == NULL) || (device == NULL) || (alloc == NULL)) {
        return;
    }

    if (pass->depthView != VK_NULL_HANDLE) {
        if (loader->deviceTable.vkDestroyImageView != NULL) {
            VkImageView view = pass->depthView;
            pass->depthView = VK_NULL_HANDLE;
            (void)loader->deviceTable.vkDestroyImageView(device->device, view, NULL);
        }
    }

    if (pass->depthImage != VK_NULL_HANDLE) {
        if (loader->deviceTable.vkDestroyImage != NULL) {
            VkImage image = pass->depthImage;
            pass->depthImage = VK_NULL_HANDLE;
            (void)loader->deviceTable.vkDestroyImage(device->device, image, NULL);
        }
    }

    if (pass->depthRange.size != 0u) {
        /* The range goes free in the allocator's own rows, which is what makes a pass that was reti
           red here stop naming a carve that is still live - the same reason xud_descriptor.c releas
           es its uniform buffer's range rather than freeing the memory itself: the allocator owns t
           he VkDeviceMemory, this rung owns the release of the range. */
        (void)xud_allocator_release(alloc, &pass->depthRange);
        pass->depthRange.memory = VK_NULL_HANDLE;
        pass->depthRange.size = 0u;
        pass->depthRange.offset = 0u;
    }
}

/* Internal: the whole pass's teardown - the PASS first, then the depth row through the order
   above. The pass goes first because its attachment description is what names the depth image's
   layout, so a pass that still names one is a pass the driver is still reading that layout from;
   the rest of the order is the depth helper's and is stated there once. Shared by xud_renderpas
   s_open()'s failure paths and by xud_renderpass_close() below, which is why both are functions
   of their own rather than two copies of one order. */
static void xud_renderpass_retire(XudRenderPass* pass, const XudDevice* device, XudLoader* loader, XudAllocator* alloc) {
    if ((pass == NULL) || (loader == NULL) || (device == NULL)) {
        return;
    }

    if (pass->renderPass != VK_NULL_HANDLE) {
        if (loader->deviceTable.vkDestroyRenderPass != NULL) {
            VkRenderPass handle = pass->renderPass;
            pass->renderPass = VK_NULL_HANDLE;
            (void)loader->deviceTable.vkDestroyRenderPass(device->device, handle, NULL);
        }
    }

    xud_renderpass_retire_depth(pass, device, loader, alloc);
}

XudResult xud_renderpass_open(XudRenderPass** outPass, XudLoader* loader, const XudDevice* device, const XudDeviceCaps* caps,
    XudAllocator* alloc, const XudSwapchain* swapchain) {
    XudRenderPass* pass = NULL;
    VkRenderPassCreateInfo createInfo;
    VkMemoryRequirements needs;
    VkRenderPass created = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    XudResult result = XUD_INVALID_ARG;

    if ((outPass == NULL) || (*outPass != NULL)) {
        return XUD_INVALID_ARG;
    }
    if (loader == NULL) {
        return XUD_INVALID_ARG;
    }
    if ((device == NULL) || (caps == NULL) || (alloc == NULL) || (swapchain == NULL)) {
        return XUD_UNSUPPORTED;
    }
    if (!loader->open) {
        return XUD_UNSUPPORTED;
    }
    if (device->device == VK_NULL_HANDLE) {
        return XUD_INVALID_ARG;
    }
    if ((swapchain->swapchain == VK_NULL_HANDLE) || (swapchain->images == NULL) || (swapchain->imageCount == 0u)) {
        return XUD_INVALID_ARG;
    }
    if (swapchain->createInfo.imageFormat == VK_FORMAT_UNDEFINED) {
        return XUD_UNSUPPORTED;
    }
    if ((swapchain->createInfo.imageExtent.width == 0u) || (swapchain->createInfo.imageExtent.height == 0u)) {
        return XUD_INVALID_ARG;
    }

    /* EVERY destroy row this teardown needs, checked before any create row it would otherwise lea
       k - the rule xud_window.c states for a surface and xud_swapchain.c for a swapchain, one rung
       below, now applied to three objects rather than one: an image this layer cannot destroy is an
       image it must not create, and so is a view or a pass. The allocator's own rows (allocate/fre
       e) are in this list because the depth image's memory goes through them, and xud_allocator.c
       checks them again at its own call sites - a half-present table is a refusal rather than a cr
       ash half-way through a bring-up. */
    if ((loader->deviceTable.vkDestroyRenderPass == NULL) || (loader->deviceTable.vkCreateRenderPass == NULL)
        || (loader->deviceTable.vkDestroyImageView == NULL) || (loader->deviceTable.vkCreateImageView == NULL)
        || (loader->deviceTable.vkDestroyImage == NULL) || (loader->deviceTable.vkCreateImage == NULL)
        || (loader->deviceTable.vkGetImageMemoryRequirements == NULL) || (loader->deviceTable.vkBindImageMemory == NULL)
        || (loader->deviceTable.vkAllocateMemory == NULL) || (loader->deviceTable.vkFreeMemory == NULL)) {
        return XUD_UNSUPPORTED;
    }

    /* The rows must be THIS device's rows - a pass or an image created through another device's ro
       ws is one this device cannot destroy. */
    if (!xud_loader_device_bound(loader, device->device)) {
        return XUD_UNSUPPORTED;
    }

    pass = calloc(1u, sizeof(*pass));
    if (pass == NULL) {
        return XUD_OUT_OF_MEMORY;
    }

    /* 1. the pass's rows, then its create-info: the attachment description is what says an image of
       that format and that extent exists, so it is filled before the image is asked for rather tha
       n after it. format is the swapchain's chosen format, read off the swapchain's own record rat
       her than passed in as a scalar - the extent the depth image is created at comes from the sam
       e record, and a create-info and the record of it must not be able to disagree. */
    result = xud_renderpass_fill_state(pass, swapchain->createInfo.imageFormat);
    if (result != XUD_OK) {
        free(pass);
        return result;
    }

    result = xud_renderpass_fill_create_info(pass, &createInfo);
    if (result != XUD_OK) {
        free(pass);
        return result;
    }

    /* 2. the depth image. Its extent is the swapchain's because VUID-VkFramebufferCreateInfo-attach
       ments-02702 requires every attachment of one framebuffer to have the same extent, so a depth
       buffer of a different size is a framebuffer the driver is entitled to refuse outright. */
    result = xud_renderpass_fill_depth_info(
        &pass->depthImageInfo, swapchain->createInfo.imageExtent.width, swapchain->createInfo.imageExtent.height);
    if (result != XUD_OK) {
        free(pass);
        return result;
    }

    result = xud_result_from_vk(loader->deviceTable.vkCreateImage(device->device, &pass->depthImageInfo, NULL, &image));
    if (result != XUD_OK) {
        /* Nothing of this rung is live yet - the image handle never left the out-parameter - so th
           e retire below has nothing to do, and calling it is what keeps one teardown order in the
           file rather than a second copy of it in the failure path. */
        xud_renderpass_retire(pass, device, loader, alloc);
        free(pass);
        return result;
    }
    pass->depthImage = image;

    /* 3. what the image needs before anything asks the allocator for it: vkGetImageMemoryRequirements
       is the question the image asks of the driver, and the allocator's job is the answer, not the
       question. The row returns VOID, so there is no VkResult to map and the discipline is the one
       xud_descriptor.c states for vkGetBufferMemoryRequirements and vkUpdateDescriptorSets - zero t
       he record BEFORE the call, and a size that came back 0 is a refusal rather than a success, be
       cause after the call there is nothing left to check. A zero-size request would be an allocati
       on of nothing, which is the shape the validation layer answers VUID-VkMemoryAllocateInfo-allo
       cationSize-07897 about and the driver accepts silently (see xud_allocator.h's note). */
    memset(&needs, 0, sizeof needs);
    loader->deviceTable.vkGetImageMemoryRequirements(device->device, image, &needs);
    if (needs.size == 0u) {
        xud_renderpass_retire(pass, device, loader, alloc);
        free(pass);
        return XUD_VULKAN_ERROR;
    }

    /* 4. the memory, through the allocator - the only owner of GPU memory in this tree, and the re
       ason this rung does not call vkAllocateMemory of its own. The requirements are the image's an
       d the type is chosen from them against the physical layer's recorded memory rows by the alloc
       ator's own rule (xud_allocator_select_memory_type()), which is what makes the depth buffer's
       memory a fact about the device rather than a guess about this one. A refusal is REPORTED, not
       grown past the block ceiling: XUD_ALLOCATOR_BLOCK_MAX_SIZE is a request this layer cannot ser
       ve, and a quietly-larger block is a block whose later carve sites are unreachable. */
    result = xud_allocator_acquire(&pass->depthRange, alloc, loader, device, caps, &needs);
    if (result != XUD_OK) {
        xud_renderpass_retire(pass, device, loader, alloc);
        free(pass);
        return result;
    }

    /* 5. the image and its memory tied together before anything can read either: an unbound image i
       s an image whose contents are undefined, and a pass whose attachment description names one is
       a pass that would clear undefined memory. vkBindImageMemory takes FOUR arguments - device, im
       age, memory, and the offset inside it (grepped: vulkan_core.h 4559, and there is no size argu
       ment, the size belongs to the allocation the memory row already names) - and it DOES return a
       VkResult, unlike the vkBindBufferMemory row xud_descriptor.c follows, so the answer is mapped a
       fter the call rather than the inputs checked before it. The inputs are still checked live abo
       ve, because a range whose memory row is NULL would be a bind of nothing. */
    if ((pass->depthRange.memory == VK_NULL_HANDLE) || (pass->depthRange.size == 0u)) {
        xud_renderpass_retire(pass, device, loader, alloc);
        free(pass);
        return XUD_INTERNAL_ERROR;
    }

    result = xud_result_from_vk(
        loader->deviceTable.vkBindImageMemory(device->device, image, pass->depthRange.memory, pass->depthRange.offset));
    if (result != XUD_OK) {
        xud_renderpass_retire(pass, device, loader, alloc);
        free(pass);
        return result;
    }

    /* 6. the ONE view every framebuffer row attaches. It is created here rather than per row for t
       he reason stated at the top of the header: the pass declares the attachment and every row na
       mes the same view, so the view's owner is the pass and the rows only borrow it - the opposite
       of the colour half, where each swapchain image has its own view and that view belongs to the
       row that attaches it. */
    result = xud_renderpass_fill_depth_view_info(&pass->depthViewInfo, image);
    if (result != XUD_OK) {
        xud_renderpass_retire(pass, device, loader, alloc);
        free(pass);
        return result;
    }

    result = xud_result_from_vk(loader->deviceTable.vkCreateImageView(device->device, &pass->depthViewInfo, NULL, &view));
    if (result != XUD_OK) {
        xud_renderpass_retire(pass, device, loader, alloc);
        free(pass);
        return result;
    }
    pass->depthView = view;

    /* 7. the pass itself, last, so a pass that failed to create retires everything it was created
       against rather than leaving an image, a view and a carve of allocator memory nobody names. */
    result = xud_result_from_vk(loader->deviceTable.vkCreateRenderPass(device->device, &createInfo, NULL, &created));
    if (result != XUD_OK) {
        xud_renderpass_retire(pass, device, loader, alloc);
        free(pass);
        return result;
    }
    pass->renderPass = created;

    *outPass = pass;
    return XUD_OK;
}

XudResult xud_renderpass_close(XudRenderPass** ioPass, const XudDevice* device, XudLoader* loader, XudAllocator* alloc) {
    XudRenderPass* victim = NULL;
    XudResult guard = XUD_OK;

    if ((ioPass == NULL) || (*ioPass == NULL)) {
        return XUD_INVALID_ARG;
    }
    if ((loader == NULL) || (device == NULL) || (alloc == NULL)) {
        return (loader == NULL) ? XUD_INVALID_ARG : XUD_UNSUPPORTED;
    }
    if (!loader->open) {
        return XUD_UNSUPPORTED;
    }
    if (device->device == VK_NULL_HANDLE) {
        return XUD_UNSUPPORTED;
    }

    victim = *ioPass;

    if ((victim->renderPass == VK_NULL_HANDLE) && (victim->depthImage == VK_NULL_HANDLE) && (victim->depthView == VK_NULL_HANDLE)
        && (victim->depthRange.size == 0u)) {
        free(victim);
        *ioPass = NULL;
        return XUD_ALREADY;
    }

    if (!xud_loader_device_bound(loader, device->device)) {
        return XUD_UNSUPPORTED;
    }

    /* The guard first, and it is not decoration: the frame loop above may still have a submission o
       r a presentation in flight, and the submission is the one that reads this very depth buffer -
       the same boundary xud_swapchain_close(), xud_allocator_close() and xud_descriptor_close() sta
       te. A guard that cannot run (XUD_UNSUPPORTED) is a teardown this rung must not start, because
       freeing the struct would leak every handle the driver still holds; a LOST device is still tor
       n down and still reported, the rule xud_device_close() follows. */
    guard = xud_device_wait_idle(device, loader);
    if (guard == XUD_UNSUPPORTED) {
        return XUD_UNSUPPORTED;
    }
    if (guard != XUD_OK) {
        (void)guard; /* a LOST device is still torn down below and still reported by the rung above,
                       which is the rule xud_device_close()/xud_swapchain_close()/xud_allocator_clos
                       e() follow - the handles the driver still holds have to go even when the devi
                       ce is dying. */
    }

    xud_renderpass_retire(victim, device, loader, alloc);
    free(victim);
    *ioPass = NULL;

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The depth attachment at a new extent - the driver half
 *-----------------------------------------------------------------------------------------------*/

/// See xud_renderpass.h. The depth image is the pass's OWN image and the pass's OWN one view, and
/// VUID-VkFramebufferCreateInfo-attachments-02702 requires every attachment of one framebuffer to
/// be at the framebuffer's own extent, so a new swapchain extent is a depth buffer that has to be
/// created again - the one attachment in this tree that a swapchain recreate forces. The new row
/// is created BEFORE the old one is retired, so a recreate that fails leaves the pass exactly as it
/// was rather than as a pass whose description names an image that was never made.
XudResult xud_renderpass_recreate_depth(XudRenderPass** ioPass, XudLoader* loader, const XudDevice* device, const XudDeviceCaps* caps,
    XudAllocator* alloc, const XudSwapchain* swapchain) {
    XudRenderPass* pass = NULL;
    XudRenderPass oldRow;
    VkImageCreateInfo imageInfo;
    VkImageViewCreateInfo viewInfo;
    VkMemoryRequirements needs;
    VkImage newImage = VK_NULL_HANDLE;
    VkImageView newView = VK_NULL_HANDLE;
    XudResult result = XUD_INVALID_ARG;
    XudResult guard = XUD_OK;

    if ((ioPass == NULL) || (*ioPass == NULL)) {
        return XUD_INVALID_ARG;
    }
    pass = *ioPass;

    if ((loader == NULL) || (device == NULL) || (caps == NULL) || (alloc == NULL) || (swapchain == NULL)) {
        return (loader == NULL) ? XUD_INVALID_ARG : XUD_UNSUPPORTED;
    }
    if (!loader->open) {
        return XUD_UNSUPPORTED;
    }
    if (device->device == VK_NULL_HANDLE) {
        return XUD_UNSUPPORTED;
    }
    if ((swapchain->swapchain == VK_NULL_HANDLE) || (swapchain->createInfo.imageExtent.width == 0u)
        || (swapchain->createInfo.imageExtent.height == 0u)) {
        return XUD_INVALID_ARG;
    }
    if ((pass->renderPass == VK_NULL_HANDLE) || (pass->depthImage == VK_NULL_HANDLE) || (pass->depthView == VK_NULL_HANDLE)
        || (pass->depthRange.size == 0u)) {
        /* The attachment this rung replaces has to exist: xud_renderpass_open() never leaves a pass
           with a description and no image, so a pass that has none is a broken pass rather than a
           pass whose attachment can be swapped, and the rung above that creates one is the call for
           it. */
        return XUD_INVALID_ARG;
    }
    if ((pass->depthImageInfo.extent.width == swapchain->createInfo.imageExtent.width)
        && (pass->depthImageInfo.extent.height == swapchain->createInfo.imageExtent.height)) {
        /* The state asked for is the state present, so there is nothing to recreate - the rule
           xud_swapchain_recreate() follows one rung up, and the reason a resize step that acted on
           a recreate ask can be handed an extent that no longer needs one. */
        return XUD_ALREADY;
    }

    /* Every destroy row this teardown needs, checked before any create row it would otherwise lea
       k - the rule xud_renderpass_open() applies one rung down, now to the pair this rung makes an
       d breaks: an image this layer cannot destroy is an image it must not create, and so is the
       view beside it. The allocator's rows are in the list because the new image's memory goes thr
       ough them. */
    if ((loader->deviceTable.vkDestroyImageView == NULL) || (loader->deviceTable.vkDestroyImage == NULL)
        || (loader->deviceTable.vkCreateImage == NULL) || (loader->deviceTable.vkCreateImageView == NULL)
        || (loader->deviceTable.vkGetImageMemoryRequirements == NULL) || (loader->deviceTable.vkBindImageMemory == NULL)
        || (loader->deviceTable.vkAllocateMemory == NULL) || (loader->deviceTable.vkFreeMemory == NULL)) {
        return XUD_UNSUPPORTED;
    }
    if (!xud_loader_device_bound(loader, device->device)) {
        return XUD_UNSUPPORTED;
    }

    /* The guard first, and it is not decoration: the frame loop above may still have a submission o
       r a presentation in flight, and that submission is the one that reads this very depth buffer -
       the same boundary xud_renderpass_close() states above. A guard that cannot run is a recreate
       this rung must not start. A LOST device is NOT still recreated here, unlike the close's stil
       l-torn-down rule: a teardown on a dying device has to run because the handles would otherwis
       e leak, but a recreate on one has nothing to create against, and half-swapping the attachmen
       t would leave a pass whose description names an image that was never made. Nothing has been
       created at this point, so nothing leaks. */
    guard = xud_device_wait_idle(device, loader);
    if (guard != XUD_OK) {
        return guard;
    }

    /* The old row moved aside, and the pass's own depth row cleared, so the shared teardown order
       below reads ONLY the new half: the new attachment is created first, and the only way to retir
       e the old one through that one order is to have it in a struct that order can read. */
    oldRow = *pass;
    pass->depthView = VK_NULL_HANDLE;
    pass->depthImage = VK_NULL_HANDLE;
    memset(&pass->depthRange, 0, sizeof pass->depthRange);

    /* The new image's create-info, at the swapchain's new extent, through the same fill rung open(
       ) uses below - and the same VUID open() quotes: a depth buffer of the OLD extent is a frame
       buffer the driver is entitled to refuse outright. */
    result = xud_renderpass_fill_depth_info(&imageInfo, swapchain->createInfo.imageExtent.width, swapchain->createInfo.imageExtent.height);
    if (result != XUD_OK) {
        *pass = oldRow;
        return result;
    }
    pass->depthImageInfo = imageInfo;

    result = xud_result_from_vk(loader->deviceTable.vkCreateImage(device->device, &imageInfo, NULL, &newImage));
    if (result != XUD_OK) {
        *pass = oldRow;
        return result;
    }
    pass->depthImage = newImage;

    /* What the new image needs before anything asks the allocator for it, and the same zero-the-re
       cord-before-the-call discipline xud_renderpass_open() states for this row: the row returns VO
       ID, so after the call there is nothing left to check, and a size that came back 0 is a refus
       al rather than an allocation of nothing. */
    memset(&needs, 0, sizeof needs);
    loader->deviceTable.vkGetImageMemoryRequirements(device->device, newImage, &needs);
    if (needs.size == 0u) {
        xud_renderpass_retire_depth(pass, device, loader, alloc);
        *pass = oldRow;
        return XUD_VULKAN_ERROR;
    }

    /* The memory, through the allocator - the only owner of GPU memory in this tree. A refusal is
       REPORTED, not grown past the block ceiling, the rule xud_allocator.h states. */
    result = xud_allocator_acquire(&pass->depthRange, alloc, loader, device, caps, &needs);
    if (result != XUD_OK) {
        xud_renderpass_retire_depth(pass, device, loader, alloc);
        *pass = oldRow;
        return result;
    }

    if ((pass->depthRange.memory == VK_NULL_HANDLE) || (pass->depthRange.size == 0u)) {
        xud_renderpass_retire_depth(pass, device, loader, alloc);
        *pass = oldRow;
        return XUD_INTERNAL_ERROR;
    }

    result = xud_result_from_vk(
        loader->deviceTable.vkBindImageMemory(device->device, newImage, pass->depthRange.memory, pass->depthRange.offset));
    if (result != XUD_OK) {
        xud_renderpass_retire_depth(pass, device, loader, alloc);
        *pass = oldRow;
        return result;
    }

    result = xud_renderpass_fill_depth_view_info(&viewInfo, newImage);
    if (result != XUD_OK) {
        xud_renderpass_retire_depth(pass, device, loader, alloc);
        *pass = oldRow;
        return result;
    }
    pass->depthViewInfo = viewInfo;

    result = xud_result_from_vk(loader->deviceTable.vkCreateImageView(device->device, &viewInfo, NULL, &newView));
    if (result != XUD_OK) {
        xud_renderpass_retire_depth(pass, device, loader, alloc);
        *pass = oldRow;
        return result;
    }
    pass->depthView = newView;

    /* The OLD row retired last, and only once the new one is whole: the framebuffer rows that were
       the only things naming the old view were closed by xud_renderer_resize() before this call ev
       er happened, which is the one reason the old view can die here at all. */
    xud_renderpass_retire_depth(&oldRow, device, loader, alloc);

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * One framebuffer - the driver half
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_framebuffer_open(XudFramebuffer** outFramebuffer, XudLoader* loader, const XudDevice* device, const XudRenderPass* pass,
    VkImage image, VkFormat format, uint32_t width, uint32_t height) {
    XudFramebuffer* framebuffer = NULL;
    VkImageViewCreateInfo viewInfo;
    VkFramebufferCreateInfo createInfo;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer created = VK_NULL_HANDLE;
    VkResult vk = VK_ERROR_UNKNOWN;
    XudResult result = XUD_INVALID_ARG;

    if ((outFramebuffer == NULL) || (*outFramebuffer != NULL)) {
        return XUD_INVALID_ARG;
    }
    if ((loader == NULL) || (device == NULL) || (pass == NULL)) {
        return (loader == NULL) ? XUD_INVALID_ARG : XUD_UNSUPPORTED;
    }
    if (!loader->open) {
        return XUD_UNSUPPORTED;
    }
    if ((device->device == VK_NULL_HANDLE) || (image == VK_NULL_HANDLE) || (pass->renderPass == VK_NULL_HANDLE)) {
        return XUD_INVALID_ARG;
    }
    if (pass->depthView == VK_NULL_HANDLE) {
        /* The pass above owns the one depth view every row attaches, so a pass that has none is a p
           ass this row cannot attach the second half of - and xud_framebuffer_fill_create_info() be
           low refuses a row whose depth slot is empty for the same reason, so the refusal here is t
           he pass's half of the same rule rather than a second copy of it. */
        return XUD_INVALID_ARG;
    }
    if ((width == 0u) || (height == 0u) || (format == VK_FORMAT_UNDEFINED)) {
        return (format == VK_FORMAT_UNDEFINED) ? XUD_UNSUPPORTED : XUD_INVALID_ARG;
    }

    /* Both destroy rows before both create rows: a view or a framebuffer this layer cannot destroy
       is one it must not create. */
    if ((loader->deviceTable.vkDestroyImageView == NULL) || (loader->deviceTable.vkCreateImageView == NULL)
        || (loader->deviceTable.vkDestroyFramebuffer == NULL) || (loader->deviceTable.vkCreateFramebuffer == NULL)) {
        return XUD_UNSUPPORTED;
    }
    if (!xud_loader_device_bound(loader, device->device)) {
        return XUD_UNSUPPORTED;
    }

    framebuffer = calloc(1u, sizeof(*framebuffer));
    if (framebuffer == NULL) {
        return XUD_OUT_OF_MEMORY;
    }

    result = xud_framebuffer_fill_view_info(&viewInfo, image, format);
    if (result != XUD_OK) {
        free(framebuffer);
        return result;
    }

    vk = loader->deviceTable.vkCreateImageView(device->device, &viewInfo, NULL, &view);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK) {
        free(framebuffer);
        return result;
    }
    framebuffer->view = view;
    framebuffer->viewInfo = viewInfo;
    framebuffer->image = image; /* borrowed, never destroyed here - see the header's note */

    /* The attachment list, in the pass's attachment order: row 0 is this row's OWN colour view and
       row 1 is the pass's depth view, which this row only borrows and never destroys. The list is
       filled here rather than at the create-info call because xud_framebuffer_fill_create_info() po
       ints pAttachments at this very array - the create-info is read after the call returns, and th
       e only storage that is still alive then is the struct's. */
    framebuffer->attachmentViews[XUD_RENDERPASS_COLOR_ATTACHMENT] = view;
    framebuffer->attachmentViews[XUD_RENDERPASS_DEPTH_ATTACHMENT] = pass->depthView;

    result = xud_framebuffer_fill_create_info(&createInfo, framebuffer, pass->renderPass, width, height);
    if (result != XUD_OK) {
        (void)loader->deviceTable.vkDestroyImageView(device->device, view, NULL);
        framebuffer->view = VK_NULL_HANDLE;
        free(framebuffer);
        return result;
    }

    vk = loader->deviceTable.vkCreateFramebuffer(device->device, &createInfo, NULL, &created);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK) {
        (void)loader->deviceTable.vkDestroyImageView(device->device, view, NULL);
        framebuffer->view = VK_NULL_HANDLE;
        free(framebuffer);
        return result;
    }

    framebuffer->framebuffer = created;
    framebuffer->createInfo = createInfo;

    *outFramebuffer = framebuffer;
    return XUD_OK;
}

XudResult xud_framebuffer_close(XudFramebuffer** ioFramebuffer, const XudDevice* device, XudLoader* loader) {
    XudFramebuffer* victim = NULL;

    if ((ioFramebuffer == NULL) || (*ioFramebuffer == NULL)) {
        return XUD_INVALID_ARG;
    }
    victim = *ioFramebuffer;

    if ((victim->framebuffer == VK_NULL_HANDLE) && (victim->view == VK_NULL_HANDLE)) {
        free(victim);
        *ioFramebuffer = NULL;
        return XUD_ALREADY;
    }

    if ((loader == NULL) || (device == NULL)) {
        return (loader == NULL) ? XUD_INVALID_ARG : XUD_UNSUPPORTED;
    }
    if ((!loader->open) || (device->device == VK_NULL_HANDLE)) {
        return XUD_UNSUPPORTED;
    }
    if (!xud_loader_device_bound(loader, device->device)) {
        return XUD_UNSUPPORTED;
    }

    /* The framebuffer first, then the view: the framebuffer's attachment list names the view, so the
       view cannot die while something still names it. A nil destroy row while the object is live is a
       refusal, and the struct is not freed, because freeing it would leak the other handle silently. */
    if (victim->framebuffer != VK_NULL_HANDLE) {
        if (loader->deviceTable.vkDestroyFramebuffer == NULL) {
            return XUD_UNSUPPORTED;
        }
        loader->deviceTable.vkDestroyFramebuffer(device->device, victim->framebuffer, NULL);
        victim->framebuffer = VK_NULL_HANDLE;
    }
    if (victim->view != VK_NULL_HANDLE) {
        if (loader->deviceTable.vkDestroyImageView == NULL) {
            return XUD_UNSUPPORTED;
        }
        loader->deviceTable.vkDestroyImageView(device->device, victim->view, NULL);
        victim->view = VK_NULL_HANDLE;
    }

    /* The depth view is NOT destroyed here and cannot be: it belongs to the pass, which destroys it
       once for every row. Clearing the slots is what makes a retired row read as nothing rather tha
       n as a handle the pass above is about to destroy.
     */
    victim->attachmentViews[XUD_RENDERPASS_COLOR_ATTACHMENT] = VK_NULL_HANDLE;
    victim->attachmentViews[XUD_RENDERPASS_DEPTH_ATTACHMENT] = VK_NULL_HANDLE;

    free(victim);
    *ioFramebuffer = NULL;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The array of them - the driver half
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_framebuffer_array_open(
    XudFramebufferArray** outArray, XudLoader* loader, const XudDevice* device, const XudRenderPass* pass, const XudSwapchain* swapchain) {
    XudFramebufferArray* array = NULL;
    XudFramebuffer** rows = NULL;
    XudResult result = XUD_INVALID_ARG;
    uint32_t index = 0u;
    uint32_t created = 0u;

    if ((outArray == NULL) || (*outArray != NULL)) {
        return XUD_INVALID_ARG;
    }
    if ((loader == NULL) || (device == NULL) || (pass == NULL) || (swapchain == NULL)) {
        return (loader == NULL) ? XUD_INVALID_ARG : XUD_UNSUPPORTED;
    }
    if (!loader->open) {
        return XUD_UNSUPPORTED;
    }
    if ((device->device == VK_NULL_HANDLE) || (pass->renderPass == VK_NULL_HANDLE)) {
        return XUD_INVALID_ARG;
    }
    if ((swapchain->swapchain == VK_NULL_HANDLE) || (swapchain->imageCount == 0u) || (swapchain->images == NULL)) {
        return XUD_INVALID_ARG;
    }
    if (swapchain->createInfo.imageFormat == VK_FORMAT_UNDEFINED) {
        return XUD_UNSUPPORTED;
    }

    array = calloc(1u, sizeof(*array));
    if (array == NULL) {
        return XUD_OUT_OF_MEMORY;
    }

    /* The count is the swapchain's own image count, read rather than re-queried - and the array is
       allocated exactly, not doubled: the doubling shape xud_swapchain.c's image query uses is for a
       list whose length cannot be read without the array, and here it can (imageCount is the count,
       bounded above by the surface's maxImageCount, see xud_swapchain_choose_image_count()). */
    rows = calloc(swapchain->imageCount, sizeof(*rows));
    if (rows == NULL) {
        free(array);
        return XUD_OUT_OF_MEMORY;
    }
    array->rows = rows;
    array->capacity = swapchain->imageCount;

    for (index = 0u; index < swapchain->imageCount; index++) {
        XudFramebuffer* row = NULL;
        VkImage image = VK_NULL_HANDLE;

        /* The image comes through the swapchain layer's own accessor, which is the rule the repo
           records for a rung above the swapchain: a caller above holds an INDEX, never a VkImage,
           because a recreate replaces the whole array. */
        result = xud_swapchain_images_get(swapchain, index, &image);
        if (result != XUD_OK) {
            break;
        }

        result = xud_framebuffer_open(&row, loader, device, pass, image, swapchain->createInfo.imageFormat,
            swapchain->createInfo.imageExtent.width, swapchain->createInfo.imageExtent.height);
        if (result != XUD_OK) {
            break;
        }
        array->rows[created] = row;
        created++;
    }

    if (result != XUD_OK) {
        uint32_t unwind = 0u;
        for (unwind = created; unwind > 0u; unwind--) {
            (void)xud_framebuffer_close(&array->rows[unwind - 1u], device, loader);
        }
        free(array->rows);
        free(array);
        return result;
    }

    array->count = created;
    *outArray = array;
    return XUD_OK;
}

XudResult xud_framebuffer_array_get(const XudFramebufferArray* array, uint32_t index, XudFramebuffer** outFramebuffer) {
    if ((array == NULL) || (outFramebuffer == NULL)) {
        return XUD_INVALID_ARG;
    }
    if (index >= array->count) {
        return XUD_INVALID_ARG;
    }

    *outFramebuffer = array->rows[index];
    return XUD_OK;
}

XudResult xud_framebuffer_array_close(XudFramebufferArray** ioArray, const XudDevice* device, XudLoader* loader) {
    XudFramebufferArray* victim = NULL;
    XudResult first = XUD_OK;
    uint32_t index = 0u;
    XudResult closed = XUD_INVALID_ARG;

    if ((ioArray == NULL) || (*ioArray == NULL)) {
        return XUD_INVALID_ARG;
    }
    victim = *ioArray;

    if ((loader == NULL) || (device == NULL)) {
        return (loader == NULL) ? XUD_INVALID_ARG : XUD_UNSUPPORTED;
    }
    if ((!loader->open) || (device->device == VK_NULL_HANDLE)) {
        return XUD_UNSUPPORTED;
    }
    if (!xud_loader_device_bound(loader, device->device)) {
        return XUD_UNSUPPORTED;
    }

    /* Reverse creation order, and every row is still attempted after one refuses: a refused row is
       reported, not hidden, and skipping the rows below it would leak them - the rule xud_close_conte
       xt() states for its own rungs. */
    for (index = victim->count; index > 0u; index--) {
        if (victim->rows[index - 1u] == NULL) {
            continue;
        }
        closed = xud_framebuffer_close(&victim->rows[index - 1u], device, loader);
        if ((closed != XUD_OK) && (closed != XUD_ALREADY) && (first == XUD_OK)) {
            first = closed;
        }
    }

    free(victim->rows);
    victim->rows = NULL;
    victim->count = 0u;
    victim->capacity = 0u;
    free(victim);
    *ioArray = NULL;

    return first;
}
