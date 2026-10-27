/***********************************************************************************************
 *   xud_vulkan.c - the shared vocabulary of the Vulkan-calling layers
 *   Owns: the two VkResult -> XudResult funnels (the general one and the swapchain-boundary
 *         one, which is the only place a positive VkResult is not a fault) and the fixed-buffer
 *         name helpers.
 *   Layer: shared by every layer above the loader; it calls no driver function.
 ***********************************************************************************************/

#include "xud_vulkan.h"
#include "xud.h"
#include <assert.h>
#include <string.h>
#include <vulkan/vulkan.h>

XudResult xud_result_from_vk(VkResult vkResult) {
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

/* The second funnel, and the reason there are two: vkAcquireNextImageKHR and vkQueuePresentKHR
   are the only two entry points in this tree whose VkResult carries a code that is not a verdict
   on the CALL but a verdict on the swapchain the call was made against. For those two the image
   is usable and the presentation happens, so the frame runs and records that a recreate is
   wanted; everywhere else a positive code is a driver fault and the general funnel above is the
   right mapping. The two codes' Vulkan wording is quoted in xud_vulkan.h above - the out-of-date
   one is the harsher of the pair and still says "the image this call handed back is still valid".
*/
XudResult xud_swapchain_result_from_vk(VkResult vkResult, uint32_t* outRecreateWanted) {
    if (outRecreateWanted == NULL) {
        return XUD_INVALID_ARG;
    }

    /* Written before the switch rather than only on the rows below: the flag is an out-param and
       an out-param that is undefined on a path is a question a caller cannot ask. */
    *outRecreateWanted = 0u;

    switch (vkResult) {
    case VK_SUCCESS:
        return XUD_OK;
    /* Both of these are "the frame ran, the swapchain did not": the image is acquired/presented
       as asked and the only thing left to do is create a swapchain that matches the surface, at
       the next acquire rather than in the middle of this frame. */
    case VK_SUBOPTIMAL_KHR:
    case VK_ERROR_OUT_OF_DATE_KHR:
        *outRecreateWanted = 1u;
        return XUD_OK;
    /* Everything else, including VK_ERROR_SURFACE_LOST_KHR and VK_ERROR_INCOMPATIBLE_DISPLAY_KHR:
       those two name a lost surface and an unusable display mode, which are refusals rather than
       a swapchain shape a recreate answers, so they go to the general funnel and come back XUD_VULKAN_ERROR.
    */
    default:
        return xud_result_from_vk(vkResult);
    }
}

bool xud_name_equal(const char* left, const char* right) {
    if (left == NULL || right == NULL)
        return left == right;
    return strcmp(left, right) == 0;
}

XudResult xud_name_copy(char* destination, uint32_t destinationSize, const char* source) {
    const uint32_t sourceLength = (source != NULL) ? (uint32_t)strlen(source) : 0u;

    if (destination == NULL || source == NULL || destinationSize == 0u)
        return XUD_INVALID_ARG;
    if (sourceLength + 1u > destinationSize)
        return XUD_INVALID_ARG;

    memcpy(destination, source, (size_t)sourceLength + 1u);
    return XUD_OK;
}
