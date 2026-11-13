/***********************************************************************************************
 *   xud_pipeline.h - the pipeline rung: the built-in SPIR-V to one graphics pipeline per row
 *                 of the batch's (topology x variant) list
 *
 *   Layer: above the shader rung, below the render pass. This rung owns the whole set of
 *   objects a pipeline is made of - the three VkShaderModules, the VkPipelineLayout and the
 *   VkPipeline - because a pipeline names its layout and its layout's modules are the only
 *   thing that keeps them alive, so all four die together and in one order: pipeline,
 *   layout, modules. "Explicit pipelines, created once at init, a small fixed set of
 *   variants, never created per draw" (the repo's rule) is what this rung is: today the set is
 *   the batch's **eight-row** list - the plan's opaque/transparent/textured/ui quadruple at each
 *   of the two topologies the batch's draw rows name - and the variants the rest of the plan asks
 *   for land beside them in this same file as rows of that list, never as a
 *   pipeline-per-draw path anywhere above it.
 *
 *   What this rung does NOT have, and why: no further blend variant (the variant axis of the
 *   list below is the batch's, `XUD_BATCH_VARIANT_COUNT` wide, and its four columns are the
 *   plan's opaque/transparent/textured/ui quadruple - raylib's `BlendMode` has nine modes and
 *   we want none of the other seven yet, so growing that axis stays a column in the list, not a
 *   member in a struct, when a step wants one) and no per-draw pipeline (the last columns are
 *   COLUMNS, so they are more rows of the same list under the same one layout, and the only
 *   things that differ between the columns' pipelines are which frag module their stage rows
 *   name, the blend row and the depth row - the sets, the bindings and the push range are
 *   identical across the whole list, which is what lets one layout serve all eight rows and
 *   every vkCmdBindDescriptorSets name one layout).
 *
 *   What it DOES have now is the vertex input. The batch rung below it owns the four SoA streams
 *   and the one index buffer, and xud_pipeline_fill_vertex_input overwrites the zero row that
 *   xud_pipeline_fill_state assembles with the batch's four binding and four attribute rows, so
 *   the pipeline names those streams and the built-in SPIR-V reads them at those locations
 *   instead of generating its three vertices from gl_VertexIndex. xud_pipeline_open takes the
 *   batch for exactly that reason: the row points at the BATCH's own arrays, and the renderer's
 *   open/close order opens the batch before this rung and closes it after, which is what makes
 *   that pointer legal for the pipeline whole life rather than a dangling read. The layout is
 *   one layout for the whole list, and every pass binds its one set and pushes its row - so a
 *   shader reads the frame block, the push row and the sampler array, which is what the textu
 *   red column's frag module needs and what makes the two solid columns' shaders that never
 *   touch the sampler array and the one that does agree on one layout.
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

/* The frag modules this rung builds, and the axis the variant axis is mapped ONTO: the variant axis
   is the batch's and the pipeline list's, and this is the axis of the built-in SPIR-V set. Two
   modules for three variants, because the opaque and transparent columns compile from one source
   and only the textured column reads the sampler array. The mapping itself is a table in the .c beside the
   blend tables, in the same shape as they are: the row of the list is a pair of batch indices and
   which frag module it names is one more column of that table, so growing the variant axis is a
   row there and not a branch in the create loop. */
#define XUD_PIPELINE_FRAG_SOLID 0u /* the module the two untextured columns are built from */
#define XUD_PIPELINE_FRAG_TEXTURED 1u /* the module the textured column is built from */
#define XUD_PIPELINE_FRAG_MODULE_COUNT 2u /* the length of this axis */
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
    XudShader* vertexShader; /* owned: the module the vertex stage was built from, one for the
                            whole list - the eight rows differ in frag, blend and depth state,
                            never in vertex state */
    XudShader* fragmentShader[XUD_PIPELINE_FRAG_MODULE_COUNT]; /* owned, one per FRAG MODULE rather
                            than one per row of the list: the variant axis names which module a row's
                            fragment stage is built from, so the opaque and transparent columns share
                            the solid module and the textured column names the one that reads the sampler array.
                            Indexing by module rather than by variant is what makes two variants that
                            compile from one source own one VkShaderModule rather than two copies of
                            the same bytes */

    /* --- the state it was handed, recorded rather than discarded: the create-info below
         points at these rows, so the create-info and the record of the pipeline ARE the
         same bytes and cannot disagree - the rule xud_swapchain.c states for its create-info
         and its record, one rung up --- */
    VkPipelineShaderStageCreateInfo shaderStages[XUD_PIPELINE_ROW_COUNT][XUD_PIPELINE_STAGE_COUNT]; /*
      one PAIR of stage rows per row of the list rather than one pair for the whole rung: the frag
      module is pipeline state and the third column of the variant axis names a different one, so a
      shared pair would be a pair that could name only one of the two frag modules */
    VkPipelineVertexInputStateCreateInfo vertexInput;
    VkPipelineInputAssemblyStateCreateInfo inputAssembly[XUD_BATCH_TOPOLOGY_COUNT]; /* one row per
                            topology of the list rather than one row for the whole rung: topology is
                            pipeline state, so a list that carries two topologies carries two of
                            these rows, and the row a create-info points at is the one the draw row's
                            topology indexes */
    VkPipelineViewportStateCreateInfo viewportState;
    VkPipelineRasterizationStateCreateInfo rasterizer;
    VkPipelineMultisampleStateCreateInfo multisample;
    VkPipelineDepthStencilStateCreateInfo depthStencil[XUD_BATCH_VARIANT_COUNT]; /* one row per
                            variant column rather than one row for the whole rung: depth test and
                            depth write are PIPELINE state, and the UI column is the list's only
                            depth-off row (its screen-space draws must neither test against nor
                            write the pass's depth attachment), so a shared row would either give
                            the UI column a test it must not have or give the other three a write
                            they must keep. The row a create-info points at is the one the draw
                            row's variant indexes, like the blend rows beside it */
    VkPipelineColorBlendStateCreateInfo blending[XUD_BATCH_VARIANT_COUNT];
    VkPipelineColorBlendAttachmentState blendAttachment[XUD_BATCH_VARIANT_COUNT];
    VkPipelineDynamicStateCreateInfo dynamicState;
    VkDynamicState dynamicStates[XUD_PIPELINE_DYNAMIC_STATE_COUNT];
    VkGraphicsPipelineCreateInfo createInfo[XUD_PIPELINE_ROW_COUNT]; /* what vkCreateGraphicsPipelines
                                                            was handed, one row per row of the list -
                                                            the record and the create call read the
                                                            same bytes, which is the rule xud_swapchain
                                                            .c states for its create-info, one rung up */

    /* --- the resolve pass's own pipeline: the one that turns the high-dynamic-range scratch image
         into the window's picture. It joins the rung rather than becoming a second rung because it
         shares the ONE layout (its sampler is one entry of the array a textured batch
         draw indexes, named by push rather than by a set of its own) and every fixed-state row it needs is one this struct already
         carries - triangle input assembly, dynamic viewport and scissor, blend-disabled attachment -
         and because the two passes it binds to are both arguments of the one open below --- */
    VkPipeline resolvePipeline; /* owned: the full-screen-triangle pipeline, created against the
                    SCREEN pass handle below - the pass whose format it writes */
    VkRenderPass resolveRenderPass; /* BORROWED, like renderPass: the screen pass this pipeline is
                    bound to at create (the same core-1.4 pair the note in
                    xud_pipeline_fill_create_info() quotes); renderPass above is the HIGH-DYNAMIC-
                    RANGE pass the batch rows' pipelines are bound to, and the two must differ -
                    pass compatibility is format-for-format, and the scratch image is float */
    XudShader* presentVertexShader; /* owned: the module that builds the triangle from gl_VertexIndex
                    alone - a second vertex module, not the batch's: the batch's reads four attribute
                    streams and this one reads none */
    XudShader* presentFragmentShader; /* owned: the tonemap module - the only stage in the tree that
                    converts linear high-dynamic-range light to the display's coded range */
    VkPipelineShaderStageCreateInfo resolveShaderStages[XUD_PIPELINE_STAGE_COUNT]; /* the resolve
                    pipeline's own stage pair, filled by xud_pipeline_fill_shader_stages() from the
                    two present modules */
    VkPipelineDepthStencilStateCreateInfo resolveDepthStencil; /* its own depth row, and the reason
                    it is not one of the variant rows: the resolve pass neither tests nor writes
                    depth - every pixel it runs is written, and a depth test against a buffer the
                    scratch pass already resolved would be a second, later answer to a question the
                    scratch pass answered */
    VkGraphicsPipelineCreateInfo resolveCreateInfo; /* what vkCreateGraphicsPipelines was handed for
                    the resolve row, same record-as-created-bytes rule as createInfo above */

    /* --- the depth pre-pass's own pipeline: the colorless draw of the same geometry that fills
         the HDR depth buffer before the shaded pass replays it. One pipeline rather than a copy of
         the list: the pre-pass draws TRIANGLES only (the frame loop skips the TRANSPARENT and UI
         columns and line geometry never wins a depth race it would otherwise lose), reads ONE
         attribute stream, writes no colour and needs no texture - and it shares the ONE layout,
         because the draw's push row is pushed the same way and set 0 is the same frame block --- */
    VkPipeline prepassPipeline; /* owned: created against the PRE-PASS render pass below */
    VkRenderPass prepassRenderPass; /* BORROWED, like the other two pass fields: the pass
                    xud_renderpass_open_prepass() builds over the scratch pass's depth - an
                    unreferenced colour attachment, a depth reference, and the depth cleared. It is
                    NOT the scratch pass even though both draw into the same images: the shaded
                    pass writes colour and LOADs the depth the pre-pass sorted, and writing colour
                    is precisely what the pre-pass exists to avoid paying twice for */
    XudShader* prepassVertexShader; /* owned: the position-only module - a third vertex module,
                    because the batch's reads four streams and computes uv/normal/world outputs the
                    pre-pass has no fragment stage to receive */
    VkPipelineShaderStageCreateInfo prepassShaderStage; /* its ONE stage row: a pre-pass with a
                    fragment stage would be the shaded pass again */
    VkPipelineVertexInputStateCreateInfo prepassVertexInput; /* one binding and one attribute: the
                    batch's position stream alone, filled by xud_pipeline_fill_prepass_vertex_input
                    () from the batch's own row 0 */
    VkPipelineColorBlendStateCreateInfo prepassBlending; /* attachmentCount 0: the pre-pass
                    subpass references no colour attachment, and a blend attachment row for
                    attachment 0 of a subpass that has none is VUID 05722 territory */
    VkPipelineDepthStencilStateCreateInfo prepassDepthStencil; /* test ON, write ON, GREATER - the
                    pre-pass rasterizes each surface exactly once per pixel today, so a tie there
                    is the same surface drawn twice and either answer is the same number; the
                    GEQUAL subtlety belongs to the SHADED pass, which is where the table lives */
    VkGraphicsPipelineCreateInfo prepassCreateInfo; /* what vkCreateGraphicsPipelines was handed for
                    the pre-pass row, same record-as-created-bytes rule */

    /* --- the shadow pass's own pipeline (S2.5): the sun's colorless draw list. It joins beside
         the pre-pass's because it IS the pre-pass's shape - one vertex module reading the
         position stream, no fragment stage, no colour, depth test and write on - and differs
         from it in exactly two bytes of state: the module (shadow.vert reads the SUN's matrix
         out of the block where prepass.vert reads the camera's) and the pass handle it is bound
         to (the one-attachment map, not the scratch pass's two-row pre-pass). Every other row -
         vertex input, depth, blend, layout - is the pre-pass's row reused, because a pipeline
         that disagreed with the pre-pass about the position stream would rasterize the occluder
         differently than the camera rasterizes it, and a shadow is only trustworthy when it is
         the same triangles. */
    VkPipeline shadowPipeline; /* owned: created against the SHADOW render pass below; stays
                    VK_NULL_HANDLE when open was handed no shadow pass, which is the
                    shadows-off configuration the frame loop reads the same way */
    VkRenderPass shadowRenderPass; /* BORROWED, like the other three pass fields: the one-depth-
                    attachment pass xud_renderpass_open_shadow() builds - it shares nothing with
                    the pre-pass's handle, and pass compatibility says so: one attachment versus
                    two, SHADER_READ_ONLY final layout versus OPTIMAL */
    XudShader* shadowVertexShader; /* owned: the sun's position-only module - a fourth vertex
                    module for the same reason as the pre-pass's: the matrix it multiplies by is
                    not the matrix the batch's or the pre-pass's multiplies by */
    VkPipelineShaderStageCreateInfo shadowShaderStage; /* its ONE stage row, stamped the same way
                    the pre-pass's is: a shadow pass with a fragment stage is not a shadow map */
    VkGraphicsPipelineCreateInfo shadowCreateInfo; /* what vkCreateGraphicsPipelines was handed
                    for the shadow row, same record-as-created-bytes rule */

    /* --- the bloom chain's own pipelines (S2.6): the bright-pass row and ONE blur row, each
         the resolve's shape - the SAME present vertex module (the chain's fullscreen triangle
         is the resolve's triangle at a smaller viewport), the SAME zero vertex-input row, no
         depth, blend disabled - bound to the ONE bloom render pass all four levels share. One
         blur pipeline serves the three tail levels because the level it reads is not a
         pipeline decision at all: it is the sampler-array entry the frame loop pushes for
         the level, the same way the resolve's source is a push rather than a compile-time row. All rows
         stay VK_NULL_HANDLE when open was handed no bloom pass, which is the bloom-off answer
         the frame loop reads. --- */
    VkPipeline brightPipeline; /* owned: created against the bloom render pass below */
    VkPipeline bloomBlurPipeline; /* owned: the one blur row, bound by every tail level */
    VkRenderPass bloomRenderPass; /* BORROWED, like the other pass fields: the one-attachment
                    colour pass xud_renderpass_open_bloom() builds - one handle for every level
                    because an attachment description carries a format, not an extent, and all
                    four levels share the scratch format */
    XudShader* bloomBrightFragmentShader; /* owned: the threshold module */
    XudShader* bloomBlurFragmentShader; /* owned: the blur module, opened once - three modules
                    holding nine identical taps would be three owners of one kernel */
    VkPipelineShaderStageCreateInfo brightShaderStages[XUD_PIPELINE_STAGE_COUNT]; /* the present
                    vertex row and the threshold fragment row */
    VkPipelineShaderStageCreateInfo bloomBlurShaderStages[XUD_PIPELINE_STAGE_COUNT]; /* the same
                    present vertex row and the blur fragment row - one pair, one pipeline */
    VkGraphicsPipelineCreateInfo brightCreateInfo; /* what vkCreateGraphicsPipelines was handed
                    for the bright row, same record-as-created-bytes rule */
    VkGraphicsPipelineCreateInfo bloomBlurCreateInfo; /* and for the blur row */
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
    VK_COMPARE_OP_GREATER /* the depth buffer's rule for the two variant columns the depth                                                    \
           pre-pass does NOT replay (transparent sorts against the nearest real surface, the                                          \
           UI ignores depth entirely) and for the pre-pass row itself; the reversed 1/z                                                     \
           projection writes against it: nearer is LARGER, clear is 0.0 */
#define XUD_PIPELINE_DEPTH_REPLAY_COMPARE_OP                                                                                               \
    VK_COMPARE_OP_GREATER_OR_EQUAL /* the rule for the OPAQUE and TEXTURED columns, which replay what the                                            \
           pre-pass already rasterized: the fragment that owns a stored depth value IS that                                                \
           surface, so EQUAL must pass - GREATER would discard every pre-passed fragment and                                              \
           erase the scene - while a strictly smaller depth is a fragment behind the surface                                              \
           the pre-pass already credited, which is exactly the occluded work S2.4 exists to skip                                           \
           at the rasterizer */
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
static_assert(
    XUD_PIPELINE_FRAG_MODULE_COUNT == (XUD_PIPELINE_FRAG_TEXTURED + 1u), "the frag-module axis is contiguous from the solid module up");
static_assert(XUD_PIPELINE_FRAG_MODULE_COUNT <= XUD_BATCH_VARIANT_COUNT,
    "fewer frag modules than variants, which is the point of the mapping axis: the opaque and"
    " transparent columns share the solid module, the textured and ui columns share the one that reads"
    " the sampler array, and a module-per-variant shape would be the same bytes compiled twice");
static_assert(XUD_BATCH_VARIANT_COUNT >= 2u, "the variant axis has both the opaque and the transparent column");
static_assert(sizeof(((XudPipeline*)0)->pipeline) / sizeof(VkPipeline) == XUD_PIPELINE_ROW_COUNT,
    "one pipeline per row of the (topology x variant) list, which is what makes the batch's draw row"
    " name this object by one number");
static_assert(sizeof(((XudPipeline*)0)->inputAssembly) / sizeof(VkPipelineInputAssemblyStateCreateInfo) == XUD_BATCH_TOPOLOGY_COUNT,
    "one input-assembly row per topology, which is what makes the topology a row of the list rather"
    " than a decoration on one row");
static_assert(sizeof(((XudPipeline*)0)->blendAttachment) / sizeof(VkPipelineColorBlendAttachmentState) == XUD_BATCH_VARIANT_COUNT,
    "one blend attachment per variant column, which is what makes the create-info's pAttachments the variant's own");
static_assert(sizeof(((XudPipeline*)0)->depthStencil) / sizeof(VkPipelineDepthStencilStateCreateInfo) == XUD_BATCH_VARIANT_COUNT,
    "one depth row per variant column, which is what makes the UI column's depth-off state the column's own rather than the list's");
static_assert(sizeof(((XudPipeline*)0)->createInfo) / sizeof(VkGraphicsPipelineCreateInfo) == XUD_PIPELINE_ROW_COUNT,
    "one create-info per row of the list, the record of what each create call was handed");
static_assert(XUD_PIPELINE_ROW(XUD_BATCH_TOPOLOGY_TRIANGLES, XUD_BATCH_VARIANT_OPAQUE) == 0u,
    "the triangles/opaque pair is the list's first row, the one every draw used before this step");
static_assert(XUD_PIPELINE_ROW(XUD_BATCH_TOPOLOGY_LINES, XUD_BATCH_VARIANT_UI) == (XUD_PIPELINE_ROW_COUNT - 1u),
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
/// to-coverage (see the note above); depth test and depth write BOTH ON with GREATER recorded and
/// no depth bounds and no stencil (the render pass under this pipeline HAS a depth attachment now -
/// xud_renderpass.c creates the VkImage and the allocator owns its memory, which is what the opa
/// que variant of the plan's opaque/transparent pair needs: the spec makes the depth contents of a
/// subpass with no depth attachment implementation-dependent, so the same two flags were VK_FALSE
/// for the first-triangle step and are VK_TRUE against an attachment cleared to XUD_RENDERPASS_CLE
/// AR_DEPTH; the compare OP is a per-variant table (S2.4): XUD_PIPELINE_DEPTH_REPLAY_COMPARE_OP
/// GEQUAL for the OPAQUE and TEXTURED columns, which replay what the depth pre-pass already
/// rasterized - the fragment that owns a stored depth IS that surface, so EQUAL must pass - and
/// XUD_PIPELINE_DEPTH_COMPARE_OP GREATER for the TRANSPARENT and UI columns, which the pre-pass
/// skips and whose depth answer stays the nearest surface drawn before them; and stencil does not
/// exist in this library at all). The struct's own prepassDepthStencil (GREATER, test and write
/// ON) and prepassBlending (attachmentCount 0 - the pre-pass subpass references no colour) are
/// stamped here too, beside the resolve row; one blend attachment with blen
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

/// Pure: overwrite the pipeline's prepassVertexInput row with the batch's ONE position stream -
/// binding 0 and attribute 0 of the batch's own arrays, count 1 and 1. The shared vertexInput row
/// above already carries the batch's four streams for the shaded list and cannot be reused: the
/// pre-pass pipeline that declared four attributes but ran a module that reads one would be a
/// pipeline whose declared input is a lie about its own stage. The row points at the BATCH's rows
/// like the four-stream fill above, so the same outlives rule applies. Refuses the same way: a
/// batch whose rows are still the zero row is refused here rather than built a pipeline that
/// reads nothing. XUD_INVALID_ARG on a NULL pipeline or batch.
XudResult xud_pipeline_fill_prepass_vertex_input(XudPipeline* pipeline, const XudBatch* batch);

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
/// out-create-info or a NULL descriptor, or one whose set 0 layout or uniform buffer
/// handle is not live - the bindless layout has ONE set, and its one layout is required to be
/// a live handle here.
XudResult xud_pipeline_fill_layout_info(const XudDescriptor* descriptor, VkPipelineLayoutCreateInfo* outCreateInfo);

/// Pure: the VkGraphicsPipelineCreateInfo the pipeline will be created with, assembled
/// from the rows above. Refuses XUD_UNSUPPORTED when any row of the struct in hand is
/// not stamped - the assembly below is only ever a step from xud_pipeline_fill_state(),
/// and a struct whose rows were never filled is a bad state of this layer's own record,
/// not a bad argument from a caller. pStages points at the struct's OWN shaderStages PAIR for the
/// row this pair selects - the array is per row of the list, not per rung, because the frag module
/// is pipeline state and the third column of the variant axis names a different one. Every other
/// state pointer is at the struct's own row, pTessellationState NULL (no tessellation stage), pPipelineLayout the layout handle in hand,
/// renderPass the pass handle recorded in the record and subpass XUD_PIPELINE_FIRST_SUBPASS (the core-1.4 pair that binds a graphics
/// pipeline to the render pass it belongs to, which the dynamicRendering VUID makes REQUIRED here - the validation layer reports
/// VK_NULL_HANDLE as a hard error, probed today that tie a pipeline to a render pass at creation time, which this library does not do: the
/// render pass is asked for per draw instead, and these headers have no VK_SUBPASS_TYPE_* constant at all, so 0 is written as the enum's
/// own zero), basePi pelineHandle VK_NULL_HANDLE and basePipelineIndex -1 (the spec's "no base pipeline" pair - 0 would name pipeline row 0
/// of a pipeline-derivation this rung does not take). XUD_INVALID_ARG on a NULL out-create-info or a pipeline that owns no layout, or on a
/// topology at or past XUD_BATCH_TOPOLOGY_COUNT or a variant at or past XUD_BATCH_VARIANT_COUNT.
/// The pair selects which `inputAssembly[topology]` / `blending[variant]` / `blendAttachment[variant]
/// ` / `shaderStages[row]` row the create-info points at, and which frag module the variant's stage
/// pair names, which is what makes the batch's draw row (which names the same pair) and this
/// create-info the same row of the list (todo.md's D1, 2026-10-26, plus the topology axis this step
/// adds to it and the textured column the step above adds).
XudResult xud_pipeline_fill_create_info(
    const XudPipeline* pipeline, uint32_t topology, uint32_t variant, VkGraphicsPipelineCreateInfo* outCreateInfo);

/// Driver: the whole rung up in one call - the three built-in SPIR-V modules (one vertex module for
/// the whole list and one frag module per entry of the frag-module axis), the layout, the
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

/// Pure: the VkGraphicsPipelineCreateInfo the RESOLVE pipeline will be created with. The rows it
/// points at are the rung's own: the triangle row of inputAssembly, the shared viewport/rasterizer/
/// multisample/dynamic rows, the OPAQUE column's blend pair (blend DISABLED - the resolve pass writes
/// every pixel it covers and composites against nothing), the struct's OWN resolveDepthStencil row,
/// and the resolve stage pair. The vertex input is the zero row xud_pipeline_fill_state() wrote and
/// xud_pipeline_fill_vertex_input() never overwrote for this pipeline: the triangle's only input is
/// gl_VertexIndex, and a vertex-input row naming the batch's streams would be a pipeline that waits
/// for buffers its pass never binds. Refuses XUD_UNSUPPORTED when any shared row is unstamped, and
/// XUD_INVALID_ARG on a NULL out-create-info or pipeline or a pipeline whose layout or resolveRender
/// Pass handle is not live.
XudResult xud_pipeline_fill_resolve_create_info(const XudPipeline* pipeline, VkGraphicsPipelineCreateInfo* outCreateInfo);

/// Pure: the VkGraphicsPipelineCreateInfo the DEPTH PRE-PASS pipeline will be created with. Its
/// rows are the rung's own prepassShaderStage (the one vertex stage), prepassVertexInput (the
/// position stream), the triangle row of inputAssembly, the shared viewport/rasterizer/multisample/
/// dynamic rows, its own prepassDepthStencil and prepassBlending, the shared layout, and
/// prepassRenderPass - the pass whose depth it writes and whose unreferenced colour it must not
/// paint. Refuses XUD_UNSUPPORTED when any shared row is unstamped, and XUD_INVALID_ARG on a NULL
/// out-create-info or pipeline, a pipeline whose layout or prepassRenderPass handle is not live,
/// or a prepassPipeline-stage row naming no module - the one difference from the resolve fill,
/// which shares the batch vertex row's stamping: a pre-pass assembled before its module was opened
/// is this layer's own ordering error.
XudResult xud_pipeline_fill_prepass_create_info(const XudPipeline* pipeline, VkGraphicsPipelineCreateInfo* outCreateInfo);

/// Pure: the VkGraphicsPipelineCreateInfo the SHADOW pipeline will be created with - the
/// pre-pass's fill read for the shadow's two different rows: shadowShaderStage (shadow.vert's
/// one vertex stage) and shadowRenderPass (the one-attachment map). Every state row is the
/// pre-pass's: prepassVertexInput, prepassDepthStencil, prepassBlending, the shared triangle
/// assembly and the shared layout - the occluder must rasterize exactly as the camera rasterizes
/// the same triangles, and one set of rows for both colorless passes is what makes that one fact
/// rather than two that can drift. Refuses XUD_UNSUPPORTED when any shared row is unstamped, and
/// XUD_INVALID_ARG on a NULL out-create-info or pipeline, a pipeline whose layout or
/// shadowRenderPass handle is not live, or a shadow stage row naming no module.
XudResult xud_pipeline_fill_shadow_create_info(const XudPipeline* pipeline, VkGraphicsPipelineCreateInfo* outCreateInfo);

/// Pure: the VkGraphicsPipelineCreateInfo ONE bloom row will be created with - the resolve's
/// fill read for the two rows that make it a bloom row: `stages` (the shared present vertex
/// module plus one of the two bloom fragment modules, supplied by the caller because the rung
/// holds both stage pairs) and bloomRenderPass (the one colour pass all four levels share).
/// Every state row is the resolve's: the zero vertex input, no depth, blend disabled, the
/// shared layout - a glow drawn by a pipeline that disagreed with the resolve about the
/// fullscreen triangle would be a glow in the wrong place. Refuses XUD_UNSUPPORTED when any
/// shared row is unstamped, and XUD_INVALID_ARG on a NULL out-create-info, pipeline or stages
/// row, a pipeline whose layout or bloomRenderPass handle is not live, or a stages pair naming
/// no module.
XudResult xud_pipeline_fill_bloom_create_info(
    const XudPipeline* pipeline, VkGraphicsPipelineCreateInfo* outCreateInfo, VkPipelineShaderStageCreateInfo* stages);

/// Driver: as before, and additionally the shadow pass's pipeline (S2.5): `shadowRenderPass`
/// binds the shadow row, and MAY be VK_NULL_HANDLE - the shadows-off configuration - in which
/// case no fourth module opens and shadowPipeline stays a null handle for the frame loop to
/// step around. When it is live: the sun's position-only module opens after the pre-pass's, its
/// one vertex stage is stamped from it, the two bloom fragment modules after it, their stage
/// rows reuse the present vertex module, and each further vkCreateGraphicsPipelines call binds
/// its row against its own pass - after the row before it is complete. The five pass arguments
/// five different handles: the batch list writes the float scratch colour, the tonemap row
/// writes the swapchain colour, the pre-pass row writes the scratch depth through a two-row
/// pass, the shadow row writes the map's depth through a one-row pass, and the bloom rows write
/// the chain's levels through the one-row colour pass they all share.
XudResult xud_pipeline_open(XudPipeline** outPipeline, XudLoader* loader, const XudDevice* device, const XudDescriptor* descriptor,
    const XudBatch* batch, VkRenderPass renderPass, VkRenderPass resolveRenderPass, VkRenderPass prepassRenderPass,
    VkRenderPass shadowRenderPass, VkRenderPass bloomRenderPass);

/// Driver: the teardown - vkDestroyPipeline for the shadow row, the pre-pass row, the resolve row
/// and every row of the list, then vkDestroyPipelineLayout, then every module of the frag-module
/// axis, the vertex module, the two present modules, the pre-pass module and the shadow module
/// through xud_shader_close(), then free. That order is load-bearing: the pipelines name their
/// layout and the layout's modules are the only thing keeping them alive, so anything else leaves
/// a handle whose owner is already gone.
/// XUD_ALREADY when the pointer owns no pipeline; XUD_INVALID_ARG on a NULL pointer-to-pointer, a NULL loader/device or
/// a handle-less device; XUD_UNSUPPORTED when the device rows are not this device's or a
/// destroy row is nil while a row of the list is live (the struct is not freed,
/// because freeing it would leak the handles silently).
XudResult xud_pipeline_close(XudPipeline** ioPipeline, const XudDevice* device, XudLoader* loader);

#endif /* XUD_PIPELINE_H_ */
