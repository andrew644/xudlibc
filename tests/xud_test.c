/***********************************************************************************************
 *
 *   xud_test.c - assert-based test harness
 *
 *   Owns : the test runner and the checks for every public function in xud.h plus the
 *          loader layer, the diagnostics layer, the shared Vulkan vocabulary and the instance
 *          layer below it. Headless
 *          by default: this run must stay green on a machine with no Vulkan driver present -
 *          the loader checks degrade to a note when dlopen fails, they never fail because of a
 *          missing driver. GPU checks are opt-in via XUD_GPU_TESTS=1 and they exist now: the
 *          instance layer is the first one that needs a device.
 *   Layer: tests. Sees xud.h and the internal module headers of the layers under the public
 *          API (xud_loader.h, xud_vulkan.h, xud_diagnostics.h, xud_instance.h) so the layers can
 *          be unit-tested on
 *          their own; user code includes xud.h only. xud_core.h stays private to the library.
 *
 *   Deterministic: fixed loop counts, no time, no randomness, no filesystem.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xud.h"
#include "xud_loader.h"
#include "xud_vulkan.h"
#include "xud_diagnostics.h"
#include "xud_instance.h"

static int xudChecks = 0;
static int xudFailures = 0;

/* One named check. Failures are counted rather than fatal, so the whole file runs. */
static void xud_check(int condition, const char *name, const char *detail)
{
    xudChecks++;
    if (condition) return;
    xudFailures++;
    printf("FAIL %-36s %s\n", name, detail);
}

/* The GPU gate: compile-time default XUD_GPU_TESTS, overridden by the environment. */
static int xud_gpu_tests_enabled(void)
{
    const char *env = getenv(XUD_ENV_GPU_TESTS);
    if (env == NULL) return XUD_GPU_TESTS;
    return (*env != '0') && (*env != '\0');
}

static void xud_test_version(void)
{
    xud_check(XUD_VERSION_MAJOR == 0, "version/major", "XUD_VERSION_MAJOR is not 0");
    xud_check(XUD_VERSION_MINOR == 1, "version/minor", "XUD_VERSION_MINOR is not 1");
    xud_check(XUD_VERSION_PATCH == 0, "version/patch", "XUD_VERSION_PATCH is not 0");
    xud_check(strlen(XUD_VERSION) > 0, "version/string", "XUD_VERSION is empty");
    xud_check(strcmp(XUD_VERSION, "0.1") == 0, "version/consistent",
              "XUD_VERSION does not match the major.minor macros");
}

static void xud_test_result_names(void)
{
    const XudResult all[] = { XUD_OK, XUD_UNSUPPORTED, XUD_INVALID_ARG, XUD_OUT_OF_MEMORY,
                              XUD_OUT_OF_RESOURCES, XUD_ALREADY, XUD_LOST, XUD_VULKAN_ERROR,
                              XUD_INTERNAL_ERROR };
    const size_t count = sizeof(all) / sizeof(all[0]);

    xud_check(XUD_OK == 0, "result/ok-is-zero", "XUD_OK is not 0");

    for (size_t i = 0; i < count; i++) {
        const char *name = xud_result_name(all[i]);
        xud_check(name != NULL && name[0] != '\0', "result/name-non-null", "empty name");
        xud_check(strcmp(name, "XUD_OUT_OF_RANGE") != 0, "result/name-known",
                  "an enum member falls through to the out-of-range name");
        for (size_t j = i + 1; j < count; j++) {
            xud_check(strcmp(name, xud_result_name(all[j])) != 0, "result/names-unique",
                      "two error codes share a name");
        }
    }
}

/* The shared Vulkan vocabulary: VkResult -> XudResult, and the fixed-buffer name helpers.
   Pure, so the headless run covers them completely. */
static void xud_test_result_mapping(void)
{
    xud_check(xud_result_from_vk(VK_SUCCESS) == XUD_OK, "vk/ok", "VK_SUCCESS must map to XUD_OK");
    xud_check(xud_result_from_vk(VK_ERROR_DEVICE_LOST) == XUD_LOST, "vk/lost", "device lost must map to XUD_LOST");
    xud_check(xud_result_from_vk(VK_ERROR_OUT_OF_HOST_MEMORY) == XUD_OUT_OF_MEMORY, "vk/host-memory", "host OOM must map to XUD_OUT_OF_MEMORY");
    xud_check(xud_result_from_vk(VK_ERROR_OUT_OF_DEVICE_MEMORY) == XUD_OUT_OF_RESOURCES, "vk/device-memory", "device OOM must map to XUD_OUT_OF_RESOURCES");
    xud_check(xud_result_from_vk(VK_ERROR_TOO_MANY_OBJECTS) == XUD_OUT_OF_RESOURCES, "vk/too-many-objects", "object exhaustion must map to XUD_OUT_OF_RESOURCES");
    xud_check(xud_result_from_vk(VK_ERROR_INCOMPATIBLE_DRIVER) == XUD_UNSUPPORTED, "vk/incompatible-driver", "an incompatible driver must map to XUD_UNSUPPORTED");
    xud_check(xud_result_from_vk(VK_ERROR_LAYER_NOT_PRESENT) == XUD_UNSUPPORTED, "vk/layer-not-present", "a missing layer must map to XUD_UNSUPPORTED");
    xud_check(xud_result_from_vk(VK_ERROR_EXTENSION_NOT_PRESENT) == XUD_UNSUPPORTED, "vk/extension-not-present", "a missing extension must map to XUD_UNSUPPORTED");
    xud_check(xud_result_from_vk(VK_ERROR_FEATURE_NOT_PRESENT) == XUD_UNSUPPORTED, "vk/feature-not-present", "a missing feature must map to XUD_UNSUPPORTED");
    /* Everything else is a Vulkan failure this library cannot classify: one code for all of it. */
    xud_check(xud_result_from_vk(VK_ERROR_INITIALIZATION_FAILED) == XUD_VULKAN_ERROR, "vk/init-failed", "init failure must map to XUD_VULKAN_ERROR");
    xud_check(xud_result_from_vk(VK_ERROR_MEMORY_MAP_FAILED) == XUD_VULKAN_ERROR, "vk/map-failed", "a map failure must map to XUD_VULKAN_ERROR");
    xud_check(xud_result_from_vk(VK_ERROR_FORMAT_NOT_SUPPORTED) == XUD_VULKAN_ERROR, "vk/format-unsupported", "an unsupported format must map to XUD_VULKAN_ERROR");
    xud_check(xud_result_from_vk(VK_ERROR_NOT_PERMITTED) == XUD_VULKAN_ERROR, "vk/not-permitted", "not-permitted must map to XUD_VULKAN_ERROR");
    /* VK_INCOMPLETE is not an error in Vulkan, but it means the list was truncated: a caller
       that wants the whole list grows its array and calls again before it maps anything. */
    xud_check(xud_result_from_vk(VK_INCOMPLETE) == XUD_VULKAN_ERROR, "vk/incomplete", "a truncated list must not map to XUD_OK");
    xud_check(xud_result_from_vk(VK_NOT_READY) == XUD_VULKAN_ERROR, "vk/not-ready", "not-ready must map to XUD_VULKAN_ERROR");
    xud_check(xud_result_from_vk(VK_TIMEOUT) == XUD_VULKAN_ERROR, "vk/timeout", "timeout must map to XUD_VULKAN_ERROR");
    xud_check(xud_result_from_vk(VK_EVENT_SET) == XUD_VULKAN_ERROR, "vk/event-set", "event-set must map to XUD_VULKAN_ERROR");
    xud_check(xud_result_from_vk(VK_EVENT_RESET) == XUD_VULKAN_ERROR, "vk/event-reset", "event-reset must map to XUD_VULKAN_ERROR");
    xud_check(xud_result_from_vk(-4242) == XUD_VULKAN_ERROR, "vk/unknown-code", "a code outside the enum must map to XUD_VULKAN_ERROR");
}

static void xud_test_name_helpers(void)
{
    char buffer[VK_MAX_EXTENSION_NAME_SIZE];

    memset(buffer, 0, sizeof(buffer));
    xud_check(xud_name_copy(buffer, sizeof(buffer), VK_KHR_SURFACE_EXTENSION_NAME) == XUD_OK, "name/copy-fits", "a name that fits must be copied");
    xud_check(xud_name_equal(buffer, VK_KHR_SURFACE_EXTENSION_NAME) == true, "name/copy-roundtrip", "the copy must be readable");

    /* The boundary: a name that fills the buffer exactly (size-1 characters plus NUL) fits. */
    xud_check(xud_name_copy(buffer, 3u, "ab") == XUD_OK, "name/copy-exact-fit", "a name that fills the buffer exactly must be accepted");
    xud_check(xud_name_equal(buffer, "ab") == true, "name/copy-exact-roundtrip", "the exact fit must round-trip");

    /* One character too many is rejected, not truncated: the buffer still holds "ab". */
    xud_check(xud_name_copy(buffer, 3u, "abc") == XUD_INVALID_ARG, "name/copy-too-long", "a name that does not fit must be rejected, not truncated");
    xud_check(xud_name_equal(buffer, "abc") == false, "name/no-truncate", "a rejected copy must not have written the name");

    xud_check(xud_name_copy(NULL, sizeof(buffer), "x") == XUD_INVALID_ARG, "name/copy-null-destination", "a null destination must be rejected");
    xud_check(xud_name_copy(buffer, 0u, "x") == XUD_INVALID_ARG, "name/copy-zero-size", "a zero-size buffer must be rejected");
    xud_check(xud_name_copy(buffer, sizeof(buffer), NULL) == XUD_INVALID_ARG, "name/copy-null-source", "a null source must be rejected");

    xud_check(xud_name_equal(NULL, NULL) == true, "name/equal-both-null", "two absent names are equal");
    xud_check(xud_name_equal(NULL, "x") == false, "name/equal-one-null", "an absent name is not equal to a present one");
    xud_check(xud_name_equal("x", NULL) == false, "name/equal-other-side-null", "the rule is symmetric");
    xud_check(xud_name_equal("VK_KHR_surface", VK_KHR_SURFACE_EXTENSION_NAME) == true, "name/equal-literal", "a literal matches the header macro");
}

static void xud_test_init_arg_validation(void)
{
    XudContext *context = NULL;

    xud_check(xud_init_context(NULL, 640, 480) == XUD_INVALID_ARG, "init/null-out-param",
              "a null out-param must be rejected, not dereferenced");

    xud_check(xud_init_context(&context, 0, 480) == XUD_INVALID_ARG, "init/zero-width",
              "zero width must be rejected");
    xud_check(xud_init_context(&context, 640, 0) == XUD_INVALID_ARG, "init/zero-height",
              "zero height must be rejected");
    xud_check(xud_init_context(&context, XUD_MAX_WINDOW_EXTENT + 1u, 480) == XUD_INVALID_ARG,
              "init/extent-too-large", "an absurd extent must be rejected host-side");
    xud_check(xud_init_context(&context, XUD_MAX_WINDOW_EXTENT, XUD_MAX_WINDOW_EXTENT) ==
              XUD_UNSUPPORTED, "init/extent-at-limit", "the limit itself must be accepted");

    xud_check(context == NULL, "init/out-param-stays-null",
              "every failure path must leave the out-param NULL");
}

/* The loader layer, unit-tested on its own: open, table contents, resolve, close. */
static void xud_test_loader_table(void)
{
    XudLoader *loader = NULL;
    const XudResult opened = xud_loader_open(&loader);

    if (opened == XUD_UNSUPPORTED) {
        printf("note: no Vulkan loader on this machine, loader checks skipped\n");
        return;
    }

    xud_check(opened == XUD_OK, "loader/open-ok", "loader open must succeed or say unsupported");
    xud_check(loader != NULL, "loader/non-null-out", "loader out-param must be set");
    xud_check(loader->library != NULL, "loader/handle-owned", "the dlopen handle must be owned");
    xud_check(xud_loader_is_open(loader) == true, "loader/is-open", "an opened loader is open");
    xud_check(loader->resolvedCount == XUD_LOADER_GLOBAL_SLOT_COUNT, "loader/table-populated",
              "the populated-slot count must be the global-level count at step S2");

    xud_check(loader->getInstanceProcAddr != NULL, "loader/seed-resolved", "seed PFN must be resolved");
    xud_check(loader->enumerateInstanceLayerProperties != NULL, "loader/global-slot",
              "instance-layer PFN must be resolved");
    xud_check(loader->enumerateInstanceExtensionProperties != NULL, "loader/global-slot",
              "instance-extension PFN must be resolved");

    /* Slots the later layers have not reached must still be NULL: opening the loader may not
       claim what was not resolved. The instance layer binds createInstance/destroyInstance
       later, when it creates an instance, not when the loader opens. */
    xud_check(loader->createInstance == NULL, "loader/slot-null-at-open",
              "instance-level slot must stay NULL at loader open");
    xud_check(loader->createDevice == NULL, "loader/slot-null-at-open",
              "device-level slot must stay NULL at loader open");
    xud_check(loader->createSwapchainKHR == NULL, "loader/slot-null-at-open",
              "swapchain slot must stay NULL at loader open");
    xud_check(loader->destroySurfaceKHR == NULL, "loader/slot-null-at-open",
              "surface teardown slot must stay NULL at loader open");

    /* An unknown entry point is reported, not guessed, and the out-param stays NULL. */
    {
        XudPfnBits bits;
        bits.function = NULL;
        xud_check(xud_loader_resolve(loader, VK_NULL_HANDLE, "vkNotAFunction", &bits) == XUD_UNSUPPORTED,
                  "loader/unknown-name", "an unknown entry point must be unsupported");
        xud_check(bits.function == NULL, "loader/unknown-name-out-null",
                  "a failed resolve must leave the out-param NULL");
    }

    /* A second open into an already-set out-param is rejected without touching it. */
    xud_check(xud_loader_open(&loader) == XUD_INVALID_ARG, "loader/open-non-null-out",
              "a non-null out-param on entry must be rejected, not clobbered");
    xud_check(loader != NULL, "loader/open-no-clobber",
              "a rejected open must leave the existing loader alone");

    xud_check(xud_loader_close(&loader) == XUD_OK, "loader/close-ok", "close must succeed");
    xud_check(loader == NULL, "loader/close-nulls", "close must set the pointer NULL");
}

/* The guard every call site relies on: a nil PFN is reported, never called. */
static void xud_test_loader_nil_guard(void)
{
    XudLoader loader;
    XudPfnBits bits;

    memset(&loader, 0, sizeof(loader)); /* a loader whose seed never resolved */
    bits.function = NULL;

    xud_check(xud_loader_is_open(NULL) == false, "loader/is-open-null", "a null loader is not open");
    xud_check(xud_loader_is_open(&loader) == false, "loader/is-open-zeroed", "a table whose seed never resolved is not open");
    xud_check(xud_loader_resolve(&loader, VK_NULL_HANDLE, "vkX", &bits) == XUD_UNSUPPORTED,
              "loader/nil-pfn-guard", "a nil PFN must be reported, never called");
    xud_check(bits.function == NULL, "loader/nil-pfn-out-null", "out-param must stay NULL");

    xud_check(xud_loader_resolve(NULL, VK_NULL_HANDLE, "vkX", &bits) == XUD_INVALID_ARG,
              "loader/null-loader", "a null loader must be rejected, not dereferenced");
    xud_check(xud_loader_resolve(&loader, VK_NULL_HANDLE, NULL, &bits) == XUD_INVALID_ARG,
              "loader/null-name", "a null name must be rejected");
    xud_check(xud_loader_resolve(&loader, VK_NULL_HANDLE, "", &bits) == XUD_INVALID_ARG,
              "loader/empty-name", "an empty name must be rejected, not resolved to the seed");
    xud_check(xud_loader_resolve(&loader, VK_NULL_HANDLE, "vkX", NULL) == XUD_INVALID_ARG,
              "loader/null-out", "a null out-param must be rejected");

    {
        XudLoader *existing = NULL;
        xud_check(xud_loader_open(NULL) == XUD_INVALID_ARG, "loader/open-null-out",
                  "a null out-param must be rejected");
        xud_check(xud_loader_close(NULL) == XUD_INVALID_ARG, "loader/close-null",
                  "a null pointer must be rejected");
        xud_check(xud_loader_close(&existing) == XUD_ALREADY, "loader/close-already",
                  "closing an already-closed loader is XUD_ALREADY");
        xud_check(existing == NULL, "loader/close-leaves-null", "close must leave the pointer NULL");
    }
}

/* The diagnostics layer's pure parts: the wanted-extension accessor the instance layer asks
   through, the validation-feature request, the messenger create-info, severity naming, the log
   callback itself, and layer selection. None of it touches the driver, so the headless run
   covers it completely; the driver half (query_layers, open, create_messenger, close) is
   xud_test_diagnostics_gpu() below, and its guards are here, driven with a loader that has no
   slots bound. */
static void xud_test_diagnostics_pure(void)
{
    XudDiagnostics diag;
    XudLoader closed;
    XudDiagnostics *noneYet = NULL;
    VkLayerProperties *queried = NULL;
    const char *wanted[XUD_DIAGNOSTICS_WANTED_MAX] = {NULL};
    const char *severityName = NULL;
    VkLayerProperties advertised[3];
    VkDebugUtilsMessengerCallbackDataEXT synthetic;
    uint32_t wantedCount = 0u;
    uint32_t queriedCount = 0u;
    XudResult result;

    memset(&diag, 0, sizeof(diag));
    memset(&closed, 0, sizeof(closed));
    memset(advertised, 0, sizeof(advertised));
    memset(&synthetic, 0, sizeof(synthetic));

    /* The instance-level extensions the diagnostics layer wants, in the order the instance
       layer must enable them. Asking through this accessor is the only mechanism there is:
       a declaration of the list that nothing defines compiles and fails at link time. */
    result = xud_diagnostics_wanted_extensions(wanted, &wantedCount);
    xud_check(result == XUD_OK, "diagnostics/wanted-ok", "the accessor must answer");
    xud_check(wantedCount == XUD_DIAGNOSTICS_WANTED_MAX, "diagnostics/wanted-count", "exactly its cap");
    xud_check(xud_name_equal(wanted[0], VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == true, "diagnostics/wanted-first", "the messenger extension first");
    xud_check(xud_name_equal(wanted[1], VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME) == true, "diagnostics/wanted-second", "the validation-feature extension second");
    xud_check(xud_diagnostics_wanted_extensions(NULL, &wantedCount) == XUD_INVALID_ARG, "diagnostics/wanted-null-names", "a null name array must be rejected");
    xud_check(xud_diagnostics_wanted_extensions(wanted, NULL) == XUD_INVALID_ARG, "diagnostics/wanted-null-count", "a null count must be rejected");

    /* The validation-feature request is a pNext chain member, so it must be complete before
       the instance layer assembles VkInstanceCreateInfo. Only two enables are asked for -
       best practices and synchronization validation - and nothing is disabled, because the
       driver's default set is already the one this library wants. */
    result = xud_diagnostics_fill_validation_features(&diag);
    xud_check(result == XUD_OK, "diagnostics/features-ok", "the request must assemble");
    xud_check(diag.validationFeatures.sType == VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT, "diagnostics/features-s-type", "sType must be the validation-features struct type");
    xud_check(diag.validationFeatures.pNext == NULL, "diagnostics/features-p-next-null", "nothing is chained onto the validation-feature request");
    xud_check(diag.validationFeatures.enabledValidationFeatureCount == 2u, "diagnostics/features-enabled-count", "best practices plus synchronization validation");
    xud_check(diag.validationFeatures.pEnabledValidationFeatures == diag.enabledValidationFeatures, "diagnostics/features-points-at-own-storage", "the request points at the struct's own fixed array");
    xud_check(diag.validationFeatures.pEnabledValidationFeatures[0] == VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT, "diagnostics/features-best-practices", "best practices first");
    xud_check(diag.validationFeatures.pEnabledValidationFeatures[1] == VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT, "diagnostics/features-sync-validation", "synchronization validation second");
    xud_check(diag.validationFeatures.disabledValidationFeatureCount == 0u, "diagnostics/features-disabled-count", "nothing is disabled: the default set is already right");
    xud_check(diag.validationFeatures.pDisabledValidationFeatures == NULL, "diagnostics/features-disabled-null", "no array without a count");
    xud_check(xud_diagnostics_fill_validation_features(NULL) == XUD_INVALID_ARG, "diagnostics/features-null-out", "a null struct must be rejected");

    /* The messenger create-info: what the driver is asked to install. VERBOSE is deliberately
       out of the severity mask - it is chatter, not something a caller acts on - and the
       ERROR bit is deliberately in, because the validation-clean signal of the GPU run is
       built on the callback seeing every error. */
    result = xud_diagnostics_fill_messenger_create_info(&diag);
    xud_check(result == XUD_OK, "diagnostics/messenger-info-ok", "the create-info must assemble");
    xud_check(diag.messengerCreateInfo.sType == VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT, "diagnostics/messenger-s-type", "sType must be the messenger-create struct type");
    xud_check(diag.messengerCreateInfo.pNext == NULL, "diagnostics/messenger-p-next-null", "nothing is chained onto the messenger create-info");
    xud_check(diag.messengerCreateInfo.flags == 0u, "diagnostics/messenger-flags", "no flags on the messenger create-info");
    xud_check((diag.messengerCreateInfo.messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) != 0u, "diagnostics/severity-info", "INFO is reported");
    xud_check((diag.messengerCreateInfo.messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0u, "diagnostics/severity-warning", "WARNING is reported");
    xud_check((diag.messengerCreateInfo.messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0u, "diagnostics/severity-error", "ERROR is reported");
    xud_check((diag.messengerCreateInfo.messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) == 0u, "diagnostics/severity-no-verbose", "VERBOSE stays out");
    xud_check((diag.messengerCreateInfo.messageType & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0u, "diagnostics/type-validation", "validation complaints are reported");
    xud_check((diag.messengerCreateInfo.messageType & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) != 0u, "diagnostics/type-performance", "performance warnings are reported");
    xud_check((diag.messengerCreateInfo.messageType & VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT) != 0u, "diagnostics/type-general", "everything else the layer says is reported");
    xud_check(diag.messengerCreateInfo.pfnUserCallback == xud_diagnostics_message_callback, "diagnostics/callback-bound", "the one log callback is the one installed");
    xud_check(diag.messengerCreateInfo.pUserData == (void *)&diag, "diagnostics/user-data-is-struct", "the callback counts into the struct it was given");
    xud_check(xud_diagnostics_fill_messenger_create_info(NULL) == XUD_INVALID_ARG, "diagnostics/messenger-info-null-out", "a null struct must be rejected");

    /* Severity naming: the exact single bits, and nothing else - a combination is not one
       severity, so it has no name. */
    severityName = xud_diagnostics_severity_name(VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT);
    xud_check((severityName != NULL) && xud_name_equal(severityName, "ERROR"), "diagnostics/severity-name-error", "ERROR names as ERROR");
    severityName = xud_diagnostics_severity_name(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT);
    xud_check((severityName != NULL) && xud_name_equal(severityName, "WARNING"), "diagnostics/severity-name-warning", "WARNING names as WARNING");
    severityName = xud_diagnostics_severity_name(VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT);
    xud_check((severityName != NULL) && xud_name_equal(severityName, "INFO"), "diagnostics/severity-name-info", "INFO names as INFO");
    severityName = xud_diagnostics_severity_name(VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT);
    xud_check((severityName != NULL) && xud_name_equal(severityName, "VERBOSE"), "diagnostics/severity-name-verbose", "VERBOSE names as VERBOSE");
    xud_check(xud_diagnostics_severity_name(0) == NULL, "diagnostics/severity-name-zero", "no severity names nothing");
    xud_check(xud_diagnostics_severity_name(VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) == NULL, "diagnostics/severity-name-combination", "a combination is not one severity");

    /* The callback itself, driven with synthetic callback data: the counters it keeps are the
       validation-clean signal the GPU run reads, so they must be right before a driver exists.
       One line per message goes to stderr, which is what makes a validation-layer complaint
       visible in a test run; VK_FALSE is what keeps a validation layer from aborting. */
    memset(&diag, 0, sizeof(diag)); /* counters start empty, like a fresh xud_diagnostics_open() */
    synthetic.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT;
    synthetic.flags = 0u;
    synthetic.pNext = NULL;
    synthetic.queueLabelCount = 0u;
    synthetic.pQueueLabels = NULL;
    synthetic.cmdBufLabelCount = 0u;
    synthetic.pCmdBufLabels = NULL;
    synthetic.objectCount = 0u;
    synthetic.pObjects = NULL;
    synthetic.messageIdNumber = 1;
    synthetic.pMessageIdName = "test.message.id.name";
    synthetic.pMessage = "headless probe: a synthetic validation complaint";

    xud_check(xud_diagnostics_message_callback(VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, &synthetic, &diag) == VK_FALSE, "diagnostics/callback-error", "the callback must never abort a driver");
    xud_check(diag.messagesReceived == 1u, "diagnostics/callback-counts-one", "one message counted");
    xud_check(diag.errorMessages == 1u, "diagnostics/callback-counts-error", "an ERROR severity counts as an error");
    xud_check(xud_diagnostics_message_callback(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, &synthetic, &diag) == VK_FALSE, "diagnostics/callback-warning", "WARNING is logged too, and still does not abort");
    xud_check(diag.messagesReceived == 2u, "diagnostics/callback-counts-two", "the running total travels");
    xud_check(diag.errorMessages == 1u, "diagnostics/callback-warning-not-error", "WARNING is not an error");
    xud_check(xud_diagnostics_message_callback(VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, NULL, NULL) == VK_FALSE, "diagnostics/callback-null-data", "a null callbackData and null userData must not crash it");
    xud_check(diag.messagesReceived == 2u, "diagnostics/callback-null-data-not-counted", "with no struct in hand there is nothing to count into");
    xud_check(diag.errorMessages == 1u, "diagnostics/callback-null-data-not-error", "INFO is not an error");
    xud_check(xud_diagnostics_message_callback(VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, NULL, &diag) == VK_FALSE, "diagnostics/callback-null-data-live-user", "a null callbackData with the struct in hand is the shape a driver can send");
    xud_check(diag.messagesReceived == 3u, "diagnostics/callback-null-data-counted", "and it still counts as a message");
    xud_check(diag.errorMessages == 1u, "diagnostics/callback-null-data-still-not-error", "INFO is not an error, even with no data");

    /* Layer selection: every advertised layer is enabled, in advertised order, and the
       pointer array points into the struct's own fixed buffers - which is what keeps the
       names alive across the vkCreateInstance the instance layer makes. */
    memset(&diag, 0, sizeof(diag));
    strcpy(advertised[0].layerName, "VK_LAYER_TEST_one");
    strcpy(advertised[1].layerName, "VK_LAYER_TEST_two");
    strcpy(advertised[2].layerName, "VK_LAYER_TEST_three");
    result = xud_diagnostics_select_layers(&diag, advertised, 3u);
    xud_check(result == XUD_OK, "diagnostics/layers-ok", "every advertised layer must be enabled");
    xud_check(diag.enabledLayerCount == 3u, "diagnostics/layers-count", "all of them");
    xud_check(xud_name_equal(diag.enabledLayerNameStorage[0], "VK_LAYER_TEST_one") == true, "diagnostics/layers-first", "advertised order, not sorted");
    xud_check(xud_name_equal(diag.enabledLayerNameStorage[2], "VK_LAYER_TEST_three") == true, "diagnostics/layers-last", "the last advertised layer is the last enabled one");
    xud_check(diag.enabledLayerNamePointers[0] == diag.enabledLayerNameStorage[0], "diagnostics/layers-point-at-storage", "the pointer array points at the struct's own buffers");
    xud_check(diag.enabledLayerNamePointers[2] == diag.enabledLayerNameStorage[2], "diagnostics/layers-point-at-storage-last", "the rule holds for the last row too");
    xud_check(diag.enabledLayerNamePointers[3] == NULL, "diagnostics/layers-tail-null", "the unused tail of the pointer array is NULLed");
    xud_check(xud_name_equal(diag.enabledLayerNameStorage[3], "") == true, "diagnostics/layers-storage-tail-untouched", "nothing beyond the count is written");

    /* The cap: a list that does not fit is an error, never a silent truncation - and the
       struct must not be left half-written. */
    xud_check(xud_diagnostics_select_layers(&diag, advertised, XUD_MAX_DIAGNOSTIC_LAYERS + 1u) == XUD_OUT_OF_RESOURCES, "diagnostics/layers-over-cap", "a layer list that does not fit is an error, not a truncation");
    xud_check(diag.enabledLayerCount == 0u, "diagnostics/layers-over-cap-cleared", "and it leaves nothing half-written");

    /* No advertised layers is a valid empty selection; a count without an array is a lie. */
    xud_check(xud_diagnostics_select_layers(&diag, NULL, 0u) == XUD_OK, "diagnostics/layers-none", "no advertised layers is not an error");
    xud_check(diag.enabledLayerCount == 0u, "diagnostics/layers-none-count", "and nothing is enabled");
    xud_check(xud_diagnostics_select_layers(&diag, NULL, 3u) == XUD_INVALID_ARG, "diagnostics/layers-null-array", "a count without an array must be rejected");
    xud_check(xud_diagnostics_select_layers(NULL, advertised, 3u) == XUD_INVALID_ARG, "diagnostics/layers-null-out", "a null struct must be rejected");

    /* The driver half, guarded headlessly: a loader with no layer slot cannot answer, and a
       closed loader cannot open or close a diagnostics struct. */
    xud_check(xud_diagnostics_query_layers(&closed, &queried, &queriedCount) == XUD_UNSUPPORTED, "diagnostics/query-closed-loader", "a loader with no layer slot cannot answer");
    xud_check((queried == NULL) && (queriedCount == 0u), "diagnostics/query-closed-out", "a failed query must leave both out-params empty");
    xud_check(xud_diagnostics_query_layers(&closed, NULL, NULL) == XUD_INVALID_ARG, "diagnostics/query-null-out", "a null out-param must be rejected before the driver is asked");
    xud_check(xud_diagnostics_open(&noneYet, &closed) == XUD_UNSUPPORTED, "diagnostics/open-closed-loader", "a loader with no layer slot cannot answer the query the struct is built from");
    xud_check(noneYet == NULL, "diagnostics/open-closed-out", "and nothing is allocated behind a closed loader");
    xud_check(xud_diagnostics_open(NULL, &closed) == XUD_INVALID_ARG, "diagnostics/open-null-out", "a null out-param must be rejected");
    xud_check(xud_diagnostics_close(&noneYet, &closed) == XUD_ALREADY, "diagnostics/close-already", "closing nothing is XUD_ALREADY");
}

/* The instance layer's pure parts: application info, extension selection (including what the
   diagnostics layer contributes to it), create-info assembly. No driver call, so the headless
   run covers them completely. */
static void xud_test_instance_pure(void)
{
    VkInstanceCreateInfo createInfo;
    VkInstanceCreateInfo createInfoChain;
    const char *nameSlots[XUD_MAX_INSTANCE_EXTENSIONS];
    const char *noExtras[XUD_DIAGNOSTICS_WANTED_MAX] = {NULL};
    const char *wantedFromDiagnostics[XUD_DIAGNOSTICS_WANTED_MAX] = {NULL};
    VkExtensionProperties advertised[4];
    VkExtensionProperties advertisedDiag[6];
    VkExtensionProperties advertisedChain[7];
    VkLayerProperties advertisedLayers[2];
    XudInstance instance;
    XudInstance withDiag;
    XudInstance withChain;
    XudDiagnostics diag;
    uint32_t wantedCount = 0u;
    XudResult result;

    memset(&instance, 0, sizeof(instance));
    memset(&withDiag, 0, sizeof(withDiag));
    memset(&withChain, 0, sizeof(withChain));
    memset(&diag, 0, sizeof(diag));
    memset(advertised, 0, sizeof(advertised));
    memset(advertisedDiag, 0, sizeof(advertisedDiag));
    memset(advertisedChain, 0, sizeof(advertisedChain));
    memset(advertisedLayers, 0, sizeof(advertisedLayers));

    /* Filled into the instance's own slot, which is what the create-info later points at. */
    result = xud_instance_fill_application_info(&instance.applicationInfo);
    xud_check(result == XUD_OK, "instance/app-info-ok", "fill must succeed");
    xud_check(instance.applicationInfo.sType == VK_STRUCTURE_TYPE_APPLICATION_INFO, "instance/app-info-s-type", "sType must be the application-info struct type");
    xud_check(instance.applicationInfo.pNext == NULL, "instance/app-info-p-next-null", "nothing is chained onto the application info");
    xud_check(xud_name_equal(instance.applicationInfo.pApplicationName, XUD_INSTANCE_NAME) == true, "instance/app-info-name", "the application name must be the library name");
    xud_check(xud_name_equal(instance.applicationInfo.pEngineName, XUD_INSTANCE_NAME) == true, "instance/app-info-engine-name", "this library is the engine");
    xud_check(instance.applicationInfo.applicationVersion == VK_MAKE_API_VERSION(0u, XUD_VERSION_MAJOR, XUD_VERSION_MINOR, XUD_VERSION_PATCH), "instance/app-version", "the version must be the library version");
    xud_check(instance.applicationInfo.engineVersion == instance.applicationInfo.applicationVersion, "instance/engine-version", "both version slots must agree");
    xud_check(instance.applicationInfo.apiVersion == XUD_INSTANCE_API_FLOOR, "instance/api-floor", "the floor must be VK_API_VERSION_1_0");
    xud_check(xud_instance_fill_application_info(NULL) == XUD_INVALID_ARG, "instance/app-info-null-out", "a null out-param must be rejected");

    /* Two of the three wanted extensions, one extension the library does not want at this
       level (VK_KHR_swapchain is device-level), and the wanted pair in the reverse of wanted
       order: the selection must follow wanted order and ignore what is not wanted. */
    strcpy(advertised[0].extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    strcpy(advertised[1].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    strcpy(advertised[2].extensionName, VK_KHR_SURFACE_EXTENSION_NAME);

    result = xud_instance_select_extensions(&instance, advertised, 3u, noExtras, 0u);
    xud_check(result == XUD_OK, "instance/select-ok", "selection must succeed");
    xud_check(instance.enabledExtensionCount == 2u, "instance/select-count", "only wanted-and-advertised names are enabled");
    xud_check(xud_name_equal(instance.enabledExtensionNameStorage[0], VK_KHR_SURFACE_EXTENSION_NAME) == true, "instance/select-order", "wanted order, not advertised order");
    xud_check(xud_name_equal(instance.enabledExtensionNameStorage[1], VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == true, "instance/select-second", "the second wanted-and-advertised name");
    xud_check(xud_name_equal(instance.enabledExtensionNameStorage[2], "") == true, "instance/select-tail-untouched", "nothing beyond the count is written");

    /* The selection is rewritten on every call: a reused struct cannot keep a stale list. */
    result = xud_instance_select_extensions(&instance, NULL, 0u, noExtras, 0u);
    xud_check(result == XUD_OK, "instance/select-none-ok", "an empty advertised list is not an error");
    xud_check(instance.enabledExtensionCount == 0u, "instance/select-none-count", "the previous list must not survive");
    xud_check(xud_instance_select_extensions(NULL, advertised, 3u, noExtras, 0u) == XUD_INVALID_ARG, "instance/select-null-out", "a null out-param must be rejected");
    xud_check(xud_instance_select_extensions(&instance, NULL, 3u, noExtras, 0u) == XUD_INVALID_ARG, "instance/select-null-advertised", "a null advertised array with a non-zero count must be rejected");
    xud_check(xud_instance_select_extensions(&instance, advertised, 3u, NULL, 0u) == XUD_OK, "instance/select-no-extras", "a null extra list with a zero count is the no-diagnostics shape");
    xud_check(xud_instance_select_extensions(&instance, advertised, 3u, NULL, 2u) == XUD_INVALID_ARG, "instance/select-null-extras", "a null extra list with a non-zero count must be rejected");

    /* Re-select, then assemble: the create-info must point at the caller's nameSlots array,
       which is what keeps it alive across the vkCreateInstance call. */
    result = xud_instance_select_extensions(&instance, advertised, 3u, noExtras, 0u);
    xud_check(result == XUD_OK, "instance/select-again-ok", "selection must be repeatable");
    result = xud_instance_fill_create_info(&instance, nameSlots, NULL, &createInfo);
    xud_check(result == XUD_OK, "instance/create-info-ok", "assembly must succeed");
    xud_check(createInfo.sType == VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, "instance/create-info-s-type", "sType must be the instance-create struct type");
    xud_check(createInfo.pNext == NULL, "instance/create-info-p-next-null", "nothing is chained yet - validation is the diagnostics step");
    xud_check(createInfo.flags == 0, "instance/create-info-flags", "the portability flag is deliberately not set");
    xud_check(createInfo.pApplicationInfo == &instance.applicationInfo, "instance/create-info-app-info", "the create-info carries the application info");
    xud_check(createInfo.pApplicationInfo->apiVersion == XUD_INSTANCE_API_FLOOR, "instance/create-info-floor", "the floor travels with it");
    xud_check(createInfo.enabledLayerCount == 0u, "instance/create-info-no-layers", "validation layers are the diagnostics step's job");
    xud_check(createInfo.ppEnabledLayerNames == NULL, "instance/create-info-no-layer-names", "no layer names without a count");
    xud_check(createInfo.enabledExtensionCount == 2u, "instance/create-info-extension-count", "the selected count travels");
    xud_check(createInfo.ppEnabledExtensionNames != NULL, "instance/create-info-extension-names", "a non-zero count must carry names");
    xud_check(nameSlots[0] == createInfo.ppEnabledExtensionNames[0], "instance/create-info-aliases-caller-array", "the create-info points at the caller's array, not a copy");
    xud_check(xud_name_equal(createInfo.ppEnabledExtensionNames[0], VK_KHR_SURFACE_EXTENSION_NAME) == true, "instance/create-info-first-name", "wanted order reaches the driver");
    xud_check(xud_name_equal(createInfo.ppEnabledExtensionNames[1], VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == true, "instance/create-info-second-name", "wanted order reaches the driver");
    xud_check(nameSlots[2] == NULL, "instance/create-info-name-tail-null", "the tail of the caller's array is NULLed so no stale pointer survives");
    xud_check(xud_instance_fill_create_info(NULL, nameSlots, NULL, &createInfo) == XUD_INVALID_ARG, "instance/create-info-null-instance", "a null instance must be rejected");
    xud_check(xud_instance_fill_create_info(&instance, NULL, NULL, &createInfo) == XUD_INVALID_ARG, "instance/create-info-null-name-slots", "a null name-slot array must be rejected");
    xud_check(xud_instance_fill_create_info(&instance, nameSlots, NULL, NULL) == XUD_INVALID_ARG, "instance/create-info-null-out", "a null out-param must be rejected");

    /* What the diagnostics layer contributes, checked headlessly: the wanted pair travels into
       the selection only when the driver advertised it, and the pNext chain that carries the
       validation-feature request becomes reachable only when the whole diagnostics wanted set
       is enabled - a pNext chain may carry a struct for an enabled extension and for nothing
       else. VK_EXT_validation_features is not advertised by lavvk here (verified 2026-10-16),
       which is exactly the first of these two shapes. Both lists carry all three of this layer's
       own wanted names, which is what the real driver answers, so the counts below are the
       counts the GPU run sees plus the validation-feature extension. */
    strcpy(advertisedDiag[0].extensionName, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    strcpy(advertisedDiag[1].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    strcpy(advertisedDiag[2].extensionName, VK_KHR_SURFACE_EXTENSION_NAME);
    strcpy(advertisedDiag[3].extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    strcpy(advertisedDiag[4].extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    advertisedDiag[5].extensionName[0] = 'V'; /* a name nobody wants: advertised, and it stays out */
    advertisedDiag[5].extensionName[1] = 'X';
    advertisedDiag[5].extensionName[2] = '\0';

    xud_check(xud_diagnostics_wanted_extensions(wantedFromDiagnostics, &wantedCount) == XUD_OK, "diagnostics/wanted-accessor-answers", "the instance layer's only mechanism for the wanted pair");
    xud_check(wantedCount == XUD_DIAGNOSTICS_WANTED_MAX, "diagnostics/wanted-accessor-count", "and it hands over exactly its cap");

    result = xud_instance_select_extensions(&withDiag, advertisedDiag, 6u, wantedFromDiagnostics, wantedCount);
    xud_check(result == XUD_OK, "instance/select-with-diagnostics-ok", "the diagnostics extras must be selectable");
    xud_check(withDiag.enabledExtensionCount == 4u, "instance/select-with-diagnostics-count", "three wanted plus debug utils, the validation-feature extension is not advertised here");
    xud_check(xud_name_equal(withDiag.enabledExtensionNameStorage[3], VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == true, "instance/select-diagnostics-tail", "the diagnostics pair is appended after this layer's own list, in the diagnostics layer's order");

    /* All five advertised: the chain becomes reachable, which is what makes it legal. */
    strcpy(advertisedChain[0].extensionName, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    strcpy(advertisedChain[1].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    strcpy(advertisedChain[2].extensionName, VK_KHR_SURFACE_EXTENSION_NAME);
    strcpy(advertisedChain[3].extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    strcpy(advertisedChain[4].extensionName, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
    strcpy(advertisedChain[5].extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    advertisedChain[6].extensionName[0] = 'V';
    advertisedChain[6].extensionName[1] = 'X';
    advertisedChain[6].extensionName[2] = '\0';

    result = xud_instance_select_extensions(&withChain, advertisedChain, 7u, wantedFromDiagnostics, wantedCount);
    xud_check(result == XUD_OK, "instance/select-chain-ok", "the full diagnostics pair must be selectable");
    xud_check(withChain.enabledExtensionCount == 5u, "instance/select-chain-count", "three wanted plus both diagnostics extensions");
    xud_check(xud_name_equal(withChain.enabledExtensionNameStorage[4], VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME) == true, "instance/select-chain-last", "the validation-feature extension is enabled, so its pNext chain is legal");

    /* The diagnostics struct's own parts, filled the way xud_instance_create() fills them. */
    xud_check(xud_diagnostics_fill_validation_features(&diag) == XUD_OK, "diagnostics/features-fill-for-instance", "the validation-feature request must assemble");
    strcpy(advertisedLayers[0].layerName, "VK_LAYER_TEST_validation");
    strcpy(advertisedLayers[1].layerName, "VK_LAYER_TEST_other");
    xud_check(xud_diagnostics_select_layers(&diag, advertisedLayers, 2u) == XUD_OK, "diagnostics/layers-fill-for-instance", "the advertised layers must be selected");
    xud_check(diag.enabledLayerCount == 2u, "diagnostics/layers-count-for-instance", "both of them");
    xud_check(diag.enabledLayerNamePointers[1] == diag.enabledLayerNameStorage[1], "diagnostics/layers-point-at-storage", "the pointer array points at the struct's own fixed buffers");
    xud_check(diag.enabledLayerNamePointers[2] == NULL, "diagnostics/layers-tail-null", "the unused tail of the pointer array is NULLed");

    result = xud_instance_fill_create_info(&withDiag, nameSlots, &diag, &createInfo);
    xud_check(result == XUD_OK, "instance/create-info-with-diagnostics-ok", "assembly with the diagnostics struct must succeed");
    xud_check(createInfo.enabledLayerCount == 2u, "instance/create-info-layer-count", "the diagnostics layer's selection reaches the create-info");
    xud_check(createInfo.ppEnabledLayerNames == diag.enabledLayerNamePointers, "instance/create-info-layer-names", "the create-info points at the diagnostics struct's own pointer array, not a copy");
    xud_check(xud_name_equal(createInfo.ppEnabledLayerNames[0], "VK_LAYER_TEST_validation") == true, "instance/create-info-layer-first", "advertised order reaches the driver");
    xud_check(createInfo.enabledExtensionCount == 4u, "instance/create-info-with-diagnostics-extension-count", "the diagnostics pair travels in the same create-info");
    xud_check(createInfo.pNext == NULL, "instance/create-info-chain-not-when-unadvertised", "the validation-feature request stays out while its extension is not enabled");

    result = xud_instance_fill_create_info(&withChain, nameSlots, &diag, &createInfoChain);
    xud_check(result == XUD_OK, "instance/create-info-chain-ok", "assembly with the chain must succeed");
    xud_check(createInfoChain.enabledExtensionCount == 5u, "instance/create-info-chain-extension-count", "all five names");
    xud_check(createInfoChain.enabledLayerCount == 2u, "instance/create-info-chain-layer-count", "the layers travel with the chain");
    xud_check(createInfoChain.pNext == (const void *)&diag.validationFeatures, "instance/create-info-chain-when-advertised", "the validation-feature request hangs off pNext once its extension is enabled");

    /* A layer count that does not fit the storage it came from is refused, not truncated. */
    diag.enabledLayerCount = XUD_MAX_DIAGNOSTIC_LAYERS + 1u;
    xud_check(xud_instance_fill_create_info(&withChain, nameSlots, &diag, &createInfo) == XUD_INVALID_ARG, "instance/create-info-layer-count-over-bound", "a layer list that does not fit is refused");
    diag.enabledLayerCount = 2u;

    /* The driver half refuses a loader it was not given open - checked before any dlopen. */
    {
        XudInstance *refused = NULL;
        XudLoader closed;
        XudDiagnostics *noneYet = NULL;

        memset(&closed, 0, sizeof(closed));

        xud_check(xud_instance_create(&refused, NULL, NULL) == XUD_INVALID_ARG, "instance/create-null-loader", "a null loader must be rejected");
        xud_check(refused == NULL, "instance/create-null-loader-out-untouched", "a rejected create must leave the out-param alone");
        xud_check(xud_instance_create(&refused, &closed, NULL) == XUD_INVALID_ARG, "instance/create-closed-loader", "a closed loader must be refused before any dlopen");
        xud_check(refused == NULL, "instance/create-closed-loader-out-untouched", "and the out-param must stay alone");
        xud_check(xud_instance_destroy(&refused, NULL, NULL) == XUD_INVALID_ARG, "instance/destroy-null-loader", "a null loader must be rejected");
        xud_check(refused == NULL, "instance/destroy-null-loader-out-untouched", "a rejected destroy must leave the pointer alone");
        xud_check(xud_instance_destroy(&refused, &closed, NULL) == XUD_INVALID_ARG, "instance/destroy-closed-loader", "a closed loader must be refused too");
        xud_check(refused == NULL, "instance/destroy-closed-loader-out-untouched", "and the pointer must stay alone");
        xud_check(xud_diagnostics_open(&noneYet, &closed) == XUD_UNSUPPORTED, "instance/diagnostics-open-closed-loader", "the diagnostics layer refuses the same closed loader");
        xud_check(noneYet == NULL, "instance/diagnostics-open-closed-out", "and allocates nothing behind it");
    }
}

/* The public runtime query: it must answer ok or unsupported, never another code, and it
   must not cache - the answer is recomputed from a fresh dlopen each call. */
static void xud_test_vulkan_supported(void)
{
    const XudResult first = xud_vulkan_supported();
    const XudResult second = xud_vulkan_supported();

    xud_check((first == XUD_OK) || (first == XUD_UNSUPPORTED), "loader/supported-codes",
              "the runtime query must answer XUD_OK or XUD_UNSUPPORTED");
    xud_check(first == second, "loader/supported-stable", "two calls must agree on this machine");
    printf("note: xud_vulkan_supported() -> %s\n", xud_result_name(first));
}

/* Bring-up boundary: the loader and the instance layer exist now, but the layers below the
   instance (device, queue, surface, swapchain) do not - so a valid request must still report
   XUD_UNSUPPORTED rather than hand back a context that owns an instance and no way to use it. */
static void xud_test_init_bringup_boundary(void)
{
    XudContext *context = NULL;
    const XudResult result = xud_init_context(&context, 640, 480);

    xud_check(result == XUD_UNSUPPORTED, "init/bringup-incomplete",
              "valid args must yield XUD_UNSUPPORTED until the layers below the instance land");
    xud_check(context == NULL, "init/no-context-on-unsupported",
              "an incomplete bring-up must not return a context");

    /* The same must hold for the smallest and the largest accepted extents. */
    xud_check(xud_init_context(&context, 1, 1) == XUD_UNSUPPORTED, "init/min-extent",
              "1x1 must be accepted then reported unsupported");
    xud_check(context == NULL, "init/min-extent-null", "out-param must stay NULL");
    xud_check(xud_init_context(&context, XUD_MAX_WINDOW_EXTENT, XUD_MAX_WINDOW_EXTENT) ==
              XUD_UNSUPPORTED, "init/extent-at-limit", "the limit itself must be accepted");
    xud_check(context == NULL, "init/limit-extent-null", "out-param must stay NULL");
}

static void xud_test_close(void)
{
    XudContext *context = NULL;

    xud_check(xud_close_context(NULL) == XUD_INVALID_ARG, "close/null-out-param",
              "a null out-param must be rejected, not dereferenced");
    xud_check(xud_close_context(&context) == XUD_ALREADY, "close/already-closed",
              "closing a NULL context is XUD_ALREADY, not a crash and not XUD_OK");
    xud_check(context == NULL, "close/leaves-null", "closing a NULL context keeps it NULL");
}

/* Every rejected and every incomplete init must free what it allocated - including the
   instance and the loader handle it opened before reporting the bring-up incomplete. The
   verdict is the sanitizer/valgrind report over these 1000 iterations (each one a dlopen/dlsym
   plus a vkCreateInstance/vkDestroyInstance cycle where a driver is present), not an assertion
   here. */
static void xud_test_no_leak(void)
{
    for (int i = 0; i < 1000; i++) {
        XudContext *context = NULL;
        (void)xud_init_context(&context, 64u + (uint32_t)i, 480u);
        (void)xud_close_context(&context);
    }
    xud_check(1, "no-leak/ran", "1000 init+close iterations completed");
}

/* GPU-gated: the diagnostics layer against the real driver. Verified here on 2026-10-16 with
   lavvk plus the Khronos validation layer: three instance layers are advertised, the layer
   query answers 3 with no handle in hand, VK_EXT_debug_utils is advertised, VK_EXT_validation
   features is not (with or without VK_INSTANCE_LAYERS in the environment), vkCreateInstance is
   VK_SUCCESS with all three layers enabled, and vkCreateDebugUtilsMessengerEXT resolves only
   through a live VkInstance. The run must be validation-clean, not just crash-free: the
   messenger is installed, so a validation complaint would land in the callback and the error
   counter below must stay zero. */
static void xud_test_diagnostics_gpu(void)
{
    XudLoader *loader = NULL;
    XudInstance *instance = NULL;
    XudInstance *other = NULL;
    XudDiagnostics *diagnostics = NULL;
    XudPfnBits bits;
    XudResult result;

    result = xud_loader_open(&loader);
    xud_check(result == XUD_OK, "gpu/diagnostics-loader-open", "the diagnostics run needs an open loader");
    if (loader == NULL) return;

    /* The layer query is loader-mediated and instance-level: no handle in hand, but the
       loader's global slot answers, and the count is read without an array. */
    xud_check(xud_diagnostics_query_layers(loader, NULL, NULL) == XUD_INVALID_ARG, "gpu/diagnostics-query-null-out", "a null out-param must be rejected before the driver is asked");

    result = xud_diagnostics_open(&diagnostics, loader);
    xud_check(result == XUD_OK, "gpu/diagnostics-open", "the struct must open with the advertised layers selected");
    if (diagnostics == NULL) { (void)xud_loader_close(&loader); return; }
    xud_check(diagnostics->enabledLayerCount == 3u, "gpu/diagnostics-layer-count", "three instance layers are advertised here");
    xud_check(diagnostics->enabledLayerNameStorage[0][0] != '\0', "gpu/diagnostics-layer-name-set", "the names are copied into the struct's own buffers");
    xud_check(diagnostics->enabledLayerNamePointers[0] == diagnostics->enabledLayerNameStorage[0], "gpu/diagnostics-layer-name-points-at-storage", "and the pointer array points at them");
    xud_check(diagnostics->messenger == VK_NULL_HANDLE, "gpu/diagnostics-messenger-not-yet", "no messenger before an instance exists");
    xud_check(diagnostics->instance == VK_NULL_HANDLE, "gpu/diagnostics-instance-not-yet", "and no borrowed owner yet");
    xud_check(diagnostics->messagesReceived == 0u, "gpu/diagnostics-no-messages-yet", "the counters start empty");
    xud_check(diagnostics->errorMessages == 0u, "gpu/diagnostics-no-errors-yet", "and the validation-clean signal starts clean");

    /* The instance layer asks the diagnostics layer for its wanted extensions, so the
       messenger extension is enabled before the instance exists. */
    result = xud_instance_create(&instance, loader, diagnostics);
    xud_check(result == XUD_OK, "gpu/diagnostics-instance-create", "vkCreateInstance with the validation layers enabled must succeed");
    if (instance == NULL) { (void)xud_diagnostics_close(&diagnostics, loader); (void)xud_loader_close(&loader); return; }
    xud_check(instance->instance != VK_NULL_HANDLE, "gpu/diagnostics-instance-live", "the instance must be live before the messenger is installed");
    xud_check(instance->enabledExtensionCount == 4u, "gpu/diagnostics-extension-count", "the three wanted plus VK_EXT_debug_utils, both advertised here");
    xud_check(xud_name_equal(instance->enabledExtensionNameStorage[3], VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == true, "gpu/diagnostics-debug-utils-enabled", "the messenger extension is enabled in the diagnostics layer's order");

    /* vkCreateDebugUtilsMessengerEXT is instance-level: the loader answers NULL for it with
       VK_NULL_HANDLE in hand, and only a live VkInstance resolves it - which is why the
       diagnostics layer resolves it at create time, not at loader time. */
    bits.function = NULL;
    xud_check(xud_loader_resolve(loader, VK_NULL_HANDLE, "vkCreateDebugUtilsMessengerEXT", &bits) == XUD_UNSUPPORTED, "gpu/diagnostics-messenger-not-global", "the messenger command is not resolvable without a handle");
    xud_check(bits.function == NULL, "gpu/diagnostics-messenger-out-null", "a failed resolve must leave the out-param NULL");

    result = xud_diagnostics_create_messenger(diagnostics, loader, instance->instance);
    xud_check(result == XUD_OK, "gpu/diagnostics-messenger-create", "the messenger must install through the live instance");
    xud_check(diagnostics->messenger != VK_NULL_HANDLE, "gpu/diagnostics-messenger-live", "the handle must be live");
    xud_check(diagnostics->instance == instance->instance, "gpu/diagnostics-messenger-owner", "the owning instance is borrowed for teardown, not copied");

    /* Asking again on a struct that already has one is XUD_ALREADY, not a second handle:
       the struct owns exactly one messenger, and a second handle would be uncloseable. */
    xud_check(xud_diagnostics_create_messenger(diagnostics, loader, instance->instance) == XUD_ALREADY, "gpu/diagnostics-messenger-twice", "a struct that already has a messenger is XUD_ALREADY, not a second handle");
    xud_check(diagnostics->messenger != VK_NULL_HANDLE, "gpu/diagnostics-messenger-unchanged", "and the handle it has is untouched");

    /* A messenger that belongs to a different VkInstance than the one being destroyed is a
       misuse: destroying it through this one would be a lie, so it is rejected before anything
       is torn down - and the second instance is torn down cleanly behind that refusal. */
    xud_check(xud_instance_create(&other, loader, diagnostics) == XUD_OK, "gpu/diagnostics-second-instance", "a second instance must create as cleanly as the first");
    if (other != NULL) {
        xud_check(xud_instance_destroy(&other, loader, diagnostics) == XUD_INVALID_ARG, "gpu/diagnostics-messenger-owner-mismatch", "the messenger belongs to the first instance, not this one");
        xud_check(other != NULL, "gpu/diagnostics-mismatch-leaves-instance-alive", "a refused destroy must not tear anything down");
        xud_check(diagnostics->messenger != VK_NULL_HANDLE, "gpu/diagnostics-mismatch-keeps-messenger", "and must not silence the messenger either");
        xud_check(xud_instance_destroy(&other, loader, NULL) == XUD_OK, "gpu/diagnostics-second-instance-destroy", "torn down without the diagnostics struct it does not own");
        xud_check(other == NULL, "gpu/diagnostics-second-instance-null", "and cleared");
    }

    /* Teardown order: the messenger dies through the instance it belongs to, inside
       xud_instance_destroy(), before that instance goes away. */
    result = xud_instance_destroy(&instance, loader, diagnostics);
    xud_check(result == XUD_OK, "gpu/diagnostics-instance-destroy", "the instance must be destroyed with its messenger");
    xud_check(instance == NULL, "gpu/diagnostics-instance-null", "destroy must clear the out-param");
    xud_check(diagnostics->messenger == VK_NULL_HANDLE, "gpu/diagnostics-messenger-gone", "the messenger was destroyed first");
    xud_check(diagnostics->instance == VK_NULL_HANDLE, "gpu/diagnostics-owner-cleared", "and the borrowed owner is cleared with it");

    /* The validation-clean signal: the messenger was installed for the whole instance
       lifetime, so a validation complaint would have reached the callback and counted here. */
    xud_check(diagnostics->messagesReceived == 0u, "gpu/diagnostics-no-messages", "the messenger was live for the whole instance lifetime, so a validation complaint would have counted here");
    xud_check(diagnostics->errorMessages == 0u, "gpu/diagnostics-validation-clean", "the run is validation-clean, not just crash-free");

    /* Closing with no messenger left is the free-and-clear half: no driver call. */
    result = xud_diagnostics_close(&diagnostics, loader);
    xud_check(result == XUD_OK, "gpu/diagnostics-close", "the struct must close once the messenger is gone");
    xud_check(diagnostics == NULL, "gpu/diagnostics-close-null", "and be cleared");

    xud_check(xud_loader_close(&loader) == XUD_OK, "gpu/diagnostics-loader-close", "the loader must close");
    xud_check(loader == NULL, "gpu/diagnostics-loader-close-null", "and be cleared");
}

/* GPU-gated: the instance layer against the real driver. This is the first test that needs
   a device, and the run must be validation-clean, not just crash-free. */
static void xud_test_gpu_instance(void)
{
    XudLoader *loader = NULL;
    XudInstance *instance = NULL;
    XudPfnBits bits;
    XudResult result;

    result = xud_loader_open(&loader);
    xud_check(result == XUD_OK, "gpu/loader-open", "the GPU run needs an open loader");
    if (loader == NULL) return;

    /* vkDestroyInstance is instance-level: no handle in hand means no answer, which is why
       the instance layer cannot pre-bind it at loader time (see xud_instance.c). */
    bits.function = NULL;
    xud_check(xud_loader_resolve(loader, VK_NULL_HANDLE, "vkDestroyInstance", &bits) == XUD_UNSUPPORTED, "gpu/destroy-not-global", "instance-level commands are not resolvable without a handle");
    xud_check(bits.function == NULL, "gpu/destroy-out-null", "a failed resolve must leave the out-param NULL");
    xud_check(loader->destroyInstance == NULL, "gpu/destroy-slot-null", "the slot must stay NULL before the instance exists");

    result = xud_instance_create(&instance, loader, NULL);
    xud_check(result == XUD_OK, "gpu/instance-create", "vkCreateInstance at the floor must succeed on this machine");
    if (instance == NULL) { (void)xud_loader_close(&loader); return; }
    xud_check(instance->instance != VK_NULL_HANDLE, "gpu/instance-handle", "the handle must be live");
    xud_check(instance->applicationInfo.apiVersion == XUD_INSTANCE_API_FLOOR, "gpu/instance-floor", "the floor travels to the driver");
    /* lavvk advertises all three wanted extensions here (verified 2026-10-16), so all three
       are enabled in wanted order; a driver that advertises fewer would enable fewer. */
    xud_check(instance->enabledExtensionCount == 3u, "gpu/instance-extension-count", "all three wanted extensions are advertised here");
    xud_check(xud_name_equal(instance->enabledExtensionNameStorage[0], VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) == true, "gpu/name-first", "wanted order reaches the driver");
    xud_check(xud_name_equal(instance->enabledExtensionNameStorage[1], VK_KHR_SURFACE_EXTENSION_NAME) == true, "gpu/name-second", "wanted order reaches the driver");
    xud_check(xud_name_equal(instance->enabledExtensionNameStorage[2], VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == true, "gpu/name-third", "wanted order reaches the driver");
    xud_check(loader->createInstance != NULL, "gpu/create-slot-bound", "the create slot is bound into the table");

    result = xud_instance_destroy(&instance, loader, NULL);
    xud_check(result == XUD_OK, "gpu/instance-destroy", "the instance must be destroyed through the live handle");
    xud_check(instance == NULL, "gpu/destroy-nulls-out", "destroy must clear the out-param");
    xud_check(loader->destroyInstance != NULL, "gpu/destroy-slot-bound", "destroy resolved the instance-level command and cached it");

    xud_check(xud_instance_destroy(&instance, loader, NULL) == XUD_ALREADY, "gpu/destroy-twice", "a second destroy is XUD_ALREADY, not a double free");

    /* A second create/destroy pair on the same table: the binding is reusable and nothing
       accumulates in the loader. */
    result = xud_instance_create(&instance, loader, NULL);
    xud_check(result == XUD_OK, "gpu/instance-create-second", "a second instance must create as cleanly as the first");
    xud_check(instance != NULL, "gpu/instance-second-non-null", "the out-param must be set");
    xud_check(xud_instance_destroy(&instance, loader, NULL) == XUD_OK, "gpu/instance-destroy-second", "and tear down");
    xud_check(instance == NULL, "gpu/instance-second-null", "destroy must clear it");

    /* Teardown of an instance that never got a handle: no driver call, struct freed. */
    instance = calloc(1, sizeof(XudInstance));
    xud_check(instance != NULL, "gpu/synthetic-instance-alloc", "the synthetic struct must allocate");
    if (instance != NULL) {
        instance->instance = VK_NULL_HANDLE;
        xud_check(xud_instance_destroy(&instance, loader, NULL) == XUD_OK, "gpu/destroy-handleless", "no driver call, the struct is freed");
        xud_check(instance == NULL, "gpu/destroy-handleless-null", "and the out-param cleared");
    }

    xud_check(xud_loader_close(&loader) == XUD_OK, "gpu/loader-close", "the loader must close");
    xud_check(loader == NULL, "gpu/loader-close-null", "and be cleared");
}

int main(void)
{
    const int gpu = xud_gpu_tests_enabled();

    printf("xudlibc %s test run: gpu gate %s (compile-time default %d)\n",
           XUD_VERSION, XUD_ENV_GPU_TESTS, XUD_GPU_TESTS);

    xud_test_version();
    xud_test_result_names();
    xud_test_result_mapping();
    xud_test_name_helpers();
    xud_test_loader_nil_guard();
    xud_test_loader_table();
    xud_test_diagnostics_pure();
    xud_test_instance_pure();
    xud_test_vulkan_supported();
    xud_test_init_arg_validation();
    xud_test_init_bringup_boundary();
    xud_test_close();
    xud_test_no_leak();

    /* GPU-gated section: the instance layer is the first thing that needs a device, and the
       diagnostics layer is the one that puts a validation layer behind it. */
    if (gpu) { xud_test_gpu_instance(); xud_test_diagnostics_gpu(); }
    else     printf("gpu checks: skipped (set XUD_GPU_TESTS=1 to run them)\n");

    printf("run complete: %d checks, %d failures\n", xudChecks, xudFailures);

    return xudFailures == 0 ? 0 : 1;
}
