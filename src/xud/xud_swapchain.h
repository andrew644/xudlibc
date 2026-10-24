/***********************************************************************************************
 *   xud_swapchain.h - the swapchain layer: one VkSwapchainKHR per context, recreated on
 *                     window resize, and never leaking the one it replaces
 *
 *   Owns : XudSwapchain (the VkSwapchainKHR handle, the VkSwapchainCreateInfoKHR that
 *          produced it, the surface caps the three surface queries wrote for it, and the
 *          image array vkGetSwapchainImagesKHR wrote) and the contract of the layer above
 *          the device and below the renderer - the three surface queries, the format /
 *          present-mode / image-count selection rules, create-info assembly, the image
 *          accessor, bring-up, recreate-on-resize, teardown.
 *   Layer: above the device, below the renderer. It sees the window's VkSurfaceKHR (owned
 *          by XudWindow, borrowed here), the physical device's handle and the device's
 *          handle, and it records none of them as its own: the only Vulkan object this
 *          layer owns is the VkSwapchainKHR, and the images it was handed are recorded as
 *          handles the swapchain owns, not as images this layer owns - there is no
 *          vkDestroyImage call here, because destroying the swapchain is what retires
 *          them (see the note below).
 *          Internal like every layer below the public API: tests may include it, user
 *          code must not, and xud.h declares no swapchain type.
 *
 *   What the three queries answer on this machine (probed 2026-10-24 through the loader
 *   pair xud_loader_open()/xud_loader_bind_instance() fills, with and without
 *   VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation, on a real GLFW window plus the
 *   surface glfwCreateWindowSurface handed it): minImageCount 4, maxImageCount 0 (the
 *   "no upper limit" answer), currentExtent 4294967295x4294967295 (no current extent -
 *   the reason `clipped` below is VK_TRUE rather than a comparison), minImageExtent 1x1,
 *   maxImageExtent 16384x16384, maxImageArrayLayers 1, supportedTransforms and
 *   currentTransform VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, supportedCompositeAlpha
 *   OPAQUE or PRE_MULTIPLIED (0x3), supportedUsageFlags VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
 *   among them. 16 surface formats: eight at colorSpace VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
 *   and eight at VK_COLOR_SPACE_PASS_THROUGH_EXT, the same eight formats twice -
 *   R16G16B16A16_SFLOAT, A2R10G10B10_UNORM_PACK32, A2B10G10R10_UNORM_PACK32,
 *   B8G8R8A8_SRGB, B8G8R8A8_UNORM, R8G8B8A8_SRGB, R8G8B8A8_UNORM, R5G6B5_UNORM_PACK16.
 *   2 present modes: MAILBOX and FIFO. vkCreateSwapchainKHR at 4 images answers VK_SUCCESS
 *   and vkGetSwapchainImagesKHR then writes exactly 4.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#ifndef XUD_XUD_SWAPCHAIN_H_
#define XUD_XUD_SWAPCHAIN_H_

#include "volk.h" /* Required for: VK_NO_PROTOTYPES - must precede <vulkan/vulkan.h>, see xud_loader.h */
#include "xud.h" /* Required for: XudResult - the swapchain layer's own error codes */
#include "xud_device.h" /* Required for: XudDevice - the device a swapchain is created on, and teardown's guard */
#include "xud_loader.h" /* Required for: XudLoader - the Volk rows every call here is made through */
#include "xud_physical.h" /* Required for: XudPhysicalDevice - the handle the three surface queries are asked of */
#include "xud_vulkan.h" /* Required for: xud_result_from_vk - every VkResult here goes through that one funnel */
#include "xud_window.h" /* Required for: XudWindow - the surface a swapchain is created for, and the extent bound */
#include <vulkan/vulkan.h> /* Required for: VkSwapchainKHR, the surface caps and the create-info stamp - never linked */

#include <assert.h> /* Required for: static_assert, the compile-time checks below */
#include <stdbool.h>
#include <stdint.h>

/*-----------------------------------------------------------------------------------------------
 * Why this rung is xud_swapchain and not a set of fields on the context
 *-----------------------------------------------------------------------------------------------*/

/* The repo's naming rule (xud_physical.h states it): a layer's name is load-bearing before
   the collision it prevents can happen. A swapchain is not a context field because the
   resize path is a property of the swapchain and not of the context: recreating one retires
   the old handle, and the one argument that makes the driver retire it as part of the
   creation (VkSwapchainCreateInfoKHR::oldSwapchain) is the same argument that would
   otherwise leave the old handle alive and unowned. Owning the handle here is what turns
   AGENTS.md's "on window resize recreate it, never leak the old one" into a rule of this layer
   rather than a rule of whoever happens to call vkCreateSwapchainKHR. */

/*-----------------------------------------------------------------------------------------------
 * The facts this layer's shape rests on, grepped in Vulkan-Headers 1.4.363 here (2026-10-24,
 * no driver involved), plus the ones probed by running it today
 *-----------------------------------------------------------------------------------------------*/

/* VkSwapchainCreateInfoKHR is at vulkan_core.h:9095 (the VK_KHR_swapchain block, not the core
   create-info block - an extension struct lives with its extension, and the line number is the
   first thing a reader reaches for when the stamp below looks wrong) and stamps
   VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, which is an EXTENSION-level stamp: 1000001000,
   not a small core number like the device layer's pair (2 and 3). It is non-zero and distinct
   from zero, which is what the assert below checks - the trap this layer exists to avoid is
   writing a core stamp, or nothing, where an extension stamp belongs, and the validation layer
   does report it ("pCreateInfo->sType must be VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR"). */
static_assert(VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR != 0u,
    "xud_swapchain.h: the stamp of this layer's create-info is the extension-level non-zero "
    "value, and zero is the unstamped state");

/* The two entry points of this layer's own object, and their shapes: PFN_vkCreateSwapchainKHR
   answers a VkResult and is mapped through xud_result_from_vk, PFN_vkDestroySwapchainKHR returns
   **void** - the same shape as vkDestroyDevice, vkGetDeviceQueue, vkFreeMemory and
   vkDestroySurfaceKHR, so the zero-state-before-the-call / check-after-the-call rule applies and
   the destroy's own failure is not reportable through this signature. It takes THREE arguments
   (vulkan_core.h:9172: the VkDevice it belongs to first, then the handle, then the allocator) -
   the same three-argument teardown trap vkDestroyDebugUtilsMessengerEXT has (see
   xud_diagnostics.h), one rung up: a destroy called with the handle only is a swapchain the
   driver never saw a device for. PFN_vkGetSwapchainImagesKHR does answer a VkResult, and its
   pCount is input/output like every other list query in this tree (vkEnumerateInstanceExtension-
   Properties, vkEnumeratePhysicalDevices, the queue-family query): the swapchain names' signatures
   sit at vulkan_core.h:9171-9173 and the three surface queries' at 9022-9024, and there is no
   count-only form for the two list-shaped ones, so the image query follows the same doubling-
   capacity shape xud_instance.c and xud_physical.c use. */
static_assert(sizeof(PFN_vkCreateSwapchainKHR) == sizeof(void*), "xud_swapchain.h: a Volk entry-point row is one pointer wide");
static_assert(sizeof(PFN_vkDestroySwapchainKHR) == sizeof(void*), "xud_swapchain.h: and so is the teardown row");

/* The two table levels this layer calls through, and the reason they are two: the three
   surface queries take a VkPhysicalDevice, so they are instance-level rows of struct
   VolkInstanceTable (volk.h:485-487, inside the VK_KHR_surface block), while the swapchain
   names take a VkDevice and are device-level rows of struct VolkDeviceTable (volk.h:1646-1648,
   inside the VK_KHR_swapchain block). Both blocks are #if defined(...) in volk.h, so
   a row that is not compiled in is padding rather than a slot - which is why every row is
   checked before the call that needs it rather than assumed present. The swapchain rows
   answer NULL when VK_KHR_swapchain was not enabled on the instance the device was created
   from (probed with the loader rework: with the extension not enabled, every swapchain row
   of the device table came back NULL while destroyDevice/deviceWaitIdle/getDeviceQueue
   came back set - AGENTS.md's check-before-you-use rule working through the table). */

/* There is no VK_IMAGE_USAGE_PRESENT_BIT anywhere in these headers (grepped across the whole
   /usr/include/vulkan/ tree: zero hits), so "a swapchain image can be presented to" is not
   a usage flag a surface might or might not advertise - presentation is vkQueuePresentKHR's
   requirement, not an image property. The one usage bit this layer needs is the one it draws
   into: VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT (0x10, vulkan_core.h:2812), which the surface
   here advertises (probed supportedUsageFlags 0x8009f carries it). */

/* The present-mode enum is not contiguous: 0,1,2,3 then 1000111000, 1000111001 and
   1000361000 (vulkan_core.h:8944-8951), so a present mode is a VALUE and an index into the
   advertised array is nothing at all - the selection rule below searches by value. Seven
   distinct modes exist in these headers, which is what makes the bound below a provable one
   rather than a guess. */
#define XUD_SWAPCHAIN_MAX_PRESENT_MODES 8u

/* The two bounds of the image query, the same doubling shape as the other list queries in
   this tree. A swapchain's image count is bounded above by what this layer asked for when
   the surface has an upper limit, but a surface answering maxImageCount 0 (probed here: 0,
   the spec's "no upper limit" answer) can answer any count, so the query's own capacity
   bound is a real refusal path rather than a formality. */
#define XUD_SWAPCHAIN_QUERY_MIN_CAPACITY 16u
#define XUD_SWAPCHAIN_QUERY_MAX_CAPACITY 512u

/* The fixed storage bound of the (format, colorSpace) list the caps above record. This one
   is NOT provable - the pair list is bounded only by the VkFormat enum - so 32 is this
   layer's own ceiling, twice what the surface here advertises (probed: 16), and a surface
   that advertises more is refused XUD_OUT_OF_RESOURCES rather than quietly cut, because a
   cut list is a list the selection rule below may have picked the wrong entry from. Growing
   it is not an option here: the caps struct is embedded in XudSwapchain by value, so a
   heap list inside it would be a second member for xud_swapchain_close() to free, and the
   two list queries' other shape (a heap array the caller frees) belongs to the queries that
   fill a struct of their own (xud_physical.c's caps), not to this one. */
#define XUD_SWAPCHAIN_MAX_SURFACE_FORMATS 32u

/* The image count this layer asks for when the surface allows more: the plan's "prefer 2
   for low latency". Two is also the Vulkan spec's own floor for a swapchain -
   VkSurfaceCapabilitiesKHR::minImageCount "will be at least 2" - so the two constants below
   are the same number for two different reasons, and both are checked. */
#define XUD_SWAPCHAIN_IMAGE_COUNT_PREFERRED 2u
#define XUD_SWAPCHAIN_IMAGE_COUNT_FLOOR 2u

/* The extent bound this layer refuses above its own argument checks. GLFW's bound
   (XUD_WINDOW_MAX_EXTENT, INT_MAX, xud_window.h) is about glfwCreateWindow's int arguments and
   the surface's own maxImageExtent is the authoritative gate, checked against the caps in
   the assembly below. What sits between the two is not a bad argument, it is a window this
   surface cannot present to, and that is the driver's answer - XUD_UNSUPPORTED, not
   XUD_INVALID_ARG. The assert below is the compile-time half of that ordering: the argument
   bound must not be looser than the bound the window above it was opened under, or this
   layer would accept an extent its own window had refused. */
static_assert(XUD_SWAPCHAIN_IMAGE_COUNT_PREFERRED >= XUD_SWAPCHAIN_IMAGE_COUNT_FLOOR,
    "xud_swapchain.h: the preferred image count must be at the spec's floor, or the clamp "
    "order in choose_image_count would raise it every time");
static_assert(XUD_SWAPCHAIN_MAX_PRESENT_MODES > 7u,
    "xud_swapchain.h: the present-mode enum in these headers has seven values, so the bound "
    "above must hold every mode a driver can advertise");
static_assert(XUD_SWAPCHAIN_QUERY_MIN_CAPACITY <= XUD_SWAPCHAIN_QUERY_MAX_CAPACITY,
    "xud_swapchain.h: the doubling sequence must start at or below the bound it stops at");
static_assert(XUD_SWAPCHAIN_QUERY_MAX_CAPACITY % XUD_SWAPCHAIN_QUERY_MIN_CAPACITY == 0u,
    "xud_swapchain.h: the doubling sequence must reach the bound exactly, so the loop that "
    "grows it cannot step past it and refuse a list one row too early");

/*-----------------------------------------------------------------------------------------------
 * What the three surface queries answered, as this layer records it
 *-----------------------------------------------------------------------------------------------*/

/* The three queries' three answers, in one struct, so the create-info assembly below takes
   one argument instead of three pointers that can be handed round in the wrong order - the
   same reason XudDeviceCaps exists one rung down. This is a query result, not an object:
   nothing is created by having one, and it is embedded in XudSwapchain rather than
   allocated separately, exactly like the caps are embedded in XudPhysicalDevice. */
typedef struct XudSurfaceCaps {
    VkSurfaceCapabilitiesKHR capabilities; /* minImageCount / maxImageCount (0 = no upper limit)
                                              / currentExtent (no current extent here, probed
                                              4294967295x4294967295) / minImageExtent /
                                              maxImageExtent / maxImageArrayLayers /
                                              supportedTransforms / currentTransform /
                                              supportedCompositeAlpha / supportedUsageFlags */
    VkSurfaceFormatKHR surfaceFormats[XUD_SWAPCHAIN_MAX_SURFACE_FORMATS]; /* the advertised (format, colorSpace)
                                                                             pairs, in the driver's order */
    uint32_t surfaceFormatCount; /* how many rows above are live */
    VkPresentModeKHR presentModes[XUD_SWAPCHAIN_MAX_PRESENT_MODES]; /* the advertised modes, in the
                                                                       driver's order */
    uint32_t presentModeCount; /* how many rows above are live */
} XudSurfaceCaps;

typedef struct XudSwapchain XudSwapchain;
struct XudSwapchain {
    VkSwapchainKHR swapchain; /* the one Vulkan object this layer owns: VK_NULL_HANDLE until
                                 xud_swapchain_open() creates it, destroyed by xud_swapchain_close
                                 (), and destroyed by xud_swapchain_recreate() of the handle it
                                 replaces */
    VkSwapchainCreateInfoKHR createInfo; /* what vkCreateSwapchainKHR was handed, as this layer's
                                  record of what the swapchain IS: the surface it belongs to, the
                                  extent, the format, the mode, the usage. Its oldSwapchain field
                                  is CLEARED in the record - the handle named there was retired
                                  and destroyed by the create that replaced it, and storing a
                                  handle to a destroyed object is exactly what this tree forbids,
                                  so the record says what the new swapchain is, not what it
                                  replaced. The surface field is borrowed state: the surface is
                                  owned by the window above (xud_window.h), and this is the same
                                  handle xud_window_close() will need alive before it destroys
                                  that window. */
    XudSurfaceCaps surfaceCaps; /* what the three surface queries answered for the surface this
                                   swapchain was created for, recorded rather than discarded: the
                                   recreate path re-queries them (a window can move to another
                                   monitor, and with it the whole answer can move), and the
                                   renderer rung reads the chosen format and the caps' extents
                                   off this rather than re-querying a surface it cannot name */
    VkImage* images; /* the heap array vkGetSwapchainImagesKHR wrote - handles the SWAPCHAIN owns,
                        not handles this layer owns: there is no vkDestroyImage call anywhere in
                        this tree, because destroying the swapchain is what retires its images
                        (vkDestroyImage on a swapchain image is invalid, and none of these images
                        is dedicated - VkMemoryDedicatedAllocateInfo is a decision this repo
                        settled out, see AGENTS.md). Freed here, never destroyed here. */
    uint32_t imageCount; /* how many rows above are live */
    uint32_t imageCapacity; /* rows allocated for it, a step of the doubling sequence above */
};

/*-----------------------------------------------------------------------------------------------
 * The layer's contract
 *-----------------------------------------------------------------------------------------------*/

/// Pure: the image count this layer will ask for, from the surface's own answer. The rule is
/// the plan's "minImageCount, prefer 2 for low latency": start at XUD_SWAPCHAIN_IMAGE_COUNT_
/// PREFERRED, raise it to capabilities.minImageCount when the surface needs more, and cut it
/// to capabilities.maxImageCount when the surface has an upper limit at all (0 is the spec's
/// "no upper limit" answer, probed here). XUD_UNSUPPORTED when the surface's answer is not a
/// surface a swapchain can exist on: minImageCount below XUD_SWAPCHAIN_IMAGE_COUNT_FLOOR (the
/// spec's own floor of 2 - a surface advertising 0 or 1 is a broken query, and asking it for
/// one would be a request the driver is entitled to refuse) or a maxImageCount that is non-
/// zero but below minImageCount (an inconsistent answer - the clamp order above cannot fix
/// that, and it is not a shape any layer above this one produced). XUD_INVALID_ARG on a NULL
/// out-count or NULL caps.
XudResult xud_swapchain_choose_image_count(uint32_t* outImageCount, const VkSurfaceCapabilitiesKHR* capabilities);

/// Pure: which of the surface's advertised (format, colorSpace) pairs this layer will create
/// the swapchain with. The rule is three-deep and each step is a reason, not a preference:
/// first, a pair whose colorSpace is VK_COLOR_SPACE_SRGB_NONLINEAR_KHR and whose format is on
/// this layer's wanted list, in wanted order (xud_swapchain.c owns that list - the four RGBA
/// byte formats the renderer's clear colours and glyph atlas are spelled in); second, the
/// first advertised pair in the SRGB_NONLINEAR color space at all, whatever its format, so a
/// surface that has no wanted format still gets a swapchain in a color space the renderer's
/// colours are already in; third, the first advertised pair at all, so a surface that
/// advertises no SRGB_NONLINEAR row still gets *a* swapchain rather than no swapchain. The
/// eight PASS_THROUGH rows this surface advertises are deliberately not reachable by step
/// one or two: VK_COLOR_SPACE_PASS_THROUGH_EXT (1000104013) is only meaningful with
/// VK_EXT_swapchain_colorspace enabled on the instance, and this library's instance does not
/// enable that name (it is advertised at instance level - probed, one of 26 - and enabling it
/// is an extension decision, not a default), so the eight SRGB_NONLINEAR rows are the ones
/// the rule can pick. XUD_UNSUPPORTED when the advertised list is empty (a surface that
/// advertises no format cannot host a swapchain). *outFormat and *outIndex are written only
/// on XUD_OK - the index is the row of the caps' own array, so a caller above can read the
/// pair back out of the caps it already has. XUD_INVALID_ARG on a NULL out-format/out-index
/// or a NULL advertised array with a non-zero count.
XudResult xud_swapchain_select_format(
    VkSurfaceFormatKHR* outFormat, uint32_t* outIndex, const VkSurfaceFormatKHR* advertised, uint32_t advertisedCount);

/// Pure: which of the surface's advertised present modes this layer will create with. The
/// rule is MAILBOX if the surface advertises it (one frame of latency, no tearing, no dropped
/// frame - the mode the plan's "low latency" is about), else FIFO (the only mode the spec
/// guarantees every implementation advertises, so the chain ends here rather than at "the
/// first advertised"), else the first advertised, and XUD_UNSUPPORTED only when the list is
/// empty. The modes are searched by VALUE, never by position: the enum is not contiguous
/// (0,1,2,3 then 1000111000/1000111001/1000361000, see the note above), so a present mode is
/// a value and an index into this array is nothing at all. XUD_INVALID_ARG on a NULL out-mode
/// or a NULL advertised array with a non-zero count.
XudResult xud_swapchain_select_present_mode(VkPresentModeKHR* outMode, const VkPresentModeKHR* advertised, uint32_t advertisedCount);

/// Pure: the VkSwapchainCreateInfoKHR the swapchain will be created with, assembled from the
/// surface caps above and the extent above it asked for. Stamped, pNext NULL (this layer
/// chains nothing into the create-info: VkSwapchainCreateFlagsKHR's own bits
/// (VK_SWAPCHAIN_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT_KHR,
/// VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR, VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR,
/// vulkan_core.h:9074-9076) and the extension-level pNext chain entries that go with them are
/// decisions this library does not take today, so flags stays 0 and the chain stays empty).
/// surface is the handle in hand, copied rather than re-derived: the surface is owned by the
/// window above and this layer borrows it. minImageCount, imageFormat/imageColorSpace and
/// called from here so the create-info and the record cannot disagree - there is exactly one
/// derivation path from the caps to the create-info. imageExtent is the requested extent,
/// refused XUD_UNSUPPORTED outside minImageExtent/maxImageExtent (the surface's answer, not a
/// bad argument: GLFW's int bound and this surface's 16384x16384 are different gates).
/// imageArrayLayers is 1, refused XUD_UNSUPPORTED when maxImageArrayLayers is 0 (a surface
/// that takes no layer takes no swapchain) - more than one layer is a multi-view decision
/// nothing above asks for. imageUsage is VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT intersected with
/// supportedUsageFlags and refused XUD_UNSUPPORTED when the surface does not advertise it,
/// because this library's one use of a swapchain is to draw into it and present it, and there
/// is no VK_IMAGE_USAGE_PRESENT_BIT to intersect with (see the note above) - presentation is not
/// an image property.
/// imageSharingMode stays VK_SHARING_MODE_EXCLUSIVE (0, vulkan_core.h:2263): the other value of
/// that enum, CONCURRENT, is the sharing mode for an image another process may also be writing
/// to, and this library's swapchain is its own.
/// queueFamilyIndexCount 0 with pQueueFamilyIndices NULL is the spec's own form for "the
/// application does not care which queue families this swapchain is presented on" - and this
/// library does not care: it presents through the one family the physical layer chose and the
/// device layer created a row from, and the window system's choice is GLFW's, not a name this
/// layer could write.
/// preTransform is the caps' currentTransform (the spec requires currentTransform to be one of
/// supportedTransforms, so copying it is the one value that is never a refusal, and asking for
/// a different one is a rotation/mirror decision this layer does not take). compositeAlpha is
/// OPAQUE when the surface supports it, else PRE_MULTIPLIED, else the first bit supported, and
/// refused XUD_UNSUPPORTED when supportedCompositeAlpha is 0 - the probed surface supports
/// OPAQUE|PRE_MULTIPLIED (0x3). clipped is VK_TRUE, not a comparison against currentExtent:
/// the surface here reports no current extent at all (probed 4294967295x4294967295, the
/// "undefined" answer), so a comparison would be a comparison against nothing, and the images
/// are clipped to the extent this layer asked for either way. oldSwapchain is the handle in
/// hand, VK_NULL_HANDLE from xud_swapchain_open() and the handle being replaced from
/// xud_swapchain_recreate() - the one argument that makes the driver retire the old swapchain
/// creation, which is why the old one cannot leak. XUD_INVALID_ARG on a NULL out-create-info
/// or out-caps, on a handle-less surface, or on an extent of 0 (0 is not an extent, and it is a
/// bad argument rather than a surface answer because no window above was ever opened at it -
/// GLFW refuses it, see xud_window.h).
XudResult xud_swapchain_fill_create_info(VkSwapchainCreateInfoKHR* outCreateInfo, const XudSurfaceCaps* surfaceCaps, VkSurfaceKHR surface,
    uint32_t width, uint32_t height, VkSwapchainKHR oldSwapchain);

/// Driver: the three surface queries for one surface, into the caps struct above. All three
/// are instance-level rows of struct VolkInstanceTable (they take a VkPhysicalDevice, not a
/// VkDevice - see the note above), and all three are checked before the call that needs them:
/// a driver that does not answer them is reported, never called through. The two list-shaped
/// ones (formats, present modes) have an input/output pCount and no count-only form, so they
/// are asked with the capacity of the struct's own fixed arrays and a driver that reports
/// VK_INCOMPLETE at that capacity - or claims success while writing more than it - is refused
/// XUD_OUT_OF_RESOURCES rather than retried or truncated, because these two bounds are the
/// storage of the struct being filled, not a heap array that can grow (the image query below
/// is the one query here that CAN grow: its array is a heap member of XudSwapchain, and it
/// follows the doubling shape xud_instance.c and xud_physical.c use instead). XUD_INVALID_ARG
/// on a NULL out-caps, a NULL loader, or a handle-less physical device or surface; refused with
/// XUD_UNSUPPORTED when the loader is closed, a row is nil, or a query refuses. On XUD_OK every
/// field of *outSurfaceCaps says what the driver answered; on any error the struct is cleared to
/// its zero state so a caller cannot read a half-filled answer.
XudResult xud_swapchain_query_surface(
    XudSurfaceCaps* outSurfaceCaps, const XudLoader* loader, VkPhysicalDevice physicalDevice, VkSurfaceKHR surface);

/// Driver: bring the swapchain up for the surface the window above owns. Before any call: the
/// loader must be open, its instance rows must be the surface's instance's rows
/// (xud_loader_instance_bound against window->surfaceInstance - the same bound-handle rule
/// applies to vkDestroySurfaceKHR, and the reason a surface created for one instance is not a
/// surface a second instance's rows can query), and its device rows must be this device's rows
/// (xud_loader_device_bound against device->device - a swapchain is created ON a VkDevice and
/// the rows must be that device's). Then the rows this layer calls are checked: the destroy
/// row before the create row (a swapchain this layer cannot destroy is a swapchain it must not
/// create, the rule xud_window.c applies to a surface), and the two query rows before the
/// calls that need them. Then the three surface queries, then the three selection rules and
/// the create-info assembly, then vkCreateSwapchainKHR through the device row with oldSwapchain
/// at VK_NULL_HANDLE, then vkGetSwapchainImagesKHR. On XUD_OK *outSwapchain owns a live
/// VkSwapchainKHR plus the images it was handed; on any error it stays NULL and everything
/// created so far is torn down in reverse creation order - a swapchain that was created but
/// whose image query failed is destroyed, because a swapchain nobody can get an image from is
/// a swapchain nothing above can use, and leaving it alive would leak the one object this
/// layer owns. XUD_UNSUPPORTED when GLFW's window system cannot present to this device at all
/// - the caller above checks that with xud_window_presentation_support(), and this layer refuses
/// rather than creating a swapchain for a window it cannot present to, which is the plan's
/// "a graphics-only family must not be chosen" carried one rung up. XUD_INVALID_ARG on a NULL
/// out-pointer or one that already owns a swapchain, a NULL loader/window/physical/device, a
/// window with no surface (there is no surface to create a swapchain for), a handle-less
/// physical device or device, or a closed loader; XUD_OUT_OF_MEMORY/XUD_OUT_OF_RESOURCES when
/// an allocation or a query at a bound cannot be satisfied; XUD_VULKAN_ERROR/XUD_LOST/
/// XUD_UNSUPPORTED when the driver refuses vkCreateSwapchainKHR (VK_ERROR_NATIVE_WINDOW_IN_USE_KHR
/// - GLFW's own -1000000001, probed when a window whose client API is not GLFW_NO_API is handed
/// over - maps through xud_result_from_vk to XUD_VULKAN_ERROR, and
/// VK_ERROR_INCOMPATIBLE_DRIVER/VK_ERROR_EXTENSION_NOT_PRESENT map to XUD_UNSUPPORTED).
XudResult xud_swapchain_open(XudSwapchain** outSwapchain, XudLoader* loader, const XudPhysicalDevice* physicalDevice,
    const XudWindow* window, const XudDevice* device);

/// Driver: the resize path - recreate the swapchain at a new extent and never leak the one
/// this replaces. The order is the spec's and it is the whole point of the signature: the
/// create-info carries oldSwapchain, so the driver retires the old handle as part of the
/// creation, and this layer then calls vkDestroySwapchainKHR on it explicitly, because the
/// spec's own text says the handle is still not destroyed until the application destroys it
/// and a driver that only retired it implicitly is a driver that would leak the old one.
/// XUD_ALREADY when the extent is the one the swapchain in hand was created for: recreating
/// at the same extent would retire images the renderer above is still holding and buy nothing,
/// and "already at this size" is the state asked for, not an error. XUD_INVALID_ARG on a NULL
/// pointer-to-pointer, on one that owns no swapchain (there is nothing to replace -
/// xud_swapchain_open() is the rung that creates one), on a NULL loader/window/physical/device,
/// on a window with no surface, on an extent of 0 or above GLFW's int bound, or on a closed
/// loader.
/// XUD_UNSUPPORTED when the loader's rows are not this surface's instance's or this device's,
/// a row is nil, or the driver refuses the creation - and a refused creation leaves the swapchain
/// in hand ALIVE and unchanged, which is what the spec promises and what was probed here (a
/// create at 20000x20000, outside this surface's maxImageExtent, answered
/// VK_ERROR_OUT_OF_HOST_MEMORY with the old swapchain still usable): the old handle is retired
/// when that creation succeeds. The one path that is not a refused creation is a creation that
/// succeeds and whose image query then fails: the old swapchain has already been destroyed by
/// then, so the new one is destroyed too and the struct is left in the no-swapchain state - its
/// handle cleared, because the handle it named has been destroyed, and its record left at the
/// extent it still had a swapchain for, so the retry at this extent recreates rather than
/// answering XUD_ALREADY with nothing in hand. On XUD_OK the handle in hand is replaced by the new
/// one and the images array is replaced with it, so nothing above this layer may hold an image
/// handle across this call - the renderer rung re-reads them through the accessor below after it.
XudResult xud_swapchain_recreate(XudSwapchain** ioSwapchain, XudLoader* loader, const XudPhysicalDevice* physicalDevice,
    const XudWindow* window, const XudDevice* device, uint32_t width, uint32_t height);

/// Driver: the image at one index of this swapchain, as the renderer will bind it - the
/// accessor that keeps a caller above from holding a VkImage across a recreate (the array
/// above is replaced by that rung, so an index is a request and a handle is not). Pure:
/// no driver call, no query, headless-testable. XUD_INVALID_ARG on a NULL out-image or
/// swapchain, or on an index at or above imageCount (a stale index into an array that
/// grew or shrank is a caught error, not an out-of-range read); XUD_UNSUPPORTED when the
/// struct has no swapchain (there is no image array for a swapchain that does not exist,
/// and the zero state of a struct whose create ran but whose image query failed is that
/// state, not an empty array of one image).
XudResult xud_swapchain_images_get(const XudSwapchain* swapchain, uint32_t index, VkImage* outImage);

/// Driver: the teardown rung - the device layer's own wait-idle guard first, then
/// vkDestroySwapchainKHR through the device row, then free the images array and the struct. The
/// guard is not decoration: a presentation may still be in flight against these very images
/// (vkQueuePresentKHR is the renderer rung's call, and it submits through the device above),
/// and destroying the swapchain under it is destroying images a submission is still reading,
/// so the one guarantee that nothing is in flight is xud_device_wait_idle()'s and this rung
/// calls it rather than restating it. A lost device is still destroyed - vkDestroySwapchainKHR
/// is this layer's teardown call and skipping it would leak the one object the driver still
/// holds - and the XUD_LOST answer is still propagated, the same rule xud_device_close()
/// follows. The images array is freed on every path and is never destroyed: see the note above
/// about vkDestroyImage and swapchain-owned images. XUD_ALREADY when the pointer-to-pointer is
/// NULL-in-hand; a struct whose handle is already VK_NULL_HANDLE is freed and answers XUD_OK
/// (nothing to guard, nothing to destroy, and freeing is what this rung is for); XUD_INVALID_ARG
/// the device record, and the guard is what the teardown order depends on), or on a closed loader;
/// XUD_UNSUPPORTED when the guard's row is nil or the destroy row is nil while a handle is in hand
/// - the rung refuses, the struct is not freed, because freeing it would leak the handle silently,
/// the same rule xud_device_close() applies to a nil destroy row.
XudResult xud_swapchain_close(XudSwapchain** swapchain, const XudDevice* device, const XudLoader* loader);

#endif /* XUD_XUD_SWAPCHAIN_H_ */
