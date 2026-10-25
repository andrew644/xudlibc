/***********************************************************************************************
 *
 *   xud_test_swapchain.c - the swapchain layer: images, formats, present modes, recreate on resize
 *
 *   Owns : the three surface queries' bounds, the image-count/format/present-mode selection rules, the create-info assembly, the image
 * array's accessor, the recreate that retires the old handle, the guard before the destroy - and the GPU half that creates, recreates at a
 * second extent and destroys a real VkSwapchainKHR. Layer: tests. Sees xud_swapchain.h and the rungs it borrows from: the surface (the
 * window owns it), the physical device (the three surface queries are instance-table rows) and the device (the swapchain names are
 * device-table rows).
 *
 *   Deterministic: fixed loop counts, no time, no randomness, no filesystem.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud.h" /* Required for: XudResult, XUD_VERSION, the XUD_GPU_TESTS gate */
#include "xud_device.h" /* Required for: XudDevice, the handle every swapchain row is asked of */
#include "xud_instance.h" /* Required for: XudInstance, the instance the surface queries are rows of */
#include "xud_loader.h" /* Required for: XudLoader, the two bound-handle rules and both tables */
#include "xud_physical.h" /* Required for: XudPhysicalDevice, the handle the three surface queries are asked of */
#include "xud_swapchain.h" /* Required for: XudSwapchain, XudSurfaceCaps, the three selection rules, the fill rung, the recreate and the close */
#include "xud_test_harness.h" /* Required for: xud_check, xud_gpu_tests_enabled, the area entry points */
#include "xud_window.h" /* Required for: XudWindow, the surface a swapchain is created for */
#include <stdlib.h> /* Required for: calloc and free, the synthetic records this area builds and frees */
#include <string.h> /* Required for: memset, the synthetic rows this area zeroes and compares */

/*-----------------------------------------------------------------------------------------------
 * The swapchain layer's pure half: the image-count rule, the format and present-mode selection, t
 * he create-info assembly, the image accessor and the argument guards - all of it against a synth
 * etic XudSurfaceCaps and with no driver call, which is what makes the headless run cover this ru
 * ng at all. The numbers below are the shape of the rule, not this machine's answers: the caps thi
 * s machine's surface actually reports (minImageCount 4, maxImageCount 0, an extent of 1x1 to 1638
 * 4x16384, usage flags that carry COLOR_ATTACHMENT, composite alpha OPAQUE|PRE_MULTIPLIED, IDENTIT
 * Y transform) are xud_test_gpu_swapchain()'s to assert against the driver, and the checks here ar
 * e the rule's own halves - a surface that would refuse each of them, and one that would accept i
 * t. The loader guards below are the check-before-you-use rule read at this rung: a swapchain row
 * that is nil, or a loader whose rows are not this device's, is XUD_UNSUPPORTED rather than a cal
 * l through NULL.
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_swapchain_pure(void) {
    XudSurfaceCaps caps;
    XudSurfaceCaps queried;
    XudSwapchain* swapchain = NULL;
    XudSwapchain* synthetic = NULL;
    XudLoader* openLoader = NULL;
    XudWindow* window = NULL;
    XudDevice* device = NULL;
    XudPhysicalDevice* physical = NULL;
    VkSwapchainCreateInfoKHR info;
    VkSurfaceFormatKHR formatRow;
    VkPresentModeKHR modeRow;
    VkImage image = VK_NULL_HANDLE;
    const VkSurfaceKHR fakeSurface = (VkSurfaceKHR)(uintptr_t)0x2000u;
    uint32_t count = 0u;
    uint32_t chosen = 0u;

    memset(&caps, 0, sizeof caps);
    /* queried is the caps the refusal rungs below are asked to leave at their zero state, so the
     * zero state has to be a fact here rather than whatever the stack held (gcc's stack happened to
     * be zero, clang's is not).
     */
    memset(&queried, 0, sizeof queried);
    caps.capabilities.minImageCount = 2u;
    caps.capabilities.maxImageCount = 0u; /* 0 is the spec's "no upper limit" answer */
    caps.capabilities.currentExtent.width = 0xFFFFFFFFu;
    caps.capabilities.currentExtent.height = 0xFFFFFFFFu;
    caps.capabilities.minImageExtent.width = 1u;
    caps.capabilities.minImageExtent.height = 1u;
    caps.capabilities.maxImageExtent.width = 16384u;
    caps.capabilities.maxImageExtent.height = 16384u;
    caps.capabilities.maxImageArrayLayers = 1u;
    caps.capabilities.supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    caps.capabilities.currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    caps.capabilities.supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR | VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    caps.capabilities.supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    /* The image-count rule: never below the surface's minImageCount, never above its maxImageCount
       when it states one, never below this layer's own floor. A surface whose answer is not a surf
       ace a swapchain can exist on is refused rather than clamped into a count the surface never a
       llowed.
     */
    xud_check(xud_swapchain_choose_image_count(NULL, &caps.capabilities) == XUD_INVALID_ARG, "swapchain/count-null-out",
        "no out-param to write into");
    xud_check(xud_swapchain_choose_image_count(&count, NULL) == XUD_INVALID_ARG, "swapchain/count-null-caps", "no surface to count for");
    caps.capabilities.minImageCount = 0u;
    xud_check(xud_swapchain_choose_image_count(&count, &caps.capabilities) == XUD_UNSUPPORTED, "swapchain/count-zero-min",
        "a surface that advertises no image is not a surface");
    caps.capabilities.minImageCount = 1u;
    xud_check(xud_swapchain_choose_image_count(&count, &caps.capabilities) == XUD_UNSUPPORTED, "swapchain/count-below-floor",
        "one image is below the floor this layer asks for");
    caps.capabilities.minImageCount = 2u;
    caps.capabilities.maxImageCount = 1u;
    xud_check(xud_swapchain_choose_image_count(&count, &caps.capabilities) == XUD_UNSUPPORTED, "swapchain/count-inconsistent",
        "an upper limit below the lower one is no surface at all");
    caps.capabilities.maxImageCount = 2u;
    xud_check(xud_swapchain_choose_image_count(&count, &caps.capabilities) == XUD_OK, "swapchain/count-at-max",
        "an upper limit at the preferred count is honoured");
    xud_check(count == 2u, "swapchain/count-at-max-value", "and the count is that limit");
    caps.capabilities.maxImageCount = 3u;
    xud_check(xud_swapchain_choose_image_count(&count, &caps.capabilities) == XUD_OK, "swapchain/count-cut",
        "an upper limit above the preferred count leaves the preferred one");
    xud_check(count == 2u, "swapchain/count-cut-value", "the limit is an upper bound, not a request");
    caps.capabilities.minImageCount = 4u;
    caps.capabilities.maxImageCount = 0u;
    xud_check(xud_swapchain_choose_image_count(&count, &caps.capabilities) == XUD_OK, "swapchain/count-raised",
        "no upper limit, so the count is raised to minImageCount");
    xud_check(count == 4u, "swapchain/count-raised-value", "which is this machine's case");
    caps.capabilities.minImageCount = 6u;
    caps.capabilities.maxImageCount = 12u;
    xud_check(xud_swapchain_choose_image_count(&count, &caps.capabilities) == XUD_OK, "swapchain/count-raised-cut",
        "raised and cut in the same surface");
    xud_check(count == 6u, "swapchain/count-raised-cut-value", "the lower bound wins over the floor");
    caps.capabilities.minImageCount = 2u;
    caps.capabilities.maxImageCount = 0u;

    /* The format rule: a wanted (format, colorSpace) pair in the advertised list beats driver ord
       er, an advertised SRGB row beats a PASS_THROUGH row of the same format, and nothing else is
       picked. VK_COLOR_SPACE_PASS_THROUGH_EXT is only ever a fallback, because VK_EXT_swapchain_col
       orspace is not enabled on this library's instance.
     */
    xud_check(xud_swapchain_select_format(NULL, &chosen, caps.surfaceFormats, caps.surfaceFormatCount) == XUD_INVALID_ARG,
        "swapchain/format-null-out", "no out-param to write into");
    xud_check(xud_swapchain_select_format(&formatRow, NULL, caps.surfaceFormats, caps.surfaceFormatCount) == XUD_INVALID_ARG,
        "swapchain/format-null-index", "no row index to record the pick by");
    caps.surfaceFormatCount = 0u;
    xud_check(xud_swapchain_select_format(&formatRow, &chosen, caps.surfaceFormats, caps.surfaceFormatCount) == XUD_UNSUPPORTED,
        "swapchain/format-none-advertised", "a surface that advertises no format has no swapchain");
    caps.surfaceFormatCount = 2u;
    caps.surfaceFormats[0].format = VK_FORMAT_R5G6B5_UNORM_PACK16;
    caps.surfaceFormats[0].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    caps.surfaceFormats[1].format = VK_FORMAT_B8G8R8A8_SRGB;
    caps.surfaceFormats[1].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    xud_check(xud_swapchain_select_format(&formatRow, &chosen, caps.surfaceFormats, caps.surfaceFormatCount) == XUD_OK,
        "swapchain/format-wanted", "a wanted pair is picked");
    xud_check(chosen == 1u, "swapchain/format-wanted-index", "in wanted order, not driver order");
    xud_check(formatRow.format == VK_FORMAT_B8G8R8A8_SRGB, "swapchain/format-wanted-value", "and the row written out is that row, whole");
    caps.surfaceFormats[1].format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    xud_check(xud_swapchain_select_format(&formatRow, &chosen, caps.surfaceFormats, caps.surfaceFormatCount) == XUD_OK,
        "swapchain/format-first-srgb", "with no wanted pair, the first advertised SRGB row is");
    xud_check(chosen == 0u, "swapchain/format-first-srgb-index", "and it is that row");
    xud_check(formatRow.format == VK_FORMAT_R5G6B5_UNORM_PACK16, "swapchain/format-first-srgb-value",
        "a format outside the wanted list is still a usable swapchain");
    caps.surfaceFormats[0].colorSpace = VK_COLOR_SPACE_PASS_THROUGH_EXT;
    caps.surfaceFormats[1].colorSpace = VK_COLOR_SPACE_PASS_THROUGH_EXT;
    xud_check(xud_swapchain_select_format(&formatRow, &chosen, caps.surfaceFormats, caps.surfaceFormatCount) == XUD_OK,
        "swapchain/format-fallback", "with no SRGB row at all, the first advertised row is");
    xud_check(chosen == 0u, "swapchain/format-fallback-index", "and it is that row");
    xud_check(formatRow.colorSpace == VK_COLOR_SPACE_PASS_THROUGH_EXT, "swapchain/format-fallback-space",
        "and the color space carried is the surface's own answer, not one this layer hoped for");
    caps.surfaceFormats[0].format = VK_FORMAT_B8G8R8A8_SRGB;
    caps.surfaceFormats[1].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    xud_check(xud_swapchain_select_format(&formatRow, &chosen, caps.surfaceFormats, caps.surfaceFormatCount) == XUD_OK,
        "swapchain/format-through", "a wanted format offered only in PASS_THROUGH color space loses to an advertised SRGB row");
    xud_check(chosen == 1u, "swapchain/format-through-index", "because the color space is the pair");
    xud_check(formatRow.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32, "swapchain/format-through-value",
        "and the row chosen is that SRGB one, not the wanted format's PASS_THROUGH row");
    xud_check(formatRow.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, "swapchain/format-through-space",
        "and the color space handed over is that row's, so the pair stays a pair");
    caps.surfaceFormats[0].format = VK_FORMAT_R5G6B5_UNORM_PACK16;
    caps.surfaceFormats[0].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    caps.surfaceFormats[1].format = VK_FORMAT_B8G8R8A8_SRGB;
    caps.surfaceFormats[1].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;

    /* The present-mode rule: MAILBOX when advertised, else FIFO (the one mode the spec guarantees
       every implementation advertises), else the first advertised. Modes are searched by VALUE - t
       he enum is not contiguous, so an index into this array is nothing at all.
     */
    xud_check(xud_swapchain_select_present_mode(NULL, caps.presentModes, caps.presentModeCount) == XUD_INVALID_ARG,
        "swapchain/mode-null-out", "no out-param to write into");
    caps.presentModeCount = 0u;
    xud_check(xud_swapchain_select_present_mode(&modeRow, caps.presentModes, caps.presentModeCount) == XUD_UNSUPPORTED,
        "swapchain/mode-none-advertised", "a surface that advertises no mode has no presentation");
    caps.presentModeCount = 2u;
    caps.presentModes[0] = VK_PRESENT_MODE_FIFO_KHR;
    caps.presentModes[1] = VK_PRESENT_MODE_MAILBOX_KHR;
    xud_check(xud_swapchain_select_present_mode(&modeRow, caps.presentModes, caps.presentModeCount) == XUD_OK, "swapchain/mode-mailbox",
        "MAILBOX is preferred over the vsync'd row that comes first in the list");
    xud_check(modeRow == VK_PRESENT_MODE_MAILBOX_KHR, "swapchain/mode-mailbox-value", "and the mode chosen is that one, by value");
    caps.presentModes[1] = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    xud_check(xud_swapchain_select_present_mode(&modeRow, caps.presentModes, caps.presentModeCount) == XUD_OK, "swapchain/mode-fifo",
        "without MAILBOX, FIFO is the fallback");
    xud_check(modeRow == VK_PRESENT_MODE_FIFO_KHR, "swapchain/mode-fifo-value", "as advertised");
    caps.presentModes[0] = VK_PRESENT_MODE_IMMEDIATE_KHR;
    xud_check(xud_swapchain_select_present_mode(&modeRow, caps.presentModes, caps.presentModeCount) == XUD_OK, "swapchain/mode-first",
        "without either preferred mode, the first advertised one is");
    xud_check(modeRow == VK_PRESENT_MODE_IMMEDIATE_KHR, "swapchain/mode-first-value",
        "IMMEDIATE is a present mode, not an absence of one, so it is not refused");
    caps.presentModeCount = 1u;
    caps.presentModes[0] = VK_PRESENT_MODE_MAILBOX_KHR;
    xud_check(xud_swapchain_select_present_mode(&modeRow, caps.presentModes, caps.presentModeCount) == XUD_OK, "swapchain/mode-only",
        "a single advertised mode is chosen even when it is a preferred one");
    xud_check(modeRow == VK_PRESENT_MODE_MAILBOX_KHR, "swapchain/mode-only-value", "and it is it");
    caps.presentModeCount = 2u;
    caps.presentModes[0] = VK_PRESENT_MODE_FIFO_KHR;
    caps.presentModes[1] = VK_PRESENT_MODE_MAILBOX_KHR;

    /* The create-info assembly: every field comes from the queried caps or from the extent asked
       for, and every refusal is a surface this layer cannot make a swapchain for rather than a cr
       eate-info the driver would have to be trusted to reject.
     */
    xud_check(xud_swapchain_fill_create_info(NULL, &caps, fakeSurface, 320u, 240u, VK_NULL_HANDLE) == XUD_INVALID_ARG,
        "swapchain/info-null-out", "no out-param to write into");
    xud_check(xud_swapchain_fill_create_info(&info, NULL, fakeSurface, 320u, 240u, VK_NULL_HANDLE) == XUD_INVALID_ARG,
        "swapchain/info-null-caps", "no queried caps to assemble from");
    xud_check(xud_swapchain_fill_create_info(&info, &caps, VK_NULL_HANDLE, 320u, 240u, VK_NULL_HANDLE) == XUD_INVALID_ARG,
        "swapchain/info-null-surface", "a swapchain needs the surface it is created for");
    xud_check(xud_swapchain_fill_create_info(&info, &caps, fakeSurface, 0u, 240u, VK_NULL_HANDLE) == XUD_INVALID_ARG,
        "swapchain/info-zero-extent", "a zero extent is a bad argument, not a surface refusal");
    xud_check(xud_swapchain_fill_create_info(&info, &caps, fakeSurface, 20000u, 240u, VK_NULL_HANDLE) == XUD_UNSUPPORTED,
        "swapchain/info-extent-too-large", "outside the surface's own maxImageExtent");
    caps.capabilities.minImageExtent.width = 16u;
    caps.capabilities.minImageExtent.height = 16u;
    xud_check(xud_swapchain_fill_create_info(&info, &caps, fakeSurface, 8u, 240u, VK_NULL_HANDLE) == XUD_UNSUPPORTED,
        "swapchain/info-extent-too-small", "and outside its own minImageExtent");
    caps.capabilities.minImageExtent.width = 1u;
    caps.capabilities.minImageExtent.height = 1u;
    caps.capabilities.maxImageArrayLayers = 0u;
    xud_check(xud_swapchain_fill_create_info(&info, &caps, fakeSurface, 320u, 240u, VK_NULL_HANDLE) == XUD_UNSUPPORTED,
        "swapchain/info-no-layers", "a surface that takes no layer takes no swapchain");
    caps.capabilities.maxImageArrayLayers = 1u;
    caps.capabilities.supportedUsageFlags = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    xud_check(xud_swapchain_fill_create_info(&info, &caps, fakeSurface, 320u, 240u, VK_NULL_HANDLE) == XUD_UNSUPPORTED,
        "swapchain/info-no-usage", "a surface that cannot be a colour attachment has no swapchain");
    caps.capabilities.supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    caps.capabilities.supportedCompositeAlpha = 0u;
    xud_check(xud_swapchain_fill_create_info(&info, &caps, fakeSurface, 320u, 240u, VK_NULL_HANDLE) == XUD_UNSUPPORTED,
        "swapchain/info-no-alpha", "a surface that advertises no composite alpha has no swapchain");
    caps.capabilities.supportedCompositeAlpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    xud_check(xud_swapchain_fill_create_info(&info, &caps, fakeSurface, 320u, 240u, VK_NULL_HANDLE) == XUD_OK, "swapchain/info-assembles",
        "a surface that can carry the swapchain gets a create-info");
    xud_check(info.sType == VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, "swapchain/info-stamped",
        "the create-info is stamped with the EXTENSION-level stamp, not a core one");
    xud_check(info.pNext == NULL, "swapchain/info-pnext-empty", "and nothing is chained onto it");
    xud_check(info.flags == 0u, "swapchain/info-flags-zero", "and no swapchain create flag is asked for");
    xud_check(info.surface == fakeSurface, "swapchain/info-surface", "the surface is the handle in hand, copied rather than re-derived");
    xud_check(info.minImageCount == 2u, "swapchain/info-count", "the image-count rule's answer");
    xud_check(info.imageFormat == VK_FORMAT_B8G8R8A8_SRGB, "swapchain/info-format", "the format rule's answer");
    xud_check(info.imageColorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, "swapchain/info-colorspace",
        "the color space of the row that was picked, not one this layer hoped for");
    xud_check((info.imageExtent.width == 320u) && (info.imageExtent.height == 240u), "swapchain/info-extent",
        "the extent asked for, not the surface's currentExtent");
    xud_check(info.imageArrayLayers == 1u, "swapchain/info-layers", "one layer, always here");
    xud_check(info.imageUsage == VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, "swapchain/info-usage",
        "the one usage this library needs, and it was checked to be advertised");
    xud_check(info.imageSharingMode == VK_SHARING_MODE_EXCLUSIVE, "swapchain/info-sharing",
        "EXCLUSIVE: CONCURRENT is for an image another process may also be writing to");
    xud_check((info.queueFamilyIndexCount == 0u) && (info.pQueueFamilyIndices == NULL), "swapchain/info-no-family",
        "the spec's own 'the application does not care' form");
    xud_check(info.preTransform == VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, "swapchain/info-transform",
        "the surface's current transform, never one the surface may not support");
    xud_check(info.compositeAlpha == VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, "swapchain/info-alpha",
        "the first bit the surface supports, which is PRE_MULTIPLIED once OPAQUE is not "
        "advertised");
    xud_check(info.presentMode == VK_PRESENT_MODE_MAILBOX_KHR, "swapchain/info-presentmode", "the present-mode rule's answer");
    xud_check(info.clipped == VK_TRUE, "swapchain/info-clipped",
        "clipped, because the surface's currentExtent is the undefined answer, not an extent");
    xud_check(info.oldSwapchain == VK_NULL_HANDLE, "swapchain/info-no-old", "a fresh create");
    xud_check(xud_swapchain_fill_create_info(&info, &caps, fakeSurface, 320u, 240u, (VkSwapchainKHR)(uintptr_t)7u) == XUD_OK,
        "swapchain/info-old-recorded", "a recreate create-info names the swapchain it replaces");
    xud_check(info.oldSwapchain == (VkSwapchainKHR)(uintptr_t)7u, "swapchain/info-old-value",
        "and the handle is carried through to the driver, which is what retires the old one");

    /* The accessor, pure: a struct with no swapchain in it has no image array in any form, and an
       index at or above the count is a caught error, not an out-of-range read.
     */
    xud_check(
        xud_swapchain_images_get(NULL, 0u, &image) == XUD_INVALID_ARG, "swapchain/get-null-struct", "no struct to read an image out of");
    synthetic = calloc(1u, sizeof(XudSwapchain));
    xud_check(synthetic != NULL, "swapchain/synthetic-alloc", "the synthetic struct must allocate");
    if (synthetic != NULL) {
        xud_check(xud_swapchain_images_get(synthetic, 0u, &image) == XUD_UNSUPPORTED, "swapchain/get-handleless",
            "a struct with no swapchain has no image to hand out");
        xud_check(image == VK_NULL_HANDLE, "swapchain/get-handleless-clears", "and the out-param is left alone");
        synthetic->imageCount = 2u;
        synthetic->images = calloc(synthetic->imageCount, sizeof(VkImage));
        xud_check(synthetic->images != NULL, "swapchain/synthetic-images-alloc", "the synthetic image array must allocate");
        if (synthetic->images != NULL) {
            synthetic->images[0] = (VkImage)(uintptr_t)0x100u;
            synthetic->images[1] = (VkImage)(uintptr_t)0x101u;
            synthetic->swapchain = (VkSwapchainKHR)(uintptr_t)0x1234u;
            xud_check(xud_swapchain_images_get(synthetic, 0u, &image) == XUD_OK, "swapchain/get-first", "the image at index zero");
            xud_check(image == (VkImage)(uintptr_t)0x100u, "swapchain/get-first-value", "is the handle the array holds");
            xud_check(xud_swapchain_images_get(synthetic, 1u, &image) == XUD_OK, "swapchain/get-second", "and the one at index one");
            xud_check(image == (VkImage)(uintptr_t)0x101u, "swapchain/get-second-value", "is the next handle, not the first one again");
            xud_check(xud_swapchain_images_get(synthetic, 2u, &image) == XUD_INVALID_ARG, "swapchain/get-stale-index",
                "an index at the count is caught, not read");
            xud_check(xud_swapchain_images_get(synthetic, 99u, &image) == XUD_INVALID_ARG, "swapchain/get-far-index",
                "and so is one far above it");
            xud_check(image == (VkImage)(uintptr_t)0x101u, "swapchain/get-refusal-keeps-out",
                "a refused read does not write the out-param at all");
        }
    }

    if (xud_loader_open(&openLoader) == XUD_OK) {
        window = calloc(1u, sizeof(XudWindow));
        device = calloc(1u, sizeof(XudDevice));
        physical = calloc(1u, sizeof(XudPhysicalDevice));
        xud_check(
            (window != NULL) && (device != NULL) && (physical != NULL), "swapchain/synthetic-rungs", "the synthetic rungs must allocate");
        if ((window != NULL) && (device != NULL) && (physical != NULL)) {
            window->surface = fakeSurface;
            window->surfaceInstance = (VkInstance)(uintptr_t)0x2100u;
            window->width = 320u;
            window->height = 240u;
            device->device = (VkDevice)(uintptr_t)0x2200u;
            physical->physicalDevice = (VkPhysicalDevice)(uintptr_t)0x2300u;

            /* The queries, refused here rather than called: the surface rows are instance-level, s
               o a loader not bound to the surface's instance has no rows to ask through.
             */
            xud_check(xud_swapchain_query_surface(NULL, openLoader, physical->physicalDevice, window->surface) == XUD_INVALID_ARG,
                "swapchain/query-null-out", "no caps to write into");
            xud_check(xud_swapchain_query_surface(&queried, NULL, physical->physicalDevice, window->surface) == XUD_INVALID_ARG,
                "swapchain/query-null-loader", "no table to call through");
            xud_check(xud_swapchain_query_surface(&queried, openLoader, VK_NULL_HANDLE, window->surface) == XUD_INVALID_ARG,
                "swapchain/query-null-physical", "no device to query about");
            xud_check(xud_swapchain_query_surface(&queried, openLoader, physical->physicalDevice, VK_NULL_HANDLE) == XUD_INVALID_ARG,
                "swapchain/query-null-surface", "no surface to query");
            xud_check(xud_swapchain_query_surface(&queried, openLoader, physical->physicalDevice, window->surface) == XUD_UNSUPPORTED,
                "swapchain/query-rows-unbound",
                "the surface rows are instance-level: a loader not bound to the surface's instance "
                "has no rows to ask through, and nothing is called");
            xud_check(queried.surfaceFormatCount == 0u, "swapchain/query-refusal-clears-caps",
                "and a refused query leaves the caps at their zero state, not half-filled");

            xud_check(xud_swapchain_open(&swapchain, NULL, physical, window, device) == XUD_INVALID_ARG, "swapchain/open-null-loader",
                "no table to call through");
            xud_check(xud_swapchain_open(&swapchain, openLoader, NULL, window, device) == XUD_INVALID_ARG, "swapchain/open-null-physical",
                "no surface to query");
            xud_check(xud_swapchain_open(&swapchain, openLoader, physical, NULL, device) == XUD_INVALID_ARG, "swapchain/open-null-window",
                "no surface to create for");
            xud_check(xud_swapchain_open(&swapchain, openLoader, physical, window, NULL) == XUD_INVALID_ARG, "swapchain/open-null-device",
                "no device to create on");
            xud_check(xud_swapchain_open(&swapchain, openLoader, physical, window, device) == XUD_UNSUPPORTED, "swapchain/open-not-bound",
                "the loader's rows are not this window's instance's and not this device's, so the "
                "rung refuses rather than calling through a nil row");
            xud_check(swapchain == NULL, "swapchain/open-refusal-null", "and a refused create hands back nothing");

            if (synthetic != NULL) {
                synthetic->createInfo.imageExtent.width = 64u;
                synthetic->createInfo.imageExtent.height = 64u;
                xud_check(xud_swapchain_recreate(&synthetic, openLoader, physical, window, device, 64u, 64u) == XUD_ALREADY,
                    "swapchain/recreate-same-extent", "the same extent is not a resize: nothing is retired and nothing is called");
                xud_check(synthetic->swapchain != VK_NULL_HANDLE, "swapchain/recreate-already-keeps",
                    "and the swapchain in hand is still the one it was");
                xud_check(xud_swapchain_recreate(&synthetic, openLoader, physical, window, device, 32u, 32u) == XUD_UNSUPPORTED,
                    "swapchain/recreate-not-bound", "a resize this layer cannot call through is reported, not attempted");
                xud_check(synthetic->swapchain != VK_NULL_HANDLE, "swapchain/recreate-refusal-keeps",
                    "a refused resize must not destroy the swapchain it replaces, and did not");
                xud_check(synthetic->createInfo.imageExtent.width == 64u, "swapchain/recreate-refusal-keeps-record",
                    "and the record still describes the extent that swapchain exists for");
                xud_check(xud_swapchain_recreate(&synthetic, openLoader, physical, window, device, 0u, 0u) == XUD_INVALID_ARG,
                    "swapchain/recreate-zero-extent", "a zero extent is a bad argument");
                xud_check(xud_swapchain_recreate(&synthetic, openLoader, physical, window, device, 64u, 0u) == XUD_INVALID_ARG,
                    "swapchain/recreate-zero-side", "and so is a zero on one side only");

                xud_check(xud_swapchain_close(&synthetic, device, openLoader) == XUD_UNSUPPORTED, "swapchain/close-not-bound",
                    "a swapchain this layer cannot destroy is not destroyed, and not freed either");
                xud_check(synthetic != NULL, "swapchain/close-refusal-keeps-struct",
                    "freeing it would have leaked the handle silently, so the struct stays");

                xud_check(xud_swapchain_close(&swapchain, device, openLoader) == XUD_ALREADY, "swapchain/close-nothing",
                    "a null-in-hand slot is already closed");
                swapchain = synthetic;
                xud_check(xud_swapchain_close(&swapchain, device, openLoader) == XUD_UNSUPPORTED, "swapchain/close-alias-refusal",
                    "the same refusal through a second pointer to the same struct");
                xud_check(synthetic != NULL, "swapchain/close-alias-keeps", "and the struct is still alive behind both of them");
                swapchain = NULL;
                xud_check(xud_swapchain_close(&swapchain, NULL, openLoader) == XUD_INVALID_ARG, "swapchain/close-null-device",
                    "the guard needs the device record");
                xud_check(xud_swapchain_close(&swapchain, device, NULL) == XUD_INVALID_ARG, "swapchain/close-null-loader",
                    "and it needs the loader");
                xud_check(synthetic != NULL, "swapchain/close-arg-refusal-keeps", "a rejected close leaves the struct alone");
            }
        }
        if ((synthetic != NULL) && (device != NULL)) {
            synthetic->swapchain = VK_NULL_HANDLE;
            xud_check(xud_swapchain_close(&synthetic, device, openLoader) == XUD_OK, "swapchain/close-handleless",
                "a struct that owns no swapchain frees without a call");
            xud_check(synthetic == NULL, "swapchain/close-handleless-null", "and the slot cleared");
            xud_check(xud_swapchain_close(&synthetic, device, openLoader) == XUD_ALREADY, "swapchain/close-twice",
                "a second teardown is reported, not a double free");
        }
        if (window != NULL)
            free(window);
        if (device != NULL)
            free(device);
        if (physical != NULL)
            free(physical);
        xud_check(xud_loader_close(&openLoader) == XUD_OK, "swapchain/loader-close", "the loader closes");
        xud_check(openLoader == NULL, "swapchain/loader-null", "and cleared");
    }

    if (synthetic != NULL) {
        if (synthetic->images != NULL)
            free(synthetic->images);
        free(synthetic);
    }
}

/*-----------------------------------------------------------------------------------------------
 * The swapchain layer against the driver: a real surface (a real GLFW window on this machine's Wa
 * yland session) and a real VkDevice, and a real VkSwapchainKHR created for that surface on that
 * device - then recreated at a new extent and destroyed, with the old swapchain never leaked. The
 * three surface queries are instance-level rows and the swapchain names are device-level ones, so
 * the loader's bound state has to name the surface's instance and this device before any of them
 * is called, which is what the checks below read. The teardown order is the one xud_close_context
 * () uses: swapchain before device, device before surface, surface before window, window before i
 * nstance. The recreate path is the load-bearing one: it is the only place in this tree a swapcha
 * in is replaced, and the old handle has to die there, not be leaked.
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_gpu_swapchain(void) {
    XudLoader* loader = NULL;
    XudInstance* instance = NULL;
    XudPhysicalDevice* physical = NULL;
    XudWindow* window = NULL;
    XudDevice* device = NULL;
    XudSwapchain* swapchain = NULL;
    const char* required[XUD_INSTANCE_EXTRA_MAX] = { NULL };
    uint32_t requiredCount = 0u;
    bool supports = false;
    XudResult result;
    VkImage image = VK_NULL_HANDLE;
    VkSwapchainKHR firstHandle = VK_NULL_HANDLE;

    result = xud_loader_open(&loader);
    xud_check(result == XUD_OK, "gpu-swapchain/loader-open", "the GPU run needs an open loader");
    if (loader == NULL)
        return;

    result = xud_window_open(&window, 320u, 240u);
    xud_check(result == XUD_OK, "gpu-swapchain/window-open", "the surface needs a real window");
    if (window == NULL) {
        (void)xud_loader_close(&loader);
        return;
    }

    xud_window_required_instance_extensions(window, required, &requiredCount);
    result = xud_instance_create(&instance, loader, NULL, required, requiredCount);
    xud_check(result == XUD_OK, "gpu-swapchain/instance-create", "the merged instance must create");
    if (instance == NULL) {
        (void)xud_window_close(&window);
        (void)xud_loader_close(&loader);
        return;
    }

    result = xud_physical_open(&physical, loader, instance->instance);
    xud_check(result == XUD_OK, "gpu-swapchain/physical-open", "the device is queried through it");
    if (physical == NULL) {
        (void)xud_window_close(&window);
        (void)xud_instance_destroy(&instance, loader, NULL);
        (void)xud_loader_close(&loader);
        return;
    }

    xud_check(xud_window_presentation_support(&supports, window, instance, physical) == XUD_OK, "gpu-swapchain/presentation-query",
        "the window system's answer runs for this triple");
    xud_check(supports == true, "gpu-swapchain/presentation-true", "and this machine's window system presents to this physical device");

    result = xud_window_surface_create(window, loader, instance);
    xud_check(result == XUD_OK, "gpu-swapchain/surface-create", "the window gets a surface");
    if (window->surface == VK_NULL_HANDLE) {
        (void)xud_physical_close(&physical);
        (void)xud_window_close(&window);
        (void)xud_instance_destroy(&instance, loader, NULL);
        (void)xud_loader_close(&loader);
        return;
    }

    result = xud_device_open(&device, loader, physical, instance->instance);
    xud_check(result == XUD_OK, "gpu-swapchain/device-open", "the swapchain is created ON a device");
    if (device == NULL) {
        (void)xud_window_surface_close(window, loader);
        (void)xud_physical_close(&physical);
        (void)xud_window_close(&window);
        (void)xud_instance_destroy(&instance, loader, NULL);
        (void)xud_loader_close(&loader);
        return;
    }

    /* The rows this layer calls, checked before the calls that need them: VK_KHR_swapchain is enab
       led on this instance (the device layer enabled it from the physical layer's advertised list)
       , so the device-level rows are filled, and VK_KHR_surface is what fills the instance-level o
       nes. A driver that does not answer them is reported here rather than called through.
     */
    xud_check(loader->deviceTable.vkCreateSwapchainKHR != NULL, "gpu-swapchain/create-row-live",
        "VK_KHR_swapchain is enabled here, so the create row is filled");
    xud_check(loader->deviceTable.vkDestroySwapchainKHR != NULL, "gpu-swapchain/destroy-row-live",
        "and so is the teardown row - a swapchain this layer cannot destroy it must not create");
    xud_check(loader->deviceTable.vkGetSwapchainImagesKHR != NULL, "gpu-swapchain/images-row-live", "and the image query row");
    xud_check(loader->instanceTable.vkGetPhysicalDeviceSurfaceCapabilitiesKHR != NULL, "gpu-swapchain/caps-row-live",
        "the surface queries are instance-level rows, and filled");
    xud_check(loader->instanceTable.vkGetPhysicalDeviceSurfaceFormatsKHR != NULL, "gpu-swapchain/formats-row-live",
        "and so are the two list-shaped ones");
    xud_check(loader->instanceTable.vkGetPhysicalDeviceSurfacePresentModesKHR != NULL, "gpu-swapchain/modes-row-live",
        "and the present-mode one");
    xud_check(xud_loader_instance_bound(loader, window->surfaceInstance), "gpu-swapchain/instance-bound",
        "the instance rows are the surface's instance's rows");
    xud_check(xud_loader_device_bound(loader, device->device), "gpu-swapchain/device-bound", "and the device rows are this device's rows");

    result = xud_swapchain_open(&swapchain, loader, physical, window, device);
    xud_check(result == XUD_OK, "gpu-swapchain/open", "the three queries, the three selection rules and the create all completed");
    if (swapchain == NULL) {
        (void)xud_device_close(&device, loader);
        (void)xud_window_surface_close(window, loader);
        (void)xud_physical_close(&physical);
        (void)xud_window_close(&window);
        (void)xud_instance_destroy(&instance, loader, NULL);
        (void)xud_loader_close(&loader);
        return;
    }

    xud_check(swapchain->swapchain != VK_NULL_HANDLE, "gpu-swapchain/handle-live", "the one Vulkan object this layer owns is live");
    xud_check(swapchain->surfaceCaps.capabilities.minImageCount >= 2u, "gpu-swapchain/min-image-floor",
        "the spec's own floor of 2 is what the query answered here, not a smaller number");
    xud_check(swapchain->imageCount >= swapchain->surfaceCaps.capabilities.minImageCount, "gpu-swapchain/image-count-respects-min",
        "the count asked for was never below the surface's own lower bound");
    xud_check(swapchain->imageCount >= 2u, "gpu-swapchain/image-count-two",
        "at least two images, which is what the plan's low-latency preference is about");
    xud_check(swapchain->imageCapacity >= swapchain->imageCount, "gpu-swapchain/capacity-covers",
        "the array allocated for the images covers every live row");
    xud_check(
        swapchain->images != NULL, "gpu-swapchain/images-array-live", "vkGetSwapchainImagesKHR wrote an array, and this layer owns it");
    xud_check(swapchain->surfaceCaps.surfaceFormatCount >= 1u, "gpu-swapchain/formats-advertised",
        "the surface advertised at least one (format, colorSpace) pair, so the list is never "
        "empty");
    xud_check(swapchain->surfaceCaps.presentModeCount >= 1u, "gpu-swapchain/modes-advertised", "and at least one present mode");
    xud_check(swapchain->createInfo.surface == window->surface, "gpu-swapchain/info-surface",
        "the create-info records the surface this swapchain was created for");
    xud_check((swapchain->createInfo.imageExtent.width == 320u) && (swapchain->createInfo.imageExtent.height == 240u),
        "gpu-swapchain/info-extent", "at the window's extent, not at the surface's currentExtent");
    xud_check(swapchain->createInfo.oldSwapchain == VK_NULL_HANDLE, "gpu-swapchain/info-no-old",
        "the retired-handle field is cleared in the record, so no handle to a destroyed object is "
        "kept here");
    xud_check(swapchain->createInfo.imageUsage == VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, "gpu-swapchain/info-usage",
        "the one usage this library needs");
    xud_check(swapchain->createInfo.clipped == VK_TRUE, "gpu-swapchain/info-clipped",
        "clipped, because this surface reports no current extent at all");
    xud_check(swapchain->createInfo.minImageCount == swapchain->surfaceCaps.capabilities.minImageCount, "gpu-swapchain/info-count-is-caps",
        "the count the create-info carries is the surface's answer, not a constant");

    /* The chosen pair and the chosen mode have to be rows the surface actually advertised - the ch
       eck-before-you-use rule read at the two lists this layer selects from.
     */
    {
        bool formatAdvertised = false;
        bool modeAdvertised = false;
        uint32_t row = 0u;
        for (row = 0u; row < swapchain->surfaceCaps.surfaceFormatCount; row++) {
            if ((swapchain->surfaceCaps.surfaceFormats[row].format == swapchain->createInfo.imageFormat)
                && (swapchain->surfaceCaps.surfaceFormats[row].colorSpace == swapchain->createInfo.imageColorSpace))
                formatAdvertised = true;
        }
        for (row = 0u; row < swapchain->surfaceCaps.presentModeCount; row++) {
            if (swapchain->surfaceCaps.presentModes[row] == swapchain->createInfo.presentMode)
                modeAdvertised = true;
        }
        xud_check(formatAdvertised, "gpu-swapchain/format-advertised",
            "the (format, colorSpace) pair the create-info carries is one the surface advertised");
        xud_check(modeAdvertised, "gpu-swapchain/mode-advertised", "and so is the present mode");
        xud_check(swapchain->createInfo.imageColorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, "gpu-swapchain/format-srgb",
            "machine-specific: this surface advertises SRGB rows, and one was picked over its "
            "PASS_THROUGH twins");
    }

    xud_check(xud_swapchain_images_get(swapchain, 0u, &image) == XUD_OK, "gpu-swapchain/first-image",
        "the renderer above binds an image by index, and index zero exists");
    xud_check(image != VK_NULL_HANDLE, "gpu-swapchain/first-image-live", "and the handle it hands out is a real one");
    xud_check(xud_swapchain_images_get(swapchain, swapchain->imageCount, &image) == XUD_INVALID_ARG, "gpu-swapchain/image-index-at-count",
        "an index at the count is caught, not read");
    xud_check(xud_swapchain_images_get(swapchain, swapchain->imageCount + 1u, &image) == XUD_INVALID_ARG,
        "gpu-swapchain/image-index-above-count", "and so is one above it");

    firstHandle = swapchain->swapchain;

    xud_check(xud_swapchain_recreate(&swapchain, loader, physical, window, device, 320u, 240u) == XUD_ALREADY,
        "gpu-swapchain/recreate-same-extent", "the extent the swapchain was created for is not a resize, and nothing was called");
    xud_check(swapchain->swapchain == firstHandle, "gpu-swapchain/recreate-same-keeps", "and the handle in hand is still the one it was");

    xud_check(xud_swapchain_recreate(&swapchain, loader, physical, window, device, 20000u, 20000u) == XUD_UNSUPPORTED,
        "gpu-swapchain/recreate-outside-extent", "outside this surface's own maxImageExtent is the surface's answer, not a bad argument");
    xud_check(swapchain->swapchain == firstHandle, "gpu-swapchain/refused-resize-keeps-old",
        "a refused creation retires nothing: the old swapchain is still alive and usable");
    xud_check(swapchain->createInfo.imageExtent.width == 320u, "gpu-swapchain/refused-resize-record",
        "and the record still describes the extent that swapchain exists for");
    xud_check(xud_swapchain_images_get(swapchain, 0u, &image) == XUD_OK, "gpu-swapchain/images-still-readable",
        "and its images are still readable after the refusal");

    result = xud_swapchain_recreate(&swapchain, loader, physical, window, device, 200u, 150u);
    xud_check(result == XUD_OK, "gpu-swapchain/recreate-resized",
        "the resize completed: the create carried oldSwapchain, the old handle was destroyed here");
    xud_check(swapchain->swapchain != firstHandle, "gpu-swapchain/handle-replaced",
        "the handle in hand is the new swapchain, not the one that was retired");
    xud_check((swapchain->createInfo.imageExtent.width == 200u) && (swapchain->createInfo.imageExtent.height == 150u),
        "gpu-swapchain/new-extent-recorded", "and the record describes the new extent");
    xud_check(swapchain->createInfo.oldSwapchain == VK_NULL_HANDLE, "gpu-swapchain/new-record-no-old",
        "with the retired handle cleared out of it, so the record names nothing that was "
        "destroyed");
    xud_check(swapchain->imageCount >= swapchain->surfaceCaps.capabilities.minImageCount, "gpu-swapchain/new-image-count-respects-min",
        "the new swapchain's images were queried again, against the new handle");
    xud_check(swapchain->images != NULL, "gpu-swapchain/new-images-array",
        "and the array was replaced with it, so nothing above may hold an image across a recreate");
    xud_check(
        xud_swapchain_images_get(swapchain, 0u, &image) == XUD_OK, "gpu-swapchain/new-first-image", "the accessor reads the new array");
    xud_check(image != VK_NULL_HANDLE, "gpu-swapchain/new-first-image-live", "and the handle is a live one");

    /* The teardown, in the order xud_close_context() uses: the swapchain goes before the device it
       is presented through, and its close runs that device's wait-idle guard first.
     */
    result = xud_swapchain_close(&swapchain, device, loader);
    xud_check(result == XUD_OK, "gpu-swapchain/close", "the guard, then vkDestroySwapchainKHR through the device row, then the free");
    xud_check(swapchain == NULL, "gpu-swapchain/close-clears", "and the slot is cleared");
    xud_check(xud_swapchain_close(&swapchain, device, loader) == XUD_ALREADY, "gpu-swapchain/close-twice",
        "a second teardown is reported, not a double free");

    xud_check(xud_device_close(&device, loader) == XUD_OK, "gpu-swapchain/device-close",
        "the device goes next, and nothing of the swapchain is still in flight against it");
    xud_check(device == NULL, "gpu-swapchain/device-null", "and cleared");
    xud_check(xud_window_surface_close(window, loader) == XUD_OK, "gpu-swapchain/surface-close",
        "the surface the swapchain was created for goes before the window");
    xud_check(window->surface == VK_NULL_HANDLE, "gpu-swapchain/surface-cleared", "and cleared");
    xud_check(xud_window_close(&window) == XUD_OK, "gpu-swapchain/window-close", "then the window");
    xud_check(window == NULL, "gpu-swapchain/window-null", "and cleared");
    xud_check(xud_physical_close(&physical) == XUD_OK, "gpu-swapchain/physical-close", "free-only");
    xud_check(physical == NULL, "gpu-swapchain/physical-null", "and cleared");
    xud_check(xud_instance_destroy(&instance, loader, NULL) == XUD_OK, "gpu-swapchain/instance-destroy",
        "the instance the surface was created for goes after the surface went");
    xud_check(instance == NULL, "gpu-swapchain/instance-null", "and cleared");
    xud_check(xud_loader_close(&loader) == XUD_OK, "gpu-swapchain/loader-close", "the loader closes");
    xud_check(loader == NULL, "gpu-swapchain/loader-null", "and cleared");
}

void xud_test_swapchain(void) {
    xud_test_swapchain_pure();
    if (xud_gpu_tests_enabled()) {
        xud_test_gpu_swapchain();
    }
}
