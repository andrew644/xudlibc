/***********************************************************************************************
 *   xud_vulkan.c - the shared vocabulary of the Vulkan-calling layers
 *   Owns: the VkResult -> XudResult mapping and the fixed-buffer name helpers.
 *   Layer: shared by every layer above the loader; it calls no driver function.
 ***********************************************************************************************/

#include <assert.h>
#include <string.h>
#include "xud.h"
#include "xud_vulkan.h"
#include <vulkan/vulkan.h>

XudResult xud_result_from_vk(VkResult vkResult)
{
    switch (vkResult) {
        case VK_SUCCESS:
            return XUD_OK;
        case VK_ERROR_DEVICE_LOST:
            return XUD_LOST;
        /* Host allocation failed: the caller can retry, nothing is broken. */
        case VK_ERROR_OUT_OF_HOST_MEMORY:
            return XUD_OUT_OF_MEMORY;
        /* Device-side resource exhaustion: retrying with smaller requests can work. */
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        case VK_ERROR_TOO_MANY_OBJECTS:
            return XUD_OUT_OF_RESOURCES;
        /* "This driver/layer/extension/feature is simply not there" - the caller must
           degrade or refuse, not retry. */
        case VK_ERROR_INCOMPATIBLE_DRIVER:
        case VK_ERROR_LAYER_NOT_PRESENT:
        case VK_ERROR_EXTENSION_NOT_PRESENT:
        case VK_ERROR_FEATURE_NOT_PRESENT:
            return XUD_UNSUPPORTED;
        default:
            /* VK_ERROR_INITIALIZATION_FAILED, VK_ERROR_MEMORY_MAP_FAILED,
               VK_ERROR_FORMAT_NOT_SUPPORTED, VK_ERROR_NOT_PERMITTED,
               VK_ERROR_VALIDATION_FAILED, VK_ERROR_UNKNOWN, the positive codes
               VK_NOT_READY / VK_TIMEOUT / VK_EVENT_SET / VK_EVENT_RESET / VK_INCOMPLETE,
               and anything a newer header adds: the call was neither a clean success
               nor one of the four "not there" shapes, so it is a driver fault. */
            return XUD_VULKAN_ERROR;
    }
}

bool xud_name_equal(const char *left, const char *right)
{
    if (left == NULL || right == NULL) return left == right;
    return strcmp(left, right) == 0;
}

XudResult xud_name_copy(char *destination, uint32_t destinationSize, const char *source)
{
    const uint32_t sourceLength = (source != NULL) ? (uint32_t)strlen(source) : 0u;

    if (destination == NULL || source == NULL || destinationSize == 0u) return XUD_INVALID_ARG;
    if (sourceLength + 1u > destinationSize) return XUD_INVALID_ARG;

    memcpy(destination, source, (size_t)sourceLength + 1u);
    return XUD_OK;
}
