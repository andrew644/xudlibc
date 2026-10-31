/***********************************************************************************************
 *
 *   xud_pipeline.c - the pipeline layer: the built-in SPIR-V, one pipeline layout, the batch's
 *          variant list of graphics pipelines, and the one teardown order that hands them back
 *
 *   Owns : the pipeline layer. Fills the fixed pipeline state (pure), overwrites that state's
 *          zero vertex-input row with the batch's four binding and four attribute rows (pure),
 *          assembles one create-info per row of the batch's variant list (pure), then creates the
 *          two shader modules, the one layout the whole list shares and one pipeline per row of
 *          that list through the device rows, and tears every one of them back down in
 *          the one order that keeps every handle's owner alive.
 *   Layer: above the shader rung, below the render pass. Nothing in this file calls a Vulkan
 *          entry point directly - every one is reached through a row of XudLoader's Volk
 *          device table, so the archive holds no vk* symbol and the link line has no -lvul
 *          kan anywhere (AGENTS.md).
 *          The pipeline's shape is this file's own assembly, not a driver's answer: the
 *          validation layer's rules for these create-infos (every row stamped, pNext chains
 *          empty, blend attachment count equal to the attachment count of the render pass
 *          this pipeline will be used in) are enforced here rather than relied on from a
 *          driver that says no.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud_pipeline.h"
#include "xud.h"
#include "xud_batch.h"
#include "xud_descriptor.h" /* Required for: XudDescriptor, the record whose set layouts and push range the pipeline layout names */
#include "xud_device.h"
#include "xud_loader.h"
#include "xud_shader.h"
#include "xud_solid_spv.h" /* Required for: the built-in SPIR-V set - generated at build time
                             from shaders/solid.vert + solid.frag by CMakeLists.txt (glslang-
                             Validator, then spirv-val in the same build step), never read at
                             runtime and never compiled from GLSL at runtime, the repo's rule */
#include "xud_vulkan.h"

#include <assert.h> /* Required for: static_assert, the row and array-size checks */
#include <stdlib.h> /* Required for: calloc, free */

/* The rows this layer calls through must be pointer-sized, the same rule xud_loader.h checks
   for the table pair as a whole. */
static_assert(sizeof(PFN_vkCreateShaderModule) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkDestroyShaderModule) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkCreatePipelineLayout) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkDestroyPipelineLayout) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkCreateGraphicsPipelines) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkDestroyPipeline) == sizeof(void*), "PFN row must be pointer-sized");

/* The create-info's pStages and every state pointer below point at the struct's OWN rows, so
   the rows must exist and be the count the create-info claims. Checked, not trusted: the same
   rule xud_device.c applies to the queue row's pQueuePriorities pointing at the struct's own
   array. The member access inside sizeof is not evaluated. */
static_assert(sizeof(((XudPipeline*)0)->shaderStages) / sizeof(VkPipelineShaderStageCreateInfo) == XUD_PIPELINE_STAGE_COUNT,
    "the stage array must be exactly the count the create-info claims");
static_assert(sizeof(((XudPipeline*)0)->dynamicStates) / sizeof(VkDynamicState) == XUD_PIPELINE_DYNAMIC_STATE_COUNT,
    "the dynamic-state array must be exactly the count the dynamic row claims");

/* The built-in SPIR-V set is the one pair this rung builds its two modules from. The arrays are
   static storage in the generated header, so nothing here owns them and xud_shader_close() must
   not free them - the shader layer borrows the words and the module owns only the module. */
static_assert((XUD_SHADER_SPIRV_HEADER_WORDS * sizeof(uint32_t)) <= sizeof(xudSolidFragSpv),
    "the built-in fragment module is at least a SPIR-V header");

/*-----------------------------------------------------------------------------------------------
 * The fixed pipeline state - pure, no driver call, no handle in hand
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_pipeline_fill_vertex_input(XudPipeline* pipeline, const XudBatch* batch) {
    uint32_t stream = 0u;

    if ((pipeline == NULL) || (batch == NULL)) {
        return XUD_INVALID_ARG;
    }

    /* The rows must be the batch's FILLED rows rather than a zero row: xud_batch_open fills them before
       it creates anything, so a batch that exists has them, and a struct whose rows are still all zero is
       one that never went through that open. The check is that the four binding numbers and the four
       attribute locations are the stream numbers 0..3 - which is both what the pipeline's vertex-input row
       is about to claim and what vkCmdBindVertexBuffers will bind, so a row that disagrees is refused here
       rather than built into a pipeline that reads a binding nothing wrote.
    */
    for (stream = 0u; stream < XUD_BATCH_STREAM_COUNT; stream++) {
        if ((batch->bindings[stream].binding != stream) || (batch->attributes[stream].location != stream)) {
            return XUD_INVALID_ARG;
        }
    }

    /* The row points at the BATCH's arrays rather than at a copy of them, which is what makes the
       pipeline's vertex-input state name the same four streams the batch's buffers hold - and what
       makes the batch's life the pipeline's subject. The renderer opens the batch before the pipeline
       and closes it after, so the pointer cannot outlive what it names.
    */
    pipeline->vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    pipeline->vertexInput.pNext = NULL;
    pipeline->vertexInput.pVertexBindingDescriptions = (const VkVertexInputBindingDescription*)batch->bindings;
    pipeline->vertexInput.pVertexAttributeDescriptions = (const VkVertexInputAttributeDescription*)batch->attributes;
    pipeline->vertexInput.vertexBindingDescriptionCount = XUD_BATCH_STREAM_COUNT;
    pipeline->vertexInput.vertexAttributeDescriptionCount = XUD_BATCH_STREAM_COUNT;
    return XUD_OK;
}

XudResult xud_pipeline_fill_state(XudPipeline* pipeline) {
    /* The per-variant blend state, in the BATCH's variant order - a table rather than a switch
       because the variant IS the batch's index (todo.md's D1, 2026-10-26: raylib's blending is an
       enum-indexed list of modes, BlendMode in reference/raylib/raylib.h 918-926, and rlgl.h's
       GlState.currentBlendMode an int a switch maps to GL state, so growing the list is a row here
       and not a member in a struct). The opaque row is the identity of an overwrite (ONE/ZERO);
       the transparent row is raylib's DEFAULT mode BLEND_ALPHA (raylib.h 918, "= 0" and commented
       "(default)"), whose Vulkan form is the rlgl.h 2127 pair glBlendFunc(GL_SRC_ALPHA,
       GL_ONE_MINUS_SRC_ALPHA) + glBlendEquation(GL_FUNC_ADD). */
    static const VkBool32 blendEnable[XUD_BATCH_VARIANT_COUNT] = { VK_FALSE, VK_TRUE };
    static const VkBlendFactor blendSrc[XUD_BATCH_VARIANT_COUNT] = { VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_ALPHA };
    static const VkBlendFactor blendDst[XUD_BATCH_VARIANT_COUNT] = { VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA };
    /* The list's OTHER axis: one input-assembly row per topology the batch's draw rows name, in the
       batch's topology order. Same table shape as the blend tables above for the same reason - the
       row of the list is a pair of batch indices, so growing either axis is a row in a table here
       and not a member in this struct. The two rows differ in ONE field. */
    static const VkPrimitiveTopology topologyTable[XUD_BATCH_TOPOLOGY_COUNT]
        = { XUD_PIPELINE_TOPOLOGY_TRIANGLE, XUD_PIPELINE_TOPOLOGY_LINE };
    uint32_t variant = 0u;
    uint32_t topology = 0u;

    if (pipeline == NULL) {
        return XUD_INVALID_ARG;
    }

    /* --- vertex input: the STARTING state of this field, zero bindings and zero attributes. The
       batch rung below this one owns the four SoA streams, and xud_pipeline_open calls xud_pipeline_fill_vertex_input
       AFTER this call, which overwrites all four fields below with the batch's four rows - so the
       built pipeline names those streams and the built-in SPIR-V reads them at those locations
       rather than generating its three vertices from gl_VertexIndex. Writing the zero row here at
       all is what lets the headless assembly test ask "is the row stamped, is its pNext clear" with
       no batch in hand. The row itself must still be present and stamped: pVertexInputState may not
       be NULL when any fragment stage is present, because a fragment stage that writes gl_FragColor
       reads gl_FragCoord, and that is the spec's own rule for this pointer. --- */
    pipeline->vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    pipeline->vertexInput.pNext = NULL;
    pipeline->vertexInput.vertexBindingDescriptionCount = 0u;
    pipeline->vertexInput.pVertexBindingDescriptions = NULL;
    pipeline->vertexInput.vertexAttributeDescriptionCount = 0u;
    pipeline->vertexInput.pVertexAttributeDescriptions = NULL;

    /* --- input assembly: one row per topology of the list, TRIANGLE_LIST for the first and LINE_LIST
       for the second, neither with primitive restart. The restart index is a degenerate-index
       convention the batch step owns, not a decision of these rows, and neither of our two
       topologies uses one. Topology is PIPELINE state (it lives in this row, not in a draw call),
       which is what makes the list carry one row per topology rather than one row for all of it -
       and it is why a line drawn through the triangles' pipeline is a line made of triangles, not
       a line. --- */
    for (topology = 0u; topology < XUD_BATCH_TOPOLOGY_COUNT; topology++) {
        pipeline->inputAssembly[topology].sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        pipeline->inputAssembly[topology].pNext = NULL;
        pipeline->inputAssembly[topology].topology = topologyTable[topology];
        pipeline->inputAssembly[topology].primitiveRestartEnable = VK_FALSE;
    }

    /* --- viewport state: one viewport and one scissor, both DYNAMIC, so both pointers stay NULL
       and the frame loop sets them. pViewportState may not be NULL at all for the same reason
       the vertex-input row may not be NULL (gl_FragCoord), and a fixed extent here would be a
       second pipeline per window size, which is exactly what dynamic state buys against. --- */
    pipeline->viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    pipeline->viewportState.pNext = NULL;
    pipeline->viewportState.viewportCount = 1u;
    pipeline->viewportState.pViewports = NULL;
    pipeline->viewportState.scissorCount = 1u;
    pipeline->viewportState.pScissors = NULL;

    /* --- rasterizer: fill, no cull, CCW front face, no depth bias. lineWidth is a field of this
       row even though no line pipeline exists yet, and 1 is the only honest value in it - the
       line variant is a later rung's. --- */
    pipeline->rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    pipeline->rasterizer.pNext = NULL;
    pipeline->rasterizer.depthClampEnable = VK_FALSE;
    pipeline->rasterizer.rasterizerDiscardEnable = VK_FALSE;
    pipeline->rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    pipeline->rasterizer.lineWidth = 1.0f;
    pipeline->rasterizer.cullMode = XUD_PIPELINE_CULL_MODE;
    pipeline->rasterizer.frontFace = XUD_PIPELINE_FRONT_FACE;
    pipeline->rasterizer.depthBiasEnable = VK_FALSE;
    pipeline->rasterizer.depthBiasConstantFactor = 0.0f;
    pipeline->rasterizer.depthBiasClamp = 0.0f;
    pipeline->rasterizer.depthBiasSlopeFactor = 0.0f;

    /* --- multisample: one sample today. MSAA is a framebuffer*SampleCounts gate (this driver
       answers 0x1f for all three, see AGENTS.md) and a resolve attachment is a decision the
       render-pass step takes, not one here. --- */
    pipeline->multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    pipeline->multisample.pNext = NULL;
    pipeline->multisample.rasterizationSamples = XUD_PIPELINE_SAMPLE_COUNT;
    pipeline->multisample.sampleShadingEnable = VK_FALSE;
    pipeline->multisample.minSampleShading = 1.0f;
    pipeline->multisample.pSampleMask = NULL;
    pipeline->multisample.alphaToCoverageEnable = VK_FALSE;
    pipeline->multisample.alphaToOneEnable = VK_FALSE;

    /* --- depth stencil: depth test and depth write BOTH ON, LESS recorded, no depth bounds, no
       stencil at all. The ON pair is the opaque variant of the plan's opaque/transparent pair, and i
       t is only honest now because the render pass under this pipeline HAS a depth attachment (xud_r
       endpass.c creates the VkImage and the allocator owns its memory) - the spec makes the depth co
       ntents of a subpass with no depth attachment implementation-dependent, which is why the same t
       wo flags were VK_FALSE for the first-triangle step. With the attachment present and cleared to
       XUD_RENDERPASS_CLEAR_DEPTH by the pass, LESS plus a write is the rule that makes a nearer frag
       ment replace a farther one, which is what a depth buffer is for. depthCompareOp is the rule th
       e test uses, and XUD_PIPELINE_DEPTH_COMPARE_OP is LESS, the one form the clear-to-1.0 convention
       needs: a nearer fragment has the SMALLER depth, so it passes against the larger one already in
       the buffer. Stencil does not exist in this library, so both stencil rows are written as the di
       sabled set rather than left at calloc's zero, and the depth-bounds pair is written as the full
       [0,1] range rather than left at zero for the same reason. --- */
    pipeline->depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    pipeline->depthStencil.pNext = NULL;
    pipeline->depthStencil.depthTestEnable = VK_TRUE;
    pipeline->depthStencil.depthWriteEnable = VK_TRUE;
    pipeline->depthStencil.depthCompareOp = XUD_PIPELINE_DEPTH_COMPARE_OP;
    pipeline->depthStencil.depthBoundsTestEnable = VK_FALSE;
    pipeline->depthStencil.stencilTestEnable = VK_FALSE;
    pipeline->depthStencil.front.compareOp = VK_COMPARE_OP_ALWAYS;
    pipeline->depthStencil.front.passOp = VK_STENCIL_OP_KEEP;
    pipeline->depthStencil.front.failOp = VK_STENCIL_OP_KEEP;
    pipeline->depthStencil.front.depthFailOp = VK_STENCIL_OP_KEEP;
    pipeline->depthStencil.front.compareMask = 0u;
    pipeline->depthStencil.front.writeMask = 0u;
    pipeline->depthStencil.front.reference = 0u;
    pipeline->depthStencil.back = pipeline->depthStencil.front;
    pipeline->depthStencil.maxDepthBounds = 1.0f;
    pipeline->depthStencil.minDepthBounds = 0.0f;

    /* --- blend: the TWO variants of the plan's opaque/transparent pair, one row of each, in the
       batch's variant order. blendEnable is the variant, and the two factor pairs are written even
       where blending is off because the row must say a complete blend - these headers spell the row
       as the core-1.0 form (srcColorBlendFactor / dstColorBlendFactor / colorBlendOp and the same
       three for the alpha half; grepped: srcAlpha, colorBlendFactor and useAlphaBlend do not exist
       anywhere in vulkan_core.h 1.4.363), and leaving the alpha half at ZERO/ZERO would be a blend
       that writes 0 alpha into the swapchain the moment a later variant enables blending. srcAlpha
       VK_TRUE is the spec's default form. attachmentCount is the number of COLOUR attachments of
       the subpass this pipeline runs in, NOT the pass's attachment count: the pass has two
       attachments but only one has a colour aspect, and VUID-VkPipelineColorBlendStateCreateInfo-
       attachmentCount-00646 is about the colour half - the depth half is named by the depth-stencil
       row above. The factors are the table above, indexed by the variant, which is what makes the
       batch's variant index and this row the same number (todo.md's D1, 2026-10-26); the alpha
       half stays ONE/ZERO under BOTH rows because the swapchain's alpha is composited against
       nothing here and writing 1.0 rather than 0.0 is what a later compositor wants. Depth writes
       stay ON under the transparent row - that is what raylib does under BLEND_ALPHA, and the
       overlap artifacts that go with keeping them are raylib's own; turning them off is a later
       knob, not this row. --- */
    for (variant = 0u; variant < XUD_BATCH_VARIANT_COUNT; variant++) {
        pipeline->blendAttachment[variant].blendEnable = blendEnable[variant];
        pipeline->blendAttachment[variant].srcColorBlendFactor = blendSrc[variant];
        pipeline->blendAttachment[variant].dstColorBlendFactor = blendDst[variant];
        pipeline->blendAttachment[variant].colorBlendOp = XUD_PIPELINE_BLEND_OP;
        pipeline->blendAttachment[variant].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        pipeline->blendAttachment[variant].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        pipeline->blendAttachment[variant].alphaBlendOp = XUD_PIPELINE_BLEND_OP;
        pipeline->blendAttachment[variant].colorWriteMask = 0xFu;

        pipeline->blending[variant].sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        pipeline->blending[variant].pNext = NULL;
        pipeline->blending[variant].logicOpEnable = VK_FALSE;
        pipeline->blending[variant].logicOp = XUD_PIPELINE_LOGIC_OP;
        pipeline->blending[variant].attachmentCount = 1u;
        pipeline->blending[variant].pAttachments = &pipeline->blendAttachment[variant];
        pipeline->blending[variant].blendConstants[0] = 0.0f;
        pipeline->blending[variant].blendConstants[1] = 0.0f;
        pipeline->blending[variant].blendConstants[2] = 0.0f;
        pipeline->blending[variant].blendConstants[3] = 0.0f;
    }

    /* --- dynamic state: the viewport and the scissor, set per frame by the frame loop below. --- */
    pipeline->dynamicStates[XUD_PIPELINE_VIEWPORT_STATE] = VK_DYNAMIC_STATE_VIEWPORT;
    pipeline->dynamicStates[XUD_PIPELINE_SCISSOR_STATE] = VK_DYNAMIC_STATE_SCISSOR;

    pipeline->dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    pipeline->dynamicState.pNext = NULL;
    pipeline->dynamicState.dynamicStateCount = XUD_PIPELINE_DYNAMIC_STATE_COUNT;
    pipeline->dynamicState.pDynamicStates = pipeline->dynamicStates;

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The two shader-stage rows - pure
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_pipeline_fill_shader_stages(
    VkPipelineShaderStageCreateInfo* outStages, VkShaderModule vertexModule, VkShaderModule fragmentModule) {
    if (outStages == NULL) {
        return XUD_INVALID_ARG;
    }

    outStages[XUD_PIPELINE_VERTEX_STAGE].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    outStages[XUD_PIPELINE_VERTEX_STAGE].pNext = NULL;
    outStages[XUD_PIPELINE_VERTEX_STAGE].flags = 0u;
    outStages[XUD_PIPELINE_VERTEX_STAGE].stage = VK_SHADER_STAGE_VERTEX_BIT;
    outStages[XUD_PIPELINE_VERTEX_STAGE].module = vertexModule;
    outStages[XUD_PIPELINE_VERTEX_STAGE].pName = XUD_PIPELINE_ENTRY_POINT; /* the entry point is
                                                        named by name rather than left NULL: the spec's
                                                        "NULL means main" convenience is not what
                                                        these headers' VUIDs ask for - the
                                                        validation layer on this machine reports a
                                                        NULL pName as a hard error (probed today:
                                                        VUID-VkPipelineShaderStageCreateInfo-pName-
                                                        parameter and -pName-00707 both fire), and
                                                        naming main is the honest form for what
                                                        glslangValidator writes */
    outStages[XUD_PIPELINE_VERTEX_STAGE].pSpecializationInfo = NULL;

    outStages[XUD_PIPELINE_FRAGMENT_STAGE].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    outStages[XUD_PIPELINE_FRAGMENT_STAGE].pNext = NULL;
    outStages[XUD_PIPELINE_FRAGMENT_STAGE].flags = 0u;
    outStages[XUD_PIPELINE_FRAGMENT_STAGE].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    outStages[XUD_PIPELINE_FRAGMENT_STAGE].module = fragmentModule;
    outStages[XUD_PIPELINE_FRAGMENT_STAGE].pName = XUD_PIPELINE_ENTRY_POINT;
    outStages[XUD_PIPELINE_FRAGMENT_STAGE].pSpecializationInfo = NULL;

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The layout create-info - pure
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_pipeline_fill_layout_info(const XudDescriptor* descriptor, VkPipelineLayoutCreateInfo* outCreateInfo) {
    if ((outCreateInfo == NULL) || (descriptor == NULL)) {
        return XUD_INVALID_ARG;
    }
    /* The three handles the layout is built from must be live: the set layouts are the two the po
       ol allocated set 0 from, and the uniform buffer is what makes the per-frame data have a sour
       ce. A record that cannot name them is a bad state of the rung below, reported as a bad argu
       ment here - the rule xud_pipeline_fill_create_info() applies to a record that owns no layout
       and no render pass, one row up.
     */
    if ((descriptor->setLayouts[XUD_DESCRIPTOR_SET_PER_FRAME] == VK_NULL_HANDLE)
        || (descriptor->setLayouts[XUD_DESCRIPTOR_SET_MATERIAL] == VK_NULL_HANDLE) || (descriptor->uniformBuffer == VK_NULL_HANDLE)) {
        return XUD_INVALID_ARG;
    }

    outCreateInfo->sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    outCreateInfo->pNext = NULL;
    outCreateInfo->flags = 0u;
    outCreateInfo->setLayoutCount = XUD_DESCRIPTOR_SET_COUNT;
    outCreateInfo->pSetLayouts = descriptor->setLayouts; /* the descriptor rung's own array,
                        not a copy: the pool allocated set 0 from these same two handles, so
                        the layout and the set cannot disagree by construction */
    outCreateInfo->pushConstantRangeCount = 1u;
    outCreateInfo->pPushConstantRanges = &descriptor->pushRange; /* the one range the one
                        pipeline of this rung declares - the spec's "at most one push-constant
                        range in the entire layout", read as a bound on this rung's own row */

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The pipeline create-info - pure, assembled from the rows above
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_pipeline_fill_create_info(
    const XudPipeline* pipeline, uint32_t topology, uint32_t variant, VkGraphicsPipelineCreateInfo* outCreateInfo) {
    if (outCreateInfo == NULL) {
        return XUD_INVALID_ARG;
    }
    /* The pair is the row of the batch's list this create-info is for, and either axis at or past
       its count is refused rather than read off the end of the arrays - the same rule the batch's
       own invariant check applies to the topology and variant a draw row names. */
    if ((topology >= XUD_BATCH_TOPOLOGY_COUNT) || (variant >= XUD_BATCH_VARIANT_COUNT)) {
        return XUD_INVALID_ARG;
    }
    if ((pipeline == NULL) || (pipeline->layout == VK_NULL_HANDLE)) {
        return XUD_INVALID_ARG;
    }
    /* The core-1.4 binding makes the pass an argument of the create as well, so a record that nam
       es no pass is a bad argument of this function too - the reason is quoted where the handle
       is written below. */
    if (pipeline->renderPass == VK_NULL_HANDLE) {
        return XUD_INVALID_ARG;
    }

    /* Every row must be stamped: the assembly below is only ever a step from xud_pipeline_fill_
       state(), so a struct whose rows were never filled is a bad state of this layer's own
       record, not a bad argument from a caller. */
    if ((pipeline->vertexInput.sType != VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO)
        || (pipeline->inputAssembly[topology].sType != VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO)
        || (pipeline->viewportState.sType != VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO)
        || (pipeline->rasterizer.sType != VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO)
        || (pipeline->multisample.sType != VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO)
        || (pipeline->depthStencil.sType != VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO)
        || (pipeline->blending[variant].sType != VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO)
        || (pipeline->dynamicState.sType != VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO)) {
        return XUD_UNSUPPORTED;
    }

    outCreateInfo->sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    outCreateInfo->pNext = NULL;
    outCreateInfo->flags = 0u;
    outCreateInfo->stageCount = XUD_PIPELINE_STAGE_COUNT;
    outCreateInfo->pStages = pipeline->shaderStages;
    outCreateInfo->pTessellationState = NULL; /* no tessellation stage */
    outCreateInfo->pVertexInputState = &pipeline->vertexInput;
    outCreateInfo->pInputAssemblyState = &pipeline->inputAssembly[topology];
    outCreateInfo->pViewportState = &pipeline->viewportState;
    outCreateInfo->pRasterizationState = &pipeline->rasterizer;
    outCreateInfo->pMultisampleState = &pipeline->multisample;
    outCreateInfo->pDepthStencilState = &pipeline->depthStencil;
    outCreateInfo->pColorBlendState = &pipeline->blending[variant];
    outCreateInfo->pDynamicState = &pipeline->dynamicState;
    outCreateInfo->layout = pipeline->layout;

    /* The core-1.4 pair that ties a pipeline to a render pass at creation time, and this library
       DOES bind it: VUID-VkGraphicsPipelineCreateInfo-dynamicRendering-06576 says that when the
       dynamicRendering feature is not enabled - and it is not, see the render-pass decision in
       AGENTS.md - a pipeline with pre-rasterization or fragment state must name the render pass it
       belongs to. The validation layer on this machine reports VK_NULL_HANDLE here as a hard error
       (probed today), so the pass handle is an argument of this rung rather than a per-draw
       question, and xud_pipeline_open() takes it. subpass is the first subpass, the only one this
       library has; these headers carry no VK_SUBPASS_TYPE_* constant at all, so 0 is written as
       that enum's own zero rather than as a name. */
    outCreateInfo->renderPass = pipeline->renderPass;
    outCreateInfo->subpass = XUD_PIPELINE_FIRST_SUBPASS;

    /* The spec's "no base pipeline" pair: basePipelineHandle VK_NULL_HANDLE with basePipelineIndex
       -1. 0 would name pipeline row 0 of a pipeline-derivation this rung does not take. */
    outCreateInfo->basePipelineHandle = VK_NULL_HANDLE;
    outCreateInfo->basePipelineIndex = -1;

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * Retiring the variant rows that got made - file-local, the ladder below and xud_pipeline_close()
 * both call it
 *-----------------------------------------------------------------------------------------------*/

static void xud_pipeline_destroy_rows(XudLoader* loader, const XudDevice* device, VkPipeline* ioCreated) {
    uint32_t row = 0u;

    for (row = 0u; row < XUD_PIPELINE_ROW_COUNT; row++) {
        if (ioCreated[row] != VK_NULL_HANDLE) {
            (void)loader->deviceTable.vkDestroyPipeline(device->device, ioCreated[row], NULL);
            ioCreated[row] = VK_NULL_HANDLE;
        }
    }
}

/*-----------------------------------------------------------------------------------------------
 * The create rung - the driver half
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_pipeline_open(XudPipeline** outPipeline, XudLoader* loader, const XudDevice* device, const XudDescriptor* descriptor,
    const XudBatch* batch, VkRenderPass renderPass) {
    XudPipeline* pipeline = NULL;
    XudShader* vertexShader = NULL;
    XudShader* fragmentShader = NULL;
    VkPipelineLayoutCreateInfo layoutInfo;
    VkPipeline created[XUD_PIPELINE_ROW_COUNT] = { VK_NULL_HANDLE }; /* zeroed in full: only the
                            rows a create call reached have a handle in them, and the ladder below
                            retires the rows that did get made by skipping the ones that did not */
    VkPipelineLayout layout = VK_NULL_HANDLE;
    XudResult result = XUD_INVALID_ARG;
    VkResult vk = VK_ERROR_UNKNOWN;
    uint32_t variant = 0u;
    uint32_t topology = 0u;
    uint32_t row = 0u;

    if ((outPipeline == NULL) || (*outPipeline != NULL)) {
        return XUD_INVALID_ARG;
    }
    if (batch == NULL) {
        return XUD_INVALID_ARG;
    }
    if ((loader == NULL) || (device == NULL) || (descriptor == NULL)) {
        return (loader == NULL) ? XUD_INVALID_ARG : XUD_UNSUPPORTED;
    }
    if (!loader->open) {
        return XUD_UNSUPPORTED;
    }
    if (device->device == VK_NULL_HANDLE) {
        return XUD_INVALID_ARG;
    }
    /* The render pass is an argument of this rung, not a per-draw question, and a handle-less one
       is a bad argument: the core-1.4 VUID that makes the binding a requirement is quoted where
       xud_pipeline_fill_create_info() writes it. */
    if (renderPass == VK_NULL_HANDLE) {
        return XUD_INVALID_ARG;
    }

    /* Every destroy row before every create row: an object this layer cannot destroy is an object
       it must not create, the rule xud_window.c applies to a surface and xud_swapchain.c to a
       swapchain, read here for all three objects of this rung at once. */
    if ((loader->deviceTable.vkDestroyShaderModule == NULL) || (loader->deviceTable.vkCreateShaderModule == NULL)
        || (loader->deviceTable.vkDestroyPipelineLayout == NULL) || (loader->deviceTable.vkCreatePipelineLayout == NULL)
        || (loader->deviceTable.vkDestroyPipeline == NULL) || (loader->deviceTable.vkCreateGraphicsPipelines == NULL)) {
        return XUD_UNSUPPORTED;
    }

    /* The rows must be THIS device's rows - a pipeline created through another device's rows is a
       pipeline this device cannot destroy. */
    if (!xud_loader_device_bound(loader, device->device)) {
        return XUD_UNSUPPORTED;
    }

    pipeline = calloc(1u, sizeof(*pipeline));
    if (pipeline == NULL) {
        return XUD_OUT_OF_MEMORY;
    }

    result = xud_pipeline_fill_state(pipeline);
    if (result != XUD_OK) {
        free(pipeline);
        return result;
    }

    /* The vertex input is the batch's rows, written AFTER the fixed assembly above so the batch's four
       streams are what the built pipeline names. The batch is asked for here rather than at the create
       call because the row it hands the driver points at the batch's own arrays.
    */
    result = xud_pipeline_fill_vertex_input(pipeline, batch);
    if (result != XUD_OK) {
        free(pipeline);
        return result;
    }

    /* Borrowed, not owned: the pass belongs to the render-pass rung above, which destroys it after
       this rung's xud_pipeline_close() has retired the pipeline that names it - the one teardown
       order xud_renderer.c states. */
    pipeline->renderPass = renderPass;

    /* The two modules first: the layout and the pipeline are built from what they hand back. */
    result = xud_shader_open(&vertexShader, loader, device, xudSolidVertSpv, sizeof(xudSolidVertSpv));
    if (result != XUD_OK) {
        free(pipeline);
        return result;
    }
    result = xud_shader_open(&fragmentShader, loader, device, xudSolidFragSpv, sizeof(xudSolidFragSpv));
    if (result != XUD_OK) {
        (void)xud_shader_close(&vertexShader, device, loader);
        free(pipeline);
        return result;
    }
    pipeline->vertexShader = vertexShader;
    pipeline->fragmentShader = fragmentShader;

    result = xud_pipeline_fill_shader_stages(pipeline->shaderStages, vertexShader->module, fragmentShader->module);
    if (result != XUD_OK) {
        (void)xud_shader_close(&fragmentShader, device, loader);
        (void)xud_shader_close(&vertexShader, device, loader);
        free(pipeline);
        return result;
    }

    result = xud_pipeline_fill_layout_info(descriptor, &layoutInfo);
    if (result != XUD_OK) {
        (void)xud_shader_close(&fragmentShader, device, loader);
        (void)xud_shader_close(&vertexShader, device, loader);
        free(pipeline);
        return result;
    }

    /* vkCreatePipelineLayout returns VkResult, so the answer goes through the one funnel every
       layer maps a VkResult through - a layout the driver refused is reported with the driver's
       own reason, not with a code this layer invented. */
    vk = loader->deviceTable.vkCreatePipelineLayout(device->device, &layoutInfo, NULL, &layout);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK) {
        (void)xud_shader_close(&fragmentShader, device, loader);
        (void)xud_shader_close(&vertexShader, device, loader);
        free(pipeline);
        return result;
    }
    pipeline->layout = layout;

    /* The variant list's pipelines, created in variant order, one create call per variant, each
       create-info assembled from the rows above so the create-info and the record are the same
       bytes. ONE layout for the whole list (todo.md's D1, 2026-10-26): the rows differ only in
       VkPipelineColorBlendAttachmentState, which is PIPELINE state, not interface state - the sets,
       the bindings and the push range are identical across the list - so a second layout would be a
       second record of one interface and every vkCmdBindDescriptorSets would have to know which of
       two it names. The handles are created into a local array and recorded into the struct only
       once the whole list exists, which is what lets the ladder below retire the rows that did get
       made without reaching into the struct mid-create. */
    for (topology = 0u; topology < XUD_BATCH_TOPOLOGY_COUNT; topology++) {
        for (variant = 0u; variant < XUD_BATCH_VARIANT_COUNT; variant++) {
            row = XUD_PIPELINE_ROW(topology, variant);
            result = xud_pipeline_fill_create_info(pipeline, topology, variant, &pipeline->createInfo[row]);
            if (result != XUD_OK) {
                xud_pipeline_destroy_rows(loader, device, created);
                (void)loader->deviceTable.vkDestroyPipelineLayout(device->device, layout, NULL);
                (void)xud_shader_close(&fragmentShader, device, loader);
                (void)xud_shader_close(&vertexShader, device, loader);
                free(pipeline);
                return result;
            }

            /* vkCreateGraphicsPipelines takes an ARRAY of create-infos and an array of pipelines, with a
               count: one pipeline here, so one row of each, and the row is the struct's own createInfo
               row - so the create-info the driver reads and the create-info this record keeps ARE the
               same bytes, the rule xud_swapchain.c states for its create-info and its record, one rung
               up. pipelineCache VK_NULL_HANDLE is the spec's "no cache" form - a pipeline cache is a
               disk cache decision this library does not take today. */
            vk = loader->deviceTable.vkCreateGraphicsPipelines(
                device->device, VK_NULL_HANDLE, 1u, &pipeline->createInfo[row], NULL, &created[row]);
            result = xud_result_from_vk(vk);
            if (result != XUD_OK) {
                xud_pipeline_destroy_rows(loader, device, created);
                (void)loader->deviceTable.vkDestroyPipelineLayout(device->device, layout, NULL);
                (void)xud_shader_close(&fragmentShader, device, loader);
                (void)xud_shader_close(&vertexShader, device, loader);
                free(pipeline);
                return result;
            }
        }
    }

    for (row = 0u; row < XUD_PIPELINE_ROW_COUNT; row++) {
        pipeline->pipeline[row] = created[row];
    }

    *outPipeline = pipeline;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The teardown rung - the driver half
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_pipeline_close(XudPipeline** ioPipeline, const XudDevice* device, XudLoader* loader) {
    XudPipeline* victim = NULL;
    XudResult closedVertex = XUD_INVALID_ARG;
    XudResult closedFragment = XUD_INVALID_ARG;
    XudResult first = XUD_OK;
    uint32_t row = 0u;

    if ((ioPipeline == NULL) || (*ioPipeline == NULL)) {
        return XUD_INVALID_ARG;
    }
    victim = *ioPipeline;

    if ((loader == NULL) || (device == NULL)) {
        return (loader == NULL) ? XUD_INVALID_ARG : XUD_UNSUPPORTED;
    }
    if ((!loader->open) || (device->device == VK_NULL_HANDLE)) {
        return XUD_UNSUPPORTED;
    }
    if (!xud_loader_device_bound(loader, device->device)) {
        return XUD_UNSUPPORTED;
    }

    /* The pipelines first (every row of the variant list), then the layout, then the modules: the
       pipelines name their layout and the layout's modules are the only thing keeping them alive,
       so anything else leaves a handle whose owner is already gone. A nil destroy row while a row
       of the list is live is a refusal, and the struct is not freed, because freeing it would leak
       the handles silently - the rule xud_shader_close() and xud_swapchain_close() both state. */
    for (row = 0u; row < XUD_PIPELINE_ROW_COUNT; row++) {
        if (victim->pipeline[row] != VK_NULL_HANDLE) {
            if (loader->deviceTable.vkDestroyPipeline == NULL) {
                return XUD_UNSUPPORTED;
            }
            break;
        }
    }
    xud_pipeline_destroy_rows(loader, device, victim->pipeline);

    if (victim->layout != VK_NULL_HANDLE) {
        if (loader->deviceTable.vkDestroyPipelineLayout == NULL) {
            return XUD_UNSUPPORTED;
        }
        loader->deviceTable.vkDestroyPipelineLayout(device->device, victim->layout, NULL);
        victim->layout = VK_NULL_HANDLE;
    }

    if (victim->fragmentShader != NULL) {
        closedFragment = xud_shader_close(&victim->fragmentShader, device, loader);
        if ((closedFragment != XUD_OK) && (closedFragment != XUD_ALREADY)) {
            first = closedFragment;
        }
    }
    if (victim->vertexShader != NULL) {
        closedVertex = xud_shader_close(&victim->vertexShader, device, loader);
        if ((closedVertex != XUD_OK) && (closedVertex != XUD_ALREADY)) {
            first = closedVertex;
        }
    }

    free(victim);
    *ioPipeline = NULL;

    /* A refused teardown below is reported, not hidden - the same rule xud_close_context() follows
       with its own rungs. */
    return first;
}
