/***********************************************************************************************
 *
 *   xud_pipeline.c - the pipeline layer: the built-in SPIR-V, one pipeline layout, the batch's
 *          variant list of graphics pipelines, and the one teardown order that hands them back
 *
 *   Owns : the pipeline layer. Fills the fixed pipeline state (pure), overwrites that state's
 *          zero vertex-input row with the batch's four binding and four attribute rows (pure),
 *          assembles one create-info per row of the batch's variant list (pure), then creates the
 *          three shader modules, the one layout the whole list shares and one pipeline per row of
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
                             from shaders/solid.vert + solid.frag + solid_textured.frag by CMakeLists.txt (glslang-
                             Validator, then spirv-val in the same build step), never read at
                             runtime and never compiled from GLSL at runtime, the repo's rule */
#include "xud_vulkan.h"

#include <assert.h> /* Required for: static_assert, the row and array-size checks */
#include <stddef.h> /* Required for: size_t, the byte count of each entry of the frag axis */
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
static_assert(sizeof(((XudPipeline*)0)->shaderStages[0]) / sizeof(VkPipelineShaderStageCreateInfo) == XUD_PIPELINE_STAGE_COUNT,
    "one pair of stage rows per row of the list, and each pair exactly the count the create-info claims");
static_assert(sizeof(((XudPipeline*)0)->shaderStages) / sizeof(VkPipelineShaderStageCreateInfo)
        == (XUD_PIPELINE_ROW_COUNT * XUD_PIPELINE_STAGE_COUNT),
    "the stage array is the list's rows by the stage count, which is what makes pStages of one row a"
    " contiguous pair of that row's own");
static_assert(sizeof(((XudPipeline*)0)->dynamicStates) / sizeof(VkDynamicState) == XUD_PIPELINE_DYNAMIC_STATE_COUNT,
    "the dynamic-state array must be exactly the count the dynamic row claims");

/* The built-in SPIR-V set is the one set this rung builds its modules from. The arrays are
   static storage in the generated header, so nothing here owns them and xud_shader_close() must
   not free them - the shader layer borrows the words and the module owns only the module. */
static_assert((XUD_SHADER_SPIRV_HEADER_WORDS * sizeof(uint32_t)) <= sizeof(xudSolidFragSpv),
    "the built-in fragment module is at least a SPIR-V header");
static_assert((XUD_SHADER_SPIRV_HEADER_WORDS * sizeof(uint32_t)) <= sizeof(xudSolidFragTexturedSpv),
    "the built-in textured fragment module is at least a SPIR-V header");

/* Which frag module each column of the variant axis is built from, indexed by the BATCH's variant
   index rather than by a switch: the variant axis is the batch's and the pipeline list's, so the
   mapping is one more column of the same table shape the blend tables below are - the row of the
   list is a pair of batch indices and which frag module it names is a fact about the column, not a
   branch in the create loop. The opaque and transparent columns share the solid module because the
   two differ only in blend state; the textured and ui columns name the one that reads the sampler array, which
   is the only reason a second frag module exists at all - the ui column samples the atlas through
   the same sampler-array entry the textured column uses, so the two share the module rather than compiling
   the same bytes a third time. Growing the variant axis is a row here. */
static const uint32_t xudPipelineFragModule[XUD_BATCH_VARIANT_COUNT]
    = { XUD_PIPELINE_FRAG_SOLID, XUD_PIPELINE_FRAG_SOLID, XUD_PIPELINE_FRAG_TEXTURED, XUD_PIPELINE_FRAG_TEXTURED };

/* The words of each entry of the frag axis, in the axis's own order, beside the mapping table above
   so "which column names which module" and "what bytes that module is built from" are two rows of
   one table rather than a branch at each create call. The arrays are static storage in the generated
   header, so nothing here owns them and xud_shader_close() must not free them. */
static const uint32_t* const xudPipelineFragCode[XUD_PIPELINE_FRAG_MODULE_COUNT] = { xudSolidFragSpv, xudSolidFragTexturedSpv };
static const size_t xudPipelineFragCodeSize[XUD_PIPELINE_FRAG_MODULE_COUNT] = { sizeof(xudSolidFragSpv), sizeof(xudSolidFragTexturedSpv) };

/*-----------------------------------------------------------------------------------------------
 * The fixed pipeline state - pure, no driver call, no handle in hand
 *-----------------------------------------------------------------------------------------------*/

/* The vertex-input row of the fullscreen blits - the resolve, the bright pass, the two blurs: the
   triangle their vertex stages invent from gl_VertexIndex is bound to no buffer, so the row that
   names nothing IS the correct row. It lives HERE, a file constant, rather than in
   pipeline->vertexInput, because xud_pipeline_open overwrites that member with the batch's nine
   rows before these three pipelines are created, and pointing the blits at it would hand their
   gl_VertexIndex-only stages nine attributes they consume none of - 495 validation warnings per
   test run, and a pipeline layout that promises vertex buffers its draws never bind. */
static const VkPipelineVertexInputStateCreateInfo xudBlitVertexInput = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    .pNext = NULL,
    .flags = 0u,
    .vertexBindingDescriptionCount = 0u,
    .pVertexBindingDescriptions = NULL,
    .vertexAttributeDescriptionCount = 0u,
    .pVertexAttributeDescriptions = NULL };

XudResult xud_pipeline_fill_vertex_input(XudPipeline* pipeline, const XudBatch* batch) {
    uint32_t stream = 0u;

    if ((pipeline == NULL) || (batch == NULL)) {
        return XUD_INVALID_ARG;
    }

    /* The rows must be the batch's FILLED rows rather than a zero row: xud_batch_open fills them before
       it creates anything, so a batch that exists has them, and a struct whose rows are still all zero is
       one that never went through that open. The check is that every binding number and every attribute
       location equals its array index - which is both what the pipeline's vertex-input row is about to
       claim and what vkCmdBindVertexBuffers will bind, so a row that disagrees is refused here rather
       than built into a pipeline that reads a binding nothing wrote. The count grew with S3.2: six
       bindings (four streams, the instance two) and nine attributes (the streams, the matrix's four
       columns, the tint), and the identity check covers all of them because the frame loop binds the
       same six buffers in the same index order.
    */
    for (stream = 0u; stream < XUD_BATCH_BINDING_COUNT; stream++) {
        if (batch->bindings[stream].binding != stream) {
            return XUD_INVALID_ARG;
        }
    }
    for (stream = 0u; stream < XUD_BATCH_ATTRIBUTE_COUNT; stream++) {
        if (batch->attributes[stream].location != stream) {
            return XUD_INVALID_ARG;
        }
    }

    /* The row points at the BATCH's arrays rather than at a copy of them, which is what makes the
       pipeline's vertex-input state name the same six buffers the batch's buffers hold - and what
       makes the batch's life the pipeline's subject. The renderer opens the batch before the pipeline
       and closes it after, so the pointer cannot outlive what it names.
    */
    pipeline->vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    pipeline->vertexInput.pNext = NULL;
    pipeline->vertexInput.pVertexBindingDescriptions = (const VkVertexInputBindingDescription*)batch->bindings;
    pipeline->vertexInput.pVertexAttributeDescriptions = (const VkVertexInputAttributeDescription*)batch->attributes;
    pipeline->vertexInput.vertexBindingDescriptionCount = XUD_BATCH_BINDING_COUNT;
    pipeline->vertexInput.vertexAttributeDescriptionCount = XUD_BATCH_ATTRIBUTE_COUNT;
    return XUD_OK;
}

XudResult xud_pipeline_fill_prepass_vertex_input(XudPipeline* pipeline, const XudBatch* batch) {
    if ((pipeline == NULL) || (batch == NULL)) {
        return XUD_INVALID_ARG;
    }
    /* Stream 0 - the position stream - plus the instance matrix: the pre-pass and the shadow pass
       replay the OPAQUE and TEXTURED TRIANGLES rows of the batch, and an instanced row's geometry
       lives at N transforms, so a depth stage that ignored the instance matrix would rasterize N
       copies of the grass tuft at the ORIGIN - occluders the camera never sees, and no copy of the
       grass itself. The same "filled rows only" check the shaded fill makes, narrowed to the rows
       this pipeline points at. */
    if ((batch->bindings[0].binding != 0u) || (batch->attributes[0].location != 0u)) {
        return XUD_INVALID_ARG;
    }
    if ((batch->bindings[XUD_BATCH_BINDING_MATRIX].binding != XUD_BATCH_BINDING_MATRIX)
        || (batch->attributes[XUD_BATCH_BINDING_MATRIX].location != XUD_BATCH_BINDING_MATRIX)) {
        return XUD_INVALID_ARG;
    }

    /* The two bindings are not adjacent in the batch's array - bindings 0, 4 - so the rows are
       COPIED into the pipeline's own storage and the state names that copy. The instance TINT
       binding joins neither depth pass: a shadow does not care what colour the copy is. */
    pipeline->prepassBindings[0] = batch->bindings[0];
    pipeline->prepassBindings[1] = batch->bindings[XUD_BATCH_BINDING_MATRIX];
    pipeline->prepassAttributes[0] = batch->attributes[0];
    pipeline->prepassAttributes[1] = batch->attributes[XUD_BATCH_BINDING_MATRIX];
    pipeline->prepassAttributes[2] = batch->attributes[XUD_BATCH_BINDING_MATRIX + 1u];
    pipeline->prepassAttributes[3] = batch->attributes[XUD_BATCH_BINDING_MATRIX + 2u];
    pipeline->prepassAttributes[4] = batch->attributes[XUD_BATCH_BINDING_MATRIX + 3u];

    pipeline->prepassVertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    pipeline->prepassVertexInput.pNext = NULL;
    pipeline->prepassVertexInput.pVertexBindingDescriptions = pipeline->prepassBindings;
    pipeline->prepassVertexInput.pVertexAttributeDescriptions = pipeline->prepassAttributes;
    pipeline->prepassVertexInput.vertexBindingDescriptionCount = 2u;
    pipeline->prepassVertexInput.vertexAttributeDescriptionCount = 5u;
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
       GL_ONE_MINUS_SRC_ALPHA) + glBlendEquation(GL_FUNC_ADD). The textured and ui rows reuse that
       same pair: a glyph atlas against a cleared background and a screen-space quad are both
       straight alpha-over, so the factor tables carry one transparent entry twice rather than two
       different ones. */
    static const VkBool32 blendEnable[XUD_BATCH_VARIANT_COUNT] = { VK_FALSE, VK_TRUE, VK_TRUE, VK_TRUE };
    static const VkBlendFactor blendSrc[XUD_BATCH_VARIANT_COUNT]
        = { VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA };
    static const VkBlendFactor blendDst[XUD_BATCH_VARIANT_COUNT] = { VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA };
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
       batch rung below this one owns the four SoA streams and the instance pair, and xud_pipeline_open
       calls xud_pipeline_fill_vertex_input AFTER this call, which overwrites all four fields below
       with the batch's six bindings and nine attributes - so the built batch pipelines name those
       streams and the built-in SPIR-V reads them at those locations rather than generating their
       vertices from gl_VertexIndex. The fullscreen blits never see this member at all: their fillers
       name the file-scope zero row beside this file's open. Writing the zero row here at all is what
       lets the headless assembly test ask "is the row stamped, is its pNext clear" with no batch in
       hand. The row itself must still be present and stamped: pVertexInputState may not be NULL when
       any fragment stage is present, because a fragment stage that writes gl_FragColor reads
       gl_FragCoord, and that is the spec's own rule for this pointer. --- */
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

    /* --- depth stencil: one row per variant column, in the batch's variant order. THREE of the
       four are depth test and depth write BOTH ON with GREATER recorded, no depth bounds and no
       stencil at all; the UI column is the axis's only row with the pair BOTH off. The ON pair is
       what the opaque variant of the plan's opaque/transparent pair carries, and it is only honest
       now because the render pass under this pipeline HAS a depth attachment (xud_renderpass.c
       creates the VkImage and the allocator owns its memory) - the spec makes the depth contents
       of a subpass with no depth attachment implementation-dependent, which is why the same two
       flags were VK_FALSE for the first-triangle step. With the attachment present and cleared to
       XUD_RENDERPASS_CLEAR_DEPTH by the pass, GREATER plus a write is the rule that makes a nearer
       fragment replace a farther one, which is what a depth buffer is for - nearer meaning the
       LARGER depth, because the camera's projection writes the reversed 1/z map (xud_camera.c)
       and the pass clears to 0.0 (xud_renderpass.h), so the buffer starts at the far side of the
       frustum. depthCompareOp is the rule the test uses, and it is PER COLUMN since the depth
       pre-pass (S2.4): the OPAQUE and TEXTURED columns replay geometry the pre-pass already
       rasterized into the very buffer they test against, so their rule is
       XUD_PIPELINE_DEPTH_REPLAY_COMPARE_OP - GEQUAL, because the fragment that owns a stored
       depth value IS that surface and equality is its own win, while a strictly smaller depth
       is a fragment behind a nearer surface and the rasterizer throws it before any fragment
       shader runs. The TRANSPARENT column, which the pre-pass does not draw, keeps
       XUD_PIPELINE_DEPTH_COMPARE_OP - GREATER, the reversed-1/z form (xud_camera.c writes the
       1/z map, the pass clears to 0.0): nearer is LARGER, so a nearer fragment passes against
       the smaller one already in the buffer. The UI
       column's off pair is the screen-space half of the overlay's contract: a quad that wrote its
       depth back would be entered into the same GREATER contest as the 3D fragments - winning or
       losing the glyph by an accident of the depth map where it should simply always win - and
       every 3D fragment appended after it in the same frame would be at its mercy. The test and
       the write are both pipeline state, so the bypass lives on the column's own row here rather
       than a flag the frame loop would special-case; the vertex stage's skip of the viewProj for
       the same column (solid.vert's XUD_PUSH_FLAG_VARIANT_UI branch) is the other half of the
       screen-fixed promise. Stencil does not exist in this library, so both stencil rows are
       written as the disabled set rather than left at calloc's zero, and the depth-bounds pair is
       written as the full [0,1] range rather than left at zero for the same reason. --- */
    {
        static const VkBool32 depthTestEnable[XUD_BATCH_VARIANT_COUNT] = { VK_TRUE, VK_TRUE, VK_TRUE, VK_FALSE };
        static const VkBool32 depthWriteEnable[XUD_BATCH_VARIANT_COUNT] = { VK_TRUE, VK_TRUE, VK_TRUE, VK_FALSE };
        static const VkCompareOp depthCompare[XUD_BATCH_VARIANT_COUNT] = { XUD_PIPELINE_DEPTH_REPLAY_COMPARE_OP,
            XUD_PIPELINE_DEPTH_COMPARE_OP, XUD_PIPELINE_DEPTH_REPLAY_COMPARE_OP, XUD_PIPELINE_DEPTH_COMPARE_OP };
        uint32_t depthVariant = 0u;

        for (depthVariant = 0u; depthVariant < XUD_BATCH_VARIANT_COUNT; depthVariant++) {
            pipeline->depthStencil[depthVariant].sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            pipeline->depthStencil[depthVariant].pNext = NULL;
            pipeline->depthStencil[depthVariant].depthTestEnable = depthTestEnable[depthVariant];
            pipeline->depthStencil[depthVariant].depthWriteEnable = depthWriteEnable[depthVariant];
            pipeline->depthStencil[depthVariant].depthCompareOp = depthCompare[depthVariant];
            pipeline->depthStencil[depthVariant].depthBoundsTestEnable = VK_FALSE;
            pipeline->depthStencil[depthVariant].stencilTestEnable = VK_FALSE;
            pipeline->depthStencil[depthVariant].front.compareOp = VK_COMPARE_OP_ALWAYS;
            pipeline->depthStencil[depthVariant].front.passOp = VK_STENCIL_OP_KEEP;
            pipeline->depthStencil[depthVariant].front.failOp = VK_STENCIL_OP_KEEP;
            pipeline->depthStencil[depthVariant].front.depthFailOp = VK_STENCIL_OP_KEEP;
            pipeline->depthStencil[depthVariant].front.compareMask = 0u;
            pipeline->depthStencil[depthVariant].front.writeMask = 0u;
            pipeline->depthStencil[depthVariant].front.reference = 0u;
            pipeline->depthStencil[depthVariant].back = pipeline->depthStencil[depthVariant].front;
            pipeline->depthStencil[depthVariant].maxDepthBounds = 1.0f;
            pipeline->depthStencil[depthVariant].minDepthBounds = 0.0f;
        }
    }

    /* --- the resolve pipeline's own depth row: neither test nor write. The scratch pass has already
       resolved visibility for every pixel the triangle samples, and the resolve pass covers every
       pixel exactly once, so a depth test would ask a second buffer the same question the first one
       answered, and a depth write would overwrite the scratch pass's answer with a constant. The
       remaining fields are written because the row must be a complete depth row even when both
       enables are off - the same completeness rule the variant rows above follow. --- */
    pipeline->resolveDepthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    pipeline->resolveDepthStencil.pNext = NULL;
    pipeline->resolveDepthStencil.flags = 0u;
    pipeline->resolveDepthStencil.depthTestEnable = VK_FALSE;
    pipeline->resolveDepthStencil.depthWriteEnable = VK_FALSE;
    pipeline->resolveDepthStencil.depthCompareOp = XUD_PIPELINE_DEPTH_COMPARE_OP;
    pipeline->resolveDepthStencil.depthBoundsTestEnable = VK_FALSE;
    pipeline->resolveDepthStencil.stencilTestEnable = VK_FALSE;
    pipeline->resolveDepthStencil.front = pipeline->depthStencil[XUD_BATCH_VARIANT_UI].front;
    pipeline->resolveDepthStencil.back = pipeline->depthStencil[XUD_BATCH_VARIANT_UI].front;
    pipeline->resolveDepthStencil.minDepthBounds = 0.0f;
    pipeline->resolveDepthStencil.maxDepthBounds = 1.0f;

    /* --- blend: one row per variant column, in the batch's variant order. blendEnable is the
       variant, and the two factor pairs are written even
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
       half stays ONE/ZERO under EVERY row because the swapchain's alpha is composited against
       nothing here and writing 1.0 rather than 0.0 is what a later compositor wants. Depth writes
       stay ON under the transparent and textured rows - that is what raylib does under BLEND_ALPHA,
       and the overlap artifacts that go with keeping them are raylib's own; turning them off is a
       later knob, not this row. Only the ui column carries the OFF pair, on its own depth row
       above. --- */
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

    /* --- the depth pre-pass's own two state rows. The depth row is the plain GREATER contest -
       the pre-pass draws each surface exactly once per pixel today, so a tie would be the same
       triangle twice and either answer stores the same bits; the GEQUAL subtlety belongs to the
       SHADED columns, which is where the variant table above carries it. The blend row is the
       pre-pass's whole point said in pipeline state: attachmentCount 0, because its subpass
       references no colour attachment, and VUID-VkPipelineColorBlendStateCreateInfo-
       attachmentCount-00605 wants the count to be the subpass's colour count - a row naming one
       attachment for a zero-colour subpass is the exact bug the pre-pass exists to un-pay. The
       blend constants are written because the row must be complete even when it names nothing,
       the same completeness rule the resolve row beside it follows. --- */
    pipeline->prepassDepthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    pipeline->prepassDepthStencil.pNext = NULL;
    pipeline->prepassDepthStencil.flags = 0u;
    pipeline->prepassDepthStencil.depthTestEnable = VK_TRUE;
    pipeline->prepassDepthStencil.depthWriteEnable = VK_TRUE;
    pipeline->prepassDepthStencil.depthCompareOp = XUD_PIPELINE_DEPTH_COMPARE_OP;
    pipeline->prepassDepthStencil.depthBoundsTestEnable = VK_FALSE;
    pipeline->prepassDepthStencil.stencilTestEnable = VK_FALSE;
    pipeline->prepassDepthStencil.front = pipeline->depthStencil[XUD_BATCH_VARIANT_UI].front;
    pipeline->prepassDepthStencil.back = pipeline->depthStencil[XUD_BATCH_VARIANT_UI].front;
    pipeline->prepassDepthStencil.minDepthBounds = 0.0f;
    pipeline->prepassDepthStencil.maxDepthBounds = 1.0f;

    pipeline->prepassBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    pipeline->prepassBlending.pNext = NULL;
    pipeline->prepassBlending.logicOpEnable = VK_FALSE;
    pipeline->prepassBlending.logicOp = XUD_PIPELINE_LOGIC_OP;
    pipeline->prepassBlending.attachmentCount = 0u;
    pipeline->prepassBlending.pAttachments = NULL;
    pipeline->prepassBlending.blendConstants[0] = 0.0f;
    pipeline->prepassBlending.blendConstants[1] = 0.0f;
    pipeline->prepassBlending.blendConstants[2] = 0.0f;
    pipeline->prepassBlending.blendConstants[3] = 0.0f;

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
    /* The two handles the layout is built from must be live: the set layout is the one the pool
       allocated set 0 from, and the uniform buffer is what makes the per-frame data have a sour
       ce. A record that cannot name them is a bad state of the rung below, reported as a bad argu
       ment here - the rule xud_pipeline_fill_create_info() applies to a record that owns no layout
       and no render pass, one row up.
     */
    if ((descriptor->setLayouts[XUD_DESCRIPTOR_SET_PER_FRAME] == VK_NULL_HANDLE) || (descriptor->uniformBuffer == VK_NULL_HANDLE)) {
        return XUD_INVALID_ARG;
    }

    outCreateInfo->sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    outCreateInfo->pNext = NULL;
    outCreateInfo->flags = 0u;
    outCreateInfo->setLayoutCount = XUD_DESCRIPTOR_SET_COUNT;
    outCreateInfo->pSetLayouts = descriptor->setLayouts; /* the descriptor rung's own array,
                        not a copy: the pool allocated set 0 from this same handle, so
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
       record, not a bad argument from a caller. The depth rows are one per variant column, so the
       whole array is stamped rather than the one this (topology, variant) pair names - the pair
       selects which row the create-info points at, but a single unstamped row among the four is
       the same bad state no matter which column the pair happens to name. */
    {
        uint32_t stampedVariant = 0u;

        for (stampedVariant = 0u; stampedVariant < XUD_BATCH_VARIANT_COUNT; stampedVariant++) {
            if (pipeline->depthStencil[stampedVariant].sType != VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO) {
                return XUD_UNSUPPORTED;
            }
        }
    }
    if ((pipeline->vertexInput.sType != VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO)
        || (pipeline->inputAssembly[topology].sType != VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO)
        || (pipeline->viewportState.sType != VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO)
        || (pipeline->rasterizer.sType != VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO)
        || (pipeline->multisample.sType != VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO)
        || (pipeline->blending[variant].sType != VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO)
        || (pipeline->dynamicState.sType != VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO)) {
        return XUD_UNSUPPORTED;
    }

    outCreateInfo->sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    outCreateInfo->pNext = NULL;
    outCreateInfo->flags = 0u;
    outCreateInfo->stageCount = XUD_PIPELINE_STAGE_COUNT;
    outCreateInfo->pStages = pipeline->shaderStages[XUD_PIPELINE_ROW(topology, variant)];
    outCreateInfo->pTessellationState = NULL; /* no tessellation stage */
    outCreateInfo->pVertexInputState = &pipeline->vertexInput;
    outCreateInfo->pInputAssemblyState = &pipeline->inputAssembly[topology];
    outCreateInfo->pViewportState = &pipeline->viewportState;
    outCreateInfo->pRasterizationState = &pipeline->rasterizer;
    outCreateInfo->pMultisampleState = &pipeline->multisample;
    outCreateInfo->pDepthStencilState = &pipeline->depthStencil[variant];
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
 * The RESOLVE pipeline's create-info - pure, assembled from the rung's own rows
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_pipeline_fill_resolve_create_info(const XudPipeline* pipeline, VkGraphicsPipelineCreateInfo* outCreateInfo) {
    if (outCreateInfo == NULL) {
        return XUD_INVALID_ARG;
    }
    if ((pipeline == NULL) || (pipeline->layout == VK_NULL_HANDLE) || (pipeline->resolveRenderPass == VK_NULL_HANDLE)) {
        return XUD_INVALID_ARG;
    }
    if ((pipeline->resolveShaderStages[XUD_PIPELINE_VERTEX_STAGE].module == VK_NULL_HANDLE)
        || (pipeline->resolveShaderStages[XUD_PIPELINE_FRAGMENT_STAGE].module == VK_NULL_HANDLE)) {
        return XUD_INVALID_ARG;
    }
    /* The shared rows must be stamped, the same rule xud_pipeline_fill_create_info() applies to the
       rows it points at; the resolve row's depth-stencil row is its own and is checked beside them. */
    if ((pipeline->vertexInput.sType != VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO)
        || (pipeline->inputAssembly[XUD_BATCH_TOPOLOGY_TRIANGLES].sType != VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO)
        || (pipeline->viewportState.sType != VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO)
        || (pipeline->rasterizer.sType != VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO)
        || (pipeline->multisample.sType != VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO)
        || (pipeline->blending[XUD_BATCH_VARIANT_OPAQUE].sType != VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO)
        || (pipeline->dynamicState.sType != VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO)
        || (pipeline->resolveDepthStencil.sType != VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO)) {
        return XUD_UNSUPPORTED;
    }

    outCreateInfo->sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    outCreateInfo->pNext = NULL;
    outCreateInfo->flags = 0u;
    outCreateInfo->stageCount = XUD_PIPELINE_STAGE_COUNT;
    outCreateInfo->pStages = pipeline->resolveShaderStages;
    outCreateInfo->pTessellationState = NULL;
    outCreateInfo->pVertexInputState = &xudBlitVertexInput; /* the file-scope zero row, deliberately:
        the triangle's only input is gl_VertexIndex and the resolve pass binds no vertex buffer, so the
        row that names nothing IS the correct row here - and it is NOT pipeline->vertexInput, which the
        open above has already filled with the batch's nine rows */
    outCreateInfo->pInputAssemblyState = &pipeline->inputAssembly[XUD_BATCH_TOPOLOGY_TRIANGLES];
    outCreateInfo->pViewportState = &pipeline->viewportState;
    outCreateInfo->pRasterizationState = &pipeline->rasterizer;
    outCreateInfo->pMultisampleState = &pipeline->multisample;
    outCreateInfo->pDepthStencilState = &pipeline->resolveDepthStencil;
    outCreateInfo->pColorBlendState = &pipeline->blending[XUD_BATCH_VARIANT_OPAQUE]; /* blend DISABLED:
        the resolve pass replaces every pixel it covers */
    outCreateInfo->pDynamicState = &pipeline->dynamicState;
    outCreateInfo->layout = pipeline->layout;
    outCreateInfo->renderPass = pipeline->resolveRenderPass;
    outCreateInfo->subpass = XUD_PIPELINE_FIRST_SUBPASS;
    outCreateInfo->basePipelineHandle = VK_NULL_HANDLE;
    outCreateInfo->basePipelineIndex = -1;

    return XUD_OK;
}

XudResult xud_pipeline_fill_prepass_create_info(const XudPipeline* pipeline, VkGraphicsPipelineCreateInfo* outCreateInfo) {
    if (outCreateInfo == NULL) {
        return XUD_INVALID_ARG;
    }
    if ((pipeline == NULL) || (pipeline->layout == VK_NULL_HANDLE) || (pipeline->prepassRenderPass == VK_NULL_HANDLE)) {
        return XUD_INVALID_ARG;
    }
    if (pipeline->prepassShaderStage.module == VK_NULL_HANDLE) {
        /* The one stage the pre-pass has: assembled before the module was opened is this layer's
           own ordering error, and creating it would build a pipeline whose stage names nothing. */
        return XUD_INVALID_ARG;
    }
    /* The shared rows must be stamped, the same rule the resolve fill applies to its own; the
       pre-pass's vertex-input, depth-stencil and blend rows are its own and are checked beside
       them - the three rows that say what makes this pipeline a pre-pass at all. */
    if ((pipeline->prepassVertexInput.sType != VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO)
        || (pipeline->inputAssembly[XUD_BATCH_TOPOLOGY_TRIANGLES].sType != VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO)
        || (pipeline->viewportState.sType != VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO)
        || (pipeline->rasterizer.sType != VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO)
        || (pipeline->multisample.sType != VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO)
        || (pipeline->prepassBlending.sType != VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO)
        || (pipeline->dynamicState.sType != VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO)
        || (pipeline->prepassDepthStencil.sType != VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO)) {
        return XUD_UNSUPPORTED;
    }

    outCreateInfo->sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    outCreateInfo->pNext = NULL;
    outCreateInfo->flags = 0u;
    outCreateInfo->stageCount = 1u; /* one stage, the vertex stage: a pre-pass with a fragment
                                        stage is the shaded pass again, which is what this
                                        pipeline exists to not-be */
    outCreateInfo->pStages = &pipeline->prepassShaderStage;
    outCreateInfo->pTessellationState = NULL;
    outCreateInfo->pVertexInputState = &pipeline->prepassVertexInput; /* the batch's position
                                                                          stream alone */
    outCreateInfo->pInputAssemblyState = &pipeline->inputAssembly[XUD_BATCH_TOPOLOGY_TRIANGLES];
    outCreateInfo->pViewportState = &pipeline->viewportState;
    outCreateInfo->pRasterizationState = &pipeline->rasterizer;
    outCreateInfo->pMultisampleState = &pipeline->multisample;
    outCreateInfo->pDepthStencilState = &pipeline->prepassDepthStencil;
    outCreateInfo->pColorBlendState = &pipeline->prepassBlending; /* attachmentCount 0: the
                                                                      subpass it runs in has no
                                                                      colour reference */
    outCreateInfo->pDynamicState = &pipeline->dynamicState;
    outCreateInfo->layout = pipeline->layout;
    outCreateInfo->renderPass = pipeline->prepassRenderPass;
    outCreateInfo->subpass = XUD_PIPELINE_FIRST_SUBPASS;
    outCreateInfo->basePipelineHandle = VK_NULL_HANDLE;
    outCreateInfo->basePipelineIndex = -1;

    return XUD_OK;
}

XudResult xud_pipeline_fill_shadow_create_info(const XudPipeline* pipeline, VkGraphicsPipelineCreateInfo* outCreateInfo) {
    if (outCreateInfo == NULL) {
        return XUD_INVALID_ARG;
    }
    if ((pipeline == NULL) || (pipeline->layout == VK_NULL_HANDLE) || (pipeline->shadowRenderPass == VK_NULL_HANDLE)) {
        return XUD_INVALID_ARG;
    }
    if (pipeline->shadowShaderStage.module == VK_NULL_HANDLE) {
        return XUD_INVALID_ARG;
    }
    if ((pipeline->prepassVertexInput.sType != VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO)
        || (pipeline->inputAssembly[XUD_BATCH_TOPOLOGY_TRIANGLES].sType != VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO)
        || (pipeline->viewportState.sType != VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO)
        || (pipeline->rasterizer.sType != VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO)
        || (pipeline->multisample.sType != VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO)
        || (pipeline->prepassBlending.sType != VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO)
        || (pipeline->dynamicState.sType != VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO)
        || (pipeline->prepassDepthStencil.sType != VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO)) {
        return XUD_UNSUPPORTED;
    }

    outCreateInfo->sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    outCreateInfo->pNext = NULL;
    outCreateInfo->flags = 0u;
    outCreateInfo->stageCount = 1u; /* one stage, and the same reason the pre-pass gives: a shadow
                                        pass with a fragment stage is the shaded pass again */
    outCreateInfo->pStages = &pipeline->shadowShaderStage; /* the sun's module, not the
                                                                pre-pass's: the only two rows
                                                                that differ are this one and the
                                                                pass below */
    outCreateInfo->pTessellationState = NULL;
    outCreateInfo->pVertexInputState = &pipeline->prepassVertexInput; /* the position stream, the
                                                                          same row: the occluder
                                                                          is the same triangles */
    outCreateInfo->pInputAssemblyState = &pipeline->inputAssembly[XUD_BATCH_TOPOLOGY_TRIANGLES];
    outCreateInfo->pViewportState = &pipeline->viewportState;
    outCreateInfo->pRasterizationState = &pipeline->rasterizer;
    outCreateInfo->pMultisampleState = &pipeline->multisample;
    outCreateInfo->pDepthStencilState = &pipeline->prepassDepthStencil; /* test ON, write ON,
                                                                            GREATER: the map is the
                                                                            same reversed-depth
                                                                            contest as the
                                                                            pre-pass's */
    outCreateInfo->pColorBlendState = &pipeline->prepassBlending; /* attachmentCount 0 - and the
                                                                       shadow pass has literally
                                                                       no colour attachment, not
                                                                       merely an unreferenced one */
    outCreateInfo->pDynamicState = &pipeline->dynamicState;
    outCreateInfo->layout = pipeline->layout; /* the ONE layout: the shadow draw pushes the same
                                                   row and binds the same set 0 - which is how the
                                                   sun's matrix reaches the vertex stage without a
                                                   second block anywhere */
    outCreateInfo->renderPass = pipeline->shadowRenderPass;
    outCreateInfo->subpass = XUD_PIPELINE_FIRST_SUBPASS;
    outCreateInfo->basePipelineHandle = VK_NULL_HANDLE;
    outCreateInfo->basePipelineIndex = -1;

    return XUD_OK;
}

XudResult xud_pipeline_fill_bloom_create_info(
    const XudPipeline* pipeline, VkGraphicsPipelineCreateInfo* outCreateInfo, VkPipelineShaderStageCreateInfo* stages) {
    if ((outCreateInfo == NULL) || (stages == NULL)) {
        return XUD_INVALID_ARG;
    }
    if ((pipeline == NULL) || (pipeline->layout == VK_NULL_HANDLE) || (pipeline->bloomRenderPass == VK_NULL_HANDLE)) {
        return XUD_INVALID_ARG;
    }
    if ((stages[XUD_PIPELINE_VERTEX_STAGE].module == VK_NULL_HANDLE) || (stages[XUD_PIPELINE_FRAGMENT_STAGE].module == VK_NULL_HANDLE)) {
        return XUD_INVALID_ARG;
    }
    if ((pipeline->vertexInput.sType != VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO)
        || (pipeline->inputAssembly[XUD_BATCH_TOPOLOGY_TRIANGLES].sType != VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO)
        || (pipeline->viewportState.sType != VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO)
        || (pipeline->rasterizer.sType != VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO)
        || (pipeline->multisample.sType != VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO)
        || (pipeline->blending[XUD_BATCH_VARIANT_OPAQUE].sType != VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO)
        || (pipeline->dynamicState.sType != VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO)
        || (pipeline->resolveDepthStencil.sType != VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO)) {
        return XUD_UNSUPPORTED;
    }

    /* The resolve's rows whole - the zero vertex input (the chain's triangle is the resolve's
       triangle, invented from gl_VertexIndex, bound to no buffer), the no-depth row (a glow has
       no depth to test and none to keep), blend disabled (every covered pixel is replaced, both
       by the threshold's write and by the blur's) - and the row the caller names: the stage
       pair whose fragment half is one of the two bloom modules. */
    outCreateInfo->sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    outCreateInfo->pNext = NULL;
    outCreateInfo->flags = 0u;
    outCreateInfo->stageCount = XUD_PIPELINE_STAGE_COUNT;
    outCreateInfo->pStages = stages;
    outCreateInfo->pTessellationState = NULL;
    outCreateInfo->pVertexInputState = &xudBlitVertexInput; /* the zero row, for the same reason the
        resolve filler names it: the chain's triangle is invented and bound to no buffer */
    outCreateInfo->pInputAssemblyState = &pipeline->inputAssembly[XUD_BATCH_TOPOLOGY_TRIANGLES];
    outCreateInfo->pViewportState = &pipeline->viewportState;
    outCreateInfo->pRasterizationState = &pipeline->rasterizer;
    outCreateInfo->pMultisampleState = &pipeline->multisample;
    outCreateInfo->pDepthStencilState = &pipeline->resolveDepthStencil;
    outCreateInfo->pColorBlendState = &pipeline->blending[XUD_BATCH_VARIANT_OPAQUE];
    outCreateInfo->pDynamicState = &pipeline->dynamicState;
    outCreateInfo->layout = pipeline->layout;
    outCreateInfo->renderPass = pipeline->bloomRenderPass;
    outCreateInfo->subpass = XUD_PIPELINE_FIRST_SUBPASS;
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
 * The module teardown, in one helper rather than at every failure rung
 *-----------------------------------------------------------------------------------------------*/

/* The reverse of the order the create rung below makes them in: the frag axis first, then the vertex
   module, because the vertex module was created first and is therefore the last thing that may still
   be named. One helper rather than the ladder repeated at each of the six failure rungs is what
   keeps "every module that got made gets retired" one fact rather than six copies of it that can
   disagree about how many modules there are - the reason the frag axis's length never appears in
   those rungs. */
static void xud_pipeline_close_modules(XudShader** vertexShader, XudShader** fragModules, const XudDevice* device, XudLoader* loader) {
    uint32_t module = 0u;

    for (module = 0u; module < XUD_PIPELINE_FRAG_MODULE_COUNT; module++) {
        if (fragModules != NULL) {
            (void)xud_shader_close(&fragModules[module], device, loader);
        }
    }
    if (vertexShader != NULL) {
        (void)xud_shader_close(vertexShader, device, loader);
    }
}

/* Everything the create ladder has made BEFORE the shadow row is built - the variant list, the
   resolve row, the two present modules, the layout, the batch's modules, and the pre-pass row
   with its module. The shadow block's three failure paths all retire exactly this set, and one
   function states it once instead of the ladder growing a fourth and fifth and sixth copy of
   itself; the inline ladders above it predate the shadow row and stay as they were made. */
static void xud_pipeline_retire_up_to_prepass(XudPipeline* pipeline, XudLoader* loader, const XudDevice* device) {
    xud_pipeline_destroy_rows(loader, device, pipeline->pipeline);
    if (pipeline->resolvePipeline != VK_NULL_HANDLE) {
        (void)loader->deviceTable.vkDestroyPipeline(device->device, pipeline->resolvePipeline, NULL);
        pipeline->resolvePipeline = VK_NULL_HANDLE;
    }
    (void)xud_shader_close(&pipeline->presentFragmentShader, device, loader);
    (void)xud_shader_close(&pipeline->presentVertexShader, device, loader);
    if (pipeline->layout != VK_NULL_HANDLE) {
        (void)loader->deviceTable.vkDestroyPipelineLayout(device->device, pipeline->layout, NULL);
        pipeline->layout = VK_NULL_HANDLE;
    }
    xud_pipeline_close_modules(&pipeline->vertexShader, pipeline->fragmentShader, device, loader);
    if (pipeline->prepassPipeline != VK_NULL_HANDLE) {
        (void)loader->deviceTable.vkDestroyPipeline(device->device, pipeline->prepassPipeline, NULL);
        pipeline->prepassPipeline = VK_NULL_HANDLE;
    }
    (void)xud_shader_close(&pipeline->prepassVertexShader, device, loader);
}

static void xud_pipeline_retire_up_to_shadow(XudPipeline* pipeline, XudLoader* loader, const XudDevice* device) {
    xud_pipeline_retire_up_to_prepass(pipeline, loader, device);
    if (pipeline->shadowPipeline != VK_NULL_HANDLE) {
        (void)loader->deviceTable.vkDestroyPipeline(device->device, pipeline->shadowPipeline, NULL);
        pipeline->shadowPipeline = VK_NULL_HANDLE;
    }
    (void)xud_shader_close(&pipeline->shadowVertexShader, device, loader);
}

static void xud_pipeline_retire_bloom(XudPipeline* pipeline, XudLoader* loader, const XudDevice* device) {
    if (pipeline->bloomBlurPipeline != VK_NULL_HANDLE) {
        (void)loader->deviceTable.vkDestroyPipeline(device->device, pipeline->bloomBlurPipeline, NULL);
        pipeline->bloomBlurPipeline = VK_NULL_HANDLE;
    }
    if (pipeline->brightPipeline != VK_NULL_HANDLE) {
        (void)loader->deviceTable.vkDestroyPipeline(device->device, pipeline->brightPipeline, NULL);
        pipeline->brightPipeline = VK_NULL_HANDLE;
    }
    (void)xud_shader_close(&pipeline->bloomBlurFragmentShader, device, loader);
    (void)xud_shader_close(&pipeline->bloomBrightFragmentShader, device, loader);
}

/*-----------------------------------------------------------------------------------------------
 * The create rung - the driver half
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_pipeline_open(XudPipeline** outPipeline, XudLoader* loader, const XudDevice* device, const XudDescriptor* descriptor,
    const XudBatch* batch, VkRenderPass renderPass, VkRenderPass resolveRenderPass, VkRenderPass prepassRenderPass,
    VkRenderPass shadowRenderPass, VkRenderPass bloomRenderPass) {
    XudPipeline* pipeline = NULL;
    XudShader* vertexShader = NULL;
    XudShader* fragModule[XUD_PIPELINE_FRAG_MODULE_COUNT] = { NULL }; /* one per entry of the FRAG axis,
                            not one per row of the list: the two untextured columns are built from one
                            module */
    VkPipelineLayoutCreateInfo layoutInfo;
    VkPipeline created[XUD_PIPELINE_ROW_COUNT] = { VK_NULL_HANDLE }; /* zeroed in full: only the
                            rows a create call reached have a handle in them, and the ladder below
                            retires the rows that did get made by skipping the ones that did not */
    XudShader* presentVertexShader = NULL; /* the resolve pipeline's two modules, opened after the
                            batch list exists - they are only ever its stages */
    XudShader* presentFragmentShader = NULL;
    VkPipeline resolveCreated = VK_NULL_HANDLE; /* the resolve row, created last and recorded into
                            the struct only when it exists, the ladder shape the batch list uses */
    XudShader* prepassVertexShader = NULL; /* the pre-pass pipeline's one module, opened after the
                            resolve row exists - like the present pair, it is only ever its stage */
    VkPipeline prepassCreated = VK_NULL_HANDLE;
    XudShader* shadowVertexShader = NULL; /* the shadow pass's one module, opened after the
                            pre-pass row exists - and only opened when the rung was handed a
                            shadow pass at all: no pass, no module, no row */
    VkPipeline shadowCreated = VK_NULL_HANDLE;
    VkPipeline brightCreated = VK_NULL_HANDLE;
    VkPipeline blurCreated = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    XudResult result = XUD_INVALID_ARG;
    VkResult vk = VK_ERROR_UNKNOWN;
    uint32_t variant = 0u;
    uint32_t topology = 0u;
    uint32_t row = 0u;
    uint32_t module = 0u;

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
    /* The resolve pass is the second half of the same pair: the core-1.4 binding binds the resolve
       pipeline to the SCREEN pass at create, and a handle-less one is as bad an argument as a
       handle-less batch pass. */
    if (resolveRenderPass == VK_NULL_HANDLE) {
        return XUD_INVALID_ARG;
    }
    /* The pre-pass pass is the third of the same trio: the pre-pass row is bound to it at create
       exactly as the batch list is bound to the scratch pass and the resolve row to the screen
       pass, and a handle-less third is as bad an argument as a handle-less first. */
    if (prepassRenderPass == VK_NULL_HANDLE) {
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

    /* The pre-pass's own vertex-input row beside the four-stream one: one binding, the position
       stream, because the pre-pass module reads one attribute and a row naming four would be a
       declaration its only stage does not honor. */
    result = xud_pipeline_fill_prepass_vertex_input(pipeline, batch);
    if (result != XUD_OK) {
        free(pipeline);
        return result;
    }

    /* Borrowed, not owned: the pass belongs to the render-pass rung above, which destroys it after
       this rung's xud_pipeline_close() has retired the pipeline that names it - the one teardown
       order xud_renderer.c states. */
    pipeline->renderPass = renderPass;
    pipeline->resolveRenderPass = resolveRenderPass; /* the screen pass, borrowed exactly as the
                            batch pass above is: the resolve row names it at create and outlives
                            this rung's teardown into that pass's destroy */
    pipeline->prepassRenderPass = prepassRenderPass; /* and the third of the trio, borrowed the
                            same way: the pre-pass row names it at create */

    pipeline->shadowRenderPass = shadowRenderPass; /* the optional fourth: VK_NULL_HANDLE here is
                            the shadows-off configuration, and the shadow row below is simply
                            never built; every other pass row of the struct is unconditional */
    /* The modules first: one vertex module for the whole list and one module per entry of the FRAG
       axis, so the two untextured columns share one VkShaderModule and only the textured column
       names the second. The layout and the pipelines are built from what these hand back.
    */
    result = xud_shader_open(&vertexShader, loader, device, xudSolidVertSpv, sizeof(xudSolidVertSpv));
    if (result != XUD_OK) {
        free(pipeline);
        return result;
    }

    for (module = 0u; module < XUD_PIPELINE_FRAG_MODULE_COUNT; module++) {
        result = xud_shader_open(&fragModule[module], loader, device, xudPipelineFragCode[module], xudPipelineFragCodeSize[module]);
        if (result != XUD_OK) {
            xud_pipeline_close_modules(&vertexShader, fragModule, device, loader);
            free(pipeline);
            return result;
        }
    }

    for (module = 0u; module < XUD_PIPELINE_FRAG_MODULE_COUNT; module++) {
        pipeline->fragmentShader[module] = fragModule[module];
    }
    pipeline->vertexShader = vertexShader;

    /* The stage rows, one PAIR per row of the list rather than one pair for the whole rung: the frag
       module is pipeline state and the variant axis names which module each row's fragment stage is
       built from, so the pair is filled per row from the module that column maps to. Filled before
       any create call, so the create-info the driver reads and the record keep the same bytes.
    */
    for (topology = 0u; topology < XUD_BATCH_TOPOLOGY_COUNT; topology++) {
        for (variant = 0u; variant < XUD_BATCH_VARIANT_COUNT; variant++) {
            row = XUD_PIPELINE_ROW(topology, variant);
            result = xud_pipeline_fill_shader_stages(
                pipeline->shaderStages[row], vertexShader->module, fragModule[xudPipelineFragModule[variant]]->module);
            if (result != XUD_OK) {
                xud_pipeline_close_modules(&vertexShader, fragModule, device, loader);
                free(pipeline);
                return result;
            }
        }
    }

    result = xud_pipeline_fill_layout_info(descriptor, &layoutInfo);
    if (result != XUD_OK) {
        xud_pipeline_close_modules(&vertexShader, fragModule, device, loader);
        free(pipeline);
        return result;
    }

    /* vkCreatePipelineLayout returns VkResult, so the answer goes through the one funnel every
       layer maps a VkResult through - a layout the driver refused is reported with the driver's
       own reason, not with a code this layer invented. */
    vk = loader->deviceTable.vkCreatePipelineLayout(device->device, &layoutInfo, NULL, &layout);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK) {
        xud_pipeline_close_modules(&vertexShader, fragModule, device, loader);
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
                xud_pipeline_close_modules(&vertexShader, fragModule, device, loader);
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
                xud_pipeline_close_modules(&vertexShader, fragModule, device, loader);
                free(pipeline);
                return result;
            }
        }
    }

    for (row = 0u; row < XUD_PIPELINE_ROW_COUNT; row++) {
        pipeline->pipeline[row] = created[row];
    }

    /* The resolve pipeline, last: its two present modules open now - nothing before this line reads
       them - and the create binds them to the SCREEN pass recorded above, where the batch list above
       is bound to the high-dynamic-range pass. The two bindings cannot be one handle: pass
       compatibility - the rule that lets one pipeline serve two passes - is format-for-format on
       every attachment, and the two colour attachments differ in format, so the scene rows belong
       to the scratch pass and this row to the screen pass, one pipeline each. The failure ladder
       retires the recorded batch rows, the layout and every module, present ones first.
    */
    result = xud_shader_open(&presentVertexShader, loader, device, xudPresentVertSpv, sizeof(xudPresentVertSpv));
    if (result != XUD_OK) {
        xud_pipeline_destroy_rows(loader, device, pipeline->pipeline);
        (void)loader->deviceTable.vkDestroyPipelineLayout(device->device, pipeline->layout, NULL);
        pipeline->layout = VK_NULL_HANDLE;
        xud_pipeline_close_modules(&pipeline->vertexShader, pipeline->fragmentShader, device, loader);
        free(pipeline);
        return result;
    }
    result = xud_shader_open(&presentFragmentShader, loader, device, xudPresentFragSpv, sizeof(xudPresentFragSpv));
    if (result != XUD_OK) {
        (void)xud_shader_close(&presentVertexShader, device, loader);
        xud_pipeline_destroy_rows(loader, device, pipeline->pipeline);
        (void)loader->deviceTable.vkDestroyPipelineLayout(device->device, pipeline->layout, NULL);
        pipeline->layout = VK_NULL_HANDLE;
        xud_pipeline_close_modules(&pipeline->vertexShader, pipeline->fragmentShader, device, loader);
        free(pipeline);
        return result;
    }
    pipeline->presentVertexShader = presentVertexShader;
    pipeline->presentFragmentShader = presentFragmentShader;
    result = xud_pipeline_fill_shader_stages(pipeline->resolveShaderStages, presentVertexShader->module, presentFragmentShader->module);
    if (result != XUD_OK) {
        (void)xud_shader_close(&presentFragmentShader, device, loader);
        (void)xud_shader_close(&presentVertexShader, device, loader);
        pipeline->presentVertexShader = NULL;
        pipeline->presentFragmentShader = NULL;
        xud_pipeline_destroy_rows(loader, device, pipeline->pipeline);
        (void)loader->deviceTable.vkDestroyPipelineLayout(device->device, pipeline->layout, NULL);
        pipeline->layout = VK_NULL_HANDLE;
        xud_pipeline_close_modules(&pipeline->vertexShader, pipeline->fragmentShader, device, loader);
        free(pipeline);
        return result;
    }
    result = xud_pipeline_fill_resolve_create_info(pipeline, &pipeline->resolveCreateInfo);
    if (result != XUD_OK) {
        (void)xud_shader_close(&presentFragmentShader, device, loader);
        (void)xud_shader_close(&presentVertexShader, device, loader);
        pipeline->presentVertexShader = NULL;
        pipeline->presentFragmentShader = NULL;
        xud_pipeline_destroy_rows(loader, device, pipeline->pipeline);
        (void)loader->deviceTable.vkDestroyPipelineLayout(device->device, pipeline->layout, NULL);
        pipeline->layout = VK_NULL_HANDLE;
        xud_pipeline_close_modules(&pipeline->vertexShader, pipeline->fragmentShader, device, loader);
        free(pipeline);
        return result;
    }
    vk = loader->deviceTable.vkCreateGraphicsPipelines(
        device->device, VK_NULL_HANDLE, 1u, &pipeline->resolveCreateInfo, NULL, &resolveCreated);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK) {
        (void)xud_shader_close(&presentFragmentShader, device, loader);
        (void)xud_shader_close(&presentVertexShader, device, loader);
        pipeline->presentVertexShader = NULL;
        pipeline->presentFragmentShader = NULL;
        xud_pipeline_destroy_rows(loader, device, pipeline->pipeline);
        (void)loader->deviceTable.vkDestroyPipelineLayout(device->device, pipeline->layout, NULL);
        pipeline->layout = VK_NULL_HANDLE;
        xud_pipeline_close_modules(&pipeline->vertexShader, pipeline->fragmentShader, device, loader);
        free(pipeline);
        return result;
    }
    pipeline->resolvePipeline = resolveCreated;

    /* The depth pre-pass's pipeline, last beside the resolve's: its one module opens now, its one
       stage row is stamped from it (a single xud_pipeline_fill_shader_stages() pair would name a
       fragment stage the pre-pass must not have, so the row is written whole here), and the create
       binds the row to the PRE-PASS pass - the pass whose depth it writes and whose colour it
       references nowhere. The ladder retires everything before it in the order the resolve rung
       established. */
    result = xud_shader_open(&prepassVertexShader, loader, device, xudPrepassVertSpv, sizeof(xudPrepassVertSpv));
    if (result != XUD_OK) {
        xud_pipeline_destroy_rows(loader, device, pipeline->pipeline);
        (void)loader->deviceTable.vkDestroyPipeline(device->device, pipeline->resolvePipeline, NULL);
        pipeline->resolvePipeline = VK_NULL_HANDLE;
        (void)xud_shader_close(&pipeline->presentFragmentShader, device, loader);
        (void)xud_shader_close(&pipeline->presentVertexShader, device, loader);
        (void)loader->deviceTable.vkDestroyPipelineLayout(device->device, pipeline->layout, NULL);
        pipeline->layout = VK_NULL_HANDLE;
        xud_pipeline_close_modules(&pipeline->vertexShader, pipeline->fragmentShader, device, loader);
        free(pipeline);
        return result;
    }
    pipeline->prepassShaderStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline->prepassShaderStage.pNext = NULL;
    pipeline->prepassShaderStage.flags = 0u;
    pipeline->prepassShaderStage.stage = VK_SHADER_STAGE_VERTEX_BIT; /* the one stage: a pre-pass
                                                                          with a fragment stage is
                                                                          the shaded pass again */
    pipeline->prepassShaderStage.module = prepassVertexShader->module;
    pipeline->prepassShaderStage.pName = XUD_PIPELINE_ENTRY_POINT;
    pipeline->prepassShaderStage.pSpecializationInfo = NULL;
    result = xud_pipeline_fill_prepass_create_info(pipeline, &pipeline->prepassCreateInfo);
    if (result != XUD_OK) {
        (void)xud_shader_close(&prepassVertexShader, device, loader);
        pipeline->prepassShaderStage.module = VK_NULL_HANDLE;
        xud_pipeline_destroy_rows(loader, device, pipeline->pipeline);
        (void)loader->deviceTable.vkDestroyPipeline(device->device, pipeline->resolvePipeline, NULL);
        pipeline->resolvePipeline = VK_NULL_HANDLE;
        (void)xud_shader_close(&pipeline->presentFragmentShader, device, loader);
        (void)xud_shader_close(&pipeline->presentVertexShader, device, loader);
        (void)loader->deviceTable.vkDestroyPipelineLayout(device->device, pipeline->layout, NULL);
        pipeline->layout = VK_NULL_HANDLE;
        xud_pipeline_close_modules(&pipeline->vertexShader, pipeline->fragmentShader, device, loader);
        free(pipeline);
        return result;
    }
    vk = loader->deviceTable.vkCreateGraphicsPipelines(
        device->device, VK_NULL_HANDLE, 1u, &pipeline->prepassCreateInfo, NULL, &prepassCreated);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK) {
        (void)xud_shader_close(&prepassVertexShader, device, loader);
        pipeline->prepassShaderStage.module = VK_NULL_HANDLE;
        xud_pipeline_destroy_rows(loader, device, pipeline->pipeline);
        (void)loader->deviceTable.vkDestroyPipeline(device->device, pipeline->resolvePipeline, NULL);
        pipeline->resolvePipeline = VK_NULL_HANDLE;
        (void)xud_shader_close(&pipeline->presentFragmentShader, device, loader);
        (void)xud_shader_close(&pipeline->presentVertexShader, device, loader);
        (void)loader->deviceTable.vkDestroyPipelineLayout(device->device, pipeline->layout, NULL);
        pipeline->layout = VK_NULL_HANDLE;
        xud_pipeline_close_modules(&pipeline->vertexShader, pipeline->fragmentShader, device, loader);
        free(pipeline);
        return result;
    }
    pipeline->prepassVertexShader = prepassVertexShader;
    pipeline->prepassPipeline = prepassCreated;

    /* The shadow pass's pipeline, last: the sun's module, the pre-pass's rows, the map's pass.
       The whole row is conditional on having been handed a shadow pass - the frame loop reads
       the same null handle as the same shadows-off answer the renderer gave. */
    if (shadowRenderPass != VK_NULL_HANDLE) {
        result = xud_shader_open(&shadowVertexShader, loader, device, xudShadowVertSpv, sizeof(xudShadowVertSpv));
        if (result != XUD_OK) {
            xud_pipeline_retire_up_to_prepass(pipeline, loader, device);
            free(pipeline);
            return result;
        }
        pipeline->shadowShaderStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipeline->shadowShaderStage.pNext = NULL;
        pipeline->shadowShaderStage.flags = 0u;
        pipeline->shadowShaderStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
        pipeline->shadowShaderStage.module = shadowVertexShader->module;
        pipeline->shadowShaderStage.pName = XUD_PIPELINE_ENTRY_POINT;
        pipeline->shadowShaderStage.pSpecializationInfo = NULL;
        result = xud_pipeline_fill_shadow_create_info(pipeline, &pipeline->shadowCreateInfo);
        if (result != XUD_OK) {
            (void)xud_shader_close(&shadowVertexShader, device, loader);
            pipeline->shadowShaderStage.module = VK_NULL_HANDLE;
            xud_pipeline_retire_up_to_prepass(pipeline, loader, device);
            free(pipeline);
            return result;
        }
        vk = loader->deviceTable.vkCreateGraphicsPipelines(
            device->device, VK_NULL_HANDLE, 1u, &pipeline->shadowCreateInfo, NULL, &shadowCreated);
        result = xud_result_from_vk(vk);
        if (result != XUD_OK) {
            (void)xud_shader_close(&shadowVertexShader, device, loader);
            pipeline->shadowShaderStage.module = VK_NULL_HANDLE;
            xud_pipeline_retire_up_to_prepass(pipeline, loader, device);
            free(pipeline);
            return result;
        }
        pipeline->shadowVertexShader = shadowVertexShader;
        pipeline->shadowPipeline = shadowCreated;
    }

    /* The bloom chain's pipelines, last: the two fragment modules, the present vertex module
       reused a third time (the chain's fullscreen triangle is the resolve's triangle), and two
       create-infos that differ from the resolve's only in the pass handle and the stage pair -
       the bright row first, then the one blur row every tail level binds. The whole block is
       conditional on having been handed a bloom pass - the frame loop reads the null rows as
       the same bloom-off answer the renderer gave. */
    if (bloomRenderPass != VK_NULL_HANDLE) {
        pipeline->bloomRenderPass = bloomRenderPass;
        result
            = xud_shader_open(&pipeline->bloomBrightFragmentShader, loader, device, xudBloomBrightFragSpv, sizeof(xudBloomBrightFragSpv));
        if (result != XUD_OK) {
            xud_pipeline_retire_up_to_shadow(pipeline, loader, device);
            free(pipeline);
            return result;
        }
        result = xud_shader_open(&pipeline->bloomBlurFragmentShader, loader, device, xudBloomBlurFragSpv, sizeof(xudBloomBlurFragSpv));
        if (result != XUD_OK) {
            xud_pipeline_retire_bloom(pipeline, loader, device);
            xud_pipeline_retire_up_to_shadow(pipeline, loader, device);
            free(pipeline);
            return result;
        }
        result = xud_pipeline_fill_shader_stages(
            pipeline->brightShaderStages, presentVertexShader->module, pipeline->bloomBrightFragmentShader->module);
        if (result != XUD_OK) {
            xud_pipeline_retire_bloom(pipeline, loader, device);
            xud_pipeline_retire_up_to_shadow(pipeline, loader, device);
            free(pipeline);
            return result;
        }
        result = xud_pipeline_fill_shader_stages(
            pipeline->bloomBlurShaderStages, presentVertexShader->module, pipeline->bloomBlurFragmentShader->module);
        if (result != XUD_OK) {
            xud_pipeline_retire_bloom(pipeline, loader, device);
            xud_pipeline_retire_up_to_shadow(pipeline, loader, device);
            free(pipeline);
            return result;
        }
        result = xud_pipeline_fill_bloom_create_info(pipeline, &pipeline->brightCreateInfo, pipeline->brightShaderStages);
        if (result != XUD_OK) {
            xud_pipeline_retire_bloom(pipeline, loader, device);
            xud_pipeline_retire_up_to_shadow(pipeline, loader, device);
            free(pipeline);
            return result;
        }
        vk = loader->deviceTable.vkCreateGraphicsPipelines(
            device->device, VK_NULL_HANDLE, 1u, &pipeline->brightCreateInfo, NULL, &brightCreated);
        result = xud_result_from_vk(vk);
        if (result != XUD_OK) {
            xud_pipeline_retire_bloom(pipeline, loader, device);
            xud_pipeline_retire_up_to_shadow(pipeline, loader, device);
            free(pipeline);
            return result;
        }
        pipeline->brightPipeline = brightCreated;
        result = xud_pipeline_fill_bloom_create_info(pipeline, &pipeline->bloomBlurCreateInfo, pipeline->bloomBlurShaderStages);
        if (result != XUD_OK) {
            xud_pipeline_retire_bloom(pipeline, loader, device);
            xud_pipeline_retire_up_to_shadow(pipeline, loader, device);
            free(pipeline);
            return result;
        }
        vk = loader->deviceTable.vkCreateGraphicsPipelines(
            device->device, VK_NULL_HANDLE, 1u, &pipeline->bloomBlurCreateInfo, NULL, &blurCreated);
        result = xud_result_from_vk(vk);
        if (result != XUD_OK) {
            xud_pipeline_retire_bloom(pipeline, loader, device);
            xud_pipeline_retire_up_to_shadow(pipeline, loader, device);
            free(pipeline);
            return result;
        }
        pipeline->bloomBlurPipeline = blurCreated;
    }

    *outPipeline = pipeline;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The teardown rung - the driver half
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_pipeline_close(XudPipeline** ioPipeline, const XudDevice* device, XudLoader* loader) {
    XudPipeline* victim = NULL;
    XudResult closedModule = XUD_INVALID_ARG;
    XudResult first = XUD_OK;
    uint32_t row = 0u;
    uint32_t module = 0u;

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
    if (victim->resolvePipeline != VK_NULL_HANDLE) {
        if (loader->deviceTable.vkDestroyPipeline == NULL) {
            return XUD_UNSUPPORTED;
        }
    }
    if (victim->prepassPipeline != VK_NULL_HANDLE) {
        if (loader->deviceTable.vkDestroyPipeline == NULL) {
            return XUD_UNSUPPORTED;
        }
    }
    if (victim->shadowPipeline != VK_NULL_HANDLE) {
        if (loader->deviceTable.vkDestroyPipeline == NULL) {
            return XUD_UNSUPPORTED;
        }
    }
    if (victim->bloomBlurPipeline != VK_NULL_HANDLE) {
        if (loader->deviceTable.vkDestroyPipeline == NULL) {
            return XUD_UNSUPPORTED;
        }
    }
    if (victim->brightPipeline != VK_NULL_HANDLE) {
        if (loader->deviceTable.vkDestroyPipeline == NULL) {
            return XUD_UNSUPPORTED;
        }
    }
    xud_pipeline_destroy_rows(loader, device, victim->pipeline);
    if (victim->bloomBlurPipeline != VK_NULL_HANDLE) {
        loader->deviceTable.vkDestroyPipeline(device->device, victim->bloomBlurPipeline, NULL);
        victim->bloomBlurPipeline = VK_NULL_HANDLE;
    }
    if (victim->brightPipeline != VK_NULL_HANDLE) {
        loader->deviceTable.vkDestroyPipeline(device->device, victim->brightPipeline, NULL);
        victim->brightPipeline = VK_NULL_HANDLE;
    }
    if (victim->shadowPipeline != VK_NULL_HANDLE) {
        loader->deviceTable.vkDestroyPipeline(device->device, victim->shadowPipeline, NULL);
        victim->shadowPipeline = VK_NULL_HANDLE;
    }
    if (victim->prepassPipeline != VK_NULL_HANDLE) {
        loader->deviceTable.vkDestroyPipeline(device->device, victim->prepassPipeline, NULL);
        victim->prepassPipeline = VK_NULL_HANDLE;
    }
    if (victim->resolvePipeline != VK_NULL_HANDLE) {
        loader->deviceTable.vkDestroyPipeline(device->device, victim->resolvePipeline, NULL);
        victim->resolvePipeline = VK_NULL_HANDLE;
    }

    if (victim->layout != VK_NULL_HANDLE) {
        if (loader->deviceTable.vkDestroyPipelineLayout == NULL) {
            return XUD_UNSUPPORTED;
        }
        loader->deviceTable.vkDestroyPipelineLayout(device->device, victim->layout, NULL);
        victim->layout = VK_NULL_HANDLE;
    }

    for (module = 0u; module < XUD_PIPELINE_FRAG_MODULE_COUNT; module++) {
        if (victim->fragmentShader[module] != NULL) {
            closedModule = xud_shader_close(&victim->fragmentShader[module], device, loader);
            if ((closedModule != XUD_OK) && (closedModule != XUD_ALREADY)) {
                first = closedModule;
            }
        }
    }
    if (victim->vertexShader != NULL) {
        closedModule = xud_shader_close(&victim->vertexShader, device, loader);
        if ((closedModule != XUD_OK) && (closedModule != XUD_ALREADY)) {
            first = closedModule;
        }
    }
    if (victim->presentFragmentShader != NULL) {
        closedModule = xud_shader_close(&victim->presentFragmentShader, device, loader);
        if ((closedModule != XUD_OK) && (closedModule != XUD_ALREADY)) {
            first = closedModule;
        }
    }
    if (victim->presentVertexShader != NULL) {
        closedModule = xud_shader_close(&victim->presentVertexShader, device, loader);
        if ((closedModule != XUD_OK) && (closedModule != XUD_ALREADY)) {
            first = closedModule;
        }
    }
    if (victim->prepassVertexShader != NULL) {
        closedModule = xud_shader_close(&victim->prepassVertexShader, device, loader);
        if ((closedModule != XUD_OK) && (closedModule != XUD_ALREADY)) {
            first = closedModule;
        }
    }
    if (victim->shadowVertexShader != NULL) {
        closedModule = xud_shader_close(&victim->shadowVertexShader, device, loader);
        if ((closedModule != XUD_OK) && (closedModule != XUD_ALREADY)) {
            first = closedModule;
        }
    }
    if (victim->bloomBlurFragmentShader != NULL) {
        closedModule = xud_shader_close(&victim->bloomBlurFragmentShader, device, loader);
        if ((closedModule != XUD_OK) && (closedModule != XUD_ALREADY)) {
            first = closedModule;
        }
    }
    if (victim->bloomBrightFragmentShader != NULL) {
        closedModule = xud_shader_close(&victim->bloomBrightFragmentShader, device, loader);
        if ((closedModule != XUD_OK) && (closedModule != XUD_ALREADY)) {
            first = closedModule;
        }
    }

    free(victim);
    *ioPipeline = NULL;

    /* A refused teardown below is reported, not hidden - the same rule xud_close_context() follows
       with its own rungs. */
    return first;
}
