/***********************************************************************************************
 *
 *   xud_renderpass.c - the render-pass + framebuffer layer: one explicit VkRenderPass, and
 *          one VkFramebuffer (with its own VkImageView) per swapchain image
 *
 *   Owns : the render-pass rows and their create-info assembly (pure), the framebuffer's view
 *          and create-info assembly (pure), then vkCreateRenderPass / vkCreateImageView /
 *          vkCreateFramebuffer and the teardown that hands every one of them back.
 *   Layer: above the pipeline, below the frame loop. Nothing in this file calls a Vulkan entry
 *          point directly - every one is reached through a row of XudLoader's Volk device ta
 *          ble, so the archive holds no vk* symbol and the link line has no -lvulkan anywher
 *          e (AGENTS.md).
 *          The attachment set is ONE row today, and the reason is written at the top of the
 *          header: there is no depth attachment because a depth attachment would need a VkIma
 *          ge and a VkDeviceMemory of its own, which is the allocator rung that is not in the
 *          tree. The validation layer's rules for these create-infos (attachment count equal
 *          between the pass and every framebuffer created from it, the framebuffer's extent e
 *          qual to the images', the initial/final layout pair of a swapchain image) are enfor
 *          ced by this file's own assembly rather than by a driver that says no.
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

/* The rows this layer calls through must be pointer-sized, the same rule xud_loader.h checks
   for the table pair as a whole. */
static_assert(sizeof(PFN_vkCreateRenderPass) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkDestroyRenderPass) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkCreateImageView) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkDestroyImageView) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkCreateFramebuffer) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkDestroyFramebuffer) == sizeof(void*), "PFN row must be pointer-sized");

/* The create-info's pAttachments and pSubpasses point at the struct's OWN rows, so the rows
   must exist and be the counts the create-info claims - the same rule xud_device.c applies to
   a queue row pointing at the struct's own priorities array. The member access inside sizeof
   is not evaluated. */
static_assert(sizeof(((XudRenderPass*)0)->attachments) / sizeof(VkAttachmentDescription) == XUD_RENDERPASS_ATTACHMENT_COUNT,
    "the attachment array must be exactly the count the create-info claims");
static_assert(sizeof(((XudRenderPass*)0)->subpasses) / sizeof(VkSubpassDescription) == XUD_RENDERPASS_SUBPASS_COUNT,
    "the subpass array must be exactly the count the create-info claims");
static_assert(sizeof(((XudRenderPass*)0)->colorReferences) / sizeof(VkAttachmentReference) == XUD_RENDERPASS_SUBPASS_COUNT,
    "every subpass needs one colour reference row");
static_assert(sizeof(((XudRenderPass*)0)->dependencies) / sizeof(VkSubpassDependency) == XUD_RENDERPASS_DEPENDENCY_COUNT,
    "the dependency array must be exactly the count the create-info claims");
static_assert(sizeof(((XudRenderPass*)0)->clearValues) / sizeof(VkClearValue) == XUD_RENDERPASS_ATTACHMENT_COUNT,
    "one clear value per attachment, in attachment order - the spec's own rule for the pair");

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

    /* --- the one colour reference: which attachment the one subpass writes to. layout is the la
       yout the attachment is in AT THE SUBPASS, which is the description's initialLayout and not
       its finalLayout - the spec's own rule, and VUID-VkAttachmentReference-layout-03077, which
       names UNDEFINED, PREINITIALIZED, ZERO_INITIALIZED_EXT and PRESENT_SRC_KHR as the four layo
       uts a subpass reference may never carry (probed today: the layer fires exactly this VUID w
       hen the reference says PRESENT_SRC_KHR). --- */
    pass->colorReferences[XUD_RENDERPASS_FIRST_SUBPASS].attachment = XUD_RENDERPASS_COLOR_ATTACHMENT;
    pass->colorReferences[XUD_RENDERPASS_FIRST_SUBPASS].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    /* --- the one subpass: one colour attachment, nothing else. No input attachments (no subpass
       reads a previous one), no resolve attachment (MSAA is a framebuffer*SampleCounts gate this
       rung does not take, see AGENTS.md's render-pass note), no depth-stencil attachment (see the
       note at the top of the header - the allocator rung that would own that image is not in the
       tree), no preserved attachments (nothing is read and left alone). --- */
    pass->subpasses[XUD_RENDERPASS_FIRST_SUBPASS].flags = 0u;
    pass->subpasses[XUD_RENDERPASS_FIRST_SUBPASS].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    pass->subpasses[XUD_RENDERPASS_FIRST_SUBPASS].inputAttachmentCount = 0u;
    pass->subpasses[XUD_RENDERPASS_FIRST_SUBPASS].pInputAttachments = NULL;
    pass->subpasses[XUD_RENDERPASS_FIRST_SUBPASS].colorAttachmentCount = 1u;
    pass->subpasses[XUD_RENDERPASS_FIRST_SUBPASS].pColorAttachments = &pass->colorReferences[XUD_RENDERPASS_FIRST_SUBPASS];
    pass->subpasses[XUD_RENDERPASS_FIRST_SUBPASS].pResolveAttachments = NULL;
    pass->subpasses[XUD_RENDERPASS_FIRST_SUBPASS].pDepthStencilAttachment = NULL;
    pass->subpasses[XUD_RENDERPASS_FIRST_SUBPASS].preserveAttachmentCount = 0u;
    pass->subpasses[XUD_RENDERPASS_FIRST_SUBPASS].pPreserveAttachments = NULL;

    /* --- the one dependency: the spec's required external -> 0 pair. A pass whose first subpass
       clears an attachment has to say that the clear happens before the draw rather than rely on
       implicit ordering, which is the repo's subpass-discipline rule read at the one place it can
       be read for one subpass. The stage masks are the two the pair actually spans: the colour-att
       achment-output stage (the clear, and the draw's own output) and the vertex stage (the draw).
       The access masks are the attachment-specific pair rather than the generic READ/WRITE pair:
       the clear writes the attachment and the draw writes it, and these headers have no VK_ACCESS_
       CLEAR_AND_WRITE_BIT at all (grepped, 1.4.363), so the attachment-specific names are the ones
       that can be written. dependencyFlags 0 is the default "no export to or from an external sub
       pass" answer, which is what a single-subpass pass wants. --- */
    pass->dependencies[0u].srcSubpass = VK_SUBPASS_EXTERNAL;
    pass->dependencies[0u].dstSubpass = XUD_RENDERPASS_FIRST_SUBPASS;
    pass->dependencies[0u].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    pass->dependencies[0u].dstStageMask = (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    pass->dependencies[0u].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    pass->dependencies[0u].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    pass->dependencies[0u].dependencyFlags = 0u;

    /* --- the clear value, in attachment order: VkRenderPassBeginInfo's pClearValues is indexed by
       attachment index, which is the spec's own rule for the pair, so one attachment means one
       clear value and the order below is the order the frame loop hands them in. --- */
    pass->clearValues[XUD_RENDERPASS_COLOR_ATTACHMENT].color.float32[0] = XUD_RENDERPASS_CLEAR_COLOR_R;
    pass->clearValues[XUD_RENDERPASS_COLOR_ATTACHMENT].color.float32[1] = XUD_RENDERPASS_CLEAR_COLOR_G;
    pass->clearValues[XUD_RENDERPASS_COLOR_ATTACHMENT].color.float32[2] = XUD_RENDERPASS_CLEAR_COLOR_B;
    pass->clearValues[XUD_RENDERPASS_COLOR_ATTACHMENT].color.float32[3] = XUD_RENDERPASS_CLEAR_COLOR_A;

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
        || (pass->subpasses[XUD_RENDERPASS_FIRST_SUBPASS].pipelineBindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS)) {
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

    outCreateInfo->sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    outCreateInfo->pNext = NULL;
    outCreateInfo->flags = 0u;
    outCreateInfo->renderPass = renderPass;
    outCreateInfo->attachmentCount = 1u;

    /* The row points at the framebuffer's OWN view field, not at a copy of a handle: the create-
       info is handed to vkCreateFramebuffer AFTER this call returns, and the only storage that is
       still alive then is the struct's. The same rule xud_swapchain.c states for a create-info th
       at points at the record it was assembled from, one rung up - and it is why this function ta
       kes the struct rather than a VkImageView. */
    outCreateInfo->pAttachments = &framebuffer->view;
    outCreateInfo->width = width;
    outCreateInfo->height = height;
    outCreateInfo->layers = 1u;

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The render pass - the driver half
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_renderpass_open(XudRenderPass** outPass, XudLoader* loader, const XudDevice* device, VkFormat format) {
    XudRenderPass* pass = NULL;
    VkRenderPassCreateInfo createInfo;
    VkRenderPass created = VK_NULL_HANDLE;
    VkResult vk = VK_ERROR_UNKNOWN;
    XudResult result = XUD_INVALID_ARG;

    if ((outPass == NULL) || (*outPass != NULL)) {
        return XUD_INVALID_ARG;
    }
    if ((loader == NULL) || (device == NULL)) {
        return (loader == NULL) ? XUD_INVALID_ARG : XUD_UNSUPPORTED;
    }
    if (!loader->open) {
        return XUD_UNSUPPORTED;
    }
    if (device->device == VK_NULL_HANDLE) {
        return XUD_INVALID_ARG;
    }
    if (format == VK_FORMAT_UNDEFINED) {
        return XUD_UNSUPPORTED;
    }

    /* The destroy row before the create row: a pass this layer cannot destroy is a pass it must not
       create, the rule xud_window.c applies to a surface and xud_swapchain.c to a swapchain. */
    if ((loader->deviceTable.vkDestroyRenderPass == NULL) || (loader->deviceTable.vkCreateRenderPass == NULL)) {
        return XUD_UNSUPPORTED;
    }

    /* The rows must be THIS device's rows - a pass created through another device's rows is a pass
       this device cannot destroy. */
    if (!xud_loader_device_bound(loader, device->device)) {
        return XUD_UNSUPPORTED;
    }

    pass = calloc(1u, sizeof(*pass));
    if (pass == NULL) {
        return XUD_OUT_OF_MEMORY;
    }

    result = xud_renderpass_fill_state(pass, format);
    if (result != XUD_OK) {
        free(pass);
        return result;
    }

    result = xud_renderpass_fill_create_info(pass, &createInfo);
    if (result != XUD_OK) {
        free(pass);
        return result;
    }

    vk = loader->deviceTable.vkCreateRenderPass(device->device, &createInfo, NULL, &created);
    if (vk != VK_SUCCESS) {
        free(pass);
        return xud_result_from_vk(vk);
    }

    pass->renderPass = created;
    *outPass = pass;
    return XUD_OK;
}

XudResult xud_renderpass_close(XudRenderPass** ioPass, const XudDevice* device, XudLoader* loader) {
    XudRenderPass* victim = NULL;
    VkRenderPass handle = VK_NULL_HANDLE;

    if ((ioPass == NULL) || (*ioPass == NULL)) {
        return XUD_INVALID_ARG;
    }
    victim = *ioPass;

    if (victim->renderPass == VK_NULL_HANDLE) {
        free(victim);
        *ioPass = NULL;
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
    if (loader->deviceTable.vkDestroyRenderPass == NULL) {
        return XUD_UNSUPPORTED;
    }

    /* vkDestroyRenderPass returns void, so there is no VkResult to map - but the handle has to
       survive the call that destroys it, which is why it is saved first and cleared after. The
       zero-state-before-the-call rule this tree follows for a void teardown is about an OUT-param
       eter the call WRITES (vkDestroyDevice, vkDestroySurfaceKHR, vkFreeMemory); here the handle
       is an IN-parameter, and clearing it before the call would destroy VK_NULL_HANDLE and leave
       the pass itself alive forever - probed today: the validation layer reports exactly that as
       VUID-vkDestroyDevice-device-05137, "VkDevice ... has 1 leaked objects that have not been
       destroyed", and it names this very handle. */
    handle = victim->renderPass;
    loader->deviceTable.vkDestroyRenderPass(device->device, handle, NULL);
    victim->renderPass = VK_NULL_HANDLE;

    free(victim);
    *ioPass = NULL;
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
