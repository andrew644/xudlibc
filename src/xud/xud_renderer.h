/***********************************************************************************************
 *   xud_renderer.h - the renderer rung: the owner of one pipeline, one render pass, one
 *                    framebuffer per swapchain image and one frame loop
 *
 *   Layer: above the frame loop, below the public API. This is the rung of the plan's layer
 *   ladder - "xudrl renderer (passes, pipelines, batches, buffers)" - and it is the rung
 *   xud_init_context() wires, which is what makes the context's renderer field live rather
 *   than a sentinel. It owns four things and nothing else: the pipeline rung's one pipeline
 *   (with its layout and its two shader modules inside it), the render-pass rung's one
 *   VkRenderPass, that rung's array of one VkFramebuffer per swapchain image, and the frame
 *   loop's command pool, command buffer, acquire semaphore and per-image fences.
 *
 *   The order this rung opens and closes in is load-bearing, and it is the plan's own
 *   dependency direction read both ways. Open: render pass -> pipeline -> framebuffer array
 *   -> frame. The pass is FIRST because the core-1.4 binding makes it an argument of the pipe
 *   line's create (VUID-VkGraphicsPipelineCreateInfo-dynamicRendering-06576: with dynamicRende
 *   ring not enabled - and it is not, see the render-pass decision in AGENTS.md - a graphics
 *   pipeline with pre-rasterization or fragment state must name the pass it belongs to, and t
 *   he validation layer on this machine reports VK_NULL_HANDLE there as a hard error, probed t
 *   oday). A framebuffer is created FOR a pass too (the spec requires the framebuffer's attach
 *   ment list to match the pass's descriptions exactly) and a frame records a draw naming a pi
 *   peline, a pass and a framebuffer row, so each of the four needs the ones above it in hand.
 *   Close: the exact reverse - frame first, because a frame is the only thing in this set that
 *   can still have a presentation in flight against the swapchain's images and its own close i
 *   s the fence guard that retires it; then the framebuffer array, whose views name those very
 *   images; then the pipeline and then the pass, the two that name that pass's handle, so it d
 *   ies last.
 *
 *   What this rung does NOT own, and why: no swapchain (the context owns it, one rung below),
 *   no VkImage of a swapchain image (the swapchain owns its images, xud_swapchain.c's note
 *   about vkDestroyImage), no GPU-memory allocator of its own (the allocator rung IS in the tree
 *   now (S12), one rung below the renderer in the ownership tree and owned by the context rather
 *   than by this rung - which is why the pipeline declares zero vertex bindings and the draw's
 *   whole geometry is a vertex count). The batch and the object kinds a public XudHandle names
 *   are steps above this one that have not landed. The DESCRIPTOR rung, though, is in the tree
 *   and sits BELOW this one (allocator -> descriptor -> renderer, because a VkBuffer carved from
 *   allocator-owned memory is named by the pipeline layout above): the renderer is handed the
 *   descriptor record so the pipeline it builds is built against the two set layouts and the
 *   push-constant range that per-frame data was written into, and it does not own it - the
 *   context does, and it closes after this rung has retired the pipeline that names them.
 *
 *   THE BATCH RUNG IS NOW IN THE TREE, AND THIS RUNG OWNS IT. The batch opens AFTER the render
 *   pass and BEFORE the pipeline, and closes in the reverse slot - after the pipeline has been
 *   retired and before the pass - because the batch sits BELOW the pipeline in the ladder: the
 *   pipeline's vertex-input state is the thing that names the batch's buffers, so the buffers
 *   have to exist before the pipeline is built against them and must outlive it. That order is
 *   no longer a promise: xud_pipeline_open() takes the batch and xud_pipeline_fill_vertex_input()
 *   points the pipeline's vertex-input row at the batch's four binding and four attribute rows,
 *   and xud_frame_draw() takes it too, so the batch's six buffers are created, uploaded, bound
 *   (one vkCmdBindVertexBuffers call over the four streams, one vkCmdBindIndexBuffer call over
 *   the index buffer) and drawn by vkCmdDrawIndexed rather than by a hard-coded vkCmdDraw.
 ***********************************************************************************************/

#ifndef XUD_RENDERER_H_
#define XUD_RENDERER_H_

#include "volk.h" /* Required for: the loader's entry-point tables - and it must come before
                     <vulkan/vulkan.h>, which the header below it includes; volk.h errors when
                     VULKAN_H_ is already in (AGENTS.md, include-order trap) */
#include "xud_allocator.h"
#include "xud_batch.h"
#include "xud_descriptor.h"
#include "xud_device.h"
#include "xud_frame.h"
#include "xud_loader.h"
#include "xud_physical.h"
#include "xud_pipeline.h"
#include "xud_renderpass.h"
#include "xud_swapchain.h"
#include "xud_vulkan.h"
#include <stdint.h>

/*-----------------------------------------------------------------------------------------------
 * The one renderer of one context, and the four rungs it owns
 *-----------------------------------------------------------------------------------------------*/

typedef struct XudRenderer XudRenderer;
struct XudRenderer {
    XudBatch* batch; /* owned: the batch rung's four SoA attribute streams and the one index
                        buffer, carved out of the allocator's memory, plus the one mapped staging
                        arena the CPU-side bytes are written into. Opened BEFORE the pipeline sits
                        before it because the pipeline's vertex-input state names these buffers, so
                        they have to exist before the pipeline is built against them and must outl
                        ive it - and the frame loop uploads, binds and draws them every frame. */
    XudPipeline* pipeline; /* owned: the one graphics pipeline of this rung, with its layout
                              and its two VkShaderModules inside it */
    XudRenderPass* renderPass; /* owned: the one explicit VkRenderPass of the repo's settled
                                  render-pass decision */
    XudFramebufferArray* framebuffers; /* owned: one VkFramebuffer (and one VkImageView) per
                                          swapchain image, in image-index order */
    XudFrame* frame; /* owned: the command pool, the command buffer, the acquire semaphore and
                        the per-image fences */
    uint32_t framebufferCount; /* the count xud_frame_draw() is handed, recorded here so the
                                  draw entry above does not have to reach into the array's own
                                  field - and so that it cannot disagree with it */
};

/*-----------------------------------------------------------------------------------------------
 * The layer's contract
 *-----------------------------------------------------------------------------------------------*/

/// Driver: the whole renderer, in the one order that is correct - the render pass first (whose
/// attachment format is the swapchain's CHOSEN format, read off the swapchain's own
/// record rather than re-queried from a surface this rung cannot name), then the BATCH rung (hand-
/// ed the caps and the allocator, because the batch's buffers are carved out of allocator-owned
/// memory and its vertex-input state is gated on the caps' vertex-input limits), then the pipeline
/// (handed the pass's handle, for the reason the file-top note gives), then the framebuffer
/// array (one row per swapchain image, each row built from the image xud_swapchain_images_get()
/// hands back, which is the repo's rule that a rung above the swapchain holds an INDEX rather
/// than a VkImage), then the frame loop. Every rung's teardown is checked before its create,
/// and a rung that refuses leaves everything created so far closed in reverse order, so a
/// renderer that comes back NULL owns nothing. The render pass now takes the physical layer's
/// recorded caps and the allocator below this rung, because the pass's second attachment is a
/// depth image this rung creates and whose memory the allocator owns - the renderer does not own
/// the allocator, the context does, one rung below, and closes it after this rung's teardown has
/// released the range. On XUD_OK the five fields above are live and framebufferCount == swapcha
/// in->imageCount. XUD_INVALID_ARG on a NULL out-pointer or one that already owns a renderer, a
/// NULL loader/device/swapchain, a handle-less device, or a swapchain with no handle and no imag
/// es; XUD_UNSUPPORTED when the loader is closed, its device rows are not this device's, a row o
/// ne of the five rungs needs is nil, the caps cannot hold the batch's four vertex-input bindings
/// and four attributes, or the swapchain has no chosen format. XUD_OUT_OF_MEMORY when a host
/// allocation of one of the rungs fails.
XudResult xud_renderer_open(XudRenderer** outRenderer, XudLoader* loader, const XudDevice* device, const XudDeviceCaps* caps,
    const XudDescriptor* descriptor, XudAllocator* alloc, const XudSwapchain* swapchain);

/// Driver: one frame through the four rungs this rung owns - the caller above names no pipeline,
/// no pass, no framebuffer array and no fence, which is what keeps the whole Vulkan half of the
/// library below the public API. The image index that was acquired and presented is written out.
/// XUD_INVALID_ARG on a NULL renderer or out-index or on any rung this draw needs being handle
/// less; XUD_UNSUPPORTED when the loader is closed, its rows are not this device's, a row the
/// frame needs is nil, or the fence and framebuffer arrays no longer match the swapchain in hand
/// (the plan's "the framebuffer array is recreated with the swapchain" rule, read as a refusal).
XudResult xud_renderer_draw(
    XudRenderer* renderer, XudLoader* loader, const XudDevice* device, const XudSwapchain* swapchain, uint32_t* outImageIndex);

/// Driver: the teardown, in the reverse of the open order above and for the reasons the file-top
/// note gives. Every rung reports the first thing that refused, and the rungs after a refusal
/// still run - the rule xud_close_context() states for its own rungs, one rung up. XUD_ALREADY
/// when the pointer owns no renderer; XUD_INVALID_ARG on a NULL pointer-to-pointer or a NULL
/// loader/device/allocator.
XudResult xud_renderer_close(XudRenderer** ioRenderer, const XudDevice* device, XudLoader* loader, XudAllocator* alloc);

#endif /* XUD_RENDERER_H_ */
