/* xud_swapchain.c - the swapchain layer: the three surface queries, the selection rules,
 *   create-info assembly, bring-up, recreate-on-resize, teardown.
 *
 *   Implements: the contract of xud_swapchain.h - see that header for what this layer owns
 *   (one VkSwapchainKHR plus the images vkGetSwapchainImagesKHR handed it), for the facts its
 *   shape rests on, and for what every function below answers.
 *   Layer: above the device, below the renderer. Every Vulkan call here goes through a row of
 *   one of the loader's two Volk tables: the three surface queries are instance-level rows (they
 *   take a VkPhysicalDevice) and the swapchain names are device-level rows (they take a VkDevice),
 *   so the loader's bound state must name the surface's instance and this device before any of
 *   them is called - see xud_loader.h on why a table's rows are per-object state and not the
 *   process-wide state Volk's own globals are.
 */

#include "xud_swapchain.h" /* Required for: this layer's contract, XudSwapchain, XudSurfaceCaps, the bounds */
#include "xud.h" /* Required for: XudResult - the codes every function below answers */
#include "xud_device.h" /* Required for: XudDevice - the device a swapchain is created on, and xud_device_wait_idle() */
#include "xud_loader.h" /* Required for: XudLoader - the Volk rows every call here goes through */
#include "xud_physical.h" /* Required for: XudPhysicalDevice - the handle the three surface queries are asked of */
#include "xud_vulkan.h" /* Required for: xud_result_from_vk - the one VkResult -> XudResult funnel */
#include "xud_window.h" /* Required for: XudWindow - the surface a swapchain is created for */

#include <assert.h> /* Required for: static_assert, the compile-time checks below */
#include <stdlib.h> /* Required for: calloc, free - the swapchain struct and its one heap member */
#include <string.h> /* Required for: memset, memcpy - a refused query's zero state, a recreate's record swap */

/*-----------------------------------------------------------------------------------------------
 * The formats this layer will create a swapchain in, in the order it will pick them
 *-----------------------------------------------------------------------------------------------*/

/* The four RGBA byte formats, in the order the renderer's own colours are spelled in: a swapchain
 *   in one of these is a swapchain whose clear colour, its glyph atlas and its 0x80808080 blend
 *   base are all byte-addressable without a conversion step, which is what the SoA decision's
 *   `color ubyte4 normalized` stream (AGENTS.md) already assumes. B8G8R8A8 comes first because it
 *   is the format a compositor hands over unconverted on this window system, and the SRGB form of
 *   each before the UNORM form because the renderer's colours are authored in sRGB. The list is a
 *   preference, never a request: xud_swapchain_select_format() picks the first of these the
 *   surface actually advertises, and falls back to any SRGB_NONLINEAR row, and only to a row
 *   outside this list when the surface has no SRGB_NONLINEAR row at all - nothing here is ever
 *   asked of a driver that did not answer it. */
static const VkFormat XUD_SWAPCHAIN_WANTED_FORMATS[] = {
    VK_FORMAT_B8G8R8A8_SRGB,
    VK_FORMAT_B8G8R8A8_UNORM,
    VK_FORMAT_R8G8B8A8_SRGB,
    VK_FORMAT_R8G8B8A8_UNORM,
};
#define XUD_SWAPCHAIN_WANTED_COUNT                                                                 \
    ((uint32_t)(sizeof(XUD_SWAPCHAIN_WANTED_FORMATS) / sizeof(XUD_SWAPCHAIN_WANTED_FORMATS[0])))

static_assert(XUD_SWAPCHAIN_WANTED_COUNT <= XUD_SWAPCHAIN_MAX_SURFACE_FORMATS,
    "xud_swapchain.c: the wanted list must fit inside the storage the caps' advertised list has, "
    "or a surface advertising exactly the wanted set could be refused for the wrong reason");

/*-----------------------------------------------------------------------------------------------
 * The image count, the format, the present mode - the three selection rules, pure
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_swapchain_choose_image_count(
    uint32_t* outImageCount, const VkSurfaceCapabilitiesKHR* capabilities) {
    uint32_t count;

    if ((outImageCount == NULL) || (capabilities == NULL)) {
        return XUD_INVALID_ARG;
    }

    /* The spec's own floor, restated as a refusal rather than trusted: VkSurfaceCapabilitiesKHR::
       minImageCount "will be at least 2", so a surface answering 0 or 1 is a broken query and
       asking it for one image would be a request the driver is entitled to refuse. Refusing here
       is what keeps the clamp order below from producing a count the surface never allowed. */
    if (capabilities->minImageCount < XUD_SWAPCHAIN_IMAGE_COUNT_FLOOR) {
        return XUD_UNSUPPORTED;
    }

    /* An inconsistent answer - a non-zero upper limit below the lower one. The clamp order below
       cannot fix that (it would raise then cut, and the cut would win), and no layer above this
       one produced it, so it is reported rather than repaired. */
    if ((capabilities->maxImageCount != 0u)
        && (capabilities->maxImageCount < capabilities->minImageCount)) {
        return XUD_UNSUPPORTED;
    }

    count = XUD_SWAPCHAIN_IMAGE_COUNT_PREFERRED;
    if (count < capabilities->minImageCount) {
        count = capabilities->minImageCount;
    }
    /* 0 is the "no upper limit" answer (probed here: maxImageCount 0), so the cut only applies to
       a surface that has an upper limit at all. */
    if ((capabilities->maxImageCount != 0u) && (count > capabilities->maxImageCount)) {
        count = capabilities->maxImageCount;
    }

    *outImageCount = count;
    return XUD_OK;
}

XudResult xud_swapchain_select_format(VkSurfaceFormatKHR* outFormat, uint32_t* outIndex,
    const VkSurfaceFormatKHR* advertised, uint32_t advertisedCount) {
    uint32_t wanted;
    uint32_t row;

    if ((outFormat == NULL) || (outIndex == NULL)
        || ((advertisedCount != 0u) && (advertised == NULL))) {
        return XUD_INVALID_ARG;
    }

    /* A surface that advertises no format at all cannot host a swapchain - there is nothing to
       choose from, and that is the surface's answer, not a bad argument. */
    if (advertisedCount == 0u) {
        return XUD_UNSUPPORTED;
    }

    /* Step one: the wanted format, in wanted order, and only in the SRGB_NONLINEAR color space.
       The color-space half of the test is what keeps the eight PASS_THROUGH rows this surface
       advertises unreachable - VK_COLOR_SPACE_PASS_THROUGH_EXT is only meaningful with VK_EXT_swap
       chain_colorspace enabled on the instance, and this library's instance does not enable that
       name (probed: it is one of the 26 advertised at instance level, and enabling it is an
       extension decision, not a default). */
    for (wanted = 0u; wanted < XUD_SWAPCHAIN_WANTED_COUNT; wanted++) {
        for (row = 0u; row < advertisedCount; row++) {
            if ((advertised[row].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                && (advertised[row].format == XUD_SWAPCHAIN_WANTED_FORMATS[wanted])) {
                *outFormat = advertised[row];
                *outIndex = row;
                return XUD_OK;
            }
        }
    }

    /* Step two: any format at all in the color space the renderer's colours are already in. A
       surface that has none of the four wanted formats still gets a swapchain, and the renderer
       above reads the format it was handed rather than assuming one. */
    for (row = 0u; row < advertisedCount; row++) {
        if (advertised[row].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            *outFormat = advertised[row];
            *outIndex = row;
            return XUD_OK;
        }
    }

    /* Step three: the first advertised pair. A surface that advertises no SRGB_NONLINEAR row has
       no color space this layer can prefer, and refusing there would refuse a surface the spec
       allows - the (format, colorSpace) list is never empty on a working window system, and a
       surface whose only rows are PASS_THROUGH is a surface that can still present. */
    *outFormat = advertised[0];
    *outIndex = 0u;
    return XUD_OK;
}

XudResult xud_swapchain_select_present_mode(
    VkPresentModeKHR* outMode, const VkPresentModeKHR* advertised, uint32_t advertisedCount) {
    uint32_t row;

    if ((outMode == NULL) || ((advertisedCount != 0u) && (advertised == NULL))) {
        return XUD_INVALID_ARG;
    }

    if (advertisedCount == 0u) {
        return XUD_UNSUPPORTED;
    }

    /* Searched by VALUE, never by position: the present-mode enum is not contiguous (0,1,2,3 then
       1000111000/1000111001/1000361000), so an index into this array is nothing at all and the
       mode itself is the value the create-info must carry. */
    for (row = 0u; row < advertisedCount; row++) {
        if (advertised[row] == VK_PRESENT_MODE_MAILBOX_KHR) {
            *outMode = VK_PRESENT_MODE_MAILBOX_KHR;
            return XUD_OK;
        }
    }

    /* The only mode the spec guarantees every implementation advertises, which is why the chain
       ends here rather than at "the first advertised": a surface that has neither MAILBOX nor
       FIFO is a surface whose answer is not a working one, and the first-advertised fallback
       below is for the modes these headers name that the spec does not guarantee (IMMEDIATE, the
       two SHARED_* forms) - a surface advertising only IMMEDIATE gets IMMEDIATE rather than a
       refusal, because IMMEDIATE is a present mode, not an absence of one. */
    for (row = 0u; row < advertisedCount; row++) {
        if (advertised[row] == VK_PRESENT_MODE_FIFO_KHR) {
            *outMode = VK_PRESENT_MODE_FIFO_KHR;
            return XUD_OK;
        }
    }

    *outMode = advertised[0];
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The create-info, assembled from the caps and the extent above this layer
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_swapchain_fill_create_info(VkSwapchainCreateInfoKHR* outCreateInfo,
    const XudSurfaceCaps* surfaceCaps, VkSurfaceKHR surface, uint32_t width, uint32_t height,
    VkSwapchainKHR oldSwapchain) {
    VkSurfaceFormatKHR format;
    VkPresentModeKHR presentMode;
    VkSwapchainCreateInfoKHR info;
    VkCompositeAlphaFlagBitsKHR compositeAlpha = 0u;
    uint32_t minImageCount;
    uint32_t chosenIndex;
    uint32_t bit;
    XudResult guard;

    if ((outCreateInfo == NULL) || (surfaceCaps == NULL) || (surface == VK_NULL_HANDLE)) {
        return XUD_INVALID_ARG;
    }
    /* 0 is not an extent, and it is a bad argument rather than a surface answer: no window above
       was ever opened at it, GLFW refuses it (xud_window.h's XUD_WINDOW_MIN_EXTENT). */
    if ((width == 0u) || (height == 0u)) {
        return XUD_INVALID_ARG;
    }

    /* The three selection rules, called from here so the create-info and the record of it cannot
       disagree - there is exactly one derivation path from the caps to the create-info, and the
       caps struct is the only input. */
    guard = xud_swapchain_choose_image_count(&minImageCount, &surfaceCaps->capabilities);
    if (guard != XUD_OK) {
        return guard;
    }
    guard = xud_swapchain_select_format(
        &format, &chosenIndex, surfaceCaps->surfaceFormats, surfaceCaps->surfaceFormatCount);
    if (guard != XUD_OK) {
        return guard;
    }
    guard = xud_swapchain_select_present_mode(
        &presentMode, surfaceCaps->presentModes, surfaceCaps->presentModeCount);
    if (guard != XUD_OK) {
        return guard;
    }

    /* The extent is checked against the surface's own answer, not against GLFW's int bound: the
       two are different gates, and a window inside GLFW's bound but outside this surface's is a
       window this surface cannot present to - the surface's answer, XUD_UNSUPPORTED. */
    if ((width < surfaceCaps->capabilities.minImageExtent.width)
        || (height < surfaceCaps->capabilities.minImageExtent.height)
        || (width > surfaceCaps->capabilities.maxImageExtent.width)
        || (height > surfaceCaps->capabilities.maxImageExtent.height)) {
        return XUD_UNSUPPORTED;
    }

    /* One layer, always: more than one is a multi-view decision nothing above asks for, and the
       surface's answer decides whether even one is allowed. */
    if (surfaceCaps->capabilities.maxImageArrayLayers == 0u) {
        return XUD_UNSUPPORTED;
    }

    /* The one usage bit this layer needs, intersected with what the surface allows rather than
       asserted: there is no VK_IMAGE_USAGE_PRESENT_BIT in these headers (see the note in the
       header above), so presentation is not a flag to ask for and COLOR_ATTACHMENT is the only
       bit this library's one use of a swapchain requires. */
    if ((surfaceCaps->capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
        != VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) {
        return XUD_UNSUPPORTED;
    }

    /* The first composite-alpha bit the surface supports, in the order that means "the image is
       opaque" first and "the image is already multiplied" second - the two states the renderer's
       colours can be in. The probed surface supports OPAQUE|PRE_MULTIPLIED (0x3), so OPAQUE wins
       here; a surface supporting neither gets the next bit it does support rather than a refusal,
       and a surface supporting none at all is a broken query, refused. */
    for (bit = 0u; bit < 4u; bit++) {
        const VkCompositeAlphaFlagBitsKHR candidate = (VkCompositeAlphaFlagBitsKHR)(1u << bit);
        if ((compositeAlpha == 0u)
            && ((surfaceCaps->capabilities.supportedCompositeAlpha & candidate) == candidate)) {
            compositeAlpha = candidate;
        }
    }
    if (compositeAlpha == 0u) {
        return XUD_UNSUPPORTED;
    }

    info = (VkSwapchainCreateInfoKHR) {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .pNext = NULL,
        .flags = 0u,
        .surface = surface,
        .minImageCount = minImageCount,
        .imageFormat = format.format,
        .imageColorSpace = format.colorSpace,
        .imageExtent = { .width = width, .height = height },
        .imageArrayLayers = 1u,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0u,
        .pQueueFamilyIndices = NULL,
        .preTransform = surfaceCaps->capabilities.currentTransform,
        .compositeAlpha = compositeAlpha,
        .presentMode = presentMode,
        /* Clipped, not a comparison against currentExtent: the surface here reports no current
           extent at all (probed 4294967295x4294967295, the "undefined" answer), so a comparison
           would be a comparison against nothing, and the images are clipped to the extent this
           layer asked for either way. */
        .clipped = VK_TRUE,
        .oldSwapchain = oldSwapchain,
    };

    *outCreateInfo = info;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The three surface queries - instance-level rows, because they take a physical device
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_swapchain_query_surface(XudSurfaceCaps* outSurfaceCaps, const XudLoader* loader,
    VkPhysicalDevice physicalDevice, VkSurfaceKHR surface) {
    VkSurfaceFormatKHR formats[XUD_SWAPCHAIN_MAX_SURFACE_FORMATS];
    VkPresentModeKHR modes[XUD_SWAPCHAIN_MAX_PRESENT_MODES];
    VkSurfaceCapabilitiesKHR capabilities;
    uint32_t formatCount = XUD_SWAPCHAIN_MAX_SURFACE_FORMATS;
    uint32_t modeCount = XUD_SWAPCHAIN_MAX_PRESENT_MODES;
    VkResult vk;

    if ((outSurfaceCaps == NULL) || (loader == NULL) || (physicalDevice == VK_NULL_HANDLE)
        || (surface == VK_NULL_HANDLE)) {
        return XUD_INVALID_ARG;
    }

    /* A closed loader has no live row anywhere (xud_loader_close() memsets both tables before it
       frees), and a loader that was never opened has none either - the bring-up invariant every
       layer above checks, restated here before the rows are read. */
    if (!xud_loader_is_open(loader)) {
        return XUD_UNSUPPORTED;
    }

    /* All three rows checked before any of the three calls: a driver that does not answer the
       surface queries is reported, never called through. These are instance-level rows because
       all three take a VkPhysicalDevice, not a VkDevice. */
    if ((loader->instanceTable.vkGetPhysicalDeviceSurfaceCapabilitiesKHR == NULL)
        || (loader->instanceTable.vkGetPhysicalDeviceSurfaceFormatsKHR == NULL)
        || (loader->instanceTable.vkGetPhysicalDeviceSurfacePresentModesKHR == NULL)) {
        return XUD_UNSUPPORTED;
    }

    /* The zero state first, so a refusal cannot leave a caller reading a half-filled answer out
       of a struct it embedded in its own swapchain. */
    memset(outSurfaceCaps, 0, sizeof(XudSurfaceCaps));

    vk = loader->instanceTable.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        physicalDevice, surface, &capabilities);
    if (xud_result_from_vk(vk) != XUD_OK) {
        return xud_result_from_vk(vk);
    }

    /* The two list-shaped queries have an input/output count and no count-only form, so they are
       asked with the capacity of the struct's own fixed arrays. A driver that reports VK_INCOM-
       PLETE at that capacity, or claims success while writing more than it was given, is refused
       rather than retried or truncated: unlike the extension lists these two bounds are the
       storage of the struct being filled, not a heap array that can grow. */
    vk = loader->instanceTable.vkGetPhysicalDeviceSurfaceFormatsKHR(
        physicalDevice, surface, &formatCount, formats);
    if ((vk == VK_INCOMPLETE)
        || ((vk == VK_SUCCESS) && (formatCount > XUD_SWAPCHAIN_MAX_SURFACE_FORMATS))) {
        return XUD_OUT_OF_RESOURCES;
    }
    if (xud_result_from_vk(vk) != XUD_OK) {
        return xud_result_from_vk(vk);
    }

    vk = loader->instanceTable.vkGetPhysicalDeviceSurfacePresentModesKHR(
        physicalDevice, surface, &modeCount, modes);
    if ((vk == VK_INCOMPLETE)
        || ((vk == VK_SUCCESS) && (modeCount > XUD_SWAPCHAIN_MAX_PRESENT_MODES))) {
        return XUD_OUT_OF_RESOURCES;
    }
    if (xud_result_from_vk(vk) != XUD_OK) {
        return xud_result_from_vk(vk);
    }

    outSurfaceCaps->capabilities = capabilities;
    memcpy(outSurfaceCaps->surfaceFormats, formats, sizeof(VkSurfaceFormatKHR) * formatCount);
    outSurfaceCaps->surfaceFormatCount = formatCount;
    memcpy(outSurfaceCaps->presentModes, modes, sizeof(VkPresentModeKHR) * modeCount);
    outSurfaceCaps->presentModeCount = modeCount;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The image query - a device-level row, and the one list here that CAN grow
 *-----------------------------------------------------------------------------------------------*/

static XudResult xud_swapchain_query_images(VkImage** outImages, uint32_t* outCount,
    uint32_t* outCapacity, const XudLoader* loader, VkDevice device, VkSwapchainKHR swapchain) {
    uint32_t capacity = XUD_SWAPCHAIN_QUERY_MIN_CAPACITY;

    while (capacity <= XUD_SWAPCHAIN_QUERY_MAX_CAPACITY) {
        VkImage* rows = (VkImage*)calloc(capacity, sizeof(VkImage));
        uint32_t count = capacity;
        VkResult vk;

        if (rows == NULL) {
            return XUD_OUT_OF_MEMORY;
        }

        /* The doubling shape xud_instance.c and xud_physical.c use for the same reason: pCount is
           input/output and there is no count-only form, so the count cannot be read without an
           array, and the array this layer grows is a heap member of its own struct. */
        vk = loader->deviceTable.vkGetSwapchainImagesKHR(device, swapchain, &count, rows);
        if ((vk == VK_SUCCESS) && (count <= capacity)) {
            *outImages = rows;
            *outCount = count;
            *outCapacity = capacity;
            return XUD_OK;
        }

        free(rows);

        if ((vk != VK_INCOMPLETE) && !((vk == VK_SUCCESS) && (count > capacity))) {
            return xud_result_from_vk(vk);
        }
        if (capacity > (XUD_SWAPCHAIN_QUERY_MAX_CAPACITY / 2u)) {
            break;
        }
        capacity *= 2u;
    }

    return XUD_OUT_OF_RESOURCES;
}

/*-----------------------------------------------------------------------------------------------
 * Bring-up - the whole ladder for one surface, or nothing
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_swapchain_open(XudSwapchain** outSwapchain, XudLoader* loader,
    const XudPhysicalDevice* physicalDevice, const XudWindow* window, const XudDevice* device) {
    XudSwapchain* swapchain;
    VkSwapchainCreateInfoKHR createInfo;
    VkSwapchainKHR created = VK_NULL_HANDLE;
    VkImage* images = NULL;
    uint32_t imageCount = 0u;
    uint32_t imageCapacity = 0u;
    VkResult vk;
    XudResult guard;

    if ((outSwapchain == NULL) || (loader == NULL) || (window == NULL) || (physicalDevice == NULL)
        || (device == NULL)) {
        return XUD_INVALID_ARG;
    }
    /* The slot must be empty before the call, not after a failure: a struct this rung frees on a
       failure path is a struct the caller cannot still hold, so a caller that already owns a
       swapchain is refused rather than quietly re-created over. */
    if (*outSwapchain != NULL) {
        return XUD_INVALID_ARG;
    }
    /* A window with no surface is a window whose surface creation failed or whose surface was
       already closed - there is no surface to create a swapchain for, and that is a bad argument
       about the state in hand, not a driver's refusal. */
    if (window->surface == VK_NULL_HANDLE) {
        return XUD_INVALID_ARG;
    }
    if ((physicalDevice->physicalDevice == VK_NULL_HANDLE) || (device->device == VK_NULL_HANDLE)) {
        return XUD_INVALID_ARG;
    }
    if (!loader->open) {
        return XUD_INVALID_ARG;
    }

    /* The two bound-handle rules, and why both are needed here: the surface queries are rows of
       the instance table filled for the instance the SURFACE was created for (xud_window.c
       records that instance for exactly this reason), and the swapchain names are rows of the
       device table filled for THIS device. A surface created for one instance is not a surface a
       second instance's rows can query, and a device table filled for a dead device is pointers
       into a dead layer - see xud_loader.h on why a table's rows are per-object state. */
    if (!xud_loader_instance_bound(loader, window->surfaceInstance)) {
        return XUD_UNSUPPORTED;
    }
    if (!xud_loader_device_bound(loader, device->device)) {
        return XUD_UNSUPPORTED;
    }

    /* The destroy row BEFORE the create row: a swapchain this layer cannot destroy is a swapchain
       it must not create, the same rule xud_window.c applies to a surface. The two query rows are
       checked before the calls that need them. */
    if ((loader->deviceTable.vkDestroySwapchainKHR == NULL)
        || (loader->deviceTable.vkCreateSwapchainKHR == NULL)
        || (loader->deviceTable.vkGetSwapchainImagesKHR == NULL)
        || (loader->instanceTable.vkGetPhysicalDeviceSurfaceCapabilitiesKHR == NULL)
        || (loader->instanceTable.vkGetPhysicalDeviceSurfaceFormatsKHR == NULL)
        || (loader->instanceTable.vkGetPhysicalDeviceSurfacePresentModesKHR == NULL)) {
        return XUD_UNSUPPORTED;
    }

    swapchain = (XudSwapchain*)calloc(1u, sizeof(XudSwapchain));
    if (swapchain == NULL) {
        return XUD_OUT_OF_MEMORY;
    }

    guard = xud_swapchain_query_surface(
        &swapchain->surfaceCaps, loader, physicalDevice->physicalDevice, window->surface);
    if (guard != XUD_OK) {
        free(swapchain);
        return guard;
    }

    guard = xud_swapchain_fill_create_info(&createInfo, &swapchain->surfaceCaps, window->surface,
        window->width, window->height, VK_NULL_HANDLE);
    if (guard != XUD_OK) {
        free(swapchain);
        return guard;
    }

    /* The one Vulkan object this layer owns, created with the create-info the selection rules
       above produced. oldSwapchain is VK_NULL_HANDLE here - there is nothing to retire, and the
       recreate path below is the one that names a handle to retire. */
    vk = loader->deviceTable.vkCreateSwapchainKHR(device->device, &createInfo, NULL, &created);
    guard = xud_result_from_vk(vk);
    if (guard != XUD_OK) {
        free(swapchain);
        return guard;
    }
    swapchain->swapchain = created;

    guard = xud_swapchain_query_images(
        &images, &imageCount, &imageCapacity, loader, device->device, created);
    if (guard != XUD_OK) {
        /* A swapchain nobody can get an image from is a swapchain nothing above can use, and
           leaving it alive would leak the one object this layer owns - so it is destroyed here,
           through the row checked above, before the struct goes away. */
        loader->deviceTable.vkDestroySwapchainKHR(device->device, created, NULL);
        free(swapchain);
        return guard;
    }
    swapchain->images = images;
    swapchain->imageCount = imageCount;
    swapchain->imageCapacity = imageCapacity;

    /* The record of what the swapchain IS, with the retired-handle field cleared in it: the
       handle named there was destroyed by the create that replaced it, and storing a handle to
       a destroyed object is exactly what this tree forbids. */
    swapchain->createInfo = createInfo;
    swapchain->createInfo.oldSwapchain = VK_NULL_HANDLE;

    *outSwapchain = swapchain;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The resize path - the one place a swapchain is replaced, and the one place an old one dies
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_swapchain_recreate(XudSwapchain** ioSwapchain, XudLoader* loader,
    const XudPhysicalDevice* physicalDevice, const XudWindow* window, const XudDevice* device,
    uint32_t width, uint32_t height) {
    XudSwapchain* swapchain;
    XudSurfaceCaps surfaceCaps;
    VkSwapchainCreateInfoKHR createInfo;
    VkSwapchainKHR old;
    VkSwapchainKHR created = VK_NULL_HANDLE;
    VkImage* images = NULL;
    uint32_t imageCount = 0u;
    uint32_t imageCapacity = 0u;
    VkResult vk;
    XudResult guard;

    if ((ioSwapchain == NULL) || (loader == NULL) || (window == NULL) || (physicalDevice == NULL)
        || (device == NULL)) {
        return XUD_INVALID_ARG;
    }
    /* There is nothing to replace - xud_swapchain_open() is the rung that creates one, and a
       caller that has no swapchain has a window with no swapchain, not a window at the wrong
       size. */
    if ((*ioSwapchain) == NULL) {
        return XUD_INVALID_ARG;
    }
    if (window->surface == VK_NULL_HANDLE) {
        return XUD_INVALID_ARG;
    }
    if ((physicalDevice->physicalDevice == VK_NULL_HANDLE) || (device->device == VK_NULL_HANDLE)) {
        return XUD_INVALID_ARG;
    }
    if ((width == 0u) || (height == 0u) || (width > XUD_WINDOW_MAX_EXTENT)
        || (height > XUD_WINDOW_MAX_EXTENT)) {
        return XUD_INVALID_ARG;
    }
    if (!loader->open) {
        return XUD_INVALID_ARG;
    }

    swapchain = *ioSwapchain;
    old = swapchain->swapchain;
    if (old == VK_NULL_HANDLE) {
        return XUD_INVALID_ARG;
    }

    /* Already at this size: recreating at the extent the swapchain was created for would retire
       images the renderer above is still holding and buy nothing, and "already at this size" is
       the state asked for, not an error. */
    if ((width == swapchain->createInfo.imageExtent.width)
        && (height == swapchain->createInfo.imageExtent.height)) {
        return XUD_ALREADY;
    }

    if (!xud_loader_instance_bound(loader, window->surfaceInstance)) {
        return XUD_UNSUPPORTED;
    }
    if (!xud_loader_device_bound(loader, device->device)) {
        return XUD_UNSUPPORTED;
    }
    if ((loader->deviceTable.vkDestroySwapchainKHR == NULL)
        || (loader->deviceTable.vkCreateSwapchainKHR == NULL)
        || (loader->deviceTable.vkGetSwapchainImagesKHR == NULL)
        || (loader->instanceTable.vkGetPhysicalDeviceSurfaceCapabilitiesKHR == NULL)
        || (loader->instanceTable.vkGetPhysicalDeviceSurfaceFormatsKHR == NULL)
        || (loader->instanceTable.vkGetPhysicalDeviceSurfacePresentModesKHR == NULL)) {
        return XUD_UNSUPPORTED;
    }

    /* The surface is re-queried rather than reused: a window can move to another monitor, and
       with it the whole answer moves - the extent bounds, the format list, the present modes.
       The query goes into a LOCAL struct, not into the record: a creation that then fails must
       leave the record describing the swapchain that is still alive. */
    guard = xud_swapchain_query_surface(
        &surfaceCaps, loader, physicalDevice->physicalDevice, window->surface);
    if (guard != XUD_OK) {
        return guard;
    }

    guard = xud_swapchain_fill_create_info(
        &createInfo, &surfaceCaps, window->surface, width, height, old);
    if (guard != XUD_OK) {
        return guard;
    }

    /* The create carries oldSwapchain, so the driver retires the old handle as part of the
       creation - which is why the old one cannot leak. A creation that fails does not retire it
       (probed: a create outside this surface's maxImageExtent answered VK_ERROR_OUT_OF_HOST_
       MEMORY with the old swapchain still usable), so the handle in hand stays alive and
       unchanged on every path below that does not destroy it. */
    vk = loader->deviceTable.vkCreateSwapchainKHR(device->device, &createInfo, NULL, &created);
    guard = xud_result_from_vk(vk);
    if (guard != XUD_OK) {
        return guard;
    }

    /* The spec's own text: passing oldSwapchain means the driver has retired the old swapchain's
       images, but "the old swapchain still needs to be destroyed" - a driver that only retired it
       implicitly is a driver that would leak it, so the destroy is called here explicitly,
       through the row checked above, before anything else about the new swapchain happens. The
       record's handle is cleared at the same moment, because the handle it named is gone: every
       path below either fills it with the new swapchain or leaves the struct in the no-swapchain
       state, and no path may leave a destroyed handle in a live record. */
    loader->deviceTable.vkDestroySwapchainKHR(device->device, old, NULL);
    swapchain->swapchain = VK_NULL_HANDLE;

    guard = xud_swapchain_query_images(
        &images, &imageCount, &imageCapacity, loader, device->device, created);
    if (guard != XUD_OK) {
        /* The old swapchain is already gone - the create succeeded and destroyed it - so the new
           one is destroyed here too, and the struct is left in the no-swapchain state with its
           record still describing the extent it had a swapchain for. That is the state the rung
           above retries: a window at an extent no swapchain exists for is a window the caller
           above brings up again, and a record left at the NEW extent would make that retry answer
           XUD_ALREADY with no swapchain in hand. The old image array is still owned by the struct
           and is freed by xud_swapchain_close(); nothing can read it, because the accessor below
           refuses a struct whose handle is cleared. */
        loader->deviceTable.vkDestroySwapchainKHR(device->device, created, NULL);
        return guard;
    }

    /* The record swap: the new surface answer, the new create-info (with the retired-handle field
       cleared in it, the same rule as the open path), the new image array. The struct itself is
       the same struct, so a caller above that holds the pointer holds the new swapchain. */
    swapchain->surfaceCaps = surfaceCaps;
    swapchain->createInfo = createInfo;
    swapchain->createInfo.oldSwapchain = VK_NULL_HANDLE;
    swapchain->swapchain = created;
    free(swapchain->images);
    swapchain->images = images;
    swapchain->imageCount = imageCount;
    swapchain->imageCapacity = imageCapacity;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The image accessor - pure, and the reason a caller above holds an index and not a handle
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_swapchain_images_get(
    const XudSwapchain* swapchain, uint32_t index, VkImage* outImage) {
    if ((outImage == NULL) || (swapchain == NULL)) {
        return XUD_INVALID_ARG;
    }
    /* No swapchain means no image array in any form - the zero state of a struct whose create ran
       but whose image query failed is that state, not an empty array of one image. */
    if (swapchain->swapchain == VK_NULL_HANDLE) {
        return XUD_UNSUPPORTED;
    }
    /* A stale index into an array that grew or shrank is a caught error, not an out-of-range read
       - the repo's opaque-handle rule (xud.h) read at the one array this layer owns. */
    if (index >= swapchain->imageCount) {
        return XUD_INVALID_ARG;
    }

    *outImage = swapchain->images[index];
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * Teardown - the guard, the destroy, the free
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_swapchain_close(
    XudSwapchain** swapchain, const XudDevice* device, const XudLoader* loader) {
    XudSwapchain* victim;
    VkSwapchainKHR handle;
    XudResult guard;

    if ((swapchain == NULL) || (device == NULL) || (loader == NULL)) {
        return XUD_INVALID_ARG;
    }
    if ((*swapchain) == NULL) {
        return XUD_ALREADY;
    }
    if (!loader->open) {
        return XUD_INVALID_ARG;
    }

    victim = *swapchain;
    handle = victim->swapchain;

    /* Nothing to guard and nothing to destroy, but freeing is what this rung is for - the same
       rule xud_device_close() follows for a struct whose handle is already gone. */
    if (handle == VK_NULL_HANDLE) {
        free(victim->images);
        free(victim);
        *swapchain = NULL;
        return XUD_OK;
    }

    /* The destroy call takes the device, and the rows must be that device's rows - a handle-less
       device fails this check (both reject VK_NULL_HANDLE), and so does a device table filled for
       a dead device. Either way the rung refuses and the struct is not freed, because freeing it
       would leak the handle silently, the same rule xud_device_close() applies to a nil destroy
       row. */
    if (!xud_loader_device_bound(loader, device->device)) {
        return XUD_UNSUPPORTED;
    }
    if ((loader->deviceTable.vkDeviceWaitIdle == NULL)
        || (loader->deviceTable.vkDestroySwapchainKHR == NULL)) {
        return XUD_UNSUPPORTED;
    }

    /* The guard is not decoration: a presentation may still be in flight against these very
       images (vkQueuePresentKHR is the renderer rung's call, and it submits through the device
       above), and destroying the swapchain under it is destroying images a submission is still
       reading. The one guarantee that nothing is in flight is xud_device_wait_idle()'s, so this
       rung calls it rather than restating it - and a guard that cannot run is a teardown this
       rung must not start. */
    guard = xud_device_wait_idle(device, loader);
    if (guard == XUD_UNSUPPORTED) {
        return XUD_UNSUPPORTED;
    }

    /* vkDestroySwapchainKHR returns void, so there is no VkResult to map and nothing to check
       after the call - the zero-state-before-the-call rule this tree follows for a void teardown
       (vkDestroyDevice, vkDestroySurfaceKHR, vkFreeMemory). The handle is cleared before the
       struct goes away so no reader can see a handle that has been destroyed. */
    victim->swapchain = VK_NULL_HANDLE;
    loader->deviceTable.vkDestroySwapchainKHR(device->device, handle, NULL);

    /* The images array is freed and never destroyed - see the note in the header above about
       vkDestroyImage and swapchain-owned images. */
    free(victim->images);
    free(victim);
    *swapchain = NULL;

    /* A lost device is still destroyed above, and the answer is still propagated - the same rule
       xud_device_close() follows: XUD_LOST is a report, not a skip. */
    return (guard == XUD_LOST) ? XUD_LOST : XUD_OK;
}
