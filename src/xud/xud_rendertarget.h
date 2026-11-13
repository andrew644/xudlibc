/***********************************************************************************************
 * xud_rendertarget.h - the off-screen render target rung: one colour image of the caller's size,
 * its own depth image (through the render pass rung's off-screen variant), one framebuffer row,
 * one batch of its own, and one sampler-array slot so the image can be SAMPLED by the draws of
 * the same or a later frame.
 *
 * The rung sits above the renderer, beside the texture rung's record-owning siblings: it opens a
 * Vulkan triple no caller can hand it (image, pass, framebuffer, all at the TARGET's extent) and
 * hands the same triple to the renderer's scope list through xud_renderer_begin_target(). The
 * frame loop records every scope's pass BEFORE the screen pass, so the pass's own finalLayout -
 * SHADER_READ_ONLY - has retired the image as an attachment before any fragment stage reads it
 * (xud_frame.c's scope note); this rung's job ends at owning the objects that make that true.
 *
 * Ownership, one rung's view: the colour image and its view, the sampler, the pass (with the
 * depth image the pass rung owns inside it), the framebuffer row and the batch are ALL this
 * record's; the memory behind the image is the allocator's, released as a range; the sampler
 * slot is the descriptor's, given back before the view and sampler the entry names die - the
 * texture rung's release order, reused. The layout tracker learns the colour image at create and
 * forgets it at destroy; between those the frame moves it through the attachment layouts.
 ***********************************************************************************************/

#ifndef XUD_RENDERTEXTURE_H_
#define XUD_RENDERTEXTURE_H_

#include "volk.h" /* Required for: the loader's entry-point tables the driver halves call through */
#include "xud_allocator.h"
#include "xud_batch.h"
#include "xud_descriptor.h"
#include "xud_device.h"
#include "xud_handle.h"
#include "xud_layout.h"
#include "xud_loader.h"
#include "xud_physical.h"
#include "xud_renderpass.h"
#include "xud_vulkan.h"
#include <stdint.h>
#include <vulkan/vulkan.h>

/*-----------------------------------------------------------------------------------------------
 * One live render target, and every row it was assembled from
 *-----------------------------------------------------------------------------------------------*/

typedef struct XudRenderTargetRecord {
    XudRenderPass* pass;       /* owned: the off-screen pass - its colour finalLayout is SHADER_READ_ONLY and it
                                  owns the depth image of the same extent */
    XudFramebuffer* framebuffer; /* owned: the one row attaching this target's colour view and the pass's depth
                                    view */
    XudBatch* batch;           /* owned: the geometry the scope appends to while it is open */
    VkImage colorImage;        /* owned */
    XudMemoryRange colorRange; /* BORROWED from the allocator */
    VkImageView colorView;     /* owned: the view the framebuffer attaches AND the sampler-array entry names */
    VkImageCreateInfo colorImageInfo; /* what vkCreateImage was handed */
    VkImageViewCreateInfo colorViewInfo; /* what vkCreateImageView was handed */
    VkSampler sampler;         /* owned: the sampling state paired with the view - linear, clamped */
    VkSamplerCreateInfo samplerInfo; /* what vkCreateSampler was handed */
    uint32_t samplerSlot;      /* the descriptor sampler-array slot the pair occupies */
    uint32_t samplerSlotLive;  /* 1 once bound, 0 until - and the guard the retire reads */
    uint32_t width;
    uint32_t height;
    XudLayoutTracker* layout;  /* NOT owned: the context's tracker, held so the retire can forget the image */
} XudRenderTargetRecord;

/// Pure: the colour image's create-info - one subresource, one mip, the two usages that define
/// this rung (COLOR_ATTACHMENT: the pass writes it; SAMPLED: the sampler-array entry reads it),
/// and an initialLayout of UNDEFINED - the only layout a create may name for bytes never written,
/// the same birth the pass rung's depth image has. XUD_INVALID_ARG on a NULL out-pointer or a
/// zero extent.
XudResult xud_rendertarget_fill_color_info(VkImageCreateInfo* outCreateInfo, uint32_t width, uint32_t height, VkFormat format);

/// Driver: bring up one live render target under `handle` and hang the record off the table. The
/// order is the dependency order: colour image (create, carve, bind, tracker), colour view, sam
/// pler, the pass rung's off-screen variant (which brings its own depth image and registers it),
/// the framebuffer row, the batch, and the sampler-array slot LAST so no set ever names a half-
/// built target. Every failure retires in the reverse order. XUD_INVALID_ARG on a NULL out-handle/
/// table/loader/device/allocator/descriptor/tracker, a handle-less device, a zero extent; XUD_UN
/// SUPPORTED on a closed or unbound loader or a nil create row; XUD_OUT_OF_MEMORY and the allocator's,
/// pass's, framebuffer's, batch's and descriptor's own codes on the rows they run.
XudResult xud_rendertarget_open(XudHandle* outHandle, XudHandleTable* table, XudLoader* loader, const XudDevice* device,
    const XudDeviceCaps* caps, XudAllocator* alloc, XudDescriptor* descriptor, XudLayoutTracker* layout, uint32_t width, uint32_t height,
    VkFormat colorFormat);

/// Driver: release one live RENDERTEXTURE slot and retire everything its record owns, in the reverse
/// of the open: slot back to the descriptor, batch, framebuffer, pass (depth included), sampler,
/// tracker forget, view, allocator range, image. The caller above owns the in-flight rule: a target
/// whose scope is open this frame must not be closed while the renderer still points at its batch.
/// The codes mirror the other record rungs: XUD_INVALID_ARG on bad arguments or a wrong kind, XUD_
/// ALREADY on a free slot, XUD_UNSUPPORTED on a closed loader or a nil destroy row that leaves the
/// struct in hand.
XudResult xud_rendertarget_close(
    XudHandle handle, XudHandleTable* table, XudLoader* loader, const XudDevice* device, XudAllocator* alloc, XudDescriptor* descriptor);

/// Driver: the whole-kind teardown the close ladder runs - every slot of the kind, released or
/// still live, walked once, its record retired and freed. One device wait-idle guard for the walk.
/// The report is the first refusal, if any; *outClosed counts the records freed.
XudResult xud_rendertarget_close_all(
    XudHandleTable* table, XudLoader* loader, const XudDevice* device, XudAllocator* alloc, XudDescriptor* descriptor, uint32_t* outClosed);

/// Pure: the live record a handle names, kind-checked. XUD_INVALID_ARG on NULLs or a wrong kind;
/// the handle layer's own code on a free or stale slot.
XudResult xud_rendertarget_get(const XudHandleTable* table, XudHandle handle, const XudRenderTargetRecord** outRecord);

#endif /* XUD_RENDERTEXTURE_H_ */
