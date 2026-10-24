/***********************************************************************************************
 *   xud_diagnostics.h - the diagnostics layer: validation layers + debug messenger + log callback
 *   Owns: XudDiagnostics (the advertised instance layers selected, the validation features and
 *         the debug messenger create-info assembled, the messenger handle) and the one log
 *         callback every xudlibc messenger installs.
 *   Layer: internal, like xud_loader.h - tests may include it, user code must not. Sees xud.h,
 *          xud_loader.h (Volk's entry-point tables + the two bind rungs), xud_vulkan.h (the shared
 *          vocabulary) and <vulkan/vulkan.h> declarations only, with volk.h immediately above tha
 *          t include (VK_NO_PROTOTYPES, see xud_loader.h's include-order rule). It creates no VK
 *          Instance and calls no Vulkan entry point of its own: every call goes through a row of
 *         the loader's Volk tables.
 ***********************************************************************************************/
#ifndef XUD_XUD_DIAGNOSTICS_H_
#define XUD_XUD_DIAGNOSTICS_H_

#include "volk.h" /* Required for: VK_NO_PROTOTYPES - must precede <vulkan/vulkan.h>, see xud_loader.h */
#include "xud.h"
#include "xud_loader.h"
#include "xud_vulkan.h"
#include <assert.h>
#include <stdbool.h>
#include <vulkan/vulkan.h>

/* How many instance layers xudlibc enables at once. The loader here advertises three
   (VK_LAYER_MESA_device_select, VK_LAYER_MESA_anti_lag, VK_LAYER_KHRONOS_validation); anything
   above this cap is dropped, and xud_diagnostics_select_layers reports XUD_OUT_OF_RESOURCES
   rather than silently truncating. */
#define XUD_MAX_DIAGNOSTIC_LAYERS 8u

/* Bounds of the doubling-capacity query xud_diagnostics_query_layers runs:
   vkEnumerateInstanceLayerProperties has no count-only form (pCount is input/output), so the count
   cannot be read without an array. Same shape as the instance-extension query in xud_instance.c -
   verified here. */
#define XUD_DIAGNOSTIC_QUERY_MIN_CAPACITY 16u
#define XUD_DIAGNOSTIC_QUERY_MAX_CAPACITY 512u

/* How many validation features xudlibc asks the validation layer for. The enabled list is
   fixed (see xud_diagnostics_fill_validation_features); the disabled list is empty because
   disabling a check is a request against the library's own rules, not a capability. */
#define XUD_DIAGNOSTICS_WANTED_MAX 2u
#define XUD_MAX_ENABLED_VALIDATION_FEATURES 4u

typedef struct XudDiagnostics {
    VkDebugUtilsMessengerEXT messenger; /* the created messenger, VK_NULL_HANDLE until then */
    VkInstance instance; /* borrowed: the instance this messenger belongs to,
                            must still be alive when the messenger is destroyed */
    VkDebugUtilsMessengerCreateInfoEXT messengerCreateInfo; /* what was asked for, kept for the log side */
    VkValidationFeaturesEXT validationFeatures; /* the pNext chain piece, kept for the same reason */
    uint32_t enabledLayerCount; /* layers enabled, <= XUD_MAX_DIAGNOSTIC_LAYERS */
    char enabledLayerNameStorage[XUD_MAX_DIAGNOSTIC_LAYERS][VK_MAX_EXTENSION_NAME_SIZE]; /* layerName is that wide */
    const char* enabledLayerNamePointers[XUD_MAX_DIAGNOSTIC_LAYERS]; /* into the rows above: VkInstance
                                                                        ppEnabledLayerNames wants const char
                                                                        *const *, which a char (*)[256] is
                                                                        not (verified: gcc rejects the array
                                                                        directly) */
    VkValidationFeatureEnableEXT enabledValidationFeatures[XUD_MAX_ENABLED_VALIDATION_FEATURES];
    uint32_t messagesReceived; /* callback invocations so far */
    uint32_t errorMessages; /* of those, how many were severity >= ERROR */
} XudDiagnostics;

/// The instance-level extensions the diagnostics layer wants, in request order: VK_EXT_debug_utils
/// (the messenger entry points only resolve when it is enabled) and VK_EXT_validation_features
/// (the pNext chain piece). The instance layer intersects this with what the driver advertised,
/// exactly like it does with its own wanted list - one owner of each list, one place that decides.
/// XUD_INVALID_ARG on a NULL outNames/outCount.
XudResult xud_diagnostics_wanted_extensions(const char* outNames[XUD_DIAGNOSTICS_WANTED_MAX], uint32_t* outCount);

/// Pure: ask the validation layer for what xudlibc can act on - best practices (the library's
/// rules are the validation layer's rules, so its complaints are real) and synchronization
/// validation (xudlibc uses classic semaphores/fences/barriers, which is what this checks).
/// GPU-assisted is not asked for (no shader instrumentation exists yet) and debug printf is not
/// asked for (no VK_EXT_debug_printf anywhere yet). Nothing is disabled - disabling a check is a
/// request against the library's own rules, not a capability - so the disabled count is 0 and its
/// pointer NULL, and no storage for it exists. Fills the struct's own validationFeatures +
/// enabledValidationFeatures.
XudResult xud_diagnostics_fill_validation_features(XudDiagnostics* diagnostics);

/// Pure: the one messenger create-info every xudlibc messenger installs - severity
/// INFO|WARNING|ERROR (VERBOSE is left out: it is the validation layer's chatter, not something a
/// caller acts on), message types GENERAL|VALIDATION|PERFORMANCE (DEVICE_ADDRESS_BINDING is left
/// out: no VK_EXT_debug_utils_address_binding path exists yet), pfnUserCallback =
/// xud_diagnostics_message_callback, pUserData = this struct, so the callback can count what it
/// saw. Fills the struct's own messengerCreateInfo. XUD_INVALID_ARG on a NULL struct.
XudResult xud_diagnostics_fill_messenger_create_info(XudDiagnostics* diagnostics);

/// The name of one severity bit as the library prints it: "VERBOSE", "INFO", "WARNING", "ERROR",
/// or NULL for anything else (0 and combinations are not severities). Pure, headless-testable.
const char* xud_diagnostics_severity_name(VkDebugUtilsMessageSeverityFlagBitsEXT severity);

/// The one debug-messenger callback every xudlibc messenger installs: it counts what it saw into
/// the struct pUserData points at (messagesReceived, and errorMessages when the severity is >=
/// ERROR) and writes the message to stderr. It returns VK_FALSE always - never abort a driver
/// because of a message. Signature-compatible with PFN_vkDebugUtilsMessengerCallbackEXT.
VKAPI_ATTR VkBool32 VKAPI_CALL xud_diagnostics_message_callback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageTypes, const VkDebugUtilsMessengerCallbackDataEXT* callbackData, void* userData);

/// Pure: enable every advertised instance layer, in advertised order, into the struct's own
/// storage - never a hardcoded name, which is the whole point (a driver that advertises a layer
/// we do not know about still gets it). enabledLayerNamePointers is filled to point at the rows
/// of enabledLayerNameStorage, which is what VkInstanceCreateInfo's ppEnabledLayerNames wants;
/// both live in this struct, so both outlive the vkCreateInstance call that reads them.
/// XUD_INVALID_ARG on a NULL struct or a NULL advertised array with a non-zero count; XUD_OUT_OF
/// _RESOURCES when more layers are advertised than the struct can hold (a truncated layer list
/// would mean a silently un-validated run).
XudResult xud_diagnostics_select_layers(XudDiagnostics* outDiagnostics, const VkLayerProperties* advertised, uint32_t advertisedCount);

/// Driver: one doubling-capacity call of the loader's enumerateInstanceLayerProperties slot
/// (16 -> 512, same shape as the instance-extension query). *outProperties is a fresh array
/// sized to the returned capacity, *outCount the layers written; both NULL on every failure.
/// XUD_INVALID_ARG on a NULL loader/out pair or a closed loader; XUD_UNSUPPORTED when the slot
/// is NULL; XUD_VULKAN_ERROR on VK_INCOMPLETE (the list was cut, not an error in Vulkan but
/// unusable here).
XudResult xud_diagnostics_query_layers(const XudLoader* loader, VkLayerProperties** outProperties, uint32_t* outCount);

/// Driver: allocate a diagnostics struct and fill everything the instance layer will need before
/// the instance exists - the advertised layers queried and selected, the validation features and
/// the messenger create-info assembled. The messenger handle stays VK_NULL_HANDLE: creating it
/// needs a live VkInstance, so it happens in xud_diagnostics_create_messenger once there is one.
/// XUD_INVALID_ARG on a NULL out-param or a non-NULL one (it is filled, not reused);
/// XUD_UNSUPPORTED when the loader is closed or its layer slot is NULL; XUD_OUT_OF_MEMORY /
/// XUD_OUT_OF_RESOURCES when the query or the selection cannot be satisfied. Every failure path
/// frees and leaves NULL.
XudResult xud_diagnostics_open(XudDiagnostics** outDiagnostics, XudLoader* loader);

/// Driver: create the messenger through a live VkInstance. VK_EXT_debug_utils must have been
/// enabled on that instance - it is, when the instance layer was handed this struct and the
/// driver advertised the extension; when it was not, vkGetInstanceProcAddr answers NULL and this
/// returns XUD_UNSUPPORTED with the messenger still VK_NULL_HANDLE (the layers are still enabled,
/// only the log side is missing). Resolves vkCreateDebugUtilsMessengerEXT through the instance
/// immediately before calling it, never caches it. XUD_INVALID_ARG on a NULL struct/loader/instance
/// or a closed loader; XUD_VULKAN_ERROR when the driver refuses to create it.
XudResult xud_diagnostics_create_messenger(XudDiagnostics* diagnostics, XudLoader* loader, VkInstance instance);

/// Driver: destroy the messenger while its instance is still alive (the extension's own rule -
/// vkDestroyDebugUtilsMessengerEXT takes the owning VkInstance), then free the struct and set the
/// pointer NULL. vkDestroyDebugUtilsMessengerEXT is resolved through the instance immediately
/// before the call for the same reason; a name the loader does not know is reported, not ignored.
/// XUD_ALREADY when there is no struct (already closed); XUD_INVALID_ARG on a NULL loader or a
/// struct whose messenger exists but whose borrowed instance is VK_NULL_HANDLE.
XudResult xud_diagnostics_close(XudDiagnostics** diagnostics, XudLoader* loader);

#endif /* XUD_XUD_DIAGNOSTICS_H_ */
