/***********************************************************************************************
 *   xud_vulkan.h - the shared vocabulary of the Vulkan-calling layers
 *   Owns: VkResult -> XudResult translation, and the fixed-buffer name helpers every layer
 *         needs while copying extension/layer names around. Nothing here calls the driver
 *         and nothing here creates a Vulkan object.
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

/// Compare two NUL-terminated names byte for byte (extension and layer names are ASCII).
/// Two NULL pointers compare equal; one NULL and one not does not.
bool xud_name_equal(const char* left, const char* right);

/// Copy `source` into `destination`, a fixed buffer of `destinationSize` bytes
/// (extension names and layer names are both VK_MAX_EXTENSION_NAME_SIZE bytes).
/// XUD_INVALID_ARG when the name does not fit - a driver that lies about a name length
/// must never be able to write past the buffer.
XudResult xud_name_copy(char* destination, uint32_t destinationSize, const char* source);

#endif /* XUD_VULKAN_H_ */
