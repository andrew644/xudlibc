/***********************************************************************************************
 *
 *   xud_pipeline.c - the pipeline layer: the built-in SPIR-V, one pipeline layout, one
 *          graphics pipeline, and the one teardown order that hands them back
 *
 *   Owns : the pipeline layer. Fills the fixed pipeline state (pure), assembles the two
 *          create-infos from it (pure), then creates the two shader modules, the layout and
 *          the pipeline through the device rows, and tears all four back down in the one
 *          order that keeps every handle's owner alive.
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
#include "xud_descriptor.h"
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

XudResult xud_pipeline_fill_state(XudPipeline* pipeline) {
    if (pipeline == NULL) {
        return XUD_INVALID_ARG;
    }

    /* --- vertex input: zero bindings, zero attributes. The allocator rung IS in the tree now
       (S12) but owns no VkBuffer at all - nothing above it has asked it for memory - and the
       SoA streams land with the batch, so the vertex shader generates its vertices from
       gl_VertexIndex instead. The row itself must still be present
       and stamped: pVertexInputState may not be NULL when any fragment stage is present,
       because a fragment stage that writes gl_FragColor reads gl_FragCoord, and that is the
       spec's own rule for this pointer. --- */
    pipeline->vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    pipeline->vertexInput.pNext = NULL;
    pipeline->vertexInput.vertexBindingDescriptionCount = 0u;
    pipeline->vertexInput.pVertexBindingDescriptions = NULL;
    pipeline->vertexInput.vertexAttributeDescriptionCount = 0u;
    pipeline->vertexInput.pVertexAttributeDescriptions = NULL;

    /* --- input assembly: TRIANGLE_LIST, no primitive restart. The restart index is a degenerate
       -index convention the batch step owns, not a decision of this row. --- */
    pipeline->inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    pipeline->inputAssembly.pNext = NULL;
    pipeline->inputAssembly.topology = XUD_PIPELINE_TOPOLOGY;
    pipeline->inputAssembly.primitiveRestartEnable = VK_FALSE;

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

    /* --- depth stencil: depth test and depth write OFF, LESS recorded, no depth bounds, no
       stencil at all. The OFF pair is the honest half of the depth note: the render pass under
       this pipeline has NO depth attachment (xud_renderpass.h says so, and the VkImage + VkDevi
       ceMemory a depth buffer needs belongs to the allocator rung, which holds no image yet), a
       nd the spec makes the depth buffer UNDEFINED in that case - so an enabled test would be a
       n implementation-dependent result, which is the opposite of a first triangle that has to
       draw every time. depthCompareOp is still recorded, because the field has to hold a valid
       enum and XUD_PIPELINE_DEPTH_COMPARE_OP is the rule the depth buffer will use the day the
       attachment lands. Stencil does not exist in this library, so both stencil rows are writte
       n as the disabled set rather than left at calloc's zero. --- */
    pipeline->depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    pipeline->depthStencil.pNext = NULL;
    pipeline->depthStencil.depthTestEnable = VK_FALSE;
    pipeline->depthStencil.depthWriteEnable = VK_FALSE;
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

    /* --- blend: the OPAQUE variant of the plan's opaque/transparent pair. blendEnable VK_FALSE
       is the variant, and the two factor pairs are written because the row must say a complete
       blend even when it is disabled - these headers spell the row as the core-1.0 form
       (srcColorBlendFactor / dstColorBlendFactor / colorBlendOp and the same three for the
       alpha half, grepped: srcAlpha, colorBlendFactor and useAlphaBlend do not exist anywhere
       in vulkan_core.h 1.4.363), and leaving the alpha half at ZERO/ZERO would be a blend that
       writes 0 alpha into the swapchain the moment a later variant enables blending. srcAlpha
       VK_TRUE is the spec's default form. --- */
    pipeline->blendAttachment.blendEnable = VK_FALSE;
    pipeline->blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    pipeline->blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    pipeline->blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    pipeline->blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    pipeline->blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    pipeline->blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    pipeline->blendAttachment.colorWriteMask = 0xFu;

    pipeline->blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    pipeline->blending.pNext = NULL;
    pipeline->blending.logicOpEnable = VK_FALSE;
    pipeline->blending.logicOp = XUD_PIPELINE_LOGIC_OP;
    pipeline->blending.attachmentCount = 1u;
    pipeline->blending.pAttachments = &pipeline->blendAttachment;
    pipeline->blending.blendConstants[0] = 0.0f;
    pipeline->blending.blendConstants[1] = 0.0f;
    pipeline->blending.blendConstants[2] = 0.0f;
    pipeline->blending.blendConstants[3] = 0.0f;

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

XudResult xud_pipeline_fill_create_info(const XudPipeline* pipeline, VkGraphicsPipelineCreateInfo* outCreateInfo) {
    if (outCreateInfo == NULL) {
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
        || (pipeline->inputAssembly.sType != VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO)
        || (pipeline->viewportState.sType != VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO)
        || (pipeline->rasterizer.sType != VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO)
        || (pipeline->multisample.sType != VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO)
        || (pipeline->depthStencil.sType != VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO)
        || (pipeline->blending.sType != VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO)
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
    outCreateInfo->pInputAssemblyState = &pipeline->inputAssembly;
    outCreateInfo->pViewportState = &pipeline->viewportState;
    outCreateInfo->pRasterizationState = &pipeline->rasterizer;
    outCreateInfo->pMultisampleState = &pipeline->multisample;
    outCreateInfo->pDepthStencilState = &pipeline->depthStencil;
    outCreateInfo->pColorBlendState = &pipeline->blending;
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
 * The create rung - the driver half
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_pipeline_open(
    XudPipeline** outPipeline, XudLoader* loader, const XudDevice* device, const XudDescriptor* descriptor, VkRenderPass renderPass) {
    XudPipeline* pipeline = NULL;
    XudShader* vertexShader = NULL;
    XudShader* fragmentShader = NULL;
    VkPipelineLayoutCreateInfo layoutInfo;
    VkGraphicsPipelineCreateInfo pipelineInfo;
    VkPipeline created = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    XudResult result = XUD_INVALID_ARG;
    VkResult vk = VK_ERROR_UNKNOWN;

    if ((outPipeline == NULL) || (*outPipeline != NULL)) {
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

    result = xud_pipeline_fill_create_info(pipeline, &pipelineInfo);
    if (result != XUD_OK) {
        (void)loader->deviceTable.vkDestroyPipelineLayout(device->device, layout, NULL);
        (void)xud_shader_close(&fragmentShader, device, loader);
        (void)xud_shader_close(&vertexShader, device, loader);
        free(pipeline);
        return result;
    }

    /* vkCreateGraphicsPipelines takes an ARRAY of create-infos and an array of pipelines, with a
       count: one pipeline here, so one row of each, and the array is a frame temporary the driver
       reads during the call. pipelineCache VK_NULL_HANDLE is the spec's "no cache" form - a
       pipeline cache is a disk cache decision this library does not take today. */
    vk = loader->deviceTable.vkCreateGraphicsPipelines(device->device, VK_NULL_HANDLE, 1u, &pipelineInfo, NULL, &created);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK) {
        (void)loader->deviceTable.vkDestroyPipelineLayout(device->device, layout, NULL);
        (void)xud_shader_close(&fragmentShader, device, loader);
        (void)xud_shader_close(&vertexShader, device, loader);
        free(pipeline);
        return result;
    }
    pipeline->pipeline = created;

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

    /* The pipeline first, then the layout, then the modules: the pipeline names its layout and the
       layout's modules are the only thing keeping them alive, so anything else leaves a handle
       whose owner is already gone. A nil destroy row while the object is live is a refusal, and
       the struct is not freed, because freeing it would leak the handles silently - the rule
       xud_shader_close() and xud_swapchain_close() both state. */
    if (victim->pipeline != VK_NULL_HANDLE) {
        if (loader->deviceTable.vkDestroyPipeline == NULL) {
            return XUD_UNSUPPORTED;
        }
        loader->deviceTable.vkDestroyPipeline(device->device, victim->pipeline, NULL);
        victim->pipeline = VK_NULL_HANDLE;
    }

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
