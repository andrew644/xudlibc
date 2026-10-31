/***********************************************************************************************
 *   xud_renderer.h - the renderer rung: the owner of one batch, the pipeline rung's variant
 *                    list of pipelines, one render pass, one framebuffer per swapchain image
 *                    and one frame loop
 *
 *   Layer: above the frame loop, below the public API. This is the rung of the plan's layer
 *   ladder - "xudrl renderer (passes, pipelines, batches, buffers)" - and it is the rung
 *   xud_init_context() wires, which is what makes the context's renderer field live rather
 *   than a sentinel. It owns five things and nothing else: the batch rung's four SoA streams and
 *   one index buffer, the pipeline rung's variant list of pipelines (one layout and two shader
 *   modules shared under the whole list), the render-pass
 *   rung's one VkRenderPass, that rung's array of one VkFramebuffer per swapchain image, and the
 *   frame loop's command pool, command buffer, acquire semaphore and per-image fences.
 *
 *   The order this rung opens and closes in is load-bearing, and it is the plan's own
 *   dependency direction read both ways. Open: render pass -> batch -> pipeline -> framebuffer array
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
 *   ies last. Resize: the same order run down and back up again - frame, framebuffer array, the
 *   context's swapchain SLOT, the pass's depth image, rows, frame - which is what xud_renderer_
 *   resize() is, and why the swapchain is handed to it as the context's SLOT rather than the swap
 *   chain itself.
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
#include "xud_camera.h" /* Required for: XudCamera3D (xud.h's, which this header reaches) and the
                           camera rung's apply, which the draw entry above runs every frame */
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
#include "xud_window.h"
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
    XudDescriptor* descriptor; /* NOT owned: the descriptor rung the context below this one
                                        created and closes, held here because this rung's pipelin
                                        e was built against THIS descriptor's two set layouts and
                                        the frame loop binds THIS descriptor's set 0 every frame.
                                        The same rule the batch's note above gives: the owner of
                                        this record must outlive this renderer, and closing the t
                                        wo in the other order is a pipeline layout naming a freed
                                        one. */
    XudRenderPass* renderPass; /* owned: the one explicit VkRenderPass of the repo's settled
                                  render-pass decision */
    XudFramebufferArray* framebuffers; /* owned: one VkFramebuffer (and one VkImageView) per
                                          swapchain image, in image-index order */
    XudFrame* frame; /* owned: the command pool, the command buffer, the acquire semaphore and
                        the per-image fences */
    uint32_t framebufferCount; /* the count xud_frame_draw() is handed, recorded here so the
                                  draw entry above does not have to reach into the array's own
                                  field - and so that it cannot disagree with it */
    XudCamera3D camera; /* the camera the caller last asked for, AS GIVEN: a value this rung owns
                          rather than a pointer to a caller's record, so a caller's local camera go
                          ing out of scope cannot leave a dangling read here. Nothing reads it but
                          the draw entry below, which hands it to the camera rung. */
    uint32_t cameraWanted; /* 0 = no camera live, non-zero = the record above is what the next frame
                              is drawn with. A flag rather than a NULL sentinel because the record
                              is a value and not a pointer, and the draw entry below turns the flag
                              back into the NULL the camera rung reads as "write the identity matrix"
                              - which is what makes the block's matrix a function of THIS frame's
                              camera state rather than of whatever the last frame's camera wrote. */
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
    XudDescriptor* descriptor, XudAllocator* alloc, const XudSwapchain* swapchain);

/// Driver: one frame through the four rungs this rung owns - the caller above names no pipeline,
/// no pass, no framebuffer array and no fence, which is what keeps the whole Vulkan half of the
/// library below the public API. The image index that was acquired and presented is written out.
/// XUD_INVALID_ARG on a NULL renderer or out-index or on any rung this draw needs being handle
/// less; XUD_UNSUPPORTED when the loader is closed, its rows are not this device's, a row the
/// frame needs is nil, or the fence and framebuffer arrays no longer match the swapchain in hand
/// (the plan's "the framebuffer array is recreated with the swapchain" rule, read as a refusal).
/// What this rung leaves behind rather than acting on: an acquire or a present that answered the
/// swapchain's two out-of-band codes ran the frame to the end (the image was usable and the present
/// ation happened) and set renderer->frame->recreateWanted, which is xud_renderer_resize()'s rung
/// to read - recreating is not this rung's either, since the swapchain is the context's and every
/// image, framebuffer row and fence index in here dies with it.
XudResult xud_renderer_draw(
    XudRenderer* renderer, XudLoader* loader, const XudDevice* device, const XudSwapchain* swapchain, uint32_t* outImageIndex);

/// Driver: the resize step - the one rung in this tree that acts on the recreate ask the frame
/// loop above leaves behind. renderer->frame->recreateWanted IS the ask (xud_vulkan.h's swapchain-
/// boundary funnel is the only thing that sets it), and the ask is acted on at the NEXT acquire,
/// which is what this rung is called BETWEEN two draws rather than inside one: the swapchain is
/// the context's rather than this rung's, so the rung is handed the context's swapchain SLOT rather
/// than the swapchain itself, and a recreate replaces the whole array of images, every framebuffer
/// row and every fence index. The order is the open order run down and back up, and every step of
/// it is load-bearing: close the FRAME first (its fences are the ones a presentation against the
/// OLD images may still be waiting on, and xud_frame_close() is that guard), then the FRAMEBUFFER
/// ARRAY (its rows' views name the swapchain images the recreate below is about to retire - VUID-
/// vkDestroyDevice-device-05137 is what the validation layer reports a handle that survives its
/// destroy at, so a row naming a retired image is a row that has to die BEFORE the recreate and not
/// after it), then xud_swapchain_recreate() at the window's FRAMEBUFFER extent (read through
/// xud_window_framebuffer_size(), never the window's requested extent: a Hi-Wayland window is 1600x
/// 900 requested and 3200x1800 of framebuffer, and a swapchain at the first is one the compositor
/// calls out of date on every acquire, which is what sets the flag again forever), then the pass's
/// DEPTH image at the new extent (VUID-VkFramebufferCreateInfo-attachments-02702 requires every
/// attachment of one framebuffer at the framebuffer's own extent, so the depth buffer is the one
/// attachment a swapchain recreate forces), then the rows again, then the frame again. The batch,
/// the pipeline and the descriptor are NOT touched: none of them names an extent. On XUD_OK the
/// swapchain, the framebuffer array and the frame are all new, framebufferCount == the new image
/// count, and recreateWanted is cleared. XUD_ALREADY when nothing was asked for (the flag is 0 - a
/// draw that answered VK_SUCCESS leaves this rung to be called and to answer that the extent has not
/// moved) or when the ask has already been satisfied (the swapchain is already at the extent the
/// window's framebuffer is, in which case the flag is cleared too, so one ask cannot turn into a
/// loop). XUD_INVALID_ARG on a NULL renderer/pointer, a NULL loader/device/caps/allocator/physical
/// device/window/swapchain pointer or slot, a slot holding NULL, or a renderer missing any rung this
/// rebuilds; XUD_UNSUPPORTED when the loader is closed, its rows are not this device's, the fence
/// and framebuffer arrays no longer match the swapchain in hand, or a rung's guard cannot run. A
/// rebuild that fails after the teardown has run leaves the renderer with the rungs it still has and
/// nothing leaked - it is not drawable, and xud_renderer_close() retires whatever is left.
XudResult xud_renderer_resize(XudRenderer** ioRenderer, XudLoader* loader, const XudDevice* device, const XudDeviceCaps* caps,
    XudAllocator* alloc, const XudPhysicalDevice* physicalDevice, const XudWindow* window, XudSwapchain** ioSwapchain);

/// Driver: the teardown, in the reverse of the open order above and for the reasons the file-top
/// note gives. Every rung reports the first thing that refused, and the rungs after a refusal
/// still run - the rule xud_close_context() states for its own rungs, one rung up. XUD_ALREADY
/// when the pointer owns no renderer; XUD_INVALID_ARG on a NULL pointer-to-pointer or a NULL
/// loader/device/allocator.
XudResult xud_renderer_close(XudRenderer** ioRenderer, const XudDevice* device, XudLoader* loader, XudAllocator* alloc);

#endif /* XUD_RENDERER_H_ */
