/***********************************************************************************************
 *
 *   xud_test_pipeline.c - the pipeline rung: one graphics pipeline, its layout, its two modules
 *
 *   Owns : the layout assembly that refuses a descriptor record whose set layouts or uniform buffer are not live, the shader-stage rows,
 * the pipeline create-info assembly and the state assembly that writes zero vertex bindings and no depth attachment - and says why. Layer:
 * tests. Sees xud_pipeline.h and xud_descriptor.h: the layout names the two set layouts and the push-constant range the descriptor rung
 * owns, so that record is an input to what is checked here.
 *
 *   Deterministic: fixed loop counts, no time, no randomness, no filesystem.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud.h" /* Required for: XudResult, XUD_VERSION, the XUD_GPU_TESTS gate */
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
    XudDescriptor desc = (XudDescriptor) { 0 };

    xud_check(
        xud_pipeline_fill_state(NULL) == XUD_INVALID_ARG, "pipeline/fill-state-null-out", "an out-param that cannot be filled is refused");
    xud_check(xud_pipeline_fill_state(&pipeline) == XUD_OK, "pipeline/fill-state-ok", "the assembly completes");

    xud_check(pipeline.vertexInput.sType == VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, "pipeline/vertex-input-stamped",
        "every row is stamped, which is the same rule the physical layer records for a driver");
    xud_check(pipeline.vertexInput.vertexBindingDescriptionCount == 0u, "pipeline/zero-bindings",
        "zero vertex bindings: there is still no VkBuffer and no vertex stream above the allocator, "
        "so the vertex shader generates its vertices from gl_VertexIndex instead");
    xud_check(pipeline.vertexInput.vertexAttributeDescriptionCount == 0u, "pipeline/zero-attributes",
        "and zero attributes, the other half of the same rule");
    xud_check((pipeline.vertexInput.pVertexBindingDescriptions == NULL) && (pipeline.vertexInput.pVertexAttributeDescriptions == NULL),
        "pipeline/binding-arrays-null",
        "both counts are paired with a NULL array, so a driver that reads past a count reads "
        "nothing");
    xud_check(pipeline.inputAssembly.topology == XUD_PIPELINE_TOPOLOGY, "pipeline/topology",
        "a triangle list, which is what vkCmdDraw's vertexCount means");
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
    xud_check((pipeline.depthStencil.depthTestEnable == VK_FALSE) && (pipeline.depthStencil.depthWriteEnable == VK_FALSE),
        "pipeline/depth-pair-off",
        "the honest half of the depth note: no depth ATTACHMENT exists in this tree, so no test "
        "is enabled - an enabled test against an undefined depth buffer is implementation-dependen"
        "t");
    xud_check(pipeline.depthStencil.depthCompareOp == XUD_PIPELINE_DEPTH_COMPARE_OP, "pipeline/depth-compare-op",
        "LESS recorded anyway, so the constant is not dead state: it is the rule the depth buffer "
        "will use the day the attachment lands");
    xud_check(pipeline.depthStencil.stencilTestEnable == VK_FALSE, "pipeline/no-stencil-test",
        "no stencil anywhere in this library, and both stencil rows are written as the disabled se"
        "t rather than left at zero");
    xud_check((pipeline.depthStencil.minDepthBounds == 0.0f) && (pipeline.depthStencil.maxDepthBounds == 1.0f),
        "pipeline/depth-bounds-full", "the full range, which is what a disabled depth-bounds test has to say beside it");
    xud_check(pipeline.blendAttachment.blendEnable == VK_FALSE, "pipeline/attachment-blend-disabled",
        "blend disabled at the attachment, which is the plan's opaque variant and the only varian"
        "t there is today");
    xud_check((pipeline.blendAttachment.srcColorBlendFactor == VK_BLEND_FACTOR_ONE)
            && (pipeline.blendAttachment.dstColorBlendFactor == VK_BLEND_FACTOR_ZERO),
        "pipeline/blend-factors-overwrite",
        "ONE/ZERO, the identity of an overwrite, written even though blending is off so that the "
        "day a variant enables it the factors are already the right pair");
    xud_check((pipeline.blendAttachment.srcAlphaBlendFactor == VK_BLEND_FACTOR_ONE)
            && (pipeline.blendAttachment.dstAlphaBlendFactor == VK_BLEND_FACTOR_ZERO),
        "pipeline/blend-alpha-factors-overwrite",
        "the alpha half says the same: leaving it at ZERO/ZERO would be a blend that writes 0 alph"
        "a into the swapchain the moment a later variant enables blending");
    xud_check(pipeline.blendAttachment.colorWriteMask
            == (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT),
        "pipeline/color-write-mask",
        "every component is written, alpha included, which is what a swapchain with OPAQUE composi"
        "te needs");
    xud_check(pipeline.blending.attachmentCount == 1u, "pipeline/blend-attachment-count",
        "one blend attachment, which is the pass's one colour attachment read from the other side");
    xud_check(pipeline.blending.pAttachments == &pipeline.blendAttachment, "pipeline/blend-attachment-row",
        "the row points at the record's own attachment, so the record and the create-info the drive"
        "r is handed cannot disagree");
    xud_check((pipeline.blending.logicOpEnable == VK_FALSE) && (pipeline.blending.logicOp == XUD_PIPELINE_LOGIC_OP), "pipeline/no-logic-op",
        "no logic op, with the enum's own COPY value written beside it because the field has to ho"
        "ld a valid enum");
    xud_check((pipeline.blending.blendConstants[0] == 0.0f) && (pipeline.blending.blendConstants[3] == 0.0f),
        "pipeline/blend-constants-zero",
        "the four blend constants are written as zeros, which is what a blend that does not use th"
        "em still says");
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

    xud_check(xud_pipeline_fill_create_info(NULL, &out) == XUD_INVALID_ARG, "pipeline/create-no-record",
        "a create-info with no record to assemble from is refused");
    xud_check(xud_pipeline_fill_create_info(&pipeline, NULL) == XUD_INVALID_ARG, "pipeline/create-null-out",
        "and so is one with nowhere to be written");
    xud_check(xud_pipeline_fill_create_info(&bare, &out) == XUD_INVALID_ARG, "pipeline/create-no-layout-handle",
        "a record with no layout handle is a bad argument, because the layout is an argument of the "
        "create call");
    bare.layout = (VkPipelineLayout)0x9000u;
    xud_check(xud_pipeline_fill_create_info(&bare, &out) == XUD_INVALID_ARG, "pipeline/create-no-pass-handle",
        "and so is a record that names no render pass: the core-1.4 binding makes the pass an argu"
        "ment of the create as well, so a record that cannot name one is a bad argument of this f"
        "unction, not a create-info that happens to carry VK_NULL_HANDLE");
    bare.renderPass = (VkRenderPass)0x3000u;
    xud_check(xud_pipeline_fill_create_info(&bare, &out) == XUD_UNSUPPORTED, "pipeline/create-unstamped-rows",
        "a record whose rows were never stamped is a bad state of THIS layer, reported as such rat"
        "her than assembled from calloc's zeros");
    pipeline.layout = (VkPipelineLayout)0x9000u;
    pipeline.renderPass = (VkRenderPass)0x3000u;
    xud_check(
        xud_pipeline_fill_create_info(&pipeline, &out) == XUD_OK, "pipeline/create-ok", "the create-info is assembled from the record");
    xud_check(out.sType == VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, "pipeline/create-stamped", "stamped");
    xud_check((out.pNext == NULL) && (out.flags == 0u), "pipeline/create-chain-clear", "nothing is chained into it");
    xud_check((out.stageCount == XUD_PIPELINE_STAGE_COUNT) && (out.pStages == pipeline.shaderStages), "pipeline/create-two-stages",
        "two stages, and the row is the record's own");
    xud_check(out.pTessellationState == NULL, "pipeline/create-no-tessellation",
        "no tessellation row at all, which is the stage array's own shape read from the other side");
    xud_check((out.pVertexInputState == &pipeline.vertexInput) && (out.pInputAssemblyState == &pipeline.inputAssembly)
            && (out.pViewportState == &pipeline.viewportState) && (out.pRasterizationState == &pipeline.rasterizer)
            && (out.pMultisampleState == &pipeline.multisample) && (out.pDepthStencilState == &pipeline.depthStencil)
            && (out.pColorBlendState == &pipeline.blending) && (out.pDynamicState == &pipeline.dynamicState),
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
}

void xud_test_pipeline(void) { xud_test_pipeline_pure(); }
