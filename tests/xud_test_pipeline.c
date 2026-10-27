/***********************************************************************************************
 *
 *   xud_test_pipeline.c - the pipeline rung: one graphics pipeline, its layout, its two modules
 *
 *   Owns : the layout assembly that refuses a descriptor record whose set layouts or uniform buffer are not live, the shader-stage rows,
 * the pipeline create-info assembly and the state assembly that writes the STARTING zero
 * vertex-input row plus an ON depth pair against the pass's depth attachment, the vertex-input
 * overwrite that replaces that zero row with the batch rung's four binding and four attribute
 * rows - and says why. Layer: tests. Sees xud_pipeline.h, xud_batch.h and xud_descriptor.h:
 * the layout names the two set layouts and the push-constant range the descriptor rung owns, and
 * the batch's four rows are what the pipeline's vertex-input state is handed, so both records are
 * inputs to what is checked here.
 *
 *   Deterministic: fixed loop counts, no time, no randomness, no filesystem.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud.h" /* Required for: XudResult, XUD_VERSION, the XUD_GPU_TESTS gate */
#include "xud_batch.h" /* Required for: XudBatch, the four vertex-input rows the pipeline's state is handed */
#include "xud_descriptor.h" /* Required for: XudDescriptor, the record the layout assembly is handed */
#include "xud_pipeline.h" /* Required for: XudPipeline, the fill rungs and the entry-point name */
#include "xud_shader.h" /* Required for: XudShader, the two modules the pipeline stage rows name */
#include "xud_test_harness.h" /* Required for: xud_check, xud_gpu_tests_enabled, the area entry points */
#include <string.h> /* Required for: memset, strcmp, the synthetic rows this area zeroes and compares */

/*-----------------------------------------------------------------------------------------------
 * The pipeline rung, headless: the fixed state, the two shader-stage rows, the empty layout and
 * the create-info assembly. Nothing here calls the driver, which is what makes the whole of thi
 * s rung testable with no device present - the assembly is the half the repo's headless rule lea
 * ns on, and the create-info it writes is the exact bytes vkCreateGraphicsPipelines is handed.
 * The two module handles are synthetic on purpose: the pure half never dereferences one, it only
 * copies it into a row.
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_pipeline_pure(void) {
    XudPipeline pipeline = (XudPipeline) { 0 };
    XudPipeline bare = (XudPipeline) { 0 };
    const VkShaderModule vertModule = (VkShaderModule)0x1000u;
    const VkShaderModule fragModule = (VkShaderModule)0x2000u;
    VkPipelineLayoutCreateInfo layout = (VkPipelineLayoutCreateInfo) { .sType = VK_STRUCTURE_TYPE_MAX_ENUM };
    VkGraphicsPipelineCreateInfo out = (VkGraphicsPipelineCreateInfo) { .sType = VK_STRUCTURE_TYPE_MAX_ENUM };
    VkGraphicsPipelineCreateInfo transparentInfo = (VkGraphicsPipelineCreateInfo) { .sType = VK_STRUCTURE_TYPE_MAX_ENUM };
    XudDescriptor desc = (XudDescriptor) { 0 };

    xud_check(
        xud_pipeline_fill_state(NULL) == XUD_INVALID_ARG, "pipeline/fill-state-null-out", "an out-param that cannot be filled is refused");
    xud_check(xud_pipeline_fill_state(&pipeline) == XUD_OK, "pipeline/fill-state-ok", "the assembly completes");

    xud_check(pipeline.vertexInput.sType == VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, "pipeline/vertex-input-stamped",
        "every row is stamped, which is the same rule the physical layer records for a driver");
    xud_check(pipeline.vertexInput.vertexBindingDescriptionCount == 0u, "pipeline/zero-bindings",
        "zero vertex bindings IN THE ROW fill_state writes: that row is the starting state of the field, "
        "and xud_pipeline_fill_vertex_input below is what overwrites it with the batch's four rows");
    xud_check(pipeline.vertexInput.vertexAttributeDescriptionCount == 0u, "pipeline/zero-attributes",
        "and zero attributes, the other half of the same rule");
    xud_check((pipeline.vertexInput.pVertexBindingDescriptions == NULL) && (pipeline.vertexInput.pVertexAttributeDescriptions == NULL),
        "pipeline/binding-arrays-null",
        "both counts are paired with a NULL array, so a driver that reads past a count reads "
        "nothing");

    /* The vertex-input OVERWRITE, which is the other half of the field's life: a batch whose four rows
       are filled, handed to the pipeline. The rows are filled by hand rather than by xud_batch_open
       because open creates six buffers and needs a device, and the row VALUES are all the overwrite
       reads - so a synthetic batch that carries them is the same input the renderer hands.
    */
    {
        XudBatch batch = (XudBatch) { 0 };
        XudPipeline withBatch = (XudPipeline) { 0 };
        XudBatch empty = (XudBatch) { 0 };
        XudPipeline refused = (XudPipeline) { 0 };

        batch.bindings[XUD_BATCH_STREAM_POS].binding = XUD_BATCH_STREAM_POS;
        batch.bindings[XUD_BATCH_STREAM_UV].binding = XUD_BATCH_STREAM_UV;
        batch.bindings[XUD_BATCH_STREAM_COLOR].binding = XUD_BATCH_STREAM_COLOR;
        batch.bindings[XUD_BATCH_STREAM_NORMAL].binding = XUD_BATCH_STREAM_NORMAL;
        batch.bindings[XUD_BATCH_STREAM_POS].stride = XUD_BATCH_STREAM_POS_SIZE;
        batch.bindings[XUD_BATCH_STREAM_UV].stride = XUD_BATCH_STREAM_UV_SIZE;
        batch.bindings[XUD_BATCH_STREAM_COLOR].stride = XUD_BATCH_STREAM_COLOR_SIZE;
        batch.bindings[XUD_BATCH_STREAM_NORMAL].stride = XUD_BATCH_STREAM_NORMAL_SIZE;
        batch.attributes[XUD_BATCH_STREAM_POS].location = XUD_BATCH_STREAM_POS;
        batch.attributes[XUD_BATCH_STREAM_UV].location = XUD_BATCH_STREAM_UV;
        batch.attributes[XUD_BATCH_STREAM_COLOR].location = XUD_BATCH_STREAM_COLOR;
        batch.attributes[XUD_BATCH_STREAM_NORMAL].location = XUD_BATCH_STREAM_NORMAL;
        batch.attributes[XUD_BATCH_STREAM_POS].format = XUD_BATCH_STREAM_FORMAT(XUD_BATCH_STREAM_POS);
        batch.attributes[XUD_BATCH_STREAM_UV].format = XUD_BATCH_STREAM_FORMAT(XUD_BATCH_STREAM_UV);
        batch.attributes[XUD_BATCH_STREAM_COLOR].format = XUD_BATCH_STREAM_FORMAT(XUD_BATCH_STREAM_COLOR);
        batch.attributes[XUD_BATCH_STREAM_NORMAL].format = XUD_BATCH_STREAM_FORMAT(XUD_BATCH_STREAM_NORMAL);

        xud_check(xud_pipeline_fill_vertex_input(&withBatch, &batch) == XUD_OK, "pipeline/vertex-input-from-batch",
            "the overwrite completes against a batch whose four binding numbers and four attribute locations are the stream numbers 0..3, "
            "which is both what the pipeline's vertex-input row is about to claim and what vkCmdBindVertexBuffers will bind");
        xud_check((withBatch.vertexInput.vertexBindingDescriptionCount == XUD_BATCH_STREAM_COUNT)
                && (withBatch.vertexInput.vertexAttributeDescriptionCount == XUD_BATCH_STREAM_COUNT),
            "pipeline/four-bindings-four-attributes",
            "four of each, the batch's four streams, where the row fill_state writes had zero of each");
        xud_check(withBatch.vertexInput.pVertexBindingDescriptions == (const VkVertexInputBindingDescription*)batch.bindings,
            "pipeline/binding-array-is-the-batch's",
            "the row NAMES the batch's own array rather than a copy of it, which is what makes the batch's life the pipeline's subject");
        xud_check(withBatch.vertexInput.pVertexAttributeDescriptions == (const VkVertexInputAttributeDescription*)batch.attributes,
            "pipeline/attribute-array-is-the-batch's", "the same rule on the attribute side");

        xud_check(xud_pipeline_fill_vertex_input(NULL, &batch) == XUD_INVALID_ARG, "pipeline/fill-vertex-input-null-pipeline",
            "an out-param that cannot be filled is refused");
        xud_check(xud_pipeline_fill_vertex_input(&withBatch, NULL) == XUD_INVALID_ARG, "pipeline/fill-vertex-input-null-batch",
            "a pipeline with no batch to name has no vertex input to write, which is a refusal rather than a zero row");
        xud_check(xud_pipeline_fill_vertex_input(&refused, &empty) == XUD_INVALID_ARG, "pipeline/fill-vertex-input-zero-row-refused",
            "a batch whose rows are still all zero never went through xud_batch_open's fill, and a pipeline built on that row "
            "would read a binding number and a location that name no stream at all");
        xud_check(refused.vertexInput.vertexBindingDescriptionCount == 0u, "pipeline/refused-row-untouched",
            "a refused overwrite leaves the row at the state fill_state wrote it in, rather than half-writing it");
    }

    xud_check(pipeline.inputAssembly.topology == XUD_PIPELINE_TOPOLOGY, "pipeline/topology",
        "a triangle list, which is what vkCmdDrawIndexed's indexCount means over the batch's index buffer");
    xud_check(pipeline.inputAssembly.primitiveRestartEnable == VK_FALSE, "pipeline/no-primitive-restart",
        "no restart index, so every vertex of the count is a vertex of the triangle");
    xud_check((pipeline.viewportState.viewportCount == 1u) && (pipeline.viewportState.scissorCount == 1u),
        "pipeline/one-viewport-one-scissor", "one viewport and one scissor, both dynamic");
    xud_check((pipeline.viewportState.pViewports == NULL) && (pipeline.viewportState.pScissors == NULL), "pipeline/viewport-arrays-null",
        "the arrays are NULL because the state is dynamic - but the ROW is not, which is the spec's "
        "own rule for a fragment stage that reads gl_FragCoord");
    xud_check(pipeline.rasterizer.cullMode == XUD_PIPELINE_CULL_MODE, "pipeline/cull-mode",
        "nothing is culled, which is what makes a triangle of any winding visible and the rule a "
        "stated one");
    xud_check(pipeline.rasterizer.frontFace == XUD_PIPELINE_FRONT_FACE, "pipeline/front-face",
        "the winding the shaders are written for, recorded even though nothing is culled");
    xud_check((pipeline.rasterizer.polygonMode == VK_POLYGON_MODE_FILL) && (pipeline.rasterizer.rasterizerDiscardEnable == VK_FALSE),
        "pipeline/fill-and-no-discard",
        "filled, and rasterizerDiscardEnable false, which is the one bit that would discard every "
        "fragment");
    xud_check(pipeline.rasterizer.lineWidth == 1.0f, "pipeline/line-width", "one, the only honest value in that field of this row today");
    xud_check(pipeline.rasterizer.depthBiasEnable == VK_FALSE, "pipeline/no-depth-bias",
        "no depth bias, and the three factors beside it are written as zeros rather than left at "
        "whatever calloc left");
    xud_check(pipeline.multisample.rasterizationSamples == XUD_PIPELINE_SAMPLE_COUNT, "pipeline/sample-count",
        "one sample, which is the only sample count the framebuffer of a swapchain image is asked "
        "for today");
    xud_check((pipeline.multisample.alphaToCoverageEnable == VK_FALSE) && (pipeline.multisample.alphaToOneEnable == VK_FALSE)
            && (pipeline.multisample.pSampleMask == NULL),
        "pipeline/no-msaa",
        "no MSAA at all, which is the gate the framebuffer's sample-count limits would open and "
        "this rung does not take");
    xud_check((pipeline.depthStencil.depthTestEnable == VK_TRUE) && (pipeline.depthStencil.depthWriteEnable == VK_TRUE),
        "pipeline/depth-pair-on",
        "the ON pair, and it is honest now because the render pass under this pipeline HAS a depth"
        " attachment (xud_renderpass.c's own depth row, its own VkImage): a depth test against no de"
        "pth buffer is implementation-dependent, which is the same trap the no-vertex-input check na"
        "mes for the other half of the same note. This is the OPAQUE variant of the plan's opaque/tran"
        "sparent pair - blend off above, depth write here - and the transparent variant (blend on, de"
        "pth test, sorted) is the batch step's, because sorting is a property of the batch and not of"
        " a pipeline");
    xud_check(pipeline.depthStencil.depthCompareOp == XUD_PIPELINE_DEPTH_COMPARE_OP, "pipeline/depth-compare-op",
        "LESS, and it is the rule the enabled test above actually uses now: the pass clears the buffe"
        "r to XUD_RENDERPASS_CLEAR_DEPTH 1.0, the far value of this very rule, so the first fragment"
        " of a frame always wins against a defined buffer rather than against whatever the allocato"
        "r handed back");
    xud_check(pipeline.depthStencil.stencilTestEnable == VK_FALSE, "pipeline/no-stencil-test",
        "no stencil anywhere in this library, and both stencil rows are written as the disabled se"
        "t rather than left at zero");
    xud_check((pipeline.depthStencil.minDepthBounds == 0.0f) && (pipeline.depthStencil.maxDepthBounds == 1.0f),
        "pipeline/depth-bounds-full", "the full range, which is what a disabled depth-bounds test has to say beside it");
    xud_check(pipeline.blendAttachment[XUD_BATCH_VARIANT_OPAQUE].blendEnable == VK_FALSE, "pipeline/attachment-blend-disabled",
        "blend disabled at the OPAQUE row of the variant list, which is the plan's opaque half and"
        " the row the batch's default variant indexes today");
    xud_check((pipeline.blendAttachment[XUD_BATCH_VARIANT_OPAQUE].srcColorBlendFactor == VK_BLEND_FACTOR_ONE)
            && (pipeline.blendAttachment[XUD_BATCH_VARIANT_OPAQUE].dstColorBlendFactor == VK_BLEND_FACTOR_ZERO),
        "pipeline/blend-factors-overwrite",
        "ONE/ZERO at the opaque row, the identity of an overwrite, written even though blending is "
        "off so that the row says a complete blend");
    xud_check((pipeline.blendAttachment[XUD_BATCH_VARIANT_OPAQUE].srcAlphaBlendFactor == VK_BLEND_FACTOR_ONE)
            && (pipeline.blendAttachment[XUD_BATCH_VARIANT_OPAQUE].dstAlphaBlendFactor == VK_BLEND_FACTOR_ZERO),
        "pipeline/blend-alpha-factors-overwrite",
        "the alpha half says the same at the opaque row: leaving it at ZERO/ZERO would be a blend "
        "that writes 0 alpha into the swapchain the moment a variant enables blending");
    xud_check(pipeline.blendAttachment[XUD_BATCH_VARIANT_OPAQUE].colorWriteMask
            == (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT),
        "pipeline/color-write-mask",
        "every component is written at the opaque row, alpha included, which is what a swapchain "
        "with OPAQUE composite needs");
    xud_check(pipeline.blending[XUD_BATCH_VARIANT_OPAQUE].attachmentCount == 1u, "pipeline/blend-attachment-count",
        "one blend attachment at the opaque row, which is the pass's one colour attachment read fr"
        "om the other side");
    xud_check(pipeline.blending[XUD_BATCH_VARIANT_OPAQUE].pAttachments == &pipeline.blendAttachment[XUD_BATCH_VARIANT_OPAQUE],
        "pipeline/blend-attachment-row",
        "the row points at the record's OWN opaque attachment, so the record and the create-info t"
        "he variant's create call is handed cannot disagree");
    xud_check((pipeline.blending[XUD_BATCH_VARIANT_OPAQUE].logicOpEnable == VK_FALSE)
            && (pipeline.blending[XUD_BATCH_VARIANT_OPAQUE].logicOp == XUD_PIPELINE_LOGIC_OP),
        "pipeline/no-logic-op",
        "no logic op at the opaque row, with the enum's own COPY value written beside it because t"
        "he field has to hold a valid enum");
    xud_check((pipeline.blending[XUD_BATCH_VARIANT_OPAQUE].blendConstants[0] == 0.0f)
            && (pipeline.blending[XUD_BATCH_VARIANT_OPAQUE].blendConstants[3] == 0.0f),
        "pipeline/blend-constants-zero",
        "the four blend constants are zeros at the opaque row, which is what a blend that does not "
        "use them still says");
    xud_check(pipeline.blendAttachment[XUD_BATCH_VARIANT_TRANSPARENT].blendEnable == VK_TRUE, "pipeline/attachment-blend-enabled",
        "blend ENABLED at the TRANSPARENT row of the same list, which is the plan's other half and "
        "the row a draw row naming XUD_BATCH_VARIANT_TRANSPARENT selects");
    xud_check((pipeline.blendAttachment[XUD_BATCH_VARIANT_TRANSPARENT].srcColorBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA)
            && (pipeline.blendAttachment[XUD_BATCH_VARIANT_TRANSPARENT].dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA),
        "pipeline/blend-factors-raylib-default",
        "SRC_ALPHA/ONE_MINUS_SRC_ALPHA at the transparent row, the Vulkan form of raylib's default "
        "blend: rlgl.h 2127's glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA), and BLEND_ALPHA is "
        "raylib's default mode (raylib.h 918, the enum's zeroth entry, commented \"(default)\")");
    xud_check(pipeline.blendAttachment[XUD_BATCH_VARIANT_TRANSPARENT].colorBlendOp == XUD_PIPELINE_BLEND_OP, "pipeline/blend-op-both-rows",
        "ADD at the transparent row, the same equation the opaque row carries - rlgl.h 2127's glBlendEquation(GL_FUNC_ADD) "
        "is one equation for both rows, the FACTORS are what differ between them");
    xud_check((pipeline.blendAttachment[XUD_BATCH_VARIANT_TRANSPARENT].srcAlphaBlendFactor == VK_BLEND_FACTOR_ONE)
            && (pipeline.blendAttachment[XUD_BATCH_VARIANT_TRANSPARENT].dstAlphaBlendFactor == VK_BLEND_FACTOR_ZERO),
        "pipeline/blend-alpha-factors-both-rows",
        "the alpha half stays ONE/ZERO under BOTH rows: the swapchain's alpha is composited against "
        "nothing here, and writing 1.0 rather than 0.0 is what a later compositor wants");
    xud_check(pipeline.blendAttachment[XUD_BATCH_VARIANT_TRANSPARENT].colorWriteMask
            == (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT),
        "pipeline/color-write-mask-both-rows",
        "every component is written at both rows, so the transparent row is a blend of the drawn colour "
        "over the stored one rather than a partial write");
    xud_check(pipeline.blending[XUD_BATCH_VARIANT_TRANSPARENT].pAttachments == &pipeline.blendAttachment[XUD_BATCH_VARIANT_TRANSPARENT],
        "pipeline/blend-attachment-row-transparent",
        "the transparent row points at the record's OWN transparent attachment, which is what makes "
        "the variant's create-info and the variant's attachment the same bytes");
    xud_check(pipeline.blendAttachment[XUD_BATCH_VARIANT_OPAQUE].blendEnable
            != pipeline.blendAttachment[XUD_BATCH_VARIANT_TRANSPARENT].blendEnable,
        "pipeline/blend-rows-differ",
        "the two rows differ in exactly the one field the variant is about, which is what makes the "
        "variant index a pipeline choice rather than a decoration on one pipeline");
    xud_check(pipeline.dynamicState.dynamicStateCount == XUD_PIPELINE_DYNAMIC_STATE_COUNT, "pipeline/dynamic-count", "two dynamic states");
    xud_check(pipeline.dynamicState.pDynamicStates == pipeline.dynamicStates, "pipeline/dynamic-array",
        "the row points into the owning struct, which is what keeps it valid past the call");
    xud_check((pipeline.dynamicStates[XUD_PIPELINE_VIEWPORT_STATE] == VK_DYNAMIC_STATE_VIEWPORT)
            && (pipeline.dynamicStates[XUD_PIPELINE_SCISSOR_STATE] == VK_DYNAMIC_STATE_SCISSOR),
        "pipeline/dynamic-two",
        "viewport and scissor, which is what makes one pipeline serve every window extent instead "
        "of one pipeline per size");

    xud_check(xud_pipeline_fill_shader_stages(NULL, vertModule, fragModule) == XUD_INVALID_ARG, "pipeline/stages-null-out",
        "an out-param that cannot be filled is refused");
    xud_check(xud_pipeline_fill_shader_stages(pipeline.shaderStages, vertModule, fragModule) == XUD_OK, "pipeline/stages-ok",
        "the two rows are filled");
    xud_check(pipeline.shaderStages[XUD_PIPELINE_VERTEX_STAGE].stage == VK_SHADER_STAGE_VERTEX_BIT, "pipeline/vertex-stage-bit",
        "the vertex row carries the vertex stage");
    xud_check(
        pipeline.shaderStages[XUD_PIPELINE_VERTEX_STAGE].module == vertModule, "pipeline/vertex-module", "and the module handed for it");
    xud_check(pipeline.shaderStages[XUD_PIPELINE_FRAGMENT_STAGE].stage == VK_SHADER_STAGE_FRAGMENT_BIT, "pipeline/fragment-stage-bit",
        "the fragment row carries the fragment stage");
    xud_check(pipeline.shaderStages[XUD_PIPELINE_FRAGMENT_STAGE].module == fragModule, "pipeline/fragment-module", "and its module");
    xud_check((strcmp(pipeline.shaderStages[XUD_PIPELINE_VERTEX_STAGE].pName, XUD_PIPELINE_ENTRY_POINT) == 0)
            && (strcmp(pipeline.shaderStages[XUD_PIPELINE_FRAGMENT_STAGE].pName, XUD_PIPELINE_ENTRY_POINT) == 0),
        "pipeline/stages-entry-main",
        "both rows name the entry point by name rather than leaving it NULL: the validation layer o"
        "n this machine reports a NULL pName as a hard error against both pName VUIDs (probed tod"
        "ay), so the honest form is the name glslangValidator writes");
    xud_check(pipeline.shaderStages[XUD_PIPELINE_VERTEX_STAGE].pSpecializationInfo == NULL, "pipeline/no-specialization",
        "no specialization info, so the pipeline is built from the shaders exactly as compiled");

    xud_check(xud_pipeline_fill_layout_info(NULL, NULL) == XUD_INVALID_ARG, "pipeline/layout-null-both",
        "a layout with no descriptor record to read is refused, and so is one with nowhere to be w"
        "ritten");
    xud_check(xud_pipeline_fill_layout_info(&desc, &layout) == XUD_INVALID_ARG, "pipeline/layout-no-set-handles",
        "a record with no set-0 layout handle is a bad argument: the pipeline layout names the set"
        "the pool allocated, so a record that cannot name one is not a layout this function can a"
        "ssemble");
    desc.setLayouts[XUD_DESCRIPTOR_SET_PER_FRAME] = (VkDescriptorSetLayout)0x4100u;
    xud_check(xud_pipeline_fill_layout_info(&desc, &layout) == XUD_INVALID_ARG, "pipeline/layout-no-set-1-handle",
        "set 1's layout handle is required too: the layout declares both sets, so a record that de"
        "clares a set whose layout was never created is a record that cannot be assembled");
    desc.setLayouts[XUD_DESCRIPTOR_SET_MATERIAL] = (VkDescriptorSetLayout)0x4200u;
    xud_check(xud_pipeline_fill_layout_info(&desc, &layout) == XUD_INVALID_ARG, "pipeline/layout-no-buffer",
        "and the uniform buffer is required as well: a layout that declares set 0 with no buffer b"
        "ound into it is a layout whose per-frame data has nowhere to come from");
    desc.uniformBuffer = (VkBuffer)0x5000u;
    xud_check(xud_pipeline_fill_layout_info(&desc, &layout) == XUD_OK, "pipeline/layout-ok",
        "the layout is assembled from the descriptor record below this rung");
    xud_check(layout.sType == VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, "pipeline/layout-stamped", "stamped");
    xud_check((layout.pNext == NULL) && (layout.flags == 0u), "pipeline/layout-chain-clear", "nothing is chained into it");
    xud_check(layout.setLayoutCount == XUD_DESCRIPTOR_SET_COUNT, "pipeline/layout-two-sets",
        "two descriptor sets, the plan's set 0 per-frame and set 1 per-material");
    xud_check(layout.pSetLayouts == desc.setLayouts, "pipeline/layout-set-array-is-the-record's",
        "the row points at the descriptor rung's OWN array rather than a copy of it, which is wha"
        "t makes the pipeline layout and the set the pool allocated from those same handles agree "
        "by construction");
    xud_check((layout.pushConstantRangeCount == 1u) && (layout.pPushConstantRanges == &desc.pushRange), "pipeline/layout-one-push-range",
        "one push-constant range, the descriptor rung's own row: the spec allows at most one in a"
        "layout, so the one range is a bound on this rung's row rather than a list it could grow");

    xud_check(xud_pipeline_fill_create_info(NULL, XUD_BATCH_VARIANT_OPAQUE, &out) == XUD_INVALID_ARG, "pipeline/create-no-record",
        "a create-info with no record to assemble from is refused");
    xud_check(xud_pipeline_fill_create_info(&pipeline, XUD_BATCH_VARIANT_OPAQUE, NULL) == XUD_INVALID_ARG, "pipeline/create-null-out",
        "and so is one with nowhere to be written");
    xud_check(xud_pipeline_fill_create_info(&pipeline, XUD_BATCH_VARIANT_COUNT, &out) == XUD_INVALID_ARG,
        "pipeline/create-variant-past-list",
        "a variant at or past the batch's variant count is refused rather than a row read off the end "
        "of the arrays - the same rule the batch's own invariant check applies to a draw row's variant");
    xud_check(xud_pipeline_fill_create_info(&bare, XUD_BATCH_VARIANT_OPAQUE, &out) == XUD_INVALID_ARG, "pipeline/create-no-layout-handle",
        "a record with no layout handle is a bad argument, because the layout is an argument of the "
        "create call");
    bare.layout = (VkPipelineLayout)0x9000u;
    xud_check(xud_pipeline_fill_create_info(&bare, XUD_BATCH_VARIANT_OPAQUE, &out) == XUD_INVALID_ARG, "pipeline/create-no-pass-handle",
        "and so is a record that names no render pass: the core-1.4 binding makes the pass an argu"
        "ment of the create as well, so a record that cannot name one is a bad argument of this f"
        "unction, not a create-info that happens to carry VK_NULL_HANDLE");
    bare.renderPass = (VkRenderPass)0x3000u;
    xud_check(xud_pipeline_fill_create_info(&bare, XUD_BATCH_VARIANT_OPAQUE, &out) == XUD_UNSUPPORTED, "pipeline/create-unstamped-rows",
        "a record whose rows were never stamped is a bad state of THIS layer, reported as such rat"
        "her than assembled from calloc's zeros");
    pipeline.layout = (VkPipelineLayout)0x9000u;
    pipeline.renderPass = (VkRenderPass)0x3000u;
    xud_check(xud_pipeline_fill_create_info(&pipeline, XUD_BATCH_VARIANT_OPAQUE, &out) == XUD_OK, "pipeline/create-opaque-ok",
        "the opaque row's create-info is assembled from the record");
    xud_check(out.pColorBlendState == &pipeline.blending[XUD_BATCH_VARIANT_OPAQUE], "pipeline/create-blend-row-opaque",
        "the create-info's blend pointer is the OPAQUE row's, which is what makes the variant argume"
        "nt and the blend state the same number");
    xud_check(out.sType == VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, "pipeline/create-stamped", "stamped");
    xud_check((out.pNext == NULL) && (out.flags == 0u), "pipeline/create-chain-clear", "nothing is chained into it");
    xud_check((out.stageCount == XUD_PIPELINE_STAGE_COUNT) && (out.pStages == pipeline.shaderStages), "pipeline/create-two-stages",
        "two stages, and the row is the record's own");
    xud_check(out.pTessellationState == NULL, "pipeline/create-no-tessellation",
        "no tessellation row at all, which is the stage array's own shape read from the other side");
    xud_check((out.pVertexInputState == &pipeline.vertexInput) && (out.pInputAssemblyState == &pipeline.inputAssembly)
            && (out.pViewportState == &pipeline.viewportState) && (out.pRasterizationState == &pipeline.rasterizer)
            && (out.pMultisampleState == &pipeline.multisample) && (out.pDepthStencilState == &pipeline.depthStencil)
            && (out.pColorBlendState == &pipeline.blending[XUD_BATCH_VARIANT_OPAQUE]) && (out.pDynamicState == &pipeline.dynamicState),
        "pipeline/create-rows-in-record",
        "every state pointer names the record's own row, so the record and the create-info the "
        "driver is handed are the same bytes and cannot disagree");
    xud_check(out.layout == pipeline.layout, "pipeline/create-layout-handle",
        "the layout handle the pipeline was created with is recorded in the record, which is what "
        "makes the teardown order (pipeline first, layout second) the only one that is safe");
    xud_check((out.renderPass == pipeline.renderPass) && (out.subpass == XUD_PIPELINE_FIRST_SUBPASS), "pipeline/create-pass-binding",
        "the core-1.4 pair that ties a pipeline to a render pass at creation time IS set, to the "
        "handle recorded in the record: VUID-VkGraphicsPipelineCreateInfo-dynamicRendering-06576 "
        "makes it required while dynamicRendering is not enabled, and the validation layer on thi"
        "s machine reports VK_NULL_HANDLE there as a hard error (probed today)");
    xud_check((out.basePipelineHandle == VK_NULL_HANDLE) && (out.basePipelineIndex == -1), "pipeline/create-no-base-pipeline",
        "the spec's own pairing for a pipeline that is not derived from another: a null handle wit"
        "h -1, because 0 would name pipeline row 0 of a derivation this rung does not take");

    /* The same assembly at the OTHER row of the list, which is what makes the variant argument of
       fill_create_info a choice rather than a decoration: the two create-infos are the same record
       read at two rows, and the one row they differ in is the blend half. */
    xud_check(xud_pipeline_fill_create_info(&pipeline, XUD_BATCH_VARIANT_TRANSPARENT, &transparentInfo) == XUD_OK,
        "pipeline/create-transparent-ok", "the transparent row's create-info assembles too");
    xud_check(transparentInfo.pColorBlendState == &pipeline.blending[XUD_BATCH_VARIANT_TRANSPARENT],
        "pipeline/create-blend-row-transparent",
        "and its blend pointer is the TRANSPARENT row's, so the variant argument picks the blend "
        "state the create call is handed rather than always the opaque one");
    xud_check(transparentInfo.pColorBlendState != out.pColorBlendState, "pipeline/create-blend-rows-differ",
        "the two create-infos differ in the blend row, which is the whole of what a variant is at "
        "this rung: one layout, one shader pair, one of every other row");
    xud_check(transparentInfo.layout == out.layout, "pipeline/create-one-layout-both-rows",
        "both rows name the SAME layout, which is what makes the layout one object for the whole "
        "list rather than a per-variant record of the same interface");
    xud_check(transparentInfo.pStages == out.pStages, "pipeline/create-one-shader-pair",
        "and the same two shader stages, so the variant is a blend-state choice and not a shader c"
        "hoice");
}

void xud_test_pipeline(void) { xud_test_pipeline_pure(); }
