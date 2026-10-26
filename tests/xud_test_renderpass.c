/***********************************************************************************************
 *
 *   xud_test_renderpass.c - the render-pass and framebuffer-array rung
 *
 *   Owns : the one-subpass pass with its TWO attachment rows - the swapchain's colour attachment
 * and the depth attachment the pass now owns - its layout pair, its clear-value count and its one
 * dependency, the depth image + view create-info assembly the pass's own VkImage is made from, and
 * the framebuffer + image-view create-info assembly per swapchain image. Layer: tests. Sees xud_re
 * nderpass.h and xud_pipeline.h: a framebuffer is created FOR a pass, so the pass's attachment des
 * criptions and the framebuffer's view row are checked as one area, and the pass's sample count is
 * the pipeline layer's constant the attachment rows are asked to match. The GPU half sees xud_core
 * .h and xud_allocator.h as well, because the depth attachment is the first thing in this tree tha
 * t asks the allocator for an IMAGE rather than a buffer, and the pass is the owner of that image.
 *
 *   Deterministic: fixed loop counts, no time, no randomness, no filesystem.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud.h" /* Required for: XudResult, XUD_VERSION, the XUD_GPU_TESTS gate */
#include "xud_allocator.h" /* Required for: XudMemoryRange, XudAllocRange, xud_allocator_stats/xud_allocator_range_get, the rows the depth range is read back through */
#include "xud_core.h" /* Required for: XudContext, the owner the GPU half runs a completed bring-up against */
#include "xud_pipeline.h" /* Required for: XUD_PIPELINE_SAMPLE_COUNT, the sample count BOTH attachment rows of the pass are asked to match */
#include "xud_renderpass.h" /* Required for: XudRenderPass, XudFramebuffer, the fill rungs and the state assembly */
#include "xud_test_harness.h" /* Required for: xud_check, xud_gpu_tests_enabled, the area entry points */
#include <string.h> /* Required for: memset, the local copies of the pass's own range that are compared field-wise */

/*-----------------------------------------------------------------------------------------------
 * The render pass and the framebuffer array, headless: the two attachments' descriptions, the one
 * subpass that reads both, the one dependency that orders the two clears before the two writes, an
 * d the per-image view + framebuffer assembly. The dependency's access masks are attachment-speci
 * fic names because these headers carry no VK_ACCESS_CLEAR_AND_WRITE_BIT at all (probed today: zer
 * o hits for CLEAR_AND_WRITE across the whole Vulkan include tree), which is the same reason the pi
 * peline layer's note gives for the depth pair. The two handles a framebuffer create-info needs ar
 * e synthetic on purpose: the pure half refuses a handle-less one and dereferences nothing, which i
 * s what makes the refusal testable at all.
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_renderpass_pure(void) {
    XudRenderPass pass = (XudRenderPass) { 0 };
    XudRenderPass bare = (XudRenderPass) { 0 };
    XudFramebuffer framebuffer = (XudFramebuffer) { 0 };
    VkRenderPassCreateInfo passInfo = (VkRenderPassCreateInfo) { .sType = VK_STRUCTURE_TYPE_MAX_ENUM };
    VkImageViewCreateInfo viewInfo = (VkImageViewCreateInfo) { .sType = VK_STRUCTURE_TYPE_MAX_ENUM };
    VkImageViewCreateInfo depthViewInfo = (VkImageViewCreateInfo) { .sType = VK_STRUCTURE_TYPE_MAX_ENUM };
    VkFramebufferCreateInfo fbInfo = (VkFramebufferCreateInfo) { .sType = VK_STRUCTURE_TYPE_MAX_ENUM };
    VkImageCreateInfo imageInfo = (VkImageCreateInfo) { .sType = VK_STRUCTURE_TYPE_MAX_ENUM };
    const VkFormat chosen = VK_FORMAT_B8G8R8A8_UNORM;

    xud_check(xud_renderpass_fill_state(NULL, chosen) == XUD_INVALID_ARG, "renderpass/state-null-out", "refused");
    xud_check(xud_renderpass_fill_state(&pass, VK_FORMAT_UNDEFINED) == XUD_UNSUPPORTED, "renderpass/state-no-format",
        "an attachment description with no format is not a description, so it is reported as suc"
        "h rather than written");
    xud_check(xud_renderpass_fill_state(&pass, chosen) == XUD_OK, "renderpass/state-ok", "the rows are filled");

    /* --- the colour attachment's own description --- */
    xud_check(pass.attachments[XUD_RENDERPASS_COLOR_ATTACHMENT].format == chosen, "renderpass/attachment-format",
        "the colour attachment is the swapchain's CHOSEN format, which is the half of the agreement the spec requires between a pass and "
        "the framebuffer under it");
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

    /* --- the depth attachment's own description: the second row, and the reason the pipeline's de
     *     pth pair is an ON pair now --- */
    xud_check(pass.attachments[XUD_RENDERPASS_DEPTH_ATTACHMENT].format == XUD_RENDERPASS_DEPTH_FORMAT, "renderpass/depth-format",
        "D32_SFLOAT, the one depth format whose row is not extension-gated in these headers (D32_"
        "LINEAR_SFLOAT is gated on VK_KHR_depth32float_formats, D24_UNORM on VK_KHR_depth24_unorm_"
        "8bit_clear_invariants, and the D24 pair on VK_EXT_depth_clip_negativew_one), so the row t"
        "his rung writes is a row every Vulkan 1.0 implementation has");
    xud_check(pass.attachments[XUD_RENDERPASS_DEPTH_ATTACHMENT].samples == XUD_PIPELINE_SAMPLE_COUNT, "renderpass/depth-samples",
        "the same one sample the colour row carries, because VUID-VkFramebufferCreateInfo-attachm"
        "ents-02699 requires every attachment of one framebuffer to match the pass's descriptions"
        " in samples as well as in format - a depth row at a different sample count is a pass the"
        " framebuffer under it cannot be created for");
    xud_check((pass.attachments[XUD_RENDERPASS_DEPTH_ATTACHMENT].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR)
            && (pass.attachments[XUD_RENDERPASS_DEPTH_ATTACHMENT].storeOp == VK_ATTACHMENT_STORE_OP_STORE),
        "renderpass/depth-load-store",
        "CLEAR and STORE, the same pair as the colour row: the far value is cleared every frame (t"
        "he clear value below is 1.0, the far plane of the LESS rule the pipeline above uses) and "
        "the depths are kept for the next frame's clear rather than left undefined");
    xud_check((pass.attachments[XUD_RENDERPASS_DEPTH_ATTACHMENT].stencilLoadOp == VK_ATTACHMENT_LOAD_OP_LOAD)
            && (pass.attachments[XUD_RENDERPASS_DEPTH_ATTACHMENT].stencilStoreOp == VK_ATTACHMENT_STORE_OP_STORE),
        "renderpass/depth-stencil-core-ops",
        "the same CORE pair, and the same reason it is not NONE: D32_SFLOAT has no stencil aspect a"
        "t all, so both halves are ignored, but the enum still has to be one the validation layer "
        "accepts, and NONE is not one of those here");
    xud_check((pass.attachments[XUD_RENDERPASS_DEPTH_ATTACHMENT].initialLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
            && (pass.attachments[XUD_RENDERPASS_DEPTH_ATTACHMENT].finalLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL),
        "renderpass/depth-layouts",
        "DEPTH_ATTACHMENT_OPTIMAL on both halves: unlike the swapchain image there is nothing to p"
        "resent out of, so the image stays in the layout it is drawn in, and the frame loop's tran"
        "sition still runs because the image is created UNDEFINED rather than pre-promised");
    xud_check(pass.attachments[XUD_RENDERPASS_DEPTH_ATTACHMENT].flags == 0u, "renderpass/depth-flags-clear",
        "no attachment flag, and in particular no VK_ATTACHMENT_UNUSED_BIT: this row is read by th"
        "e subpass below");

    /* --- the subpass, and the two references it reads --- */
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
        "one colour attachment for the subpass, and the pass has two attachments in total, so the "
        "two counts agree exactly only once the depth row is counted separately - which it is, by "
        "the depth-stencil reference below");
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
    xud_check(pass.subpasses[XUD_RENDERPASS_FIRST_SUBPASS].pDepthStencilAttachment == &pass.depthReferences[XUD_RENDERPASS_FIRST_SUBPASS],
        "renderpass/depth-reference-row",
        "the subpass DOES name a depth-stencil reference now, and it points at the pass's own depth"
        " reference rather than a copy of it - the same storage rule the colour row follows. This "
        "is the half of the old no-depth-attachment check that has turned around: a depth buffer ex"
        "ists in this tree, and this rung is its owner");
    xud_check(pass.depthReferences[XUD_RENDERPASS_FIRST_SUBPASS].attachment == XUD_RENDERPASS_DEPTH_ATTACHMENT,
        "renderpass/depth-reference-index",
        "the reference names attachment 1, the depth row - so the framebuffer's attachment list ha"
        "s to name the depth view at index 1 as well, which is the exact-match rule the framebu"
        "ffer section below checks");
    xud_check(pass.depthReferences[XUD_RENDERPASS_FIRST_SUBPASS].layout == pass.attachments[XUD_RENDERPASS_DEPTH_ATTACHMENT].initialLayout,
        "renderpass/depth-reference-layout",
        "the reference names the description's initialLayout, the same rule as the colour half, an"
        "d the reason the frame loop's transition has to run before the first attachment write ev"
        "en though both halves of the depth pair are the same layout");
    xud_check(pass.subpasses[XUD_RENDERPASS_FIRST_SUBPASS].preserveAttachmentCount == 0u, "renderpass/no-preserve-attachments",
        "nothing is preserved: every attachment of this pass is written by it");

    /* --- the one dependency, and the two stages a CLEAR belongs to --- */
    xud_check((pass.dependencies[XUD_RENDERPASS_FIRST_SUBPASS].srcSubpass == VK_SUBPASS_EXTERNAL)
            && (pass.dependencies[XUD_RENDERPASS_FIRST_SUBPASS].dstSubpass == XUD_RENDERPASS_FIRST_SUBPASS),
        "renderpass/dependency-external-to-zero",
        "the spec's required external -> 0 pair, which is what makes the clear of the attachment "
        "happen before the draw writes it");
    xud_check((pass.dependencies[XUD_RENDERPASS_FIRST_SUBPASS].srcStageMask
                  == (VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                      | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT)),
        "renderpass/dependency-source-stage",
        "the colour-attachment-output stage AND both fragment-test stages on the source half: the "
        "clear of a colour attachment belongs to COLOR_ATTACHMENT_OUTPUT and the clear of a depth "
        "attachment belongs to the depth-test stages, so a mask that names only the colour half i"
        "s a dependency about half of this pass's attachments - the validation layer reports the "
        "missing depth half as VUID-VkSubpassDependency-srcAccessMask-02854, probed today");
    xud_check(
        (pass.dependencies[XUD_RENDERPASS_FIRST_SUBPASS].dstStageMask
            == (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT)),
        "renderpass/dependency-destination-stage",
        "the whole pass on the destination half: the clear has to be ordered before the vertex and"
        " fragment shaders (the draw) and before both attachment-write halves (the writes), which "
        "is the pair that orders the clear before both the vertex shader and the write, on both a"
        "ttachments");
    xud_check((pass.dependencies[XUD_RENDERPASS_FIRST_SUBPASS].srcAccessMask
                  == (VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT))
            && (pass.dependencies[XUD_RENDERPASS_FIRST_SUBPASS].dstAccessMask
                == (VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)),
        "renderpass/dependency-access-masks",
        "the attachment-specific pair on BOTH halves, because these headers carry no VK_ACCESS_CLE"
        "AR_AND_WRITE_BIT at all (probed today: zero hits for CLEAR_AND_WRITE across the whole inc"
        "lude tree) and a CLEAR is an attachment write for that purpose - so the depth half is DE"
        "PTH_STENCIL_ATTACHMENT_WRITE, the name the depth row's own access is, which is what makes"
        " the two rows of this pass ordered by one dependency rather than by one that only means t"
        "he colour row");
    xud_check(pass.dependencies[XUD_RENDERPASS_FIRST_SUBPASS].dependencyFlags == 0u, "renderpass/dependency-flags-clear",
        "no flag: the dependency is a full execution-and-memory barrier, which is the conservative "
        "half of the repo's classic-synchronization rule");

    /* --- the two clear values, in attachment order --- */
    xud_check((pass.clearValues[XUD_RENDERPASS_COLOR_ATTACHMENT].color.float32[0] == XUD_RENDERPASS_CLEAR_COLOR_R)
            && (pass.clearValues[XUD_RENDERPASS_COLOR_ATTACHMENT].color.float32[3] == XUD_RENDERPASS_CLEAR_COLOR_A),
        "renderpass/clear-colour",
        "the clear colour, in attachment order, so VkRenderPassBeginInfo's pClearValues index matc"
        "hes the attachment index - the spec's own rule for that pair");
    xud_check(pass.clearValues[XUD_RENDERPASS_COLOR_ATTACHMENT].color.float32[3] == 1.0f, "renderpass/clear-alpha-opaque",
        "alpha 1, which is what the surface's OPAQUE composite answer needs: a clear with 0 alpha "
        "would be a window the compositor could see through");
    xud_check((pass.clearValues[XUD_RENDERPASS_COLOR_ATTACHMENT].color.float32[3] == 1.0f)
            && (pass.clearValues[XUD_RENDERPASS_DEPTH_ATTACHMENT].depthStencil.depth == XUD_RENDERPASS_CLEAR_DEPTH),
        "renderpass/clear-halves-are-one-union",
        "the two rows are read by their OWN halves, and the halves are not separate fields: VkClear"
        "Value is a union, so the colour row's depthStencil.depth IS its color.float32[0] (probed he"
        "re: the colour clear's alpha 1.0 sits in the same four bytes the depth half would read). Ass"
        "erting an unused half is zero would be asserting the used half of the other row - which is"
        " why the spec picks the half by the attachment's aspect and not by what was written");
    xud_check(pass.clearValues[XUD_RENDERPASS_DEPTH_ATTACHMENT].depthStencil.depth == XUD_RENDERPASS_CLEAR_DEPTH, "renderpass/clear-depth",
        "the far value 1.0 cleared into the depth buffer, which is what makes the LESS rule the pi"
        "peline above uses a rule against a defined buffer rather than against whatever the allob"
        "cator handed back - the same reason the colour row's clear is not left to the frame before");
    xud_check(pass.clearValues[XUD_RENDERPASS_DEPTH_ATTACHMENT].depthStencil.stencil == 0, "renderpass/clear-depth-stencil-zero",
        "the stencil half of the depth row is zero, and D32_SFLOAT has no stencil aspect at all, so"
        " the half that is read is the depth one");

    /* --- the depth image's own create-info: the first VkImage in this tree --- */
    xud_check(xud_renderpass_fill_depth_info(NULL, 320u, 240u) == XUD_INVALID_ARG, "renderpass/depth-info-null-out", "refused");
    xud_check(xud_renderpass_fill_depth_info(&imageInfo, 0u, 240u) == XUD_INVALID_ARG, "renderpass/depth-info-zero-extent",
        "a zero extent is a bad argument, because an image of no pixels is an image whose memory r"
        "equirements the allocator is asked for name nothing");
    xud_check(xud_renderpass_fill_depth_info(&imageInfo, 320u, 240u) == XUD_OK, "renderpass/depth-info-ok", "the create-info is assembled");
    xud_check(imageInfo.sType == VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, "renderpass/depth-info-stamped",
        "stamped, and the stamp is a third constant in this area (the pass's, the view's, the image"
        "'s), so the three create-infos cannot be confused");
    xud_check((imageInfo.pNext == NULL) && (imageInfo.flags == 0u), "renderpass/depth-info-chain-clear",
        "nothing is chained into it and no flag is set - in particular no VK_IMAGE_CREATE_SPARSE_"
        "BIT and no cube-map form, this is one plain 2D image");
    xud_check(imageInfo.imageType == VK_IMAGE_TYPE_2D, "renderpass/depth-info-type-2d",
        "a 2D image, the same form as the swapchain's images and the only one a framebuffer of thi"
        "s rung can attach");
    xud_check(imageInfo.format == XUD_RENDERPASS_DEPTH_FORMAT, "renderpass/depth-info-format",
        "the SAME constant the pass's depth attachment row carries, so the image, the view below i"
        "t and the attachment description cannot disagree about what this image holds");
    xud_check((imageInfo.extent.width == 320u) && (imageInfo.extent.height == 240u) && (imageInfo.extent.depth == 1u),
        "renderpass/depth-info-extent",
        "the swapchain's extent, and a 2D image's third dimension is 1 written rather than left at "
        "zero, because VkExtent3D is one struct and the framebuffer's attachment-extent rule (VUID-"
        "VkFramebufferCreateInfo-attachments-02702) reads all three of it");
    xud_check((imageInfo.mipLevels == 1u) && (imageInfo.arrayLayers == 1u), "renderpass/depth-info-one-mip-one-layer",
        "one mip and one layer, matching the view below it and the surface's own maxImageArrayLaye"
        "rs answer - a mip chain would be a chain nothing in this tree samples");
    xud_check(imageInfo.samples == XUD_PIPELINE_SAMPLE_COUNT, "renderpass/depth-info-samples",
        "one sample, the same constant as both attachment rows and the pipeline's rasterization ro"
        "w: an image created at a sample count that is not the attachment row's is an image the p"
        "ass cannot be created for");
    xud_check(imageInfo.tiling == VK_IMAGE_TILING_OPTIMAL, "renderpass/depth-info-tiling-optimal",
        "OPTIMAL tiling rather than LINEAR: LINEAR is the layout an image that will be sampled in a"
        " specific order asks for, and this image is only ever attached, so OPTIMAL lets the driver"
        " pick the swizzled form it prefers for a depth attachment - and these headers' VkImageCrea"
        "teInfo has no numberOfArrays member at all (grepped, 1.4.363: the row ends at initialLayou"
        "t and the array dimension is arrayLayers alone), so the array form is not a row this layer"
        " could even write");
    xud_check(imageInfo.usage == XUD_RENDERPASS_DEPTH_USAGE, "renderpass/depth-info-usage",
        "DEPTH_STENCIL_ATTACHMENT and nothing else: this image is never sampled, never copied and n"
        "ever used as an input attachment, so the usage row declares exactly the access the pass a"
        "bove asks for - and vkGetImageMemoryRequirements is asked about this row, which is why it "
        "is a fact rather than a formality");
    xud_check((imageInfo.sharingMode == VK_SHARING_MODE_EXCLUSIVE) && (imageInfo.queueFamilyIndexCount == 0u)
            && (imageInfo.pQueueFamilyIndices == NULL),
        "renderpass/depth-info-exclusive",
        "EXCLUSIVE sharing with the spec's 'not shared across families' pair written explicitly, t"
        "he same pair xud_swapchain_fill_create_info() writes for the swapchain's own images - the "
        "image belongs to one device and is never transferred");
    xud_check(imageInfo.initialLayout == VK_IMAGE_LAYOUT_UNDEFINED, "renderpass/depth-info-initial-layout",
        "UNDEFINED, not DEPTH_ATTACHMENT_OPTIMAL: the image is created with no contents at all, an"
        "d the frame loop's layout-transition barrier is what moves it into the pass's initial layo"
        "ut before the first attachment write. Declaring the optimal layout here would be a promise"
        " about bytes that do not exist yet, which is the same reason xud_frame.c starts its image "
        "layouts at UNDEFINED - and there is no finalLayout field of VkImageCreateInfo in these hea"
        "ders at all, so the layout an image ends a pass in is stated by the pass's row, not by th"
        "is one");

    /* --- the depth view's create-info: the second view helper in this area, and the reason the fi
     *     rst one cannot serve it --- */
    xud_check(xud_renderpass_fill_depth_view_info(NULL, (VkImage)0x1000u) == XUD_INVALID_ARG, "renderpass/depth-view-null-out", "refused");
    xud_check(xud_renderpass_fill_depth_view_info(&depthViewInfo, VK_NULL_HANDLE) == XUD_OK, "renderpass/depth-view-handle-borrowed",
        "the assembly rung writes whatever handle it is handed, including a handle-less one, and ref"
        "uses only a missing out-param - the same shape xud_framebuffer_fill_view_info() has. The han"
        "dle is this layer's OWN, created by xud_renderpass_open() before the view is assembled, so t"
        "he guard lives in the open rung and the GPU half below asserts the handle is live rather th"
        "an asking the assembly rung for it");
    xud_check(xud_renderpass_fill_depth_view_info(&depthViewInfo, (VkImage)0x1000u) == XUD_OK, "renderpass/depth-view-ok",
        "the view is assembled");
    xud_check(depthViewInfo.sType == VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, "renderpass/depth-view-stamped",
        "stamped with the SAME constant the colour view's helper writes, because both are views and"
        " one create-info type, which is what makes the two helpers one type and two aspects rathe"
        "r than two types");
    xud_check((depthViewInfo.pNext == NULL) && (depthViewInfo.flags == 0u), "renderpass/depth-view-chain-clear", "chain empty, no flag");
    xud_check(depthViewInfo.image == (VkImage)0x1000u, "renderpass/depth-view-image",
        "the image handle in hand is recorded as the argument it was - the pass's OWN depth image, "
        "which is the handle this layer creates and destroys, not a borrowed swapchain image");
    xud_check(depthViewInfo.viewType == VK_IMAGE_VIEW_TYPE_2D, "renderpass/depth-view-type-2d",
        "a 2D view of a 2D image, the only kind this rung has");
    xud_check(depthViewInfo.format == XUD_RENDERPASS_DEPTH_FORMAT, "renderpass/depth-view-format",
        "the depth format, the same constant as the image's and the attachment row's - a view whos"
        "e format is not the image's is a view the framebuffer under it refuses");
    xud_check((depthViewInfo.components.r == VK_COMPONENT_SWIZZLE_IDENTITY) && (depthViewInfo.components.g == VK_COMPONENT_SWIZZLE_IDENTITY)
            && (depthViewInfo.components.b == VK_COMPONENT_SWIZZLE_IDENTITY)
            && (depthViewInfo.components.a == VK_COMPONENT_SWIZZLE_IDENTITY),
        "renderpass/depth-view-swizzle-identity",
        "an identity swizzle written on all four channels rather than left at zero, because the fi"
        "eld is a value the driver reads and not an optional one - the same rule the colour view's"
        " helper follows");
    xud_check(depthViewInfo.subresourceRange.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT, "renderpass/depth-view-aspect",
        "the DEPTH aspect, and it lives only in the subresource range - VkImageViewCreateInfo has n"
        "o aspectMask of its own, which is the member-path trap of this create-info. This is the o"
        "nly difference from the colour view's helper, and it is why there are two helpers: a view "
        "of a depth image whose range names the colour aspect is a view of nothing, and the validat"
        "ion layer answers it as VUID-VkImageViewCreateInfo-subresourceRange-0959");
    xud_check((depthViewInfo.subresourceRange.baseMipLevel == 0u) && (depthViewInfo.subresourceRange.levelCount == 1u),
        "renderpass/depth-view-one-mip", "one mip from the first, which is all the image above has");
    xud_check((depthViewInfo.subresourceRange.baseArrayLayer == 0u) && (depthViewInfo.subresourceRange.layerCount == 1u),
        "renderpass/depth-view-one-layer", "one layer, the same answer as the image's, the pass's and the colour view's");

    /* --- the pass's create-info, now over two attachment rows --- */
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
        "TWO attachments, and the row points into the pass's own storage, so the record and the cr"
        "eate-info cannot disagree - the count is the layer's constant rather than a literal, whic"
        "h is what makes the framebuffer's exact-match rule below one shared fact");
    xud_check((passInfo.subpassCount == XUD_RENDERPASS_SUBPASS_COUNT) && (passInfo.pSubpasses == pass.subpasses),
        "renderpass/create-subpass-row", "and so is the subpass row");
    xud_check((passInfo.dependencyCount == XUD_RENDERPASS_DEPENDENCY_COUNT) && (passInfo.pDependencies == pass.dependencies),
        "renderpass/create-dependency-row", "and so is the dependency row");

    /* --- the colour view of one swapchain image, and the framebuffer row that attaches it --- */
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
        "the view names the SAME format the pass's colour attachment description gives, which is th"
        "e half of the agreement the spec requires between a pass and the framebuffer under it");
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
    framebuffer.attachmentViews[XUD_RENDERPASS_COLOR_ATTACHMENT] = (VkImageView)0x2000u;
    framebuffer.attachmentViews[XUD_RENDERPASS_DEPTH_ATTACHMENT] = (VkImageView)0x3000u;

    xud_check(xud_framebuffer_fill_create_info(NULL, &framebuffer, pass.renderPass, 320u, 240u) == XUD_INVALID_ARG,
        "framebuffer/create-null-out", "refused");
    xud_check(xud_framebuffer_fill_create_info(&fbInfo, NULL, pass.renderPass, 320u, 240u) == XUD_INVALID_ARG,
        "framebuffer/create-no-framebuffer",
        "a create-info with no framebuffer to point at is refused, because the attachment row has "
        "to point at that struct's own view - the create-info is read after this call returns, so a "
        "copy of a handle would already be dead by then, which is why this function takes the struct"
        " at all");
    xud_check(xud_framebuffer_fill_create_info(&fbInfo, &framebuffer, VK_NULL_HANDLE, 320u, 240u) == XUD_INVALID_ARG,
        "framebuffer/create-no-pass",
        "a framebuffer that names no pass is refused, because the attachment count it writes is th"
        "e pass's");
    xud_check(xud_framebuffer_fill_create_info(&fbInfo, &framebuffer, pass.renderPass, 0u, 240u) == XUD_INVALID_ARG,
        "framebuffer/create-zero-extent", "and so is a zero extent");
    framebuffer.view = VK_NULL_HANDLE;
    xud_check(xud_framebuffer_fill_create_info(&fbInfo, &framebuffer, pass.renderPass, 320u, 240u) == XUD_INVALID_ARG,
        "framebuffer/create-no-view", "and so is a framebuffer whose colour view is not live yet - it has nothing to attach");
    framebuffer.view = (VkImageView)0x2000u;
    framebuffer.attachmentViews[XUD_RENDERPASS_COLOR_ATTACHMENT] = framebuffer.view;
    framebuffer.attachmentViews[XUD_RENDERPASS_DEPTH_ATTACHMENT] = VK_NULL_HANDLE;
    xud_check(xud_framebuffer_fill_create_info(&fbInfo, &framebuffer, pass.renderPass, 320u, 240u) == XUD_INVALID_ARG,
        "framebuffer/create-no-depth-view",
        "and so is a framebuffer whose depth row is not live: the pass above declares two attachmen"
        "ts and a framebuffer that attaches one of two is a framebuffer the pass refuses (VUID-VkFr"
        "amebufferCreateInfo-attachmentCount-00656), so the half that is missing is reported rather "
        "than attached as VK_NULL_HANDLE");
    framebuffer.attachmentViews[XUD_RENDERPASS_DEPTH_ATTACHMENT] = (VkImageView)0x3000u;
    xud_check(xud_framebuffer_fill_create_info(&fbInfo, &framebuffer, pass.renderPass, 320u, 240u) == XUD_OK, "framebuffer/create-ok",
        "the create-info is assembled");
    xud_check(fbInfo.sType == VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, "framebuffer/create-stamped",
        "stamped, and the stamp is a different constant from the view's, so the two create-infos ca"
        "nnot be confused");
    xud_check(fbInfo.pNext == NULL, "framebuffer/create-pnext-clear", "and its chain stays empty");
    xud_check(fbInfo.renderPass == pass.renderPass, "framebuffer/create-pass-handle",
        "the pass this framebuffer was created FOR is handed over as the handle in hand, which is t"
        "he argument vkCreateFramebuffer takes");
    xud_check(fbInfo.attachmentCount == XUD_RENDERPASS_ATTACHMENT_COUNT, "framebuffer/create-attachment-count",
        "TWO attachments, which is the pass's own attachmentCount - the spec requires the two count"
        "s to agree exactly, and a row that attaches one of two is a row the pass above will refus"
        "e");
    xud_check(fbInfo.pAttachments == framebuffer.attachmentViews, "framebuffer/create-attachment-row",
        "the row points at the framebuffer's OWN attachment list, not at a copy of a handle: the cr"
        "eate-info is handed to vkCreateFramebuffer AFTER this call returns, so the only storage tha"
        "t can still be alive is the struct's, and that is why this function takes the struct");
    xud_check((framebuffer.attachmentViews[XUD_RENDERPASS_COLOR_ATTACHMENT] == framebuffer.view)
            && (framebuffer.attachmentViews[XUD_RENDERPASS_DEPTH_ATTACHMENT] == (VkImageView)0x3000u),
        "framebuffer/attachment-row-order",
        "row 0 is this row's own colour view and row 1 is the pass's depth view, which this row onl"
        "y borrows - the order is the pass's attachment order, which is the one rule that makes a "
        "two-row list mean two things rather than one swapped thing");
    xud_check((fbInfo.width == 320u) && (fbInfo.height == 240u), "framebuffer/create-extent",
        "the extent the swapchain was created at, copied rather than re-queried - and the SAME exten"
        "t the depth image above was created at, which is the attachment-extent rule VUID-VkFramebu"
        "fferCreateInfo-attachments-02702 reads");
    xud_check(fbInfo.layers == 1u, "framebuffer/create-layers", "one layer, the same answer as the view's and the pass's");
}

/*-----------------------------------------------------------------------------------------------
 * The same rung against the driver: the pass a completed context owns, its depth image and view, t
 * he allocator rows the image was carved out of, and the framebuffer rows that borrow the depth vi
 * ew. This is the first time in this tree an IMAGE is created - the descriptor rung's buffer was t
 * he first thing above the allocator, and this is the first thing above it that is not a buffer - s
 * o the memory requirements the allocator was asked with are the image's own, read back rather tha
 * n assumed, and the alignment they came with is the image's (probed today: 65536 on this driver fo
 * r a D32_SFLOAT image, which is why the allocator's alignment ceiling is not a buffer fact).
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_renderpass_gpu(void) {
    XudContext* context = NULL;
    XudResult result = XUD_OK;
    const XudRenderPass* pass = NULL;
    const XudFramebuffer* row = NULL;
    XudMemoryRange current;
    XudMemoryRange depth;
    uint32_t blocks = 0u;
    uint32_t live = 0u;
    uint32_t i = 0u;
    VkDeviceSize bytes = 0u;
    VkDeviceSize cursor = 0u;

    memset(&current, 0, sizeof current);
    memset(&depth, 0, sizeof depth);

    result = xud_init_context(&context, 96u, 64u);
    xud_check(result == XUD_OK, "gpu-renderpass/context-completes", "the ladder has to complete before a pass can exist");
    if (context == NULL) {
        return;
    }

    pass = context->renderer->renderPass;
    xud_check((pass != NULL) && (pass->renderPass != VK_NULL_HANDLE), "gpu-renderpass/pass-live",
        "the renderer owns one pass and its VkRenderPass handle is live, so the explicit-pass path the"
        " repo settled on is the one that ran");
    if (pass == NULL) {
        (void)xud_close_context(&context);
        return;
    }

    xud_check(pass->attachments[XUD_RENDERPASS_COLOR_ATTACHMENT].format == context->swapchain->createInfo.imageFormat,
        "gpu-renderpass/format-is-swapchain's",
        "the colour row's format is the format the swapchain was actually created at, read back rather"
        " than the constant the selection rule prefers - the two layers agree because one chose and the"
        " other recorded it");
    xud_check(pass->attachments[XUD_RENDERPASS_DEPTH_ATTACHMENT].format == XUD_RENDERPASS_DEPTH_FORMAT, "gpu-renderpass/depth-format-live",
        "the depth row is D32_SFLOAT against the driver, not a format the driver picked for us");
    xud_check((pass->depthImageInfo.extent.width == context->swapchain->createInfo.imageExtent.width)
            && (pass->depthImageInfo.extent.height == context->swapchain->createInfo.imageExtent.height)
            && (pass->depthImageInfo.extent.depth == 1u),
        "gpu-renderpass/depth-extent-is-swapchain's",
        "the depth image was created at the SAME extent as the swapchain's images, which is the only ex"
        "tent a framebuffer of this rung can attach (VUID-VkFramebufferCreateInfo-attachments-02702), r"
        "ead back rather than assumed");
    xud_check(pass->depthImageInfo.usage == XUD_RENDERPASS_DEPTH_USAGE, "gpu-renderpass/depth-usage-live",
        "the usage row the image was created with is the one this layer's constant names, and it is the"
        " row vkGetImageMemoryRequirements was asked about");
    xud_check(pass->depthImageInfo.initialLayout == VK_IMAGE_LAYOUT_UNDEFINED, "gpu-renderpass/depth-initial-undefined",
        "the image says UNDEFINED, so the frame loop's transition is what gives it a layout - the pass's"
        " row is a promise about bytes this image does not yet have");
    xud_check(pass->depthImage != VK_NULL_HANDLE, "gpu-renderpass/depth-image-live",
        "a real VkImage exists, which is the first image-level object in this tree and the first thing"
        " that ever asked the allocator for an image's requirements rather than a buffer's");
    xud_check(pass->depthView != VK_NULL_HANDLE, "gpu-renderpass/depth-view-live",
        "and its one view, which is the handle the framebuffer rows below borrow");
    xud_check(pass->depthViewInfo.image == pass->depthImage, "gpu-renderpass/depth-view-of-image",
        "the view was created FOR that image, so the pair is one pair and not a view of a recycled hand"
        "le");
    xud_check(pass->depthViewInfo.subresourceRange.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT, "gpu-renderpass/depth-view-aspect-live",
        "the view's range names the DEPTH aspect, which is the aspect D32_SFLOAT has - a colour-aspect v"
        "iew of this image would be a view of nothing (VUID-VkImageViewCreateInfo-subresourceRange-0959)"
        ", and it is the aspect the pass's attachment row is asked about in the same breath");
    xud_check(
        pass->depthReferences[XUD_RENDERPASS_FIRST_SUBPASS].layout == pass->attachments[XUD_RENDERPASS_DEPTH_ATTACHMENT].initialLayout,
        "gpu-renderpass/depth-reference-layout-live",
        "and the pass's attachment reference names the same layout the description says the image is in,"
        " so the description and the reference cannot mean two different things - a VkAttachmentReferenc"
        "e states an attachment index and a layout and nothing else in these headers");
    xud_check(pass->subpasses[XUD_RENDERPASS_FIRST_SUBPASS].pDepthStencilAttachment == &pass->depthReferences[XUD_RENDERPASS_FIRST_SUBPASS],
        "gpu-renderpass/depth-reference-row-live",
        "the live subpass names the depth reference, so the pass above the pipeline really does write in"
        "to the depth buffer - which is what makes the pipeline's ON depth pair a pair against a defined"
        " buffer rather than an implementation-dependent one");
    xud_check((pass->attachments[XUD_RENDERPASS_DEPTH_ATTACHMENT].initialLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
            && (pass->attachments[XUD_RENDERPASS_DEPTH_ATTACHMENT].finalLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL),
        "gpu-renderpass/depth-layout-pair-live",
        "the layout pair the frame loop's transition moves the image into and out of, read back from the"
        " pass the driver was handed");
    xud_check((pass->attachments[XUD_RENDERPASS_DEPTH_ATTACHMENT].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR)
            && (pass->attachments[XUD_RENDERPASS_DEPTH_ATTACHMENT].storeOp == VK_ATTACHMENT_STORE_OP_STORE),
        "gpu-renderpass/depth-load-store-live", "the depth buffer is cleared every frame and kept after it, against the driver");
    xud_check(pass->clearValues[XUD_RENDERPASS_DEPTH_ATTACHMENT].depthStencil.depth == XUD_RENDERPASS_CLEAR_DEPTH,
        "gpu-renderpass/depth-clear-live",
        "the far value the pass clears into it, read back from the pass rather than from the constant th"
        "is area was written against");
    xud_check(pass->depthRange.memory != VK_NULL_HANDLE, "gpu-renderpass/depth-memory-live",
        "the image is bound to a real VkDeviceMemory, which is the allocator's block the image was carved"
        " out of - the first image the allocator ever served");
    xud_check(xud_allocator_range_get(context->allocator, &pass->depthRange, &current) == XUD_OK, "gpu-renderpass/depth-range-reads-back",
        "and the allocator's own rows still name it live, so the range the pass holds is the truth of the"
        " free list rather than a copy that went stale");
    xud_check((current.offset == pass->depthRange.offset) && (current.size == pass->depthRange.size)
            && (current.memory == pass->depthRange.memory),
        "gpu-renderpass/depth-range-is-the-row",
        "the accessor answers the row itself, so what the pass holds is the allocator's truth and not a r"
        "e-derived guess");
    xud_check(xud_allocator_stats(context->allocator, &blocks, &live, &bytes, &cursor) == XUD_OK, "gpu-renderpass/stats-answers",
        "the counters answer");
    xud_check(blocks == 2u, "gpu-renderpass/two-blocks",
        "TWO blocks now rather than the descriptor rung's one: the depth image's requirements (alignment 6"
        "5536 on this driver) do not fit the block the uniform buffer was carved out of, so the image's bl"
        "ock is a second VkDeviceMemory rather than a suballocation - the allocator's rule read against an"
        " image's requirements for the first time");
    xud_check(live == 2u, "gpu-renderpass/two-live-ranges",
        "two live ranges over them, the uniform buffer's carve and the depth image's, which is what a comp"
        "leted context owns now and is the baseline every number in the allocator area is read as a delta o"
        "n");
    xud_check(bytes >= pass->depthRange.size, "gpu-renderpass/bytes-count-the-image",
        "the reserved-bytes counter includes the image's own range, so the counter is a fact about the rows"
        " rather than an estimate");
    xud_check(cursor == 0u, "gpu-renderpass/ring-untouched",
        "and the ring's cursor is still at the start, because a depth buffer is static - it is asked for on"
        "ce at init and never per frame, which is the repo's no-allocation-inside-a-frame rule read at the"
        " image");

    xud_check(context->renderer->framebufferCount == context->swapchain->imageCount, "gpu-renderpass/rows-are-images",
        "one framebuffer row per swapchain image, unchanged by the depth attachment - the depth buffer is O"
        "NE image shared by every row, not one per row, which is why the array did not grow");
    for (i = 0u; (i < context->renderer->framebufferCount); i++) {
        row = context->renderer->framebuffers->rows[i];
        xud_check((row != NULL) && (row->framebuffer != VK_NULL_HANDLE) && (row->view != VK_NULL_HANDLE), "gpu-renderpass/row-live",
            "the row and both its handles are live");
        xud_check(row->attachmentViews[XUD_RENDERPASS_COLOR_ATTACHMENT] == row->view, "gpu-renderpass/row-colour-is-own-view",
            "row 0 of the attachment list is the row's own colour view");
        xud_check(row->attachmentViews[XUD_RENDERPASS_DEPTH_ATTACHMENT] == pass->depthView, "gpu-renderpass/row-depth-is-pass's-view",
            "row 1 is the PASS's depth view, the same handle in every row - the pass owns the depth image a"
            "nd its view, and a row that had its own would be a second depth buffer for the same pass");
        xud_check(row->createInfo.attachmentCount == XUD_RENDERPASS_ATTACHMENT_COUNT, "gpu-renderpass/row-attachment-count",
            "the create-info the driver was handed names two attachments, exactly the pass's count");
        xud_check((row->createInfo.width == context->swapchain->createInfo.imageExtent.width)
                && (row->createInfo.height == context->swapchain->createInfo.imageExtent.height),
            "gpu-renderpass/row-extent-matches-both-attachments",
            "the row's extent is the extent BOTH attachments were created at, which is the attachment-exten"
            "t rule read against the driver rather than against a synthetic record");
    }

    if (context->validate != 0u) {
        xud_check(context->diagnostics->errorMessages == 0u, "gpu-renderpass/validation-clean",
            "the image, the view, the two attachment rows and the two framebuffer rows were all created val"
            "idation-clean, which is the aspect rule, the extent rule, the sample-count rule and the layout-"
            "transition rule read against the driver rather than left to it - the one ERROR line in the run i"
            "s still the diagnostics layer's own synthetic headless probe");
    }

    result = xud_close_context(&context);
    xud_check(result == XUD_OK, "gpu-renderpass/close-clean",
        "a context that owns a depth image tears back down with nothing refused, which is the pass's retire o"
        "rder (pass, then view, then image, then the allocator release) holding against the driver - a VkImag"
        "e bound to memory the allocator has freed is an image naming freed memory");
    xud_check(context == NULL, "gpu-renderpass/close-clears", "and the out-param is cleared");
}

void xud_test_renderpass(void) {
    xud_test_renderpass_pure();

    if (xud_gpu_tests_enabled()) {
        xud_test_renderpass_gpu();
    }
}
