/***********************************************************************************************
 *
 *   xud_test_layout.c - the layout tracker: the map, its refusals, and the barrier filler
 *
 *   Owns : track/get/set/forget as a map contract (the sentinel, the duplicate, the unknown key,
 *          the grow and the probe walk), and barrier_fill as a pure row (oldLayout replaced, the
 *          request's own fields kept, the refused destinations). The recording row xud_layout_bar
 *          rier is checked at its guards only - a headless run has no command buffer to record
 *          into, and the GPU run records through it for real inside the descriptor and texture
 *          areas and reads through it inside the frame, whose validation-clean checks are its
 *          behaviour evidence.
 *   Layer: tests. Sees xud_layout.h and xud_loader.h: the loader is opened (never bound to a
 *          device) to prove the recording row refuses before calling through a NULL table row.
 *
 *   Deterministic: fixed handles from heap addresses, fixed layouts, no time, no randomness.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud.h" /* Required for: XudResult, the XUD_GPU_TESTS gate */
#include "xud_layout.h" /* Required for: XudLayoutTracker, XudImageSlot, the track/forget/get/set/fill rows */
#include "xud_loader.h" /* Required for: XudLoader, the open-unbound loader the recording guards answer */
#include "xud_test_harness.h" /* Required for: xud_check, xud_gpu_tests_enabled */

#include <stdint.h> /* Required for: uint64_t, the storage the synthetic handles point at */

/*-----------------------------------------------------------------------------------------------
 * The map, headless: synthetic images are distinct heap addresses, which is all a key has to be
 * (the repo's handle assumption - a VkImage is a pointer - is the one xud_loader.c asserts).
 *-----------------------------------------------------------------------------------------------*/
static void xud_test_layout_pure(void) {
    XudLayoutTracker* tracker = NULL;
    XudLoader* loader = NULL;
    uint64_t imageStorage[8];
    VkImage images[8];
    VkImageMemoryBarrier request = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .pNext = NULL };
    VkImageMemoryBarrier filled;
    VkImageLayout layout = VK_IMAGE_LAYOUT_MAX_ENUM;
    XudResult result;
    uint32_t index = 0u;

    for (index = 0u; index < 8u; index++) {
        images[index] = (VkImage)&imageStorage[index];
    }

    /* ---- open and its guards ---- */
    xud_check(xud_layout_tracker_open(NULL, 0u) == XUD_INVALID_ARG, "layout/open-null-out", "no out-param to write into");
    tracker = (XudLayoutTracker*)1;
    xud_check(xud_layout_tracker_open(&tracker, 0u) == XUD_INVALID_ARG, "layout/open-non-null-out", "a non-NULL out-param is refused");
    xud_check(tracker == (XudLayoutTracker*)1, "layout/open-rejected-untouched", "a rejected open leaves the caller's pointer alone");
    tracker = NULL;

    xud_check(
        xud_layout_tracker_close(&tracker) == XUD_ALREADY, "layout/close-null-is-already", "closing nothing is the idempotent answer");
    xud_check(xud_layout_tracker_close(NULL) == XUD_INVALID_ARG, "layout/close-null-in", "a null address is refused");

    xud_check(xud_layout_tracker_open(&tracker, 0u) == XUD_OK, "layout/open-ok", "the hint of 0 is the honest ask");
    xud_check(tracker != NULL, "layout/open-wrote", "and the tracker exists");
    if (tracker == NULL) {
        return;
    }
    xud_check(tracker->usedCount == 0u, "layout/open-empty", "with nothing in it");
    xud_check(tracker->slotCount >= XUD_LAYOUT_MIN_SLOTS, "layout/open-floored", "and the table floored at the minimum, hint or no hint");

    /* ---- the key rules ---- */
    xud_check(xud_layout_track(NULL, images[0], VK_IMAGE_LAYOUT_UNDEFINED) == XUD_INVALID_ARG, "layout/track-null-tracker",
        "a tracker that does not exist cannot register anything");
    xud_check(xud_layout_track(tracker, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED) == XUD_INVALID_ARG, "layout/track-null-image",
        "the sentinel is a free slot, never a key");
    xud_check(xud_layout_get(&layout, tracker, VK_NULL_HANDLE) == XUD_INVALID_ARG, "layout/get-null-image", "and never looked up");
    xud_check(xud_layout_get(NULL, tracker, images[0]) == XUD_INVALID_ARG, "layout/get-null-out", "with no answer to write");

    xud_check(xud_layout_track(tracker, images[0], VK_IMAGE_LAYOUT_UNDEFINED) == XUD_OK, "layout/track-ok", "the create-time word lands");
    xud_check(xud_layout_track(tracker, images[0], VK_IMAGE_LAYOUT_GENERAL) == XUD_INTERNAL_ERROR, "layout/track-duplicate",
        "a second create-time word about a live image is the drift this rung exists to surface");
    xud_check(xud_layout_get(&layout, tracker, images[0]) == XUD_OK, "layout/get-after-duplicate", "and the refusal wrote nothing");
    xud_check(layout == VK_IMAGE_LAYOUT_UNDEFINED, "layout/get-keeps-first-word", "the first word stands");

    xud_check(xud_layout_get(&layout, tracker, images[1]) == XUD_NOT_FOUND, "layout/get-unknown", "an untracked image has no layout here");
    xud_check(xud_layout_set(tracker, images[1], VK_IMAGE_LAYOUT_GENERAL) == XUD_NOT_FOUND, "layout/set-unknown",
        "and cannot be re-laid-out through a row that would invent a record for it");
    xud_check(xud_layout_forget(tracker, images[1]) == XUD_NOT_FOUND, "layout/forget-unknown",
        "forgetting an image never registered is refused, not ignored");

    /* ---- the state changes ---- */
    xud_check(xud_layout_set(tracker, images[0], VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL) == XUD_OK, "layout/set-ok", "the note lands");
    xud_check(xud_layout_get(&layout, tracker, images[0]) == XUD_OK, "layout/get-after-set", "and is read back");
    xud_check(layout == VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL, "layout/get-sees-set", "the record moved, not the key");
    xud_check(xud_layout_forget(tracker, images[0]) == XUD_OK, "layout/forget-ok", "and the teardown deregistration runs");
    xud_check(xud_layout_get(&layout, tracker, images[0]) == XUD_NOT_FOUND, "layout/get-after-forget", "the key is gone");
    xud_check(xud_layout_track(tracker, images[0], VK_IMAGE_LAYOUT_GENERAL) == XUD_OK, "layout/retrack-after-forget",
        "a recycled handle may be tracked again with its own first word");

    /* ---- the grow: inserts past the floor keep every key with its own layout ---- */
    for (index = 1u; index < 8u; index++) {
        xud_check(xud_layout_track(tracker, images[index], (VkImageLayout)(VK_IMAGE_LAYOUT_GENERAL + index)) == XUD_OK, "layout/grow-track",
            "the table takes inserts without refusing");
    }
    for (index = 1u; index < 8u; index++) {
        layout = VK_IMAGE_LAYOUT_MAX_ENUM;
        result = xud_layout_get(&layout, tracker, images[index]);
        xud_check(result == XUD_OK, "layout/grow-key-survives", "every key stays answerable");
        xud_check(layout == (VkImageLayout)(VK_IMAGE_LAYOUT_GENERAL + index), "layout/grow-layout-survives",
            "and the layout is the one its own track wrote, not a neighbour's");
    }
    xud_check(xud_layout_get(&layout, tracker, images[0]) == XUD_OK, "layout/grow-original-survives", "including the first key");
    xud_check(layout == VK_IMAGE_LAYOUT_GENERAL, "layout/grow-original-layout", "with its own first word");

    /* The growth edge in the open: at three quarters of the CURRENT table the next insert reha
      shes into a doubled one, and a rehash that loses or shuffles a record is exactly the drift
      this rung must never show, so every key is read back after it.
     */
    {
        uint64_t bulkStorage[48];
        uint32_t slotCountBefore = tracker->slotCount;

        for (index = 0u; index < 48u; index++) {
            result = xud_layout_track(tracker, (VkImage)&bulkStorage[index], (VkImageLayout)(VK_IMAGE_LAYOUT_GENERAL + (index % 7u)));
            xud_check(result == XUD_OK, "layout/bulk-track", "inserts up past the load factor keep landing");
        }
        xud_check(
            tracker->slotCount > slotCountBefore, "layout/bulk-grew", "the table doubled on the way, it did not refuse or round-trip");
        for (index = 0u; index < 48u; index++) {
            layout = VK_IMAGE_LAYOUT_MAX_ENUM;
            result = xud_layout_get(&layout, tracker, (VkImage)&bulkStorage[index]);
            xud_check((result == XUD_OK) && (layout == (VkImageLayout)(VK_IMAGE_LAYOUT_GENERAL + (index % 7u))), "layout/bulk-keys-survive",
                "every bulk key came through the rehash with its own record");
        }
        for (index = 1u; index < 8u; index++) {
            layout = VK_IMAGE_LAYOUT_MAX_ENUM;
            result = xud_layout_get(&layout, tracker, images[index]);
            xud_check((result == XUD_OK) && (layout == (VkImageLayout)(VK_IMAGE_LAYOUT_GENERAL + index)), "layout/bulk-old-keys-survive",
                "and the keys that predate the grow did not pay for it with their records");
        }
    }

    /* ---- the barrier filler ---- */
    request.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    request.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    request.oldLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL; /* a deliberate lie: the filler must ignore it */
    request.newLayout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
    request.srcQueueFamilyIndex = 0u;
    request.dstQueueFamilyIndex = 0u;
    request.image = images[3];
    request.subresourceRange = (VkImageSubresourceRange) {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0u, .levelCount = 1u, .baseArrayLayer = 0u, .layerCount = 1u
    };

    xud_check(xud_layout_barrier_fill(NULL, tracker, images[3], &request) == XUD_INVALID_ARG, "layout/fill-null-out",
        "a filled row with nowhere to go is refused");
    xud_check(xud_layout_barrier_fill(&filled, tracker, images[3], NULL) == XUD_INVALID_ARG, "layout/fill-null-request",
        "and a request that does not exist");
    xud_check(xud_layout_barrier_fill(&filled, tracker, VK_NULL_HANDLE, &request) == XUD_INVALID_ARG, "layout/fill-null-image",
        "and a barrier about the sentinel");
    xud_check(xud_layout_barrier_fill(&filled, tracker, images[3], &filled) == XUD_INVALID_ARG, "layout/fill-aliased",
        "out aliased to request would read a row half-overwritten, and is refused");
    filled.oldLayout = VK_IMAGE_LAYOUT_MAX_ENUM;
    xud_check(xud_layout_barrier_fill(&filled, tracker, (VkImage)&layout, &request) == XUD_NOT_FOUND, "layout/fill-untracked",
        "the filler refuses to guess a layout rather than naming a wrong one");
    xud_check(filled.oldLayout == VK_IMAGE_LAYOUT_MAX_ENUM, "layout/fill-rejected-untouched", "a refused fill writes nothing");

    request.newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    xud_check(xud_layout_barrier_fill(&filled, tracker, images[3], &request) == XUD_INVALID_ARG, "layout/fill-undefined-destination",
        "a transition INTO UNDEFINED has no defined meaning and is refused here, not at the driver");
    request.newLayout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;

    xud_check(xud_layout_barrier_fill(&filled, tracker, images[3], &request) == XUD_OK, "layout/fill-ok", "the honest pair assembles");
    xud_check(filled.oldLayout == (VkImageLayout)(VK_IMAGE_LAYOUT_GENERAL + 3u), "layout/fill-old-is-tracked",
        "oldLayout is the tracker's answer, not the request's lie");
    xud_check(filled.newLayout == VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL, "layout/fill-new-is-requested", "newLayout is the caller's ask");
    xud_check(filled.image == images[3], "layout/fill-image-kept", "and every other field travels untouched from the request");
    xud_check((filled.srcAccessMask == VK_ACCESS_TRANSFER_WRITE_BIT) && (filled.dstAccessMask == VK_ACCESS_SHADER_READ_BIT)
            && (filled.subresourceRange.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT),
        "layout/fill-fields-kept", "accesses and range included, the two fields the row does not carry being the stage masks");
    xud_check(request.oldLayout == VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL, "layout/fill-request-untouched",
        "the request itself is not consumed by the fill");
    xud_check(xud_layout_get(&layout, tracker, images[3]) == XUD_OK, "layout/fill-not-recorded", "and a fill does not move the record");
    xud_check(layout == (VkImageLayout)(VK_IMAGE_LAYOUT_GENERAL + 3u), "layout/fill-did-not-move-state", "the image stays where it was");

    /* ---- the recording row's guards, with no command buffer anywhere ---- */
    xud_check(
        xud_layout_barrier(NULL, NULL, VK_NULL_HANDLE, &request, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)
            == XUD_INVALID_ARG,
        "layout/barrier-null-tracker", "the guards fire before any row is read");
    xud_check(
        xud_layout_barrier(tracker, NULL, VK_NULL_HANDLE, &request, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)
            == XUD_INVALID_ARG,
        "layout/barrier-null-loader", "including the loader's");
    result = xud_loader_open(&loader);
    if (result == XUD_OK) {
        xud_check(xud_layout_barrier(
                      tracker, loader, VK_NULL_HANDLE, &request, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)
                == XUD_INVALID_ARG,
            "layout/barrier-null-cmd", "a command buffer that does not exist is refused beside them");
        xud_check(xud_layout_barrier(tracker, loader, (VkCommandBuffer)&imageStorage[0], &request, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)
                == XUD_UNSUPPORTED,
            "layout/barrier-unbound-table",
            "and an open loader whose DEVICE table was never bound answers UNSUPPORTED, not a call "
            "through a NULL row");
        (void)xud_loader_close(&loader);
    }

    /* ---- teardown ---- */
    xud_check(xud_layout_tracker_close(&tracker) == XUD_OK, "layout/close-ok", "the map frees with no driver call at all");
    xud_check(tracker == NULL, "layout/close-clears", "and the out-param is cleared");
    xud_check(xud_layout_tracker_close(&tracker) == XUD_ALREADY, "layout/close-twice", "a second close is the idempotent answer");
}

void xud_test_layout(void) { xud_test_layout_pure(); }
