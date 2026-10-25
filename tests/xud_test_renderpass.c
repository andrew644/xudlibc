/***********************************************************************************************
 *
 *   xud_test_renderpass.c - the render-pass and framebuffer-array rung
 *
 *   Owns : the one-subpass pass with its one colour attachment row, its layout pair and its clear-value count, and the framebuffer +
 * image-view create-info assembly per swapchain image. Layer: tests. Sees xud_renderpass.h and xud_pipeline.h: a framebuffer is created FOR
 * a pass, so the pass's attachment descriptions and the framebuffer's view row are checked as one area, and the pass's sample count is the
 * pipeline layer's constant the attachment row is asked to match.
 *
 *   Deterministic: fixed loop counts, no time, no randomness, no filesystem.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud.h" /* Required for: XudResult, XUD_VERSION, the XUD_GPU_TESTS gate */
#include "xud_pipeline.h" /* Required for: XUD_PIPELINE_SAMPLE_COUNT, the sample count the one attachment row of the pass is asked to match */
#include "xud_renderpass.h" /* Required for: XudRenderPass, XudFramebuffer, the fill rungs and the state assembly */
#include "xud_test_harness.h" /* Required for: xud_check, xud_gpu_tests_enabled, the area entry points */

/*-----------------------------------------------------------------------------------------------
 * The render pass and the framebuffer array, headless: the one attachment's description, the one
 * subpass that reads it, the one dependency that orders the clear before the write, and the per-
 * image view + framebuffer assembly. The dependency's access masks are attachment-specific name
 * s because these headers carry no VK_ACCESS_CLEAR_AND_WRITE_BIT at all (probed today: zero hits
 * for CLEAR_AND_WRITE across the whole Vulkan include tree), which is the same reason the pipeli
 * ne layer's note gives for the depth pair. The two handles a framebuffer create-info needs are
 * synthetic on purpose: the pure half refuses a handle-less one and dereferences nothing, which i
 * s what makes the refusal testable at all.
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_renderpass_pure(void) {
    XudRenderPass pass = (XudRenderPass) { 0 };
    XudRenderPass bare = (XudRenderPass) { 0 };
    XudFramebuffer framebuffer = (XudFramebuffer) { 0 };
    VkRenderPassCreateInfo passInfo = (VkRenderPassCreateInfo) { .sType = VK_STRUCTURE_TYPE_MAX_ENUM };
    VkImageViewCreateInfo viewInfo = (VkImageViewCreateInfo) { .sType = VK_STRUCTURE_TYPE_MAX_ENUM };
    VkFramebufferCreateInfo fbInfo = (VkFramebufferCreateInfo) { .sType = VK_STRUCTURE_TYPE_MAX_ENUM };
    const VkFormat chosen = VK_FORMAT_B8G8R8A8_UNORM;

    xud_check(xud_renderpass_fill_state(NULL, chosen) == XUD_INVALID_ARG, "renderpass/state-null-out", "refused");
    xud_check(xud_renderpass_fill_state(&pass, VK_FORMAT_UNDEFINED) == XUD_UNSUPPORTED, "renderpass/state-no-format",
        "an attachment description with no format is not a description, so it is reported as suc"
        "h rather than written");
    xud_check(xud_renderpass_fill_state(&pass, chosen) == XUD_OK, "renderpass/state-ok", "the rows are filled");

    xud_check(pass.attachments[XUD_RENDERPASS_COLOR_ATTACHMENT].format == chosen, "renderpass/attachment-format",
        "the attachment is the swapchain's CHOSEN format, which is the half of the agreement the s"
        "pec requires between a pass and the framebuffer under it");
    xud_check(pass.attachments[XUD_RENDERPASS_COLOR_ATTACHMENT].samples == XUD_PIPELINE_SAMPLE_COUNT, "renderpass/attachment-samples",
        "one sample, and it is the SAME constant the pipeline's rasterization row carries, which i"
        "s the other half of that agreement - one name, two layers, no way to disagree");
    xud_check(pass.attachments[XUD_RENDERPASS_COLOR_ATTACHMENT].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR, "renderpass/load-clear",
        "the pass clears the attachment rather than loading whatever the previous frame left in it"
        ", which is what makes a frame's output not depend on the frame before it");
    xud_check(pass.attachments[XUD_RENDERPASS_COLOR_ATTACHMENT].storeOp == VK_ATTACHMENT_STORE_OP_STORE, "renderpass/store-store",
        "STORE is the whole point: the image is presented");
    xud_check((pass.attachments[XUD_RENDERPASS_COLOR_ATTACHMENT].stencilLoadOp == VK_ATTACHMENT_LOAD_OP_LOAD)
            && (pass.attachments[XUD_RENDERPASS_COLOR_ATTACHMENT].stencilStoreOp == VK_ATTACHMENT_STORE_OP_STORE),
        "renderpass/stencil-core-ops",
        "the stencil pair is the CORE LOAD/STORE pair, because NONE is an extension-gated enum (l"
        "oad_store_op_none / dynamic_rendering / render_pass_store_ops, none enabled here) and th"
        "e validation layer on this machine rejects it as a hard error, probed today; both halves"
        " are ignored when there is no stencil attachment");
    xud_check((pass.attachments[XUD_RENDERPASS_COLOR_ATTACHMENT].initialLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
            && (pass.attachments[XUD_RENDERPASS_COLOR_ATTACHMENT].finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),
        "renderpass/attachment-layouts",
        "COLOR_ATTACHMENT_OPTIMAL in, PRESENT_SRC_KHR out: the pass transitions the swapchain imag"
        "e into the layout it is drawn in and back out to the one the compositor reads, which is "
        "the textbook swapchain pattern and the only pair that lets the same image be both drawn "
        "into and presented");
    xud_check(pass.attachments[XUD_RENDERPASS_COLOR_ATTACHMENT].flags == 0u, "renderpass/attachment-flags-clear",
        "no attachment flag is asked for");
    xud_check(pass.colorReferences[XUD_RENDERPASS_FIRST_SUBPASS].attachment == XUD_RENDERPASS_COLOR_ATTACHMENT,
        "renderpass/color-reference-index",
        "the subpass's colour reference names attachment 0, which is the index rule VkRenderPassB"
        "eginInfo's pClearValues array is read by");
    xud_check(pass.colorReferences[XUD_RENDERPASS_FIRST_SUBPASS].layout == pass.attachments[XUD_RENDERPASS_COLOR_ATTACHMENT].initialLayout,
        "renderpass/color-reference-layout",
        "the reference names the layout the attachment is in AT THE SUBPASS, which is the descript"
        "ion's initialLayout and not its finalLayout - VUID-VkAttachmentReference-layout-03077 na"
        "mes PRESENT_SRC_KHR (with UNDEFINED, PREINITIALIZED and ZERO_INITIALIZED_EXT) as layouts "
        "a reference may never carry, and the validation layer on this machine fires that VUID a"
        "gainst a PRESENT_SRC_KHR reference, probed today");
    xud_check(pass.subpasses[XUD_RENDERPASS_FIRST_SUBPASS].pipelineBindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS,
        "renderpass/graphics-bind-point",
        "a graphics subpass, which is what makes the pipeline this rung is created under a graphi"
        "cs pipeline and not a compute one");
    xud_check(pass.subpasses[XUD_RENDERPASS_FIRST_SUBPASS].colorAttachmentCount == 1u, "renderpass/subpass-one-colour",
        "one colour attachment for the subpass, and the pass has one attachment in total, so the "
        "two counts agree exactly");
    xud_check(pass.subpasses[XUD_RENDERPASS_FIRST_SUBPASS].pColorAttachments == pass.colorReferences, "renderpass/subpass-colour-row",
        "the row points at the pass's own colour reference, which is the only place in the struct "
        "that can hold it");
    xud_check((pass.subpasses[XUD_RENDERPASS_FIRST_SUBPASS].pInputAttachments == NULL)
            && (pass.subpasses[XUD_RENDERPASS_FIRST_SUBPASS].inputAttachmentCount == 0u),
        "renderpass/no-input-attachments",
        "no input attachments: subpass discipline is about reading what an earlier subpass wrote, "
        "and this rung has one subpass");
    xud_check(pass.subpasses[XUD_RENDERPASS_FIRST_SUBPASS].pResolveAttachments == NULL, "renderpass/no-resolve-attachment",
        "no resolve attachment, which is the MSAA gate AGENTS.md's render-pass note says this dr"
        "iver could open (framebuffer*SampleCounts answer 0x1f) and this rung does not take");
    xud_check(pass.subpasses[XUD_RENDERPASS_FIRST_SUBPASS].pDepthStencilAttachment == NULL, "renderpass/no-depth-attachment",
        "no depth-stencil attachment, which is the half of the depth note that makes the pipeline'"
        "s depth pair an OFF pair - the VkImage + VkDeviceMemory a depth buffer needs belongs to t"
        "he allocator rung, which is in the tree now but holds no image in it yet");
    xud_check(pass.subpasses[XUD_RENDERPASS_FIRST_SUBPASS].preserveAttachmentCount == 0u, "renderpass/no-preserve-attachments",
        "nothing is preserved: every attachment of this pass is written by it");
    xud_check((pass.dependencies[XUD_RENDERPASS_FIRST_SUBPASS].srcSubpass == VK_SUBPASS_EXTERNAL)
            && (pass.dependencies[XUD_RENDERPASS_FIRST_SUBPASS].dstSubpass == XUD_RENDERPASS_FIRST_SUBPASS),
        "renderpass/dependency-external-to-zero",
        "the spec's required external -> 0 pair, which is what makes the clear of the attachment "
        "happen before the draw writes it");
    xud_check(pass.dependencies[XUD_RENDERPASS_FIRST_SUBPASS].srcStageMask == VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        "renderpass/dependency-source-stage",
        "the colour-attachment-output stage on the source half, which is the stage the CLEAR belon"
        "gs to");
    xud_check((pass.dependencies[XUD_RENDERPASS_FIRST_SUBPASS].dstStageMask & VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT) != 0u,
        "renderpass/dependency-destination-stage",
        "the vertex and the colour-attachment-output stages on the destination half, which is the "
        "pair that orders the clear before both the vertex shader and the write");
    xud_check((pass.dependencies[XUD_RENDERPASS_FIRST_SUBPASS].srcAccessMask == VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)
            && (pass.dependencies[XUD_RENDERPASS_FIRST_SUBPASS].dstAccessMask == VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT),
        "renderpass/dependency-access-masks",
        "the attachment-specific pair, because these headers carry no VK_ACCESS_CLEAR_AND_WRITE_BI"
        "T at all (probed today: zero hits for CLEAR_AND_WRITE across the whole include tree), whi"
        "ch is the same reason the pipeline layer's note gives for the depth pair");
    xud_check(pass.dependencies[XUD_RENDERPASS_FIRST_SUBPASS].dependencyFlags == 0u, "renderpass/dependency-flags-clear",
        "no flag: the dependency is a full execution-and-memory barrier, which is the conservative "
        "half of the repo's classic-synchronization rule");
    xud_check((pass.clearValues[XUD_RENDERPASS_COLOR_ATTACHMENT].color.float32[0] == XUD_RENDERPASS_CLEAR_COLOR_R)
            && (pass.clearValues[XUD_RENDERPASS_COLOR_ATTACHMENT].color.float32[3] == XUD_RENDERPASS_CLEAR_COLOR_A),
        "renderpass/clear-colour",
        "the clear colour, in attachment order, so VkRenderPassBeginInfo's pClearValues index matc"
        "hes the attachment index - the spec's own rule for that pair");
    xud_check(pass.clearValues[XUD_RENDERPASS_COLOR_ATTACHMENT].color.float32[3] == 1.0f, "renderpass/clear-alpha-opaque",
        "alpha 1, which is what the surface's OPAQUE composite answer needs: a clear with 0 alpha "
        "would be a window the compositor could see through");

    xud_check(xud_renderpass_fill_create_info(NULL, &passInfo) == XUD_INVALID_ARG, "renderpass/create-no-record", "refused");
    xud_check(
        xud_renderpass_fill_create_info(&pass, NULL) == XUD_INVALID_ARG, "renderpass/create-null-out", "and so is a NULL create-info");
    xud_check(xud_renderpass_fill_create_info(&bare, &passInfo) == XUD_UNSUPPORTED, "renderpass/create-unstamped-rows",
        "a record whose rows were never filled is a bad state of this layer, reported as such rat"
        "her than assembled from calloc's zeros");
    xud_check(xud_renderpass_fill_create_info(&pass, &passInfo) == XUD_OK, "renderpass/create-ok",
        "the create-info is assembled from the record");
    xud_check(passInfo.sType == VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, "renderpass/create-stamped", "stamped");
    xud_check((passInfo.pNext == NULL) && (passInfo.flags == 0u), "renderpass/create-chain-clear", "nothing is chained into it");
    xud_check((passInfo.attachmentCount == XUD_RENDERPASS_ATTACHMENT_COUNT) && (passInfo.pAttachments == pass.attachments),
        "renderpass/create-attachment-row",
        "the attachment row points into the pass's own storage, so the record and the create-info "
        "cannot disagree");
    xud_check((passInfo.subpassCount == XUD_RENDERPASS_SUBPASS_COUNT) && (passInfo.pSubpasses == pass.subpasses),
        "renderpass/create-subpass-row", "and so is the subpass row");
    xud_check((passInfo.dependencyCount == XUD_RENDERPASS_DEPENDENCY_COUNT) && (passInfo.pDependencies == pass.dependencies),
        "renderpass/create-dependency-row", "and so is the dependency row");

    xud_check(xud_framebuffer_fill_view_info(NULL, (VkImage)0x1000u, chosen) == XUD_INVALID_ARG, "framebuffer/view-null-out", "refused");
    xud_check(
        xud_framebuffer_fill_view_info(&viewInfo, (VkImage)0x1000u, chosen) == XUD_OK, "framebuffer/view-ok", "the view is assembled");
    xud_check(viewInfo.sType == VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, "framebuffer/view-stamped", "stamped");
    xud_check((viewInfo.pNext == NULL) && (viewInfo.flags == 0u), "framebuffer/view-chain-clear", "chain empty, no flag");
    xud_check(viewInfo.image == (VkImage)0x1000u, "framebuffer/view-image",
        "the image handle in hand is recorded as the argument it was, which is the swapchain's own "
        "image - borrowed, this layer never destroys it");
    xud_check(
        viewInfo.viewType == VK_IMAGE_VIEW_TYPE_2D, "framebuffer/view-type-2d", "a 2D view, the only view kind a swapchain image has");
    xud_check(viewInfo.format == chosen, "framebuffer/view-format",
        "the view names the SAME format the pass's attachment description gives, which is the half "
        "of the agreement the spec requires between a pass and the framebuffer under it");
    xud_check((viewInfo.components.r == VK_COMPONENT_SWIZZLE_IDENTITY) && (viewInfo.components.g == VK_COMPONENT_SWIZZLE_IDENTITY)
            && (viewInfo.components.b == VK_COMPONENT_SWIZZLE_IDENTITY) && (viewInfo.components.a == VK_COMPONENT_SWIZZLE_IDENTITY),
        "framebuffer/view-swizzle-identity",
        "an identity swizzle written on all four channels rather than left at zero, because the fi"
        "eld is a value the driver reads and not an optional one");
    xud_check(viewInfo.subresourceRange.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT, "framebuffer/view-range-aspect",
        "the colour aspect, and it lives only in the subresource range - VkImageViewCreateInfo has "
        "no aspectMask of its own, which is the member-path trap of this create-info");
    xud_check((viewInfo.subresourceRange.baseMipLevel == 0u) && (viewInfo.subresourceRange.levelCount == 1u), "framebuffer/view-one-mip",
        "one mip from the first, which is all a swapchain image has");
    xud_check((viewInfo.subresourceRange.baseArrayLayer == 0u) && (viewInfo.subresourceRange.layerCount == 1u),
        "framebuffer/view-one-layer", "one layer, the surface's own maxImageArrayLayers answer");

    pass.renderPass = (VkRenderPass)0x1000u;
    framebuffer.view = (VkImageView)0x2000u;

    xud_check(xud_framebuffer_fill_create_info(NULL, &framebuffer, pass.renderPass, 320u, 240u) == XUD_INVALID_ARG,
        "framebuffer/create-null-out", "refused");
    xud_check(xud_framebuffer_fill_create_info(&fbInfo, NULL, pass.renderPass, 320u, 240u) == XUD_INVALID_ARG,
        "framebuffer/create-no-framebuffer",
        "a create-info with no framebuffer to point at is refused, because the attachment row has "
        "to point at that struct's own view - the create-info is read after this call returns, so a "
        "copy of a handle would already be dead by then, which is why this function takes the struct "
        "at all");
    xud_check(xud_framebuffer_fill_create_info(&fbInfo, &framebuffer, VK_NULL_HANDLE, 320u, 240u) == XUD_INVALID_ARG,
        "framebuffer/create-no-pass",
        "a framebuffer that names no pass is refused, because the attachment count it writes is th"
        "e pass's");
    xud_check(xud_framebuffer_fill_create_info(&fbInfo, &framebuffer, pass.renderPass, 0u, 240u) == XUD_INVALID_ARG,
        "framebuffer/create-zero-extent", "and so is a zero extent");
    framebuffer.view = VK_NULL_HANDLE;
    xud_check(xud_framebuffer_fill_create_info(&fbInfo, &framebuffer, pass.renderPass, 320u, 240u) == XUD_INVALID_ARG,
        "framebuffer/create-no-view", "and so is a framebuffer whose view is not live yet - it has nothing to attach");
    framebuffer.view = (VkImageView)0x2000u;
    xud_check(xud_framebuffer_fill_create_info(&fbInfo, &framebuffer, pass.renderPass, 320u, 240u) == XUD_OK, "framebuffer/create-ok",
        "the create-info is assembled");
    xud_check(fbInfo.sType == VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, "framebuffer/create-stamped",
        "stamped, and the stamp is a different constant from the view's, so the two create-infos ca"
        "nnot be confused");
    xud_check(fbInfo.pNext == NULL, "framebuffer/create-pnext-clear", "and its chain stays empty");
    xud_check(fbInfo.renderPass == pass.renderPass, "framebuffer/create-pass-handle",
        "the pass this framebuffer was created FOR is handed over as the handle in hand, which is t"
        "he argument vkCreateFramebuffer takes");
    xud_check(fbInfo.attachmentCount == 1u, "framebuffer/create-attachment-count",
        "one attachment, which is the pass's own attachmentCount - the spec requires the two counts "
        "to agree exactly");
    xud_check(fbInfo.pAttachments == &framebuffer.view, "framebuffer/create-attachment-row",
        "the row points at the framebuffer's OWN view field, not at a copy of a handle: the create-"
        "info is read after this call returns, so the only storage that can still be alive is the "
        "struct's, and that is why this function takes the struct");
    xud_check((fbInfo.width == 320u) && (fbInfo.height == 240u), "framebuffer/create-extent",
        "the extent the swapchain was created at, copied rather than re-queried");
    xud_check(fbInfo.layers == 1u, "framebuffer/create-layers", "one layer, the same answer as the view's and the pass's");
}

void xud_test_renderpass(void) { xud_test_renderpass_pure(); }
