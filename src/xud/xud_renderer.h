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
#include "xud_layout.h" /* Required for: XudLayoutTracker - the context's map the pass's depth image and the frame's barriers speak through */
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
    XudLayoutTracker* layout; /* NOT owned: the context's layout tracker, held here because the RESI
                                ZE path reopens the frame loop, and the reopened frame needs the map
                                the old one read its barriers from - the swapchain hands the map to
                                no one else. */
    XudBatch* batch; /* owned: the batch rung's four SoA attribute streams and the one index
                        buffer, carved out of the allocator's memory, plus the one mapped staging
                        arena the CPU-side bytes are written into. Opened BEFORE the pipeline sits
                        before it because the pipeline's vertex-input state names these buffers, so
                        they have to exist before the pipeline is built against them and must outl
                        ive it - and the frame loop uploads, binds and draws them every frame. */
    XudBatch* screenBatch; /* the SAME pointer as batch whenever no render-target scope is open, and
                              the batch the draw paths return to when one closes. Owned here exactly
                              as batch is - this is the pointer xud_renderer_close() retires, because
                              `batch` alone cannot say which batch it names mid-frame: a target's own
                              batch belongs to the render target's record above this rung. */
    XudFramePassScope scopes[XUD_FRAME_SCOPE_MAX]; /* the off-screen pass scopes collected for THIS
                                                     frame, in begin order; xud_renderer_draw() hands
                                                     them to the frame loop and the count says how
                                                     many rows are live */
    uint32_t scopeCount; /* live rows of scopes */
    uint32_t inTarget; /* 0: the draw paths append to screenBatch; non-zero: batch names a target's batch */
    const XudFramebuffer* activeFramebuffer; /* the framebuffer of the scope that is open right now,
                                                NULL when none is: a render target whose row this is
                                                cannot be closed mid-scope, and the rung above reads
                                                this to say so */
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
    XudRenderPass* hdrPass; /* owned: the off-screen high-dynamic-range pass the batch rows draw
                               into - the same two-attachment shape as the screen pass (colour + D32
                               depth) with one difference: its colour format is a float format and
                               its colour finalLayout is SHADER_READ_ONLY, because the resolve pass
                               samples it. The batch list's pipelines are bound to THIS pass; the
                               screen pass above binds only the resolve pipeline. */
    XudFramebuffer* hdrFramebuffer; /* owned: the one framebuffer row attaching the scratch colour
                                       view above and the scratch pass's own depth view - one row
                                       because the scratch image is one image, recreated (with the
                                       pass) at the swapchain's extent whenever the swapchain is */
    XudRenderPass* prepassPass; /* owned: the depth pre-pass paired with the scratch pass above -
                                   the same two attachments, the same extent, but a subpass that
                                   references only the depth and a depth loadOp of CLEAR while the
                                   scratch pass's row reads LOAD (xud_renderpass_open_prepass()
                                   pairs the two at one call): the pre-pass sorts the depth buffer,
                                   the scratch pass shades against the sort. Its depth image, view
                                   and range are the scratch pass's, borrowed; it is closed before
                                   hdrPass and after the pipeline that names it. */
    XudFramebuffer* hdrPrepassFramebuffer; /* owned: the pre-pass's own framebuffer row over the
                                              SAME scratch colour image and the SAME depth view - a
                                              second row rather than a reuse of hdrFramebuffer,
                                              because render-pass compatibility counts the number
                                              of colour references per subpass, and a framebuffer
                                              built against the scratch pass is not compatible with
                                              the pre-pass's zero-colour subpass */
    XudRenderPass* shadowPass; /* owned: the occluder map's one-attachment pass (S2.5) - the
                                  D32 image sized by XUD_SHADOW_MAP_SIZE, DEPTH_ATTACHMENT |
                                  SAMPLED, cleared and written each frame before the scratch pass
                                  samples it and left in SHADER_READ_ONLY for that sampling. It
                                  is NOT swapchain-bound: a resize never touches it, and unlike
                                  every other pass here it is created ONCE for the renderer's
                                  life. */
    XudFramebuffer* shadowFramebuffer; /* owned: the map's one-attachment framebuffer row - the
                                          pass's own depth view attached, no colour view at all;
                                          the row owns no view and the generic close destroys
                                          only the VkFramebuffer */
    VkSampler shadowSampler; /* owned: the comparison sampler every draw's set 0 binding 1 names
                                - GREATER_OR_EQUAL against the surface asking, Vulkan reading
                                compareOp(reference, texel), because the surface that OWNS its
                                texel must pass its own test; and LINEAR, which
                                comparison sampling answers without any filter-feature promise
                                from the depth format: nine of these taps per fragment is the
                                PCF the frag's helper counts */
    VkSamplerCreateInfo shadowSamplerInfo; /* the bytes it was created from, recorded as every
                                              create in this tree records its own */
    VkImage hdrColorImage; /* owned: the scratch image itself - COLOR_ATTACHMENT | SAMPLED, the two
                              uses the two passes give it */
    VkImageView hdrColorView; /* owned: the view the framebuffer attaches AND the sampler-array
                                 slot names - one view, two readers, the render target's rule */
    VkSampler hdrSampler; /* owned: linear, clamped - the same sampling state a render target's
                             pair carries, for the same reason: a full-screen sample of an image
                             at its own extent never resamples, so the filter only shows at the
                             resolve's edges and linear is what an edge wants */
    VkImageCreateInfo hdrColorImageInfo; /* what vkCreateImage was handed, recorded as every rung
                                            in this tree records its create-infos */
    VkImageViewCreateInfo hdrColorViewInfo;
    VkSamplerCreateInfo hdrSamplerInfo;
    XudMemoryRange hdrColorRange; /* BORROWED from the allocator, released before the allocator closes */
    uint32_t hdrSlot; /* the descriptor sampler-array slot the scratch pair occupies - the frame
                         loop binds it as the resolve pass's set 1, and it is one slot of the same
                         pool the textures take (the ceiling holds XUD_MAX_TEXTURES textures plus
                         this one row) */
    uint32_t hdrSlotLive; /* 1 once the slot is bound, the guard the retire reads */
    uint32_t hdrWidth; /* the scratch extent, kept because the resize ask compares against it */
    uint32_t hdrHeight;
    VkFormat hdrFormat; /* the chosen float format (or the swapchain's, on a driver that offers no
                           float colour-attachment-and-sampled format - the resolve still runs and
                           still tonemaps, so the pipeline works everywhere and only the extra
                           headroom is a hardware courtesy). xud_create_render_texture() builds the
                           caller's targets in THIS format: pass compatibility is format-for-format,
                           and a target in another format would be a pass the scene pipelines
                           cannot draw into. */
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
    uint32_t cameraLatched; /* 0 = nothing latched, non-zero = the scope opened with the record above
                              has CLOSED since the last draw, and the draw below still projects with
                              it once. The close and the flush are not the same moment: a caller who
                              opens the camera, records draws under it and closes it before the frame
                              boundary (raylib's BeginMode3D/EndMode3D inside BeginDrawing/EndDrawing)
                              has recorded a frame that MEANS that camera, and the deferred flush at
                              the boundary must honour it - which is the one thing the flag above,
                              cleared by the close, can no longer say. Consumed by the draw, so a
                              camera begun and ended inside one frame still does not leak into the next. */
    uint32_t cameraLatchDraw; /* drawCount as of the close that set the latch: the latch belongs to
                              the draws recorded since the last flush, and this is the flush it waits
                              for. A close after draw 7 latches for draw 8; a close at frame 9 of a
                              camera that had been live through draw 8 is the same statement, and the
                              next draw is the same draw either way. */
    uint32_t drawCount; /* the draws the draw entry below has completed, counted from the open: the
                              stamp the latch is set with, and nothing else */
    XudHandle material; /* the material the caller last asked for, AS GIVEN: a value this rung owns
                          rather than a pointer into the handle table, so this rung cannot hold a
                          record of a table it does not own. The index of that handle is what the
                          batch's material channel carries, and the whole handle is what xud_close_
                          material() compares against before it clears the state below. */
    uint32_t materialVariant; /* the variant that material draws through, copied off its record at
                                 the begin call rather than re-read from the table: the record the
                                 frame opening moves is a PAIR, and this rung cannot reach the tabl
                                 e it would read it from (the table is the last rung xud_init_context
                                 opens, so it does not exist yet when this one is created). */
    uint32_t materialTexture; /* the set-1 slot that material's map occupies, resolved off the
                                 texture record at the begin call: the texture half of the triple
                                 the frame opening moves, XUD_BATCH_NO_TEXTURE while the material
                                 names no map */
    uint32_t materialWanted; /* 0 = no material live, non-zero = the pair above is what the next
                                frame is drawn with. The same shape as the flag above, and the same
                                reason: the state is re-applied at every frame opening, so a material
                                begun in one frame is drawn with in the next rather than leaked into
                                it, and xud_batch_reset() has already moved the batch back to the
                                default when this rung did not ask for a material. */
    XudColor background; /* the background the caller last asked for, AS GIVEN - the public
                             xud_clear_background() writes it and the draw entry above applies it
                             to the render pass's clear value at every frame opening, the same
                             shape the camera's record and flag give the block's matrix except
                             without a wanted half: a camera is a scope the caller opens and
                             closes, a background is a state that stays, so it is re-applied
                             whether or not the caller asked for one in this frame. The value is
                             the render pass's own default, quantized to the byte grid this
                             struct lives on, when the renderer opens (see xud_renderer.c's open). */
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
/// in->imageCount, the scratch pass and its one framebuffer row and one float colour image exist at
/// the swapchain's extent, and the pipeline list's rows are bound to the SCRATCH pass while the
/// resolve row is bound to the screen pass. `physicalDevice` is queried for the scratch format - the
/// first of R16G16B16A16_SFLOAT, R32G32B32A32_SFLOAT that answers COLOR_ATTACHMENT | SAMPLED, and
/// the swapchain's own format otherwise - so the choice is this driver's answer and not a guess.
/// XUD_INVALID_ARG on a NULL out-pointer or one that already owns a renderer, a
/// NULL loader/device/swapchain/tracker, a handle-less device or physicalDevice, or a swapchain with
/// no handle and no images; XUD_UNSUPPORTED when the loader is closed, its device rows are not this
/// device's, a row of one of the five rungs needs is nil, the caps cannot hold the batch's four
/// vertex-input bindings and four attributes, or the swapchain has no chosen format. XUD_OUT_OF_MEMORY
/// when a host allocation of one of the rungs fails.
XudResult xud_renderer_open(XudRenderer** outRenderer, XudLoader* loader, const XudDevice* device, VkPhysicalDevice physicalDevice,
    const XudDeviceCaps* caps, XudDescriptor* descriptor, XudAllocator* alloc, const XudSwapchain* swapchain, XudLayoutTracker* layout);

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

/// Driver: open one off-screen pass scope for this frame - the triple the frame loop records before
/// the screen pass - and move the draw paths onto the target's own batch. Re-opening the same target
/// in the same frame reuses its row rather than clearing its content twice. XUD_INVALID_ARG on a NULL
/// triple member or a NULL renderer; XUD_UNSUPPORTED when the scope list is already full.
XudResult xud_renderer_begin_target(XudRenderer* renderer, XudRenderPass* pass, const XudFramebuffer* framebuffer, XudBatch* batch);

/// Driver: close the open scope - the draw paths go back to the screen batch. The scope's row stays
/// in this frame's list; it is the frame that replays it, not this call. XUD_INVALID_ARG on a NULL
/// renderer; XUD_ALREADY when no scope is open.
XudResult xud_renderer_end_target(XudRenderer* renderer);

/// Driver: the frame boundary - the scope list empties and the draw paths come back to the screen
/// batch. xud_begin_drawing() runs it, the same rung that resets the batch. XUD_INVALID_ARG on a
/// NULL renderer.
XudResult xud_renderer_reset_targets(XudRenderer* renderer);

/// Pure: the extent pixel coordinates mean RIGHT NOW - the active scope's when a target's scope is
/// open, the screen's otherwise. The 2D draw paths map pixels to clip through this rather than the
/// window's size, so a rectangle drawn inside a 256x256 target covers the target the way the same
/// call covers the window outside any scope. XUD_INVALID_ARG on a NULL renderer or either out.
XudResult xud_renderer_pixel_extent(
    const XudRenderer* renderer, uint32_t screenWidth, uint32_t screenHeight, uint32_t* outWidth, uint32_t* outHeight);

/// Pure: whether the recreate ask the frame loop left behind needs a rebuild. (fbWidth) / (fbHe
/// ight) is the window's framebuffer extent as xud_window_framebuffer_size() read it, (swapchain
/// Width) / (swapchainHeight) the extent the swapchain in hand renders at. True only when the two
/// differ AND the window has an extent to build at: a 0/0 framebuffer is a window with no
/// presentable extent (the answer a Wayland compositor gives a minimized window), and the answer
/// to that is to hold the frame the swapchain already has, not a rebuild at an extent no swapchain
/// can be built at - a swapchain's extent has a floor of 1 (VUID-VkSwapchainCreateInfoKHR-imageEx
/// tent-01789), so a rebuild here would be one that has to fail, or one silently clamped to a size
/// the window does not have. The restore is the half that puts the ask back: a window restored to
/// a size the swapchain does not have is a surface whose extent moved again, which is what the
/// out-of-date code the next acquire answers with re-sets the flag through.
bool xud_renderer_recreate_needed(uint32_t fbWidth, uint32_t fbHeight, uint32_t swapchainWidth, uint32_t swapchainHeight);

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
/// loop), and in both of those the flag is cleared with no rung touched. A framebuffer with no
/// extent (0/0 - the answer a Wayland compositor gives a minimized window) is the same answer with
/// no rebuild: the flag is cleared, no rung is touched, and the swapchain keeps presenting the
/// frame it already has until the window has an extent back, which is what keeps a frame loop left
/// running with its window minimized seeing XUD_OK out of xud_end_drawing() rather than a bad-
/// argument loop. XUD_INVALID_ARG on a NULL renderer/pointer, a NULL loader/device/caps/allocator/physical
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
