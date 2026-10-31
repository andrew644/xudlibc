/***********************************************************************************************
 *   xud_pipeline.h - the pipeline rung: the built-in SPIR-V to one graphics pipeline per row
 *                 of the batch's (topology x variant) list
 *
 *   Layer: above the shader rung, below the render pass. This rung owns the whole set of
 *   objects a pipeline is made of - the two VkShaderModules, the VkPipelineLayout and the
 *   VkPipeline - because a pipeline names its layout and its layout's modules are the only
 *   thing that keeps them alive, so all four die together and in one order: pipeline,
 *   layout, modules. "Explicit pipelines, created once at init, a small fixed set of
 *   variants, never created per draw" (the repo's rule) is what this rung is: today the set is
 *   the batch's **four-row** list - the plan's opaque/transparent pair at each of the two
 *   topologies the batch's draw rows name - and the variants the rest of the plan asks for
 *   (textured, glyph) land beside them in this same file as rows of that list, never as a
 *   pipeline-per-draw path anywhere above it.
 *
 *   What this rung does NOT have, and why: no third blend variant (the variant axis of the list
 *   below is the batch's, `XUD_BATCH_VARIANT_COUNT` wide, and its two columns are the plan's opaque/
 *   transparent pair - raylib's `BlendMode` has nine modes and we want none of the other seven yet,
 *   so growing that axis is a column in the list, not a member in a struct, when a step wants one)
 *   and no set-1-reading shader (the layout below is the declaration this rung makes and the frame
 *   loop binds both channels of - set 1 is the other half, and it has no allocation to name yet).
 *
 *   What it DOES have now is the vertex input. The batch rung below it owns the four SoA streams
 *   and the one index buffer, and xud_pipeline_fill_vertex_input overwrites the zero row that
 *   xud_pipeline_fill_state assembles with the batch's four binding and four attribute rows, so
 *   the pipeline names those streams and the built-in SPIR-V reads them at those locations
 *   instead of generating its three vertices from gl_VertexIndex. xud_pipeline_open takes the
 *   batch for exactly that reason: the row points at the BATCH's own arrays, and the renderer's
 *   open/close order opens the batch before this rung and closes it after, which is what makes
 *   that pointer legal for the pipeline whole life rather than a dangling read. The layout is
 *   one layout for the whole list, and the frame loop binds both of its channels - set 0 and the
 *   push range - so the shader reads the frame block and the push row while set 1 stays declared
 *   and unbound, which is legal because a shader need not touch everything its layout declares.
 ***********************************************************************************************/

#ifndef XUD_PIPELINE_H_
#define XUD_PIPELINE_H_

#include "volk.h"
#include "xud_batch.h" /* Required for: XudBatch, the four SoA streams whose vertex-input rows this names */
#include "xud_descriptor.h" /* Required for: XudDescriptor, the one pipeline layout this is built under */
#include "xud_device.h" /* Required for: XudDevice, the handle the build and teardown are asked of */
#include "xud_loader.h" /* Required for: XudLoader, the PFN rows vkCreateGraphicsPipelines and vkDestroyPipeline go through */
#include "xud_shader.h" /* Required for: XudShader, the SPIR-V bytes this is built from */
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

/* The row of the pipeline list a (topology, variant) pair names, and how many rows the list has.
   ONE flat list because the batch's draw row names BOTH axes and the frame loop binds ONE pipeline
   per recorded draw row: a row that carried two indices would be a row whose pipeline had to be
   looked up twice in the frame loop, and a row that carried a pipeline handle would be a row whose
   pipeline went stale the moment this rung recreated one. Topology-major, so the two variants of
   one topology are adjacent rows and growing either axis is a row/col in the list (todo.md's D1).
*/
#define XUD_PIPELINE_ROW(topology, variant) (((topology) * XUD_BATCH_VARIANT_COUNT) + (variant))
#define XUD_PIPELINE_ROW_COUNT (XUD_BATCH_TOPOLOGY_COUNT * XUD_BATCH_VARIANT_COUNT)

struct XudPipeline {
    VkPipeline pipeline[XUD_PIPELINE_ROW_COUNT]; /* the Vulkan objects this layer owns: one per
                            row of the batch's (topology x variant) list, VK_NULL_HANDLE until xud_
                            pipeline_open() creates it, cleared and destroyed by xud_pipeline_close().
                            The array is indexed by XUD_PIPELINE_ROW(topology, variant) - the one
                            index space that makes the batch's draw row (which names a topology and
                            a variant) and this object the same number. The list is flat rather than
                            a struct of two arrays because the frame loop picks ONE pipeline per
                            recorded draw row, so the pick has to be one number (todo.md's D1, 2026-
                            10-26: raylib's blending is an enum-indexed list of modes, `BlendMode`
                            raylib.h 918-926 with `BLEND_ALPHA = 0` the default, and its topology
                            axis is the row's own `mode` field, rlgl.h 407) */
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
    VkPipelineInputAssemblyStateCreateInfo inputAssembly[XUD_BATCH_TOPOLOGY_COUNT]; /* one row per
                            topology of the list rather than one row for the whole rung: topology is
                            pipeline state, so a list that carries two topologies carries two of
                            these rows, and the row a create-info points at is the one the draw row's
                            topology indexes */
    VkPipelineViewportStateCreateInfo viewportState;
    VkPipelineRasterizationStateCreateInfo rasterizer;
    VkPipelineMultisampleStateCreateInfo multisample;
    VkPipelineDepthStencilStateCreateInfo depthStencil;
    VkPipelineColorBlendStateCreateInfo blending[XUD_BATCH_VARIANT_COUNT];
    VkPipelineColorBlendAttachmentState blendAttachment[XUD_BATCH_VARIANT_COUNT];
    VkPipelineDynamicStateCreateInfo dynamicState;
    VkDynamicState dynamicStates[XUD_PIPELINE_DYNAMIC_STATE_COUNT];
    VkGraphicsPipelineCreateInfo createInfo[XUD_PIPELINE_ROW_COUNT]; /* what vkCreateGraphicsPipelines
                                                            was handed, one row per row of the list -
                                                            the record and the create call read the
                                                            same bytes, which is the rule xud_swapchain
                                                            .c states for its create-info, one rung up */
};

/*-----------------------------------------------------------------------------------------------
 * The pipeline's shape, as this layer writes it
 *-----------------------------------------------------------------------------------------------*/

#define XUD_PIPELINE_TOPOLOGY_TRIANGLE                                                                                                     \
    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST /* the batch step's index                                                                          \
                           buffer feeds the same topology; a strip                                                                         \
                           would make the first triangle's three                                                                           \
                           vertices mean a different thing */
#define XUD_PIPELINE_TOPOLOGY_LINE                                                                                                         \
    VK_PRIMITIVE_TOPOLOGY_LINE_LIST /* the topology the line shapes assemble their indices as, and                                         \
                        the second row of the list: raylib's DrawLine3D opens with rlBegin(RL_LINES)                                       \
                        (its own header says so at raylib.h 1585) and its replay hands                                                     \
                        batch->draws[i].mode to glDrawArrays (rlgl.h 3116), which is exactly this                                          \
                        row - a topology is a row of the list, not a flag on one pipeline */

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
#define XUD_PIPELINE_BLEND_OP                                                                                                              \
    VK_BLEND_OP_ADD /* the one blend equation both variant rows use:                                                                       \
raylib's default BLEND_ALPHA is glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)                                                          \
+ glBlendEquation(GL_FUNC_ADD) (rlgl.h 2127), and VK_BLEND_OP_ADD is that                                                                  \
equation's Vulkan form - the FACTORS are what differ between the rows, not                                                                 \
the equation, so the equation is one name and the factors are a table */

static_assert(XUD_PIPELINE_STAGE_COUNT == (XUD_PIPELINE_FRAGMENT_STAGE + 1u), "the stage rows are contiguous from the vertex stage up");
static_assert(XUD_BATCH_VARIANT_COUNT >= 2u, "the variant axis has both the opaque and the transparent column");
static_assert(sizeof(((XudPipeline*)0)->pipeline) / sizeof(VkPipeline) == XUD_PIPELINE_ROW_COUNT,
    "one pipeline per row of the (topology x variant) list, which is what makes the batch's draw row"
    " name this object by one number");
static_assert(sizeof(((XudPipeline*)0)->inputAssembly) / sizeof(VkPipelineInputAssemblyStateCreateInfo) == XUD_BATCH_TOPOLOGY_COUNT,
    "one input-assembly row per topology, which is what makes the topology a row of the list rather"
    " than a decoration on one row");
static_assert(sizeof(((XudPipeline*)0)->blendAttachment) / sizeof(VkPipelineColorBlendAttachmentState) == XUD_BATCH_VARIANT_COUNT,
    "one blend attachment per variant column, which is what makes the create-info's pAttachments the variant's own");
static_assert(sizeof(((XudPipeline*)0)->createInfo) / sizeof(VkGraphicsPipelineCreateInfo) == XUD_PIPELINE_ROW_COUNT,
    "one create-info per row of the list, the record of what each create call was handed");
static_assert(XUD_PIPELINE_ROW(XUD_BATCH_TOPOLOGY_TRIANGLES, XUD_BATCH_VARIANT_OPAQUE) == 0u,
    "the triangles/opaque pair is the list's first row, the one every draw used before this step");
static_assert(XUD_PIPELINE_ROW(XUD_BATCH_TOPOLOGY_LINES, XUD_BATCH_VARIANT_TRANSPARENT) == (XUD_PIPELINE_ROW_COUNT - 1u),
    "the pair the list's two axes span at their far ends is its last row, which is what makes the"
    " pair a bijection onto the list rather than a partial map");
static_assert(VK_SHADER_STAGE_VERTEX_BIT != VK_SHADER_STAGE_FRAGMENT_BIT,
    "the two stage bits are distinct, which is what makes a swapped row a caught "
    "error rather than a pipeline that compiles twice");
static_assert(
    VK_DYNAMIC_STATE_VIEWPORT != VK_DYNAMIC_STATE_SCISSOR, "the two dynamic states are distinct values, not positions in an enum");
static_assert(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST != VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
    "the two topologies of the list are distinct values, which is what makes a row bound at the wrong"
    " one a caught error rather than two identical pipelines");

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
/// The decisions, each with its reason: zero vertex bindings and zero attributes IN THIS ROW (the
/// row is the starting state of the field and xud_pipeline_fill_vertex_input below is what overwrites
/// it with the batch's four and four - a pipeline that has no batch to name has no vertex input to
/// write, and that is a refusal rather than a zero row);
/// TRIANGLE_LIST with no primitive restart for the list's first topology row, and LINE_LIST for
/// the second (restart is a degenerate-index convention the batch step owns, and neither of our
/// topologies uses it); the two rows differ in ONE field, which is why they are a table written in
/// a loop rather than two hand-written blocks; viewportCount 1 and scissorCount 1 with NULL pViewports/pScissors
/// (both are dynamic - and pViewportState may not be NULL at all, because a fragment
/// stage that writes gl_FragColor reads gl_FragCoord, which is the spec's own rule for
/// that pointer); no tessellation and no geometry stage (the stage array is two rows);
/// fill polygon mode, no cull, CCW front face, no depth bias, lineWidth 1 (the line
/// pipeline variant is a later rung's, but lineWidth is a field of THIS row and 1 is the
/// only honest value in it); VK_SAMPLE_COUNT_1_BIT with no sample shading and no alpha-
/// to-coverage (see the note above); depth test and depth write BOTH ON with LESS recorded and
/// no depth bounds and no stencil (the render pass under this pipeline HAS a depth attachment now -
/// xud_renderpass.c creates the VkImage and the allocator owns its memory, which is what the opa
/// que variant of the plan's opaque/transparent pair needs: the spec makes the depth contents of a
/// subpass with no depth attachment implementation-dependent, so the same two flags were VK_FALSE
/// for the first-triangle step and are VK_TRUE against an attachment cleared to XUD_RENDERPASS_CLE
/// AR_DEPTH; XUD_PIPELINE_DEPTH_COMPARE_OP is LESS, the one form that clear-to-1.0 convention need
/// s, because a nearer fragment has the SMALLER depth and so passes against the larger one already
/// in the buffer; and stencil does not exist in this library at all); one blend attachment with blen
/// d DISABLED and both factor
/// pairs ONE/ZERO - these headers spell the row as the core-1.0 form (srcColorBlendFactor
/// / dstColorBlendFactor / colorBlendOp and the same three for the alpha half; grepped,
/// srcAlpha, colorBlendFactor and useAlphaBlend do not exist in vulkan_core.h 1.4.363 at
/// all), and leaving the alpha half at ZERO/ZERO would be a blend that writes 0 alpha
/// into the swapchain the moment a later variant enables blending; attachmentCount is the COLOUR
/// attachment count of the subpass, not the pass's two-attachment count - the depth half of the
/// pass is named by the depth-stencil row, not by this one; colourWriteMask RGBA
/// and logicOp disabled (the opaque variant of the plan's opaque/transparent pair - trans
/// parent is a second pipeline, not a flag on this one, and it lands with the batch, because sort
/// ed transparency is a property of what the batch draws and there is no batch to sort yet); and
/// VIEWPORT + SCISSOR as the two
/// dynamic states. XUD_INVALID_ARG on a NULL pipeline.
XudResult xud_pipeline_fill_state(XudPipeline* pipeline);

/// Pure: overwrite the pipeline's vertex-input row with the batch rung's four binding and four
/// attribute rows, so the pipeline this struct builds NAMES the batch's four SoA streams instead of
/// declaring no vertex input at all. xud_pipeline_fill_state above writes the zero row and this one
/// overwrites it, in that order, which is why xud_pipeline_open calls them in that order too: the
/// zero row is the starting state of the field, not what the built pipeline ends up carrying.
/// The row points at the BATCH's arrays rather than at a copy of them, so the batch has to outlive
/// the pipeline, and the renderer's open/close order is what gives it that (the batch is opened
/// before the pipeline is built and closed after the pipeline is retired). The caps gate that keeps
/// the four streams inside maxVertexInputBindings and maxVertexInputAttributes already ran inside
/// xud_batch_open when the batch filled these very rows, so a batch whose rows are live is a batch
/// that passed it; a batch whose rows are still the zero row is refused here rather than built a
/// pipeline that reads nothing. XUD_INVALID_ARG on a NULL pipeline or batch.
XudResult xud_pipeline_fill_vertex_input(XudPipeline* pipeline, const XudBatch* batch);

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

/// Pure: the VkPipelineLayoutCreateInfo the layout will be created with, read off the
/// descriptor record below this rung. TWO set layouts, the record's own array - the pipeline
/// layout does not copy them, it points at the descriptor rung's storage, which is what makes
/// the layout and the sets the pool allocated from those same layouts agree by construction -
/// and ONE push-constant range, the record's own row. Zero of either would be a layout that
/// cannot be bound to: the per-frame uniform buffer lives in set 0 and the model matrix the
/// batch step above writes is a push constant, so a pipeline built against a layout that decl
/// ares neither is a pipeline that will never see its own camera. XUD_INVALID_ARG on a NULL
/// out-create-info or a NULL descriptor, or one whose set 0 layout or uniform buffer handle
/// is not live - set 1's layout is created but its SET is not (the texture step allocates it),
/// so only set 0's layout is required to be a handle here.
XudResult xud_pipeline_fill_layout_info(const XudDescriptor* descriptor, VkPipelineLayoutCreateInfo* outCreateInfo);

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
/// XUD_INVALID_ARG on a NULL out-create-info or a pipeline that owns no layout, or on a
/// topology at or past XUD_BATCH_TOPOLOGY_COUNT or a variant at or past XUD_BATCH_VARIANT_COUNT.
/// The pair selects which `inputAssembly[topology]` / `blending[variant]` / `blendAttachment[variant]
/// ` row the create-info points at, which is what makes the batch's draw row (which names the same
/// pair) and this create-info the same row of the list (todo.md's D1, 2026-10-26, plus the topology
/// axis this step adds to it).
XudResult xud_pipeline_fill_create_info(
    const XudPipeline* pipeline, uint32_t topology, uint32_t variant, VkGraphicsPipelineCreateInfo* outCreateInfo);

/// Driver: the whole rung up in one call - the two built-in SPIR-V modules, the layout, the
/// pipeline list (one pipeline per row of the batch's (topology x variant) list, created in row
/// order), in that order, and each one's teardown rung checked before the create that
/// needs it. The render pass handle is an argument because the core-1.4 binding makes it one
/// (see the note in xud_pipeline_fill_create_info()); a handle-less pass is XUD_INVALID_ARG (a module this layer cannot destroy is a module
/// it must not create, and the same for the layout and the pipeline). The SPIR-V words come from the header CMakeLists txt generates from
/// shaders/ at build time, so nothing here reads a file or compiles GLS L, and the words are checked by xud_shader_check_spv() before the
/// driver sees them. On any error *outPipeline stays NULL and everything created so far is destroyed in reverse creation order.
/// XUD_INVALID_ARG on a NULL out-pointer or one that already owns a pipe line, a NULL loader/device, or a handle-less device;
/// XUD_UNSUPPORTED when the loader is closed, its device rows are not this device's, a row this rung needs is nil, or the SPI R-V is not
/// SPIR-V; XUD_OUT_OF_MEMORY/XUD_OUT_OF_RESOURCES when the driver refuses on host or device memory; XUD_VULKAN_ERROR otherwise (a pipeline
/// that fails to compile is the common one, VK_ERROR_PIPELINE_COMPILE_SHADER_INTERNAL_ERROR and its neighbours).
XudResult xud_pipeline_open(XudPipeline** outPipeline, XudLoader* loader, const XudDevice* device, const XudDescriptor* descriptor,
    const XudBatch* batch, VkRenderPass renderPass);

/// Driver: the teardown - vkDestroyPipeline for every row of the list, then vkDestroyPipelineLayout,
/// then the two
/// modules through xud_shader_close(), then free. That order is load-bearing: the pipelines name
/// their layout and the layout's modules are the only thing keeping them alive, so
/// anything else leaves a handle whose owner is already gone. XUD_ALREADY when the pointer
/// owns no pipeline; XUD_INVALID_ARG on a NULL pointer-to-pointer, a NULL loader/device or
/// a handle-less device; XUD_UNSUPPORTED when the device rows are not this device's or a
/// destroy row is nil while a row of the list is live (the struct is not freed,
/// because freeing it would leak the handles silently).
XudResult xud_pipeline_close(XudPipeline** ioPipeline, const XudDevice* device, XudLoader* loader);

#endif /* XUD_PIPELINE_H_ */
