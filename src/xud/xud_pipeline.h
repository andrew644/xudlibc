/***********************************************************************************************
 *   xud_pipeline.h - the pipeline rung: the built-in SPIR-V to one graphics pipeline
 *
 *   Layer: above the shader rung, below the render pass. This rung owns the whole set of
 *   objects a pipeline is made of - the two VkShaderModules, the VkPipelineLayout and the
 *   VkPipeline - because a pipeline names its layout and its layout's modules are the only
 *   thing that keeps them alive, so all four die together and in one order: pipeline,
 *   layout, modules. "Explicit pipelines, created once at init, a small fixed set of
 *   variants, never created per draw" (the repo's rule) is what this rung is: the first
 *   triangle needs one variant, and the variants the rest of the plan asks for (textured,
 *   line, glyph, opaque/transparent) land beside this one in this same file, not as a
 *   pipeline-per-draw path anywhere above it.
 *
 *   What this rung does NOT have, and why: no descriptor set (set 0 and set 1 are the
 *   descriptor-layout step's, and a layout with zero sets is the honest answer for a
 *   shader that reads no binding), no push-constant range (the first triangle's shaders
 *   take no uniform in at all - the model matrix arrives with the batch step), and no
 *   vertex input (the allocator rung IS in the tree now (S12) but owns no VkBuffer at all -
 *   nothing above it has asked it for memory - so the pipeline declares zero bindings and
 *   zero attributes and the vertex shader generates its three vertices from gl_VertexIndex).
 *   The descriptor-layout and batch rungs above the allocator have not landed; the allocator
 *   has, and it is the rung that will hand this one the memory a VkBuffer sits in.
 ***********************************************************************************************/

#ifndef XUD_PIPELINE_H_
#define XUD_PIPELINE_H_

#include "volk.h"
#include "xud_device.h"
#include "xud_loader.h"
#include "xud_shader.h"
#include "xud_vulkan.h"
#include <assert.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

/*-----------------------------------------------------------------------------------------------
 * The one pipeline of this rung, and every row it was assembled from
 *-----------------------------------------------------------------------------------------------*/

#define XUD_PIPELINE_STAGE_COUNT 2u /* vertex + fragment, the only two stages this rung writes */
#define XUD_PIPELINE_VERTEX_STAGE 0u /* which row of shaderStages the vertex stage is in */
#define XUD_PIPELINE_FRAGMENT_STAGE 1u /* which row of shaderStages the fragment stage is in */
#define XUD_PIPELINE_DYNAMIC_STATE_COUNT                                                                                                   \
    2u /* VIEWPORT + SCISSOR, the two this rung sets per                                                                                   \
          frame - a fixed extent would be a second pipeline                                                                                \
          per window size, which is what dynamic state buys                                                                                \
          against */
#define XUD_PIPELINE_VIEWPORT_STATE 0u /* which row of dynamicStates the viewport is in */
#define XUD_PIPELINE_SCISSOR_STATE 1u /* which row of dynamicStates the scissor is in */
#define XUD_PIPELINE_ENTRY_POINT                                                                                                           \
    "main" /* the entry point of both modules, named rather than                                                                           \
                        left NULL: the spec's "NULL means main"                                                                            \
                        convenience is not what these headers'                                                                             \
                        VUIDs ask for - the validation layer on                                                                            \
                        this machine reports a NULL pName as a                                                                             \
                        hard error (probed today: VUID-VkPipeline                                                                          \
                        ShaderStageCreateInfo-pName-parameter and                                                                          \
                        -pName-00707 both fire), and "main" is                                                                             \
                        what glslangValidator writes here (probed                                                                          \
                        in the disassembly of both modules) */
#define XUD_PIPELINE_FIRST_SUBPASS                                                                                                         \
    0u /* the only subpass this library has, and the subpass                                                                               \
                      index the core-1.4 pipeline/pass binding                                                                             \
                      names */

typedef struct XudPipeline XudPipeline;
struct XudPipeline {
    VkPipeline pipeline; /* the one Vulkan object this layer owns: VK_NULL_HANDLE until
                            xud_pipeline_open() creates it, cleared and destroyed by
                            xud_pipeline_close() */
    VkPipelineLayout layout; /* owned beside it: a pipeline names its layout, and there is no
                                way to retire the layout while the pipeline that names it is
                                alive, so the two are one teardown step */
    VkRenderPass renderPass; /* BORROWED, not owned: the core-1.4 binding makes the pass an
                                argument of vkCreateGraphicsPipelines (see the note where
                                xud_pipeline_fill_create_info() writes it), so the pass handle
                                is recorded here and the render-pass rung above owns it and
                                destroys it after this rung's teardown has retired the
                                pipeline that names it */
    XudShader* vertexShader; /* owned: the module the vertex stage was built from */
    XudShader* fragmentShader; /* owned: the module the fragment stage was built from */

    /* --- the state it was handed, recorded rather than discarded: the create-info below
         points at these rows, so the create-info and the record of the pipeline ARE the
         same bytes and cannot disagree - the rule xud_swapchain.c states for its create-info
         and its record, one rung up --- */
    VkPipelineShaderStageCreateInfo shaderStages[XUD_PIPELINE_STAGE_COUNT];
    VkPipelineVertexInputStateCreateInfo vertexInput;
    VkPipelineInputAssemblyStateCreateInfo inputAssembly;
    VkPipelineViewportStateCreateInfo viewportState;
    VkPipelineRasterizationStateCreateInfo rasterizer;
    VkPipelineMultisampleStateCreateInfo multisample;
    VkPipelineDepthStencilStateCreateInfo depthStencil;
    VkPipelineColorBlendStateCreateInfo blending;
    VkPipelineColorBlendAttachmentState blendAttachment;
    VkPipelineDynamicStateCreateInfo dynamicState;
    VkDynamicState dynamicStates[XUD_PIPELINE_DYNAMIC_STATE_COUNT];
    VkGraphicsPipelineCreateInfo createInfo; /* what vkCreateGraphicsPipelines was handed */
};

/*-----------------------------------------------------------------------------------------------
 * The pipeline's shape, as this layer writes it
 *-----------------------------------------------------------------------------------------------*/

#define XUD_PIPELINE_TOPOLOGY                                                                                                              \
    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST /* the batch step's index                                                                          \
                           buffer feeds the same topology; a strip                                                                         \
                           would make the first triangle's three                                                                           \
                           vertices mean a different thing */
#define XUD_PIPELINE_CULL_MODE                                                                                                             \
    VK_CULL_MODE_NONE /* no cull: the first triangle is one                                                                                \
                         triangle, its winding is the shader's,                                                                            \
                         and a wrong winding here is a triangle                                                                            \
                         that is silently not drawn - the                                                                                  \
                         rasterizer step's gate, not a guess */
#define XUD_PIPELINE_FRONT_FACE                                                                                                            \
    VK_FRONT_FACE_COUNTER_CLOCKWISE /* the spec's own default,                                                                             \
                       recorded rather than left at 0 by accident */
#define XUD_PIPELINE_SAMPLE_COUNT                                                                                                          \
    VK_SAMPLE_COUNT_1_BIT /* one sample today: MSAA is a                                                                                   \
             framebuffer*SampleCounts gate (this driver answers                                                                            \
             0x1f for all three, see AGENTS.md) and a resolve                                                                              \
             attachment is a decision the render-pass step                                                                                 \
             takes, not one here */
#define XUD_PIPELINE_DEPTH_COMPARE_OP                                                                                                      \
    VK_COMPARE_OP_LESS /* the depth buffer's own rule, and                                                                                 \
           the one the plan's depth buffer is written against */
#define XUD_PIPELINE_LOGIC_OP                                                                                                              \
    VK_LOGIC_OP_COPY /* the enum's value with logicOpEnable VK_FALSE,                                                                      \
                        written because the field must be a valid                                                                          \
                        enum, not because the op is used */

static_assert(XUD_PIPELINE_STAGE_COUNT == (XUD_PIPELINE_FRAGMENT_STAGE + 1u), "the stage rows are contiguous from the vertex stage up");
static_assert(VK_SHADER_STAGE_VERTEX_BIT != VK_SHADER_STAGE_FRAGMENT_BIT,
    "the two stage bits are distinct, which is what makes a swapped row a caught "
    "error rather than a pipeline that compiles twice");
static_assert(
    VK_DYNAMIC_STATE_VIEWPORT != VK_DYNAMIC_STATE_SCISSOR, "the two dynamic states are distinct values, not positions in an enum");
static_assert(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST != VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
    "the topology is a value, and the first triangle is not a point list");

/*-----------------------------------------------------------------------------------------------
 * The layer's contract
 *-----------------------------------------------------------------------------------------------*/

/// Pure: the fixed pipeline state, written into the struct's own rows. No driver call, no
/// handle in hand, headless-testable - this is the create-info assembly the repo's headless
/// rule leans on. Every row is stamped and its pNext cleared, and every field of every row
/// is written in struct order, because a row left half-filled is a row whose defaults are
/// whatever calloc left rather than a decision (the validation layer's rule that the physical
/// layer records: an unstamped create-info is a hard error, and here the same rule is a
/// property of this file's own assembly).
///
/// The decisions, each with its reason: zero vertex bindings and zero attributes (the
/// allocator rung IS in the tree now (S12) but owns no VkBuffer at all - nothing above it has
/// asked it for memory - and the SoA streams land with the batch, so the vertex shader
/// generates its vertices from gl_VertexIndex instead);
/// TRIANGLE_LIST with no primitive restart (restart is a degenerate-index convention the
/// batch step owns); viewportCount 1 and scissorCount 1 with NULL pViewports/pScissors
/// (both are dynamic - and pViewportState may not be NULL at all, because a fragment
/// stage that writes gl_FragColor reads gl_FragCoord, which is the spec's own rule for
/// that pointer); no tessellation and no geometry stage (the stage array is two rows);
/// fill polygon mode, no cull, CCW front face, no depth bias, lineWidth 1 (the line
/// pipeline variant is a later rung's, but lineWidth is a field of THIS row and 1 is the
/// only honest value in it); VK_SAMPLE_COUNT_1_BIT with no sample shading and no alpha-
/// to-coverage (see the note above); depth test and depth write BOTH OFF with LESS recorded and
/// no depth bounds and no stencil (the render pass under this pipeline has NO depth attachment -
/// the VkImage + VkDeviceMemory a depth buffer needs belongs to the allocator rung, which holds
/// no image yet - and the spec makes the depth buffer UNDEFINED in that case, so an enabled test
/// would be an implementation-dependent result rather than a first triangle that draws every ti
/// me; XUD_PIPELINE_DEPTH_COMPARE_OP is the rule the depth buffer will use the day that attach
/// ment lands, and stencil does not exist in this library at all); one blend attachment with blen
/// d DISABLED and both factor
/// pairs ONE/ZERO - these headers spell the row as the core-1.0 form (srcColorBlendFactor
/// / dstColorBlendFactor / colorBlendOp and the same three for the alpha half; grepped,
/// srcAlpha, colorBlendFactor and useAlphaBlend do not exist in vulkan_core.h 1.4.363 at
/// all), and leaving the alpha half at ZERO/ZERO would be a blend that writes 0 alpha
/// into the swapchain the moment a later variant enables blending; colourWriteMask RGBA
/// and logicOp disabled (the opaque variant of the plan's opaque/transparent pair - trans
/// parent is a second pipeline, not a flag on this one); and VIEWPORT + SCISSOR as the two
/// dynamic states. XUD_INVALID_ARG on a NULL pipeline.
XudResult xud_pipeline_fill_state(XudPipeline* pipeline);

/// Pure: the two shader-stage rows, from the two module handles in hand. Stamped, pName
/// XUD_PIPELINE_ENTRY_POINT (named, not NULL: the validation layer on this machine reports a
/// NULL pName as a hard error against both pName VUIDs, probed today, and "main" is what
/// glslangValidator writes - probed in the disassembly of both modules), pSpecializationInfo
/// NULL (a
/// specialization constant is a variant knob this rung does not take today). XUD_INVALID_
/// ARG on a NULL out-array. The two module handles are passed through as they come, VK_NU
/// LL_HANDLE included: a handle-less module is a bad argument for the DRIVER half, not for
/// this assembly, which is what lets the headless run exercise the assembly without a driv
/// er.
XudResult xud_pipeline_fill_shader_stages(
    VkPipelineShaderStageCreateInfo* outStages, VkShaderModule vertexModule, VkShaderModule fragmentModule);

/// Pure: the VkPipelineLayoutCreateInfo the layout will be created with. Zero descriptor
/// sets and zero push-constant ranges, with both arrays NULL - the honest answer for the
/// two shaders this rung builds, which read no binding and take no push constant. The
/// descriptor-layout step grows this to set 0 (per-frame) and set 1 (per-material) and
/// the batch step grows the push-constant range; neither is a placeholder here.
XudResult xud_pipeline_fill_layout_info(VkPipelineLayoutCreateInfo* outCreateInfo);

/// Pure: the VkGraphicsPipelineCreateInfo the pipeline will be created with, assembled
/// from the rows above. Refuses XUD_UNSUPPORTED when any row of the struct in hand is
/// not stamped - the assembly below is only ever a step from xud_pipeline_fill_state(),
/// and a struct whose rows were never filled is a bad state of this layer's own record,
/// not a bad argument from a caller. pStages points at the struct's own shaderStages
/// array, every other state pointer at the struct's own row, pTessellationState NULL
/// (no tessellation stage), pPipelineLayout the layout handle in hand, renderPass the
/// pass handle recorded in the record and subpass XUD_PIPELINE_FIRST_SUBPASS (the core-1.4
/// pair that binds a graphics pipeline to the render pass it belongs to, which the
/// dynamicRendering VUID makes REQUIRED here - the validation layer reports VK_NULL_HANDLE
/// as a hard error, probed today
/// that tie a pipeline to a render pass at creation time, which this library does not
/// do: the render pass is asked for per draw instead, and these headers have no
/// VK_SUBPASS_TYPE_* constant at all, so 0 is written as the enum's own zero), basePi
/// pelineHandle VK_NULL_HANDLE and basePipelineIndex -1 (the spec's "no base pipeline"
/// pair - 0 would name pipeline row 0 of a pipeline-derivation this rung does not take).
/// XUD_INVALID_ARG on a NULL out-create-info or a pipeline that owns no layout.
XudResult xud_pipeline_fill_create_info(const XudPipeline* pipeline, VkGraphicsPipelineCreateInfo* outCreateInfo);

/// Driver: the whole rung up in one call - the two built-in SPIR-V modules, the layout, the
/// pipeline, in that order, and each one's teardown rung checked before the create that
/// needs it. The render pass handle is an argument because the core-1.4 binding makes it one
/// (see the note in xud_pipeline_fill_create_info()); a handle-less pass is XUD_INVALID_ARG (a module this layer cannot destroy is a module
/// it must not create, and the same for the layout and the pipeline). The SPIR-V words come from the header CMakeLists txt generates from
/// shaders/ at build time, so nothing here reads a file or compiles GLS L, and the words are checked by xud_shader_check_spv() before the
/// driver sees them. On any error *outPipeline stays NULL and everything created so far is destroyed in reverse creation order.
/// XUD_INVALID_ARG on a NULL out-pointer or one that already owns a pipe line, a NULL loader/device, or a handle-less device;
/// XUD_UNSUPPORTED when the loader is closed, its device rows are not this device's, a row this rung needs is nil, or the SPI R-V is not
/// SPIR-V; XUD_OUT_OF_MEMORY/XUD_OUT_OF_RESOURCES when the driver refuses on host or device memory; XUD_VULKAN_ERROR otherwise (a pipeline
/// that fails to compile is the common one, VK_ERROR_PIPELINE_COMPILE_SHADER_INTERNAL_ERROR and its neighbours).
XudResult xud_pipeline_open(XudPipeline** outPipeline, XudLoader* loader, const XudDevice* device, VkRenderPass renderPass);

/// Driver: the teardown - vkDestroyPipeline, then vkDestroyPipelineLayout, then the two
/// modules through xud_shader_close(), then free. That order is load-bearing: the pipeline
/// names its layout and the layout's modules are the only thing keeping them alive, so
/// anything else leaves a handle whose owner is already gone. XUD_ALREADY when the pointer
/// owns no pipeline; XUD_INVALID_ARG on a NULL pointer-to-pointer, a NULL loader/device or
/// a handle-less device; XUD_UNSUPPORTED when the device rows are not this device's or a
/// destroy row is nil while the object it would destroy is live (the struct is not freed,
/// because freeing it would leak the handles silently).
XudResult xud_pipeline_close(XudPipeline** ioPipeline, const XudDevice* device, XudLoader* loader);

#endif /* XUD_PIPELINE_H_ */
