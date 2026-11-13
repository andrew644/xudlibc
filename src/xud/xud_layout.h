/***********************************************************************************************
 * xud_layout.h - the layout tracker: the ONE place that knows what every image is being used
 *                for right now, and the choke point every layout transition is recorded through
 *
 *   Layer: beside the allocator, below every rung that names an image in a barrier. This is the
 * rung the plan's S1.3 asks for: one central record of which VkImage is in which VkImageLayout,
 * so a barrier's oldLayout is a LOOKUP rather than a hand-carried guess, and the rung that
 * transitions an image cannot drift from the rung that later transitions it again.
 *
 *   Why a hand-carried layout is the bug this rung kills: VkImageMemoryBarrier::oldLayout must
 * name the layout the image is ACTUALLY in (VUID-vkCmdPipelineBarrier-image-0407). Before this
 * rung existed each producer remembered that fact in its own field - the frame kept
 * XudFrame::imageLayouts[] and depthLayout, the descriptor kept the white image's two recorded
 * transition rows, the texture named its create-time layout in its upload barrier - and every
 * new effect (S1.4's off-screen targets above all) would have had to remember it too, in yet
 * another field, correctly, forever. A forgotten update is not a crash at the update: it is a
 * validation error or a corrupted image at some later barrier, far from the rung that forgot -
 * the "rare crashes that are miserable to trace" the plan names. After this rung there is one
 * answer to "what layout is this image in" and one way to change it.
 *
 *   What is tracked, by whom, and for how long: the rung that CREATES an image tracks it (the
 * swapchain's images and the pass's depth image at the frame's open, the white image at the
 * descriptor's open, each texture at its upload's open) and the same rung FORGETS it on the way
 * out (the frame's close forgets the swapchain and depth images it registered even though it
 * does not own them - it registered their layouts, so their records leave with it; the
 * descriptor and the texture forget their own). VkImage handles are recycled by the driver once
 * destroyed, so a stale entry is not merely untidy: it would answer a NEW image with the OLD
 * image's layout. The forget-on-teardown rule is what makes every key in the map live.
 *
 *   What is NOT here: subresource ranges, aspect masks, stage and access masks. Those describe
 * the transition, not the state, and the barrier's filler takes them from the caller's request
 * row. Only oldLayout is the tracker's to answer, and only newLayout is its to record - which
 * is exactly the pair that drifts. Buffer barriers are not here either: a buffer has one layout,
 * VK_BUFFER_LAYOUT_UNDEFINED, and nothing to track.
 *
 *   The storage is an open-addressed map keyed by the handle itself: entries are few (the
 * swapchain's images, one depth image, one white image, and the caller's textures), lookups are
 * one multiply-shift and a short probe, and no driver call is ever made from inside this rung -
 * the only entry point that touches the driver at all is the recording helper, which calls the
 * loader's one vkCmdPipelineBarrier row and nothing else.
 ***********************************************************************************************/

#ifndef XUD_LAYOUT_H_
#define XUD_LAYOUT_H_

#include "volk.h" /* Required for: VkImage, VkImageLayout, VkImageMemoryBarrier, VkCommandBuffer */

#include "xud.h"         /* Required for: XudResult */
#include "xud_loader.h"  /* Required for: XudLoader, the device table the recording helper calls through */

/*-----------------------------------------------------------------------------------------------
 * The map: one slot per tracked image, keyed by the handle
 *-----------------------------------------------------------------------------------------------*/

#define XUD_LAYOUT_MIN_SLOTS                                                                                                               \
    64u /* the smallest table, sized for a swapchain's images, the depth
                                    image, the white image and the first textures without a grow */

typedef struct {
    VkImage image; /* the key; VK_NULL_HANDLE is the free-slot sentinel, and every entry point
                      refuses a barrier or a track about VK_NULL_HANDLE for that reason */
    VkImageLayout layout; /* the layout the image is in as of the last record this rung saw */
} XudImageSlot;

typedef struct XudLayoutTracker {
    XudImageSlot* slots; /* the one host allocation, calloc'd at open and re-calloc'd by every grow */
    uint32_t slotCount;  /* the size of the array above; a slot whose image is VK_NULL_HANDLE is free */
    uint32_t usedCount;  /* the live entries, checked against the load factor before every insert */
} XudLayoutTracker;

/*-----------------------------------------------------------------------------------------------
 * The layer's contract
 *-----------------------------------------------------------------------------------------------*/

/// Bring-up: an empty tracker, every slot free. capacityHint is the caller's guess at the live
/// images - it is floored at XUD_LAYOUT_MIN_SLOTS, so 0 is the honest "I do not know" ask.
/// XUD_INVALID_ARG on a NULL out-param or a non-NULL one; XUD_OUT_OF_MEMORY only.
XudResult xud_layout_tracker_open(XudLayoutTracker** outTracker, uint32_t capacityHint);

/// Teardown guard: the host array is the only thing this rung holds, so there is no driver call
/// to order, and a NULL or already-NULL tracker answers XUD_ALREADY. The rung does NOT check that
/// the map is empty - a close-all teardown walks out of every producer in one sweep, and the
/// sweep's order is the context's, not this rung's to police.
XudResult xud_layout_tracker_close(XudLayoutTracker** ioTracker);

/// The create-time registration: the image entered `initialLayout` (the VkImageCreateInfo's own
/// initialLayout - the create's word is the tracker's first truth about the image). XUD_INVALID_ARG
/// on a NULL tracker or VK_NULL_HANDLE; XUD_INTERNAL_ERROR when the image is already tracked,
/// because a second create-time word about a live image means the producer never forgot the first.
XudResult xud_layout_track(XudLayoutTracker* tracker, VkImage image, VkImageLayout initialLayout);

/// The teardown deregistration. XUD_NOT_FOUND when the image is not tracked - forgetting an image
/// this rung never registered is the same drift the whole rung exists to kill.
XudResult xud_layout_forget(XudLayoutTracker* tracker, VkImage image);

/// The lookup: the layout the image is in. XUD_NOT_FOUND when it is not tracked, which is the
/// answer a barrier filler turns into a refusal rather than a guess.
XudResult xud_layout_get(VkImageLayout* outLayout, const XudLayoutTracker* tracker, VkImage image);

/// The record an effect makes WITHOUT a barrier: a render pass's own initialLayout/finalLayout
/// transitions happen inside the subpass, invisible to any barrier, and the rung that runs the
/// pass notes their result through this row after the submit. XUD_NOT_FOUND on an untracked image.
XudResult xud_layout_set(XudLayoutTracker* tracker, VkImage image, VkImageLayout layout);

/// The pure half of recording: outBarrier is request copied, with request->oldLayout REPLACED by
/// the tracked layout and everything else - stages, accesses, subresource range, image - kept as
/// the caller filled it. request->oldLayout itself is never read: the tracker is its only source.
/// XUD_INVALID_ARG on a NULL out, NULL request, out aliased to request, a request whose newLayout
/// is UNDEFINED (illegal as a destination, spec 32.5), or an untracked image (XUD_NOT_FOUND).
XudResult xud_layout_barrier_fill(
    VkImageMemoryBarrier* outBarrier, const XudLayoutTracker* tracker, VkImage image, const VkImageMemoryBarrier* request);

/// The recording half: fill (above), call the loader's vkCmdPipelineBarrier with the ONE barrier
/// and the two stage masks - the masks belong to the RECORDING, which is why they are arguments
/// here rather than fields of the row, the same split VkCmdPipelineBarrier itself makes - and
/// record newLayout for the image: the state and the command cannot drift because there is no way
/// to record one without the other. A caller that records TWO image barriers in one call (the
/// frame's colour-and-depth opening pair) uses the fill row and the set row around its own call
/// instead; this row is the single-barrier common case. XUD_UNSUPPORTED when the loader is not
/// open or the device-table row is NULL; otherwise the fill's refusals.
XudResult xud_layout_barrier(XudLayoutTracker* tracker, const XudLoader* loader, VkCommandBuffer commandBuffer,
    const VkImageMemoryBarrier* request, VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask);

#endif /* XUD_LAYOUT_H_ */
