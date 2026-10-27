/***********************************************************************************************
 *   xud_vulkan.h - the shared vocabulary of the Vulkan-calling layers
 *   Owns: the two VkResult -> XudResult funnels (the general one every layer funnels through,
 *         and the swapchain-boundary one the acquire and the present funnel through instead,
 *         which is the only place in the tree where a positive VkResult is not a fault) and the
 *         fixed-buffer name helpers every layer needs while copying extension/layer names
 *         around. Nothing here calls the driver and nothing here creates a Vulkan object.
 *   Layer: internal, like xud_loader.h - tests may include it, user code must not. Sees
 *          xud.h (the XudResult codes), volk.h (VK_NO_PROTOTYPES, which must come before the
 *          Vulkan header - see xud_loader.h's include-order note) and <vulkan/vulkan.h> decla-
 *          rations only.
 ***********************************************************************************************/
#ifndef XUD_VULKAN_H_
#define XUD_VULKAN_H_

#include "volk.h" /* Required for: VK_NO_PROTOTYPES - must precede <vulkan/vulkan.h>, see xud_loader.h */
#include "xud.h"
#include <assert.h>
#include <stdbool.h>
#include <vulkan/vulkan.h>

/// Translate one Vulkan result code into the library's own vocabulary - the one funnel
/// every Vulkan entry point in every layer passes its VkResult through, so a VkResult
/// never reaches a caller. Pure: no driver call, no state, headless-testable.
///
///   VK_SUCCESS                                              -> XUD_OK
///   VK_ERROR_DEVICE_LOST                                    -> XUD_LOST
///   VK_ERROR_OUT_OF_HOST_MEMORY                             -> XUD_OUT_OF_MEMORY
///   VK_ERROR_OUT_OF_DEVICE_MEMORY,
///   VK_ERROR_TOO_MANY_OBJECTS                               -> XUD_OUT_OF_RESOURCES
///   VK_ERROR_INCOMPATIBLE_DRIVER,
///   VK_ERROR_LAYER_NOT_PRESENT,
///   VK_ERROR_EXTENSION_NOT_PRESENT,
///   VK_ERROR_FEATURE_NOT_PRESENT                            -> XUD_UNSUPPORTED
///   everything else (VK_ERROR_INITIALIZATION_FAILED, VK_ERROR_MEMORY_MAP_FAILED,
///   VK_ERROR_FORMAT_NOT_SUPPORTED, VK_ERROR_NOT_PERMITTED, VK_ERROR_VALIDATION_FAILED,
///   the positive-but-unexpected VK_NOT_READY / VK_TIMEOUT / VK_EVENT_SET / VK_EVENT_RESET
///   / VK_INCOMPLETE, and any code that is not in the enum) -> XUD_VULKAN_ERROR
///
/// VK_INCOMPLETE is not an error in Vulkan, but it means "the list you asked for was cut"
/// - no layer here may treat that as success, so it maps to XUD_VULKAN_ERROR and a caller
/// that wants the whole list has to grow its array and re-call before mapping.
XudResult xud_result_from_vk(VkResult vkResult);

/// The SECOND funnel, for the two entry points whose VkResult is not just "did it work":
/// vkAcquireNextImageKHR and vkQueuePresentKHR are the two calls that can answer with the
/// swapchain's own out-of-band codes, and for those two a positive code is not a fault - it
/// is a fact about the swapchain the frame is drawing through.
///
///   VK_SUCCESS                    -> XUD_OK,           *outRecreateWanted = 0
///   VK_SUBOPTIMAL_KHR             -> XUD_OK,           *outRecreateWanted = 1
///   VK_ERROR_OUT_OF_DATE_KHR      -> XUD_OK,           *outRecreateWanted = 1
///   everything else               -> xud_result_from_vk(vkResult), *outRecreateWanted = 0
///
/// The two codes' Vulkan meanings, spelled out because they are the whole point of the
/// funnel: VK_SUBOPTIMAL_KHR is "the swapchain works, it is just not the most optimal one
/// for this surface - present as usual and create a better one", and VK_ERROR_OUT_OF_DATE_KHR
/// is "the swapchain no longer matches the surface's capabilities - create a new one BEFORE
/// THE NEXT ACQUIRE, but the image this call handed back is still valid and can still be
/// presented". Neither is a refused frame: the image is usable and the presentation happens,
/// so the frame runs and records that a recreate is wanted, and the recreate itself is the
/// resize step's. Refusing either one is what turns a window resize into a frame that never
/// draws - which is the bug this funnel exists to prevent, and the reason the general funnel
/// above still maps both to XUD_VULKAN_ERROR: everywhere outside those two calls a positive
/// code IS a driver fault, and a second funnel is what keeps that difference explicit rather
/// than folding it into one table every caller would have to know the exceptions of.
///
/// XUD_INVALID_ARG on a NULL out-flag; the flag is written on every path, including the
/// error paths, so a caller can read it without knowing the code first.
XudResult xud_swapchain_result_from_vk(VkResult vkResult, uint32_t* outRecreateWanted);

/* The two codes the funnel above treats as a frame that ran must be two distinct codes and
   neither may be VK_SUCCESS, or the funnel's "OK" row would be a lie about one of them. */
static_assert(VK_SUBOPTIMAL_KHR != VK_SUCCESS, "suboptimal is not success - it is success plus a recreate");
static_assert(VK_ERROR_OUT_OF_DATE_KHR != VK_SUCCESS, "out-of-date is not success - it is a usable image plus a recreate");
static_assert(VK_SUBOPTIMAL_KHR != VK_ERROR_OUT_OF_DATE_KHR, "the two out-of-band codes are two codes, not one spelled twice");

/// Compare two NUL-terminated names byte for byte (extension and layer names are ASCII).
/// Two NULL pointers compare equal; one NULL and one not does not.
bool xud_name_equal(const char* left, const char* right);

/// Copy `source` into `destination`, a fixed buffer of `destinationSize` bytes
/// (extension names and layer names are both VK_MAX_EXTENSION_NAME_SIZE bytes).
/// XUD_INVALID_ARG when the name does not fit - a driver that lies about a name length
/// must never be able to write past the buffer.
XudResult xud_name_copy(char* destination, uint32_t destinationSize, const char* source);

#endif /* XUD_VULKAN_H_ */
