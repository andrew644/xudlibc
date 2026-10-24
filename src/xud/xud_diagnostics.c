/***********************************************************************************************
 *
 *   xud_diagnostics.c - the diagnostics layer: advertised validation layers, validation
 *          features, and the debug messenger that carries the log side
 *
 *   Owns : the diagnostics layer. The pure parts (validation-feature assembly, messenger
 *          create-info assembly, layer selection, severity naming, the log callback itself) are
 *          separate functions so the headless test run covers them with no driver present; the
 *          driver parts are vkEnumerateInstanceLayerProperties, vkCreateDebugUtilsMessengerEXT
 *          and vkDestroyDebugUtilsMessengerEXT, every one of them reached through the loader's
 *          table - this file never names a vk* function directly either.
 *   Layer: internal, above the loader and beside the instance. The two messenger entry points are
 *          rows of the loader's struct VolkInstanceTable: vkGetInstanceProcAddr answers NULL for
 *          them unless VK_EXT_debug_utils is enabled on the instance in hand, so the row is NULL
 *          here on such an instance and XUD_UNSUPPORTED is the honest answer. They are no longer
 *          resolved on demand into a local and never cached in a per-name slot - the staleness r
 *          ule xud_loader.h used to state (a resolve cached across an instance recreation would b
 *          e a stale pointer into a layer that is gone) is now enforced by the bind being a rung:
 *          xud_loader_bind_instance() overwrites every row of the table, and xud_loader_instance_
 *          bound() names the instance the rows belong to, so a handle kept across a recreation f
 *          ails the check before any row is read.
 *   Verified against lavvk + the Khronos validation layer here (2026-10-16): three instance layers
 *   advertised, vkEnumerateInstanceLayerProperties answers 3 with no handle in hand; 26
 * instance-level extensions advertised, VK_EXT_debug_utils among them and
 * VK_EXT_validation_features NOT (checked both with and without
 * VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation in the environment, which is why the
 * validation-feature request below is assembled but reaches a VkInstance only on a driver that does
 * advertise it - see xud_instance_all_names_enabled); vkCreateInstance with all three layers
 * enabled and VK_EXT_debug_utils requested returns VK_SUCCESS; vkCreateDebugUtilsMessengerEXT
 *   resolved through that live instance returns a messenger and the callback below fires for the
 *   validation layer's own messages. What is deliberately NOT done: putting VkDebugUtilsMessenger
 *   CreateInfoEXT on the VkInstanceCreateInfo pNext chain - this loader segfaults inside vkCreate
 *   Instance with a messenger on that chain (probed twice, 2026-10-16), while the standalone
 * create/destroy pair above works cleanly. VkValidationFeaturesEXT on that chain IS accepted
 * (probed: VK_SUCCESS with the chain, with and without features set, even with the extension
 * unrequested) and is kept.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud_diagnostics.h"
#include "xud.h"

#include <assert.h> /* Required for: static_assert, the compile-time checks below */
#include <stdio.h> /* Required for: fprintf, fflush - the log side of a debug build goes to stderr */
#include <stdlib.h> /* Required for: calloc, free */
#include <string.h> /* Required for: memcpy, memcpy-based comparison (xud_name_equal) */

/* The two messenger rows this layer calls through must be pointer-sized, the same rule xud_loa
   der.h checks for the table pair as a whole. The rule used to be about a resolve that handed ba
   ck one pointer-sized word; Volk's rows are PFN typedefs, so it is a property of the headers. T
   he 2026-10-16 note that followed - VK_EXT_debug_utils is among the 26 instance-level extension
   s lavvk advertises, so the pair answers non-NULL through the instance xud_instance.c creates,
   and vkDestroyDebugUtilsMessengerEXT takes three arguments in these headers and the owning VkIn
   stance must still be alive at the call, which is why xud_close_context() destroys the messenge
   r inside xud_instance_destroy() before the instance goes away - is a fact about the headers an
   d the driver, not about the loader, and still holds. */
static_assert(sizeof(PFN_vkCreateDebugUtilsMessengerEXT) == sizeof(void*),
    "messenger PFN must be pointer-sized");
static_assert(sizeof(PFN_vkDestroyDebugUtilsMessengerEXT) == sizeof(void*),
    "messenger PFN must be pointer-sized");
static_assert(
    sizeof(VkDebugUtilsMessengerEXT) == sizeof(void*), "messenger handle must be pointer-sized");

/* The instance-level extensions this layer wants, in request order. The instance layer owns the
   decision of what actually goes into VkInstanceCreateInfo (it intersects this with what the
   driver advertised); this list is the request. VK_EXT_debug_utils is what makes the two
   messenger entry points resolvable at all; VK_EXT_validation_features is what makes the
   validation-feature request below legal. */
static const char* const XUD_DIAGNOSTICS_WANTED_EXTENSIONS[XUD_DIAGNOSTICS_WANTED_MAX] = {
    VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
    VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME,
};

static_assert(XUD_DIAGNOSTICS_WANTED_MAX == 2u, "the wanted list has exactly two names");
static_assert(
    XUD_MAX_ENABLED_VALIDATION_FEATURES >= 2u, "the enabled feature list must fit the struct");

/* The name of one severity bit as it is printed. 0, a combination of bits, and the MAX_ENUM
   sentinel are not severities: they name nothing and return NULL. */
const char* xud_diagnostics_severity_name(VkDebugUtilsMessageSeverityFlagBitsEXT severity) {
    switch (severity) {
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
        return "VERBOSE";
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
        return "INFO";
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
        return "WARNING";
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
        return "ERROR";
    default:
        return NULL;
    }
}

/* The one callback every xudlibc messenger installs. It counts what it saw into the struct it
   was handed as pUserData (the messenger create-info is assembled from that same struct, see
   xud_diagnostics_fill_messenger_create_info) and writes one line to stderr, so a validation
   complaint is visible without a debugger and the counts are visible to the tests. VK_FALSE is
   returned always: a message never aborts a driver. */
VKAPI_ATTR VkBool32 VKAPI_CALL xud_diagnostics_message_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageTypes,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData, void* userData) {
    XudDiagnostics* diagnostics = NULL;
    const char* severityName = NULL;
    const char* message = NULL;

    severityName = xud_diagnostics_severity_name(messageSeverity);
    message = (callbackData != NULL) ? callbackData->pMessage : NULL;

    if (userData != NULL) {
        diagnostics = (XudDiagnostics*)userData;
        diagnostics->messagesReceived += 1u;
        if ((messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0u)
            diagnostics->errorMessages += 1u;
    }

    /* messageIdNumber is int32_t, so the fallback arm is (int32_t)0 and the format is %d: the two
       arms of `?:` must agree in type, and an unsigned 0u there is a sign-compare error under
       -Werror, not a warning. The id number is a signed field in the extension, so %d is what
       prints it honestly. */
    (void)fprintf(stderr, "xudlibc[%s/%s/#%d]: %s\n",
        (severityName != NULL) ? severityName : "SEVERITY?",
        ((messageTypes & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0u) ? "validation"
                                                                                : "general",
        (callbackData != NULL) ? callbackData->messageIdNumber : (int32_t)0,
        (message != NULL) ? message : "(no message)");
    (void)fflush(stderr);
    return VK_FALSE;
}

XudResult xud_diagnostics_wanted_extensions(
    const char* outNames[XUD_DIAGNOSTICS_WANTED_MAX], uint32_t* outCount) {
    uint32_t index = 0u;

    if ((outNames == NULL) || (outCount == NULL))
        return XUD_INVALID_ARG;

    *outCount = 0u;
    for (index = 0u; index < XUD_DIAGNOSTICS_WANTED_MAX; index++)
        outNames[index] = XUD_DIAGNOSTICS_WANTED_EXTENSIONS[index];
    *outCount = XUD_DIAGNOSTICS_WANTED_MAX;
    return XUD_OK;
}

XudResult xud_diagnostics_fill_validation_features(XudDiagnostics* diagnostics) {
    if (diagnostics == NULL)
        return XUD_INVALID_ARG;

    diagnostics->enabledValidationFeatures[0] = VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT;
    diagnostics->enabledValidationFeatures[1]
        = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;

    diagnostics->validationFeatures = (VkValidationFeaturesEXT) {
        .sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
        .pNext = NULL,
        .enabledValidationFeatureCount = 2u,
        .pEnabledValidationFeatures = diagnostics->enabledValidationFeatures,
        .disabledValidationFeatureCount = 0u,
        .pDisabledValidationFeatures = NULL,
    };
    return XUD_OK;
}

XudResult xud_diagnostics_fill_messenger_create_info(XudDiagnostics* diagnostics) {
    if (diagnostics == NULL)
        return XUD_INVALID_ARG;

    diagnostics->messengerCreateInfo = (VkDebugUtilsMessengerCreateInfoEXT) {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .pNext = NULL,
        .flags = 0u,
        .messageSeverity
        = (VkDebugUtilsMessageSeverityFlagsEXT)(VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT),
        .messageType = (VkDebugUtilsMessageTypeFlagsEXT)(VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT),
        .pfnUserCallback = xud_diagnostics_message_callback,
        .pUserData = diagnostics,
    };
    return XUD_OK;
}

/* Everything advertised, in advertised order, never a name this layer knows. A truncated layer
   list would mean a run that is not validated without saying so, so it is an error, not a
   partial success. */
XudResult xud_diagnostics_select_layers(
    XudDiagnostics* outDiagnostics, const VkLayerProperties* advertised, uint32_t advertisedCount) {
    uint32_t index = 0u;

    if ((outDiagnostics == NULL) || ((advertised == NULL) && (advertisedCount != 0u)))
        return XUD_INVALID_ARG;

    outDiagnostics->enabledLayerCount = 0u;
    if (advertisedCount > XUD_MAX_DIAGNOSTIC_LAYERS)
        return XUD_OUT_OF_RESOURCES;

    for (index = 0u; index < advertisedCount; index++) {
        XudResult result = xud_name_copy(outDiagnostics->enabledLayerNameStorage[index],
            VK_MAX_EXTENSION_NAME_SIZE, advertised[index].layerName);
        if (result != XUD_OK) {
            outDiagnostics->enabledLayerCount = 0u;
            return result;
        }
        /* The row's address, not the row: ppEnabledLayerNames is a const char *const *, and a
           char (*)[256] is not assignment-compatible with it (verified: gcc rejects the array). */
        outDiagnostics->enabledLayerNamePointers[outDiagnostics->enabledLayerCount]
            = outDiagnostics->enabledLayerNameStorage[index];
        outDiagnostics->enabledLayerCount += 1u;
    }
    return XUD_OK;
}

/* The advertised instance layers as a heap array the caller frees. Same doubling shape as the
   instance-extension query in xud_instance.c, and the same reason:
   vkEnumerateInstanceLayerProperties has no count-only form, pCount is input/output, and
   VK_INCOMPLETE means "the list was cut". The slot is a global-level one, filled by loader
   bring-up; a NULL slot means the loader is not open, which is XUD_UNSUPPORTED. */
XudResult xud_diagnostics_query_layers(
    const XudLoader* loader, VkLayerProperties** outProperties, uint32_t* outCount) {
    VkLayerProperties* items = NULL;
    uint32_t capacity = XUD_DIAGNOSTIC_QUERY_MIN_CAPACITY;
    uint32_t count = 0u;
    VkResult vk;

    if ((loader == NULL) || (outProperties == NULL) || (outCount == NULL))
        return XUD_INVALID_ARG;
    if (loader->enumerateInstanceLayerProperties == NULL)
        return XUD_UNSUPPORTED;

    *outProperties = NULL;
    *outCount = 0u;

    for (;;) {
        items = calloc(capacity, sizeof(VkLayerProperties));
        if (items == NULL)
            return XUD_OUT_OF_MEMORY;

        count = capacity;
        vk = loader->enumerateInstanceLayerProperties(&count, items);

        if ((vk == VK_SUCCESS) && (count <= capacity)) {
            *outProperties = items;
            *outCount = count;
            return XUD_OK;
        }

        free(items);

        if ((vk != VK_INCOMPLETE) && ((vk != VK_SUCCESS) || (count <= capacity)))
            return xud_result_from_vk(vk); /* a real failure: mapped, not retried */
        if (capacity >= XUD_DIAGNOSTIC_QUERY_MAX_CAPACITY)
            return XUD_VULKAN_ERROR; /* the list is longer than the bound we allow */
        capacity *= 2u;
    }
}

XudResult xud_diagnostics_open(XudDiagnostics** outDiagnostics, XudLoader* loader) {
    XudDiagnostics* diagnostics = NULL;
    VkLayerProperties* advertised = NULL;
    XudResult result = XUD_INVALID_ARG;
    uint32_t advertisedCount = 0u;

    if ((outDiagnostics == NULL) || (*outDiagnostics != NULL) || (loader == NULL))
        return XUD_INVALID_ARG;

    diagnostics = calloc(1, sizeof(XudDiagnostics));
    if (diagnostics == NULL)
        return XUD_OUT_OF_MEMORY;

    /* Nothing exists yet: the messenger is created once an instance does, and the counters
       start empty. calloc already zeroed both handles, which is VK_NULL_HANDLE here. */
    diagnostics->messenger = VK_NULL_HANDLE;
    diagnostics->instance = VK_NULL_HANDLE;

    result = xud_diagnostics_query_layers(loader, &advertised, &advertisedCount);
    if (result != XUD_OK) {
        if (advertised != NULL)
            free(advertised);
        free(diagnostics);
        return result;
    }

    result = xud_diagnostics_select_layers(diagnostics, advertised, advertisedCount);
    free(advertised); /* the selection was copied into the struct's own storage */
    if (result != XUD_OK) {
        free(diagnostics);
        return result;
    }

    result = xud_diagnostics_fill_validation_features(diagnostics);
    if (result != XUD_OK) {
        free(diagnostics);
        return result;
    }

    result = xud_diagnostics_fill_messenger_create_info(diagnostics);
    if (result != XUD_OK) {
        free(diagnostics);
        return result;
    }

    *outDiagnostics = diagnostics;
    return XUD_OK;
}

XudResult xud_diagnostics_create_messenger(
    XudDiagnostics* diagnostics, XudLoader* loader, VkInstance instance) {
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    XudResult result = XUD_INVALID_ARG;
    VkResult vk;

    if ((diagnostics == NULL) || (loader == NULL) || (!xud_loader_is_open(loader)))
        return XUD_INVALID_ARG;
    if (instance == VK_NULL_HANDLE)
        return XUD_INVALID_ARG;
    if (diagnostics->messenger != VK_NULL_HANDLE)
        return XUD_ALREADY;

    /* The row belongs to the instance-level table xud_loader_bind_instance filled for the instan
       ce in hand - the check names the instance so a table filled for another one cannot be cal
       led through here. vkGetInstanceProcAddr answers NULL for this name unless VK_EXT_debug_u
       tils is enabled on that instance, so the row is NULL here on such an instance and XUD_UNS
       UPPORTED is the honest answer for "this instance has no log side" - the caller decides wh
       ether that is fatal (xud_init_context() says no: the validation layers still print their o
       wnut, only our counting callback is missing). Nothing is resolved per call any more; the r
       ule xud_loader.h used to state about caching is enforced by the bind being a rung. */
    if (!xud_loader_instance_bound(loader, instance))
        return XUD_UNSUPPORTED;
    if (loader->instanceTable.vkCreateDebugUtilsMessengerEXT == NULL)
        return XUD_UNSUPPORTED;

    vk = loader->instanceTable.vkCreateDebugUtilsMessengerEXT(
        instance, &diagnostics->messengerCreateInfo, NULL, &messenger);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK) {
        diagnostics->instance = VK_NULL_HANDLE;
        return result;
    }

    diagnostics->messenger = messenger;
    diagnostics->instance = instance; /* borrowed: it must still be alive at destroy time */
    return XUD_OK;
}

XudResult xud_diagnostics_close(XudDiagnostics** diagnostics, XudLoader* loader) {
    XudDiagnostics* victim = NULL;

    if ((diagnostics == NULL) || (loader == NULL))
        return XUD_INVALID_ARG;
    if (*diagnostics == NULL)
        return XUD_ALREADY;

    victim = *diagnostics;

    if (victim->messenger == VK_NULL_HANDLE) {
        free(victim);
        *diagnostics = NULL;
        return XUD_OK;
    }

    if (!xud_loader_is_open(loader))
        return XUD_INVALID_ARG;
    if (victim->instance == VK_NULL_HANDLE)
        return XUD_INVALID_ARG; /* a messenger without its owning instance cannot be destroyed */

    /* The extension owns the rule this one follows: destroying the messenger takes the VkInstan
       ce it belongs to, so this runs while that instance is still alive - xud_close_context() o
       rders it that way. The row is looked up for that same instance, which is what xud_loader_i
       nstance_bound() checks: a messenger whose owning instance is no longer the one the table w
       as resolved for cannot be destroyed through rows of a layer that is gone, and this layer d
       oes not paper over that, so the struct leaks and the reason is returned. */
    if (!xud_loader_instance_bound(loader, victim->instance))
        return XUD_UNSUPPORTED;
    if (loader->instanceTable.vkDestroyDebugUtilsMessengerEXT == NULL)
        return XUD_UNSUPPORTED;

    /* Three arguments, not two: PFN_vkDestroyDebugUtilsMessengerEXT is
       (VkInstance, VkDebugUtilsMessengerEXT, const VkAllocationCallbacks *) in these headers, and
       pAllocator NULL means the default allocator - the same one the create call above passes.
       It returns void, so there is no VkResult for xud_result_from_vk to map. */
    loader->instanceTable.vkDestroyDebugUtilsMessengerEXT(
        victim->instance, victim->messenger, NULL);
    victim->messenger = VK_NULL_HANDLE;
    victim->instance = VK_NULL_HANDLE;
    free(victim);
    *diagnostics = NULL;
    return XUD_OK;
}
