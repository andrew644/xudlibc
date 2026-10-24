/***********************************************************************************************
 *
 *   xud_test.c - assert-based test harness
 *
 *   Owns : the test runner and the checks for every public function in xud.h plus the
 *          loader layer, the diagnostics layer, the shared Vulkan vocabulary, the instance
 *          layer below it, the physical layer below that and the device layer below that -
 *          the physical layer's pure half (selection, the feature intersection, the queue-
 *          family rule, the apiVersion read-back, the free-only teardown, none of which cal
 *          ls the driver, so they run headless), the device layer's pure half (the queue-row
 *          assembly, the device-extension selection, the create-info assembly, the queue loo
 *          kup, the argument guards - also no driver call, so they run headless too) and, un
 *          der the GPU gate, both driver halves (xud_physical_open()/xud_physical_query_devices
 *          () and xud_device_open()/xud_device_wait_idle()/xud_device_close() against the real
 *          driver). Headless by default: this run must stay green on a machine with no Vulkan
 *          driver present - the loader checks degrade to a note when dlopen fails, they never
 *          fail because of a missing driver, and neither driver half is reached at all. GPU ch
 *          ecks are opt-in via XUD_GPU_TESTS=1 and they exist now: the instance layer, the physi
 *          cal layer and the device layer need a device.
 *   Layer: tests. Sees xud.h and the internal module headers of the layers under the public
 *          API (xud_loader.h, xud_vulkan.h, xud_diagnostics.h, xud_instance.h, xud_physical.h,
 *          xud_device.h) so the layers can be unit-tested on their own; user code includes xud.h
 *          only. xud_core.h stays private to the library.
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
#include "xud_device.h"
#include "xud_diagnostics.h"
#include "xud_instance.h"
#include "xud_loader.h"
#include "xud_physical.h"
#include "xud_vulkan.h"
#include "xud_window.h"

static int xudChecks = 0;
static int xudFailures = 0;

/* One named check. Failures are counted rather than fatal, so the whole file runs. */
static void xud_check(int condition, const char* name, const char* detail) {
    xudChecks++;
    if (condition)
        return;
    xudFailures++;
    printf("FAIL %-36s %s\n", name, detail);
}

/* The GPU gate: compile-time default XUD_GPU_TESTS, overridden by the environment. */
static int xud_gpu_tests_enabled(void) {
    const char* env = getenv(XUD_ENV_GPU_TESTS);
    if (env == NULL)
        return XUD_GPU_TESTS;
    return (*env != '0') && (*env != '\0');
}

static void xud_test_version(void) {
    xud_check(XUD_VERSION_MAJOR == 0, "version/major", "XUD_VERSION_MAJOR is not 0");
    xud_check(XUD_VERSION_MINOR == 1, "version/minor", "XUD_VERSION_MINOR is not 1");
    xud_check(XUD_VERSION_PATCH == 0, "version/patch", "XUD_VERSION_PATCH is not 0");
    xud_check(strlen(XUD_VERSION) > 0, "version/string", "XUD_VERSION is empty");
    xud_check(strcmp(XUD_VERSION, "0.1") == 0, "version/consistent",
        "XUD_VERSION does not match the major.minor macros");
}

static void xud_test_result_names(void) {
    const XudResult all[] = { XUD_OK, XUD_UNSUPPORTED, XUD_INVALID_ARG, XUD_OUT_OF_MEMORY,
        XUD_OUT_OF_RESOURCES, XUD_ALREADY, XUD_LOST, XUD_VULKAN_ERROR, XUD_INTERNAL_ERROR };
    const size_t count = sizeof(all) / sizeof(all[0]);

    xud_check(XUD_OK == 0, "result/ok-is-zero", "XUD_OK is not 0");

    for (size_t i = 0; i < count; i++) {
        const char* name = xud_result_name(all[i]);
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
static void xud_test_result_mapping(void) {
    xud_check(xud_result_from_vk(VK_SUCCESS) == XUD_OK, "vk/ok", "VK_SUCCESS must map to XUD_OK");
    xud_check(xud_result_from_vk(VK_ERROR_DEVICE_LOST) == XUD_LOST, "vk/lost",
        "device lost must map to XUD_LOST");
    xud_check(xud_result_from_vk(VK_ERROR_OUT_OF_HOST_MEMORY) == XUD_OUT_OF_MEMORY,
        "vk/host-memory", "host OOM must map to XUD_OUT_OF_MEMORY");
    xud_check(xud_result_from_vk(VK_ERROR_OUT_OF_DEVICE_MEMORY) == XUD_OUT_OF_RESOURCES,
        "vk/device-memory", "device OOM must map to XUD_OUT_OF_RESOURCES");
    xud_check(xud_result_from_vk(VK_ERROR_TOO_MANY_OBJECTS) == XUD_OUT_OF_RESOURCES,
        "vk/too-many-objects", "object exhaustion must map to XUD_OUT_OF_RESOURCES");
    xud_check(xud_result_from_vk(VK_ERROR_INCOMPATIBLE_DRIVER) == XUD_UNSUPPORTED,
        "vk/incompatible-driver", "an incompatible driver must map to XUD_UNSUPPORTED");
    xud_check(xud_result_from_vk(VK_ERROR_LAYER_NOT_PRESENT) == XUD_UNSUPPORTED,
        "vk/layer-not-present", "a missing layer must map to XUD_UNSUPPORTED");
    xud_check(xud_result_from_vk(VK_ERROR_EXTENSION_NOT_PRESENT) == XUD_UNSUPPORTED,
        "vk/extension-not-present", "a missing extension must map to XUD_UNSUPPORTED");
    xud_check(xud_result_from_vk(VK_ERROR_FEATURE_NOT_PRESENT) == XUD_UNSUPPORTED,
        "vk/feature-not-present", "a missing feature must map to XUD_UNSUPPORTED");
    /* Everything else is a Vulkan failure this library cannot classify: one code for all of it. */
    xud_check(xud_result_from_vk(VK_ERROR_INITIALIZATION_FAILED) == XUD_VULKAN_ERROR,
        "vk/init-failed", "init failure must map to XUD_VULKAN_ERROR");
    xud_check(xud_result_from_vk(VK_ERROR_MEMORY_MAP_FAILED) == XUD_VULKAN_ERROR, "vk/map-failed",
        "a map failure must map to XUD_VULKAN_ERROR");
    xud_check(xud_result_from_vk(VK_ERROR_FORMAT_NOT_SUPPORTED) == XUD_VULKAN_ERROR,
        "vk/format-unsupported", "an unsupported format must map to XUD_VULKAN_ERROR");
    xud_check(xud_result_from_vk(VK_ERROR_NOT_PERMITTED) == XUD_VULKAN_ERROR, "vk/not-permitted",
        "not-permitted must map to XUD_VULKAN_ERROR");
    /* VK_INCOMPLETE is not an error in Vulkan, but it means the list was truncated: a caller
       that wants the whole list grows its array and calls again before it maps anything. */
    xud_check(xud_result_from_vk(VK_INCOMPLETE) == XUD_VULKAN_ERROR, "vk/incomplete",
        "a truncated list must not map to XUD_OK");
    xud_check(xud_result_from_vk(VK_NOT_READY) == XUD_VULKAN_ERROR, "vk/not-ready",
        "not-ready must map to XUD_VULKAN_ERROR");
    xud_check(xud_result_from_vk(VK_TIMEOUT) == XUD_VULKAN_ERROR, "vk/timeout",
        "timeout must map to XUD_VULKAN_ERROR");
    xud_check(xud_result_from_vk(VK_EVENT_SET) == XUD_VULKAN_ERROR, "vk/event-set",
        "event-set must map to XUD_VULKAN_ERROR");
    xud_check(xud_result_from_vk(VK_EVENT_RESET) == XUD_VULKAN_ERROR, "vk/event-reset",
        "event-reset must map to XUD_VULKAN_ERROR");
    xud_check(xud_result_from_vk(-4242) == XUD_VULKAN_ERROR, "vk/unknown-code",
        "a code outside the enum must map to XUD_VULKAN_ERROR");
}

static void xud_test_name_helpers(void) {
    char buffer[VK_MAX_EXTENSION_NAME_SIZE];

    memset(buffer, 0, sizeof(buffer));
    xud_check(xud_name_copy(buffer, sizeof(buffer), VK_KHR_SURFACE_EXTENSION_NAME) == XUD_OK,
        "name/copy-fits", "a name that fits must be copied");
    xud_check(xud_name_equal(buffer, VK_KHR_SURFACE_EXTENSION_NAME) == true, "name/copy-roundtrip",
        "the copy must be readable");

    /* The boundary: a name that fills the buffer exactly (size-1 characters plus NUL) fits. */
    xud_check(xud_name_copy(buffer, 3u, "ab") == XUD_OK, "name/copy-exact-fit",
        "a name that fills the buffer exactly must be accepted");
    xud_check(xud_name_equal(buffer, "ab") == true, "name/copy-exact-roundtrip",
        "the exact fit must round-trip");

    /* One character too many is rejected, not truncated: the buffer still holds "ab". */
    xud_check(xud_name_copy(buffer, 3u, "abc") == XUD_INVALID_ARG, "name/copy-too-long",
        "a name that does not fit must be rejected, not truncated");
    xud_check(xud_name_equal(buffer, "abc") == false, "name/no-truncate",
        "a rejected copy must not have written the name");

    xud_check(xud_name_copy(NULL, sizeof(buffer), "x") == XUD_INVALID_ARG,
        "name/copy-null-destination", "a null destination must be rejected");
    xud_check(xud_name_copy(buffer, 0u, "x") == XUD_INVALID_ARG, "name/copy-zero-size",
        "a zero-size buffer must be rejected");
    xud_check(xud_name_copy(buffer, sizeof(buffer), NULL) == XUD_INVALID_ARG,
        "name/copy-null-source", "a null source must be rejected");

    xud_check(
        xud_name_equal(NULL, NULL) == true, "name/equal-both-null", "two absent names are equal");
    xud_check(xud_name_equal(NULL, "x") == false, "name/equal-one-null",
        "an absent name is not equal to a present one");
    xud_check(
        xud_name_equal("x", NULL) == false, "name/equal-other-side-null", "the rule is symmetric");
    xud_check(xud_name_equal("VK_KHR_surface", VK_KHR_SURFACE_EXTENSION_NAME) == true,
        "name/equal-literal", "a literal matches the header macro");
}

static void xud_test_init_arg_validation(void) {
    XudContext* context = NULL;

    xud_check(xud_init_context(NULL, 640, 480) == XUD_INVALID_ARG, "init/null-out-param",
        "a null out-param must be rejected, not dereferenced");

    xud_check(xud_init_context(&context, 0, 480) == XUD_INVALID_ARG, "init/zero-width",
        "zero width must be rejected");
    xud_check(xud_init_context(&context, 640, 0) == XUD_INVALID_ARG, "init/zero-height",
        "zero height must be rejected");
    xud_check(xud_init_context(&context, XUD_MAX_WINDOW_EXTENT + 1u, 480) == XUD_INVALID_ARG,
        "init/extent-too-large", "an absurd extent must be rejected host-side");
    xud_check(
        xud_init_context(&context, XUD_MAX_WINDOW_EXTENT, XUD_MAX_WINDOW_EXTENT) == XUD_UNSUPPORTED,
        "init/extent-at-limit", "the limit itself must be accepted");

    xud_check(context == NULL, "init/out-param-stays-null",
        "every failure path must leave the out-param NULL");
}

/* The loader layer, unit-tested on its own: open, table contents, the two bind rungs, close. */
static void xud_test_loader_table(void) {
    XudLoader* loader = NULL;
    const XudResult opened = xud_loader_open(&loader);

    if (opened == XUD_UNSUPPORTED) {
        printf("note: no Vulkan loader on this machine, loader checks skipped\n");
        return;
    }

    xud_check(opened == XUD_OK, "loader/open-ok", "loader open must succeed or say unsupported");
    xud_check(loader != NULL, "loader/non-null-out", "loader out-param must be set");
    xud_check(loader->open == true, "loader/open-flag", "an opened loader says it is open");
    xud_check(xud_loader_is_open(loader) == true, "loader/is-open", "an opened loader is open");
    xud_check(loader->boundInstance == VK_NULL_HANDLE, "loader/no-instance-yet",
        "no instance-level rows before an instance exists");
    xud_check(loader->boundDevice == VK_NULL_HANDLE, "loader/no-device-yet",
        "and no device-level rows before a device exists");

    /* The three loader-level rows: commands with no handle in hand, so volkInitialize() an-
       swered them before xud_loader_open() accepted the loader. The count check that used to s
       it in this test lives in the rung now, and this test reads the three rows it has to read.
       vkGetInstanceProcAddr, the seed of the hand-rolled bring-up this replaced, is Volk's own
       global and is not a row of this struct - nothing in xudlibc calls through it. */
    xud_check(loader->createInstance != NULL, "loader/loader-row-create",
        "the create row is resolved at loader open");
    xud_check(loader->enumerateInstanceLayerProperties != NULL, "loader/loader-row-layers",
        "instance-layer row must be resolved");
    xud_check(loader->enumerateInstanceExtensionProperties != NULL, "loader/loader-row-exts",
        "instance-extension row must be resolved");

    /* Both tables are still all-NULL: opening the loader may not claim what was not resolved t
       hrough a handle. The instance layer fills the instance table at xud_loader_bind_instance,
       the device layer fills the device table at xud_loader_bind_device - not here. */
    xud_check(loader->instanceTable.vkDestroyInstance == NULL, "loader/instance-row-null-at-open",
        "instance-level row must stay NULL at loader open");
    xud_check(loader->instanceTable.vkCreateDevice == NULL, "loader/create-row-null-at-open",
        "the create row is instance-level and stays NULL until the instance bind");
    xud_check(loader->deviceTable.vkCreateSwapchainKHR == NULL, "loader/swapchain-row-null-at-open",
        "swapchain row must stay NULL at loader open");
    xud_check(loader->instanceTable.vkDestroySurfaceKHR == NULL, "loader/surface-row-null-at-open",
        "surface teardown row must stay NULL at loader open");

    /* A bind with no handle in hand is an argument error, and the tables stay untouched: the t
       wo rungs replace the per-name resolve that used to report an unknown entry point. */
    xud_check(xud_loader_bind_instance(loader, VK_NULL_HANDLE) == XUD_INVALID_ARG,
        "loader/bind-instance-null", "a bind with no handle must be rejected");
    xud_check(xud_loader_bind_device(loader, VK_NULL_HANDLE) == XUD_INVALID_ARG,
        "loader/bind-device-null", "a bind with no device must be rejected");
    xud_check(xud_loader_bind_device(loader, (VkDevice)(uintptr_t)1u) == XUD_UNSUPPORTED,
        "loader/bind-device-needs-instance",
        "a device table cannot be filled before an instance table exists - the device rung res"
        "olves through this loader's own vkGetDeviceProcAddr row, so no instance means no row");
    xud_check(loader->deviceTable.vkDestroyDevice == NULL, "loader/bind-device-left-rows-null",
        "and a refused device bind must leave every device-level row NULL");
    xud_check(loader->instanceTable.vkDestroyInstance == NULL, "loader/bind-rejected-untouched",
        "a rejected bind must leave the table alone");

    /* A second open into an already-set out-param is rejected without touching it. */
    xud_check(xud_loader_open(&loader) == XUD_INVALID_ARG, "loader/open-non-null-out",
        "a non-null out-param on entry must be rejected, not clobbered");
    xud_check(loader != NULL, "loader/open-no-clobber",
        "a rejected open must leave the existing loader alone");

    xud_check(xud_loader_close(&loader) == XUD_OK, "loader/close-ok", "close must succeed");
    xud_check(loader == NULL, "loader/close-nulls", "close must set the pointer NULL");
}

/* The guard every call site relies on: a nil PFN is reported, never called. */
static void xud_test_loader_nil_guard(void) {
    XudLoader loader;
    const VkInstance fake = (VkInstance)(uintptr_t)1u;
    const VkDevice fakeDevice = (VkDevice)(uintptr_t)1u;

    memset(&loader, 0, sizeof(loader)); /* a loader whose rows never resolved */

    xud_check(
        xud_loader_is_open(NULL) == false, "loader/is-open-null", "a null loader is not open");
    xud_check(xud_loader_is_open(&loader) == false, "loader/is-open-zeroed",
        "a loader whose rows never resolved is not open");

    /* The handles below are NOT real VkInstance/VkDevice handles and must never be dereferenc
       ed: the point of every check here is that the guard answers before the call, and a mi
       ssing guard would call through a NULL row and crash the run. */
    xud_check(xud_loader_instance_bound(&loader, fake) == false, "loader/bound-nothing",
        "a zeroed loader is bound to no instance");
    xud_check(xud_loader_device_bound(&loader, fakeDevice) == false, "loader/device-bound-nothing",
        "and to no device");
    xud_check(xud_loader_instance_bound(&loader, VK_NULL_HANDLE) == false,
        "loader/bound-not-null-handle", "and never to the null handle");
    xud_check(xud_loader_bind_instance(&loader, fake) == XUD_INVALID_ARG, "loader/bind-closed",
        "a closed loader cannot bind, before anything else");
    xud_check(xud_loader_bind_device(&loader, fakeDevice) == XUD_INVALID_ARG,
        "loader/bind-device-closed", "and neither can it fill a device table");
    xud_check(xud_loader_bind_instance(NULL, fake) == XUD_INVALID_ARG, "loader/bind-null-loader",
        "a null loader must be rejected, not dereferenced");
    xud_check(xud_loader_bind_device(NULL, fakeDevice) == XUD_INVALID_ARG,
        "loader/bind-device-null-loader", "the device rung guards the same way");
    xud_check(loader.instanceTable.vkDestroyInstance == NULL, "loader/bind-left-rows-null",
        "a refused bind must leave every instance-level row NULL");
    xud_check(loader.deviceTable.vkDestroyDevice == NULL, "loader/bind-left-device-rows-null",
        "...including the device-level ones");

    {
        XudLoader* existing = NULL;
        xud_check(xud_loader_open(NULL) == XUD_INVALID_ARG, "loader/open-null-out",
            "a null out-param must be rejected");
        xud_check(xud_loader_close(NULL) == XUD_INVALID_ARG, "loader/close-null",
            "a null pointer must be rejected");
        xud_check(xud_loader_close(&existing) == XUD_ALREADY, "loader/close-already",
            "closing an already-closed loader is XUD_ALREADY");
        xud_check(
            existing == NULL, "loader/close-leaves-null", "close must leave the pointer NULL");
    }
}

/* The diagnostics layer's pure parts: the wanted-extension accessor the instance layer asks
   through, the validation-feature request, the messenger create-info, severity naming, the log
   callback itself, and layer selection. None of it touches the driver, so the headless run
   covers it completely; the driver half (query_layers, open, create_messenger, close) is
   xud_test_diagnostics_gpu() below, and its guards are here, driven with a loader that has no
   slots bound. */
static void xud_test_diagnostics_pure(void) {
    XudDiagnostics diag;
    XudLoader closed;
    XudDiagnostics* noneYet = NULL;
    VkLayerProperties* queried = NULL;
    const char* wanted[XUD_DIAGNOSTICS_WANTED_MAX] = { NULL };
    const char* severityName = NULL;
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
    xud_check(
        wantedCount == XUD_DIAGNOSTICS_WANTED_MAX, "diagnostics/wanted-count", "exactly its cap");
    xud_check(xud_name_equal(wanted[0], VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == true,
        "diagnostics/wanted-first", "the messenger extension first");
    xud_check(xud_name_equal(wanted[1], VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME) == true,
        "diagnostics/wanted-second", "the validation-feature extension second");
    xud_check(xud_diagnostics_wanted_extensions(NULL, &wantedCount) == XUD_INVALID_ARG,
        "diagnostics/wanted-null-names", "a null name array must be rejected");
    xud_check(xud_diagnostics_wanted_extensions(wanted, NULL) == XUD_INVALID_ARG,
        "diagnostics/wanted-null-count", "a null count must be rejected");

    /* The validation-feature request is a pNext chain member, so it must be complete before
       the instance layer assembles VkInstanceCreateInfo. Only two enables are asked for -
       best practices and synchronization validation - and nothing is disabled, because the
       driver's default set is already the one this library wants. */
    result = xud_diagnostics_fill_validation_features(&diag);
    xud_check(result == XUD_OK, "diagnostics/features-ok", "the request must assemble");
    xud_check(diag.validationFeatures.sType == VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
        "diagnostics/features-s-type", "sType must be the validation-features struct type");
    xud_check(diag.validationFeatures.pNext == NULL, "diagnostics/features-p-next-null",
        "nothing is chained onto the validation-feature request");
    xud_check(diag.validationFeatures.enabledValidationFeatureCount == 2u,
        "diagnostics/features-enabled-count", "best practices plus synchronization validation");
    xud_check(diag.validationFeatures.pEnabledValidationFeatures == diag.enabledValidationFeatures,
        "diagnostics/features-points-at-own-storage",
        "the request points at the struct's own fixed array");
    xud_check(diag.validationFeatures.pEnabledValidationFeatures[0]
            == VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT,
        "diagnostics/features-best-practices", "best practices first");
    xud_check(diag.validationFeatures.pEnabledValidationFeatures[1]
            == VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT,
        "diagnostics/features-sync-validation", "synchronization validation second");
    xud_check(diag.validationFeatures.disabledValidationFeatureCount == 0u,
        "diagnostics/features-disabled-count",
        "nothing is disabled: the default set is already right");
    xud_check(diag.validationFeatures.pDisabledValidationFeatures == NULL,
        "diagnostics/features-disabled-null", "no array without a count");
    xud_check(xud_diagnostics_fill_validation_features(NULL) == XUD_INVALID_ARG,
        "diagnostics/features-null-out", "a null struct must be rejected");

    /* The messenger create-info: what the driver is asked to install. VERBOSE is deliberately
       out of the severity mask - it is chatter, not something a caller acts on - and the
       ERROR bit is deliberately in, because the validation-clean signal of the GPU run is
       built on the callback seeing every error. */
    result = xud_diagnostics_fill_messenger_create_info(&diag);
    xud_check(result == XUD_OK, "diagnostics/messenger-info-ok", "the create-info must assemble");
    xud_check(
        diag.messengerCreateInfo.sType == VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        "diagnostics/messenger-s-type", "sType must be the messenger-create struct type");
    xud_check(diag.messengerCreateInfo.pNext == NULL, "diagnostics/messenger-p-next-null",
        "nothing is chained onto the messenger create-info");
    xud_check(diag.messengerCreateInfo.flags == 0u, "diagnostics/messenger-flags",
        "no flags on the messenger create-info");
    xud_check(
        (diag.messengerCreateInfo.messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)
            != 0u,
        "diagnostics/severity-info", "INFO is reported");
    xud_check(
        (diag.messengerCreateInfo.messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
            != 0u,
        "diagnostics/severity-warning", "WARNING is reported");
    xud_check(
        (diag.messengerCreateInfo.messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
            != 0u,
        "diagnostics/severity-error", "ERROR is reported");
    xud_check(
        (diag.messengerCreateInfo.messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT)
            == 0u,
        "diagnostics/severity-no-verbose", "VERBOSE stays out");
    xud_check(
        (diag.messengerCreateInfo.messageType & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT)
            != 0u,
        "diagnostics/type-validation", "validation complaints are reported");
    xud_check(
        (diag.messengerCreateInfo.messageType & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)
            != 0u,
        "diagnostics/type-performance", "performance warnings are reported");
    xud_check(
        (diag.messengerCreateInfo.messageType & VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT) != 0u,
        "diagnostics/type-general", "everything else the layer says is reported");
    xud_check(diag.messengerCreateInfo.pfnUserCallback == xud_diagnostics_message_callback,
        "diagnostics/callback-bound", "the one log callback is the one installed");
    xud_check(diag.messengerCreateInfo.pUserData == (void*)&diag, "diagnostics/user-data-is-struct",
        "the callback counts into the struct it was given");
    xud_check(xud_diagnostics_fill_messenger_create_info(NULL) == XUD_INVALID_ARG,
        "diagnostics/messenger-info-null-out", "a null struct must be rejected");

    /* Severity naming: the exact single bits, and nothing else - a combination is not one
       severity, so it has no name. */
    severityName = xud_diagnostics_severity_name(VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT);
    xud_check((severityName != NULL) && xud_name_equal(severityName, "ERROR"),
        "diagnostics/severity-name-error", "ERROR names as ERROR");
    severityName = xud_diagnostics_severity_name(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT);
    xud_check((severityName != NULL) && xud_name_equal(severityName, "WARNING"),
        "diagnostics/severity-name-warning", "WARNING names as WARNING");
    severityName = xud_diagnostics_severity_name(VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT);
    xud_check((severityName != NULL) && xud_name_equal(severityName, "INFO"),
        "diagnostics/severity-name-info", "INFO names as INFO");
    severityName = xud_diagnostics_severity_name(VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT);
    xud_check((severityName != NULL) && xud_name_equal(severityName, "VERBOSE"),
        "diagnostics/severity-name-verbose", "VERBOSE names as VERBOSE");
    xud_check(xud_diagnostics_severity_name(0) == NULL, "diagnostics/severity-name-zero",
        "no severity names nothing");
    xud_check(xud_diagnostics_severity_name(VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT
                  | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
            == NULL,
        "diagnostics/severity-name-combination", "a combination is not one severity");

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

    xud_check(xud_diagnostics_message_callback(VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
                  VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, &synthetic, &diag)
            == VK_FALSE,
        "diagnostics/callback-error", "the callback must never abort a driver");
    xud_check(
        diag.messagesReceived == 1u, "diagnostics/callback-counts-one", "one message counted");
    xud_check(diag.errorMessages == 1u, "diagnostics/callback-counts-error",
        "an ERROR severity counts as an error");
    xud_check(xud_diagnostics_message_callback(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
                  VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, &synthetic, &diag)
            == VK_FALSE,
        "diagnostics/callback-warning", "WARNING is logged too, and still does not abort");
    xud_check(diag.messagesReceived == 2u, "diagnostics/callback-counts-two",
        "the running total travels");
    xud_check(diag.errorMessages == 1u, "diagnostics/callback-warning-not-error",
        "WARNING is not an error");
    xud_check(xud_diagnostics_message_callback(VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT,
                  VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, NULL, NULL)
            == VK_FALSE,
        "diagnostics/callback-null-data",
        "a null callbackData and null userData must not crash it");
    xud_check(diag.messagesReceived == 2u, "diagnostics/callback-null-data-not-counted",
        "with no struct in hand there is nothing to count into");
    xud_check(diag.errorMessages == 1u, "diagnostics/callback-null-data-not-error",
        "INFO is not an error");
    xud_check(xud_diagnostics_message_callback(VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT,
                  VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, NULL, &diag)
            == VK_FALSE,
        "diagnostics/callback-null-data-live-user",
        "a null callbackData with the struct in hand is the shape a driver can send");
    xud_check(diag.messagesReceived == 3u, "diagnostics/callback-null-data-counted",
        "and it still counts as a message");
    xud_check(diag.errorMessages == 1u, "diagnostics/callback-null-data-still-not-error",
        "INFO is not an error, even with no data");

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
    xud_check(xud_name_equal(diag.enabledLayerNameStorage[0], "VK_LAYER_TEST_one") == true,
        "diagnostics/layers-first", "advertised order, not sorted");
    xud_check(xud_name_equal(diag.enabledLayerNameStorage[2], "VK_LAYER_TEST_three") == true,
        "diagnostics/layers-last", "the last advertised layer is the last enabled one");
    xud_check(diag.enabledLayerNamePointers[0] == diag.enabledLayerNameStorage[0],
        "diagnostics/layers-point-at-storage",
        "the pointer array points at the struct's own buffers");
    xud_check(diag.enabledLayerNamePointers[2] == diag.enabledLayerNameStorage[2],
        "diagnostics/layers-point-at-storage-last", "the rule holds for the last row too");
    xud_check(diag.enabledLayerNamePointers[3] == NULL, "diagnostics/layers-tail-null",
        "the unused tail of the pointer array is NULLed");
    xud_check(xud_name_equal(diag.enabledLayerNameStorage[3], "") == true,
        "diagnostics/layers-storage-tail-untouched", "nothing beyond the count is written");

    /* The cap: a list that does not fit is an error, never a silent truncation - and the
       struct must not be left half-written. */
    xud_check(xud_diagnostics_select_layers(&diag, advertised, XUD_MAX_DIAGNOSTIC_LAYERS + 1u)
            == XUD_OUT_OF_RESOURCES,
        "diagnostics/layers-over-cap",
        "a layer list that does not fit is an error, not a truncation");
    xud_check(diag.enabledLayerCount == 0u, "diagnostics/layers-over-cap-cleared",
        "and it leaves nothing half-written");

    /* No advertised layers is a valid empty selection; a count without an array is a lie. */
    xud_check(xud_diagnostics_select_layers(&diag, NULL, 0u) == XUD_OK, "diagnostics/layers-none",
        "no advertised layers is not an error");
    xud_check(
        diag.enabledLayerCount == 0u, "diagnostics/layers-none-count", "and nothing is enabled");
    xud_check(xud_diagnostics_select_layers(&diag, NULL, 3u) == XUD_INVALID_ARG,
        "diagnostics/layers-null-array", "a count without an array must be rejected");
    xud_check(xud_diagnostics_select_layers(NULL, advertised, 3u) == XUD_INVALID_ARG,
        "diagnostics/layers-null-out", "a null struct must be rejected");

    /* The driver half, guarded headlessly: a loader with no layer slot cannot answer, and a
       closed loader cannot open or close a diagnostics struct. */
    xud_check(xud_diagnostics_query_layers(&closed, &queried, &queriedCount) == XUD_UNSUPPORTED,
        "diagnostics/query-closed-loader", "a loader with no layer slot cannot answer");
    xud_check((queried == NULL) && (queriedCount == 0u), "diagnostics/query-closed-out",
        "a failed query must leave both out-params empty");
    xud_check(xud_diagnostics_query_layers(&closed, NULL, NULL) == XUD_INVALID_ARG,
        "diagnostics/query-null-out",
        "a null out-param must be rejected before the driver is asked");
    xud_check(xud_diagnostics_open(&noneYet, &closed) == XUD_UNSUPPORTED,
        "diagnostics/open-closed-loader",
        "a loader with no layer slot cannot answer the query the struct is built from");
    xud_check(noneYet == NULL, "diagnostics/open-closed-out",
        "and nothing is allocated behind a closed loader");
    xud_check(xud_diagnostics_open(NULL, &closed) == XUD_INVALID_ARG, "diagnostics/open-null-out",
        "a null out-param must be rejected");
    xud_check(xud_diagnostics_close(&noneYet, &closed) == XUD_ALREADY, "diagnostics/close-already",
        "closing nothing is XUD_ALREADY");
}

/* The instance layer's pure parts: application info, extension selection (including what the
   diagnostics layer contributes to it), create-info assembly. No driver call, so the headless
   run covers them completely. */
static void xud_test_instance_pure(void) {
    VkInstanceCreateInfo createInfo;
    VkInstanceCreateInfo createInfoChain;
    const char* nameSlots[XUD_MAX_INSTANCE_EXTENSIONS];
    const char* noExtras[XUD_DIAGNOSTICS_WANTED_MAX + XUD_INSTANCE_EXTRA_MAX] = { NULL };
    const char* wantedFromDiagnostics[XUD_DIAGNOSTICS_WANTED_MAX + XUD_INSTANCE_EXTRA_MAX]
        = { NULL };
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
    xud_check(instance.applicationInfo.sType == VK_STRUCTURE_TYPE_APPLICATION_INFO,
        "instance/app-info-s-type", "sType must be the application-info struct type");
    xud_check(instance.applicationInfo.pNext == NULL, "instance/app-info-p-next-null",
        "nothing is chained onto the application info");
    xud_check(xud_name_equal(instance.applicationInfo.pApplicationName, XUD_INSTANCE_NAME) == true,
        "instance/app-info-name", "the application name must be the library name");
    xud_check(xud_name_equal(instance.applicationInfo.pEngineName, XUD_INSTANCE_NAME) == true,
        "instance/app-info-engine-name", "this library is the engine");
    xud_check(instance.applicationInfo.applicationVersion
            == VK_MAKE_API_VERSION(0u, XUD_VERSION_MAJOR, XUD_VERSION_MINOR, XUD_VERSION_PATCH),
        "instance/app-version", "the version must be the library version");
    xud_check(instance.applicationInfo.engineVersion == instance.applicationInfo.applicationVersion,
        "instance/engine-version", "both version slots must agree");
    xud_check(instance.applicationInfo.apiVersion == XUD_INSTANCE_API_FLOOR, "instance/api-floor",
        "the floor must be VK_API_VERSION_1_0");
    xud_check(xud_instance_fill_application_info(NULL) == XUD_INVALID_ARG,
        "instance/app-info-null-out", "a null out-param must be rejected");

    /* Two of the three wanted extensions, one extension the library does not want at this
       level (VK_KHR_swapchain is device-level), and the wanted pair in the reverse of wanted
       order: the selection must follow wanted order and ignore what is not wanted. */
    strcpy(advertised[0].extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    strcpy(advertised[1].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    strcpy(advertised[2].extensionName, VK_KHR_SURFACE_EXTENSION_NAME);

    result = xud_instance_select_extensions(&instance, advertised, 3u, noExtras, 0u);
    xud_check(result == XUD_OK, "instance/select-ok", "selection must succeed");
    xud_check(instance.enabledExtensionCount == 2u, "instance/select-count",
        "only wanted-and-advertised names are enabled");
    xud_check(xud_name_equal(instance.enabledExtensionNameStorage[0], VK_KHR_SURFACE_EXTENSION_NAME)
            == true,
        "instance/select-order", "wanted order, not advertised order");
    xud_check(xud_name_equal(instance.enabledExtensionNameStorage[1],
                  VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)
            == true,
        "instance/select-second", "the second wanted-and-advertised name");
    xud_check(xud_name_equal(instance.enabledExtensionNameStorage[2], "") == true,
        "instance/select-tail-untouched", "nothing beyond the count is written");

    /* The selection is rewritten on every call: a reused struct cannot keep a stale list. */
    result = xud_instance_select_extensions(&instance, NULL, 0u, noExtras, 0u);
    xud_check(
        result == XUD_OK, "instance/select-none-ok", "an empty advertised list is not an error");
    xud_check(instance.enabledExtensionCount == 0u, "instance/select-none-count",
        "the previous list must not survive");
    xud_check(xud_instance_select_extensions(NULL, advertised, 3u, noExtras, 0u) == XUD_INVALID_ARG,
        "instance/select-null-out", "a null out-param must be rejected");
    xud_check(xud_instance_select_extensions(&instance, NULL, 3u, noExtras, 0u) == XUD_INVALID_ARG,
        "instance/select-null-advertised",
        "a null advertised array with a non-zero count must be rejected");
    xud_check(xud_instance_select_extensions(&instance, advertised, 3u, NULL, 0u) == XUD_OK,
        "instance/select-no-extras",
        "a null extra list with a zero count is the no-diagnostics shape");
    xud_check(
        xud_instance_select_extensions(&instance, advertised, 3u, NULL, 2u) == XUD_INVALID_ARG,
        "instance/select-null-extras", "a null extra list with a non-zero count must be rejected");

    /* Re-select, then assemble: the create-info must point at the caller's nameSlots array,
       which is what keeps it alive across the vkCreateInstance call. */
    result = xud_instance_select_extensions(&instance, advertised, 3u, noExtras, 0u);
    xud_check(result == XUD_OK, "instance/select-again-ok", "selection must be repeatable");
    result = xud_instance_fill_create_info(&instance, nameSlots, NULL, &createInfo);
    xud_check(result == XUD_OK, "instance/create-info-ok", "assembly must succeed");
    xud_check(createInfo.sType == VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        "instance/create-info-s-type", "sType must be the instance-create struct type");
    xud_check(createInfo.pNext == NULL, "instance/create-info-p-next-null",
        "nothing is chained yet - validation is the diagnostics step");
    xud_check(createInfo.flags == 0, "instance/create-info-flags",
        "the portability flag is deliberately not set");
    xud_check(createInfo.pApplicationInfo == &instance.applicationInfo,
        "instance/create-info-app-info", "the create-info carries the application info");
    xud_check(createInfo.pApplicationInfo->apiVersion == XUD_INSTANCE_API_FLOOR,
        "instance/create-info-floor", "the floor travels with it");
    xud_check(createInfo.enabledLayerCount == 0u, "instance/create-info-no-layers",
        "validation layers are the diagnostics step's job");
    xud_check(createInfo.ppEnabledLayerNames == NULL, "instance/create-info-no-layer-names",
        "no layer names without a count");
    xud_check(createInfo.enabledExtensionCount == 2u, "instance/create-info-extension-count",
        "the selected count travels");
    xud_check(createInfo.ppEnabledExtensionNames != NULL, "instance/create-info-extension-names",
        "a non-zero count must carry names");
    xud_check(nameSlots[0] == createInfo.ppEnabledExtensionNames[0],
        "instance/create-info-aliases-caller-array",
        "the create-info points at the caller's array, not a copy");
    xud_check(xud_name_equal(createInfo.ppEnabledExtensionNames[0], VK_KHR_SURFACE_EXTENSION_NAME)
            == true,
        "instance/create-info-first-name", "wanted order reaches the driver");
    xud_check(xud_name_equal(createInfo.ppEnabledExtensionNames[1],
                  VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)
            == true,
        "instance/create-info-second-name", "wanted order reaches the driver");
    xud_check(nameSlots[2] == NULL, "instance/create-info-name-tail-null",
        "the tail of the caller's array is NULLed so no stale pointer survives");
    xud_check(xud_instance_fill_create_info(NULL, nameSlots, NULL, &createInfo) == XUD_INVALID_ARG,
        "instance/create-info-null-instance", "a null instance must be rejected");
    xud_check(xud_instance_fill_create_info(&instance, NULL, NULL, &createInfo) == XUD_INVALID_ARG,
        "instance/create-info-null-name-slots", "a null name-slot array must be rejected");
    xud_check(xud_instance_fill_create_info(&instance, nameSlots, NULL, NULL) == XUD_INVALID_ARG,
        "instance/create-info-null-out", "a null out-param must be rejected");

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
    advertisedDiag[5].extensionName[0]
        = 'V'; /* a name nobody wants: advertised, and it stays out */
    advertisedDiag[5].extensionName[1] = 'X';
    advertisedDiag[5].extensionName[2] = '\0';

    xud_check(xud_diagnostics_wanted_extensions(wantedFromDiagnostics, &wantedCount) == XUD_OK,
        "diagnostics/wanted-accessor-answers",
        "the instance layer's only mechanism for the wanted pair");
    xud_check(wantedCount == XUD_DIAGNOSTICS_WANTED_MAX, "diagnostics/wanted-accessor-count",
        "and it hands over exactly its cap");

    result = xud_instance_select_extensions(
        &withDiag, advertisedDiag, 6u, wantedFromDiagnostics, wantedCount);
    xud_check(result == XUD_OK, "instance/select-with-diagnostics-ok",
        "the diagnostics extras must be selectable");
    xud_check(withDiag.enabledExtensionCount == 4u, "instance/select-with-diagnostics-count",
        "three wanted plus debug utils, the validation-feature extension is not advertised here");
    xud_check(
        xud_name_equal(withDiag.enabledExtensionNameStorage[3], VK_EXT_DEBUG_UTILS_EXTENSION_NAME)
            == true,
        "instance/select-diagnostics-tail",
        "the diagnostics pair is appended after this layer's own list, in the diagnostics layer's "
        "order");

    /* All five advertised: the chain becomes reachable, which is what makes it legal. */
    strcpy(
        advertisedChain[0].extensionName, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    strcpy(advertisedChain[1].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    strcpy(advertisedChain[2].extensionName, VK_KHR_SURFACE_EXTENSION_NAME);
    strcpy(advertisedChain[3].extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    strcpy(advertisedChain[4].extensionName, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
    strcpy(advertisedChain[5].extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    advertisedChain[6].extensionName[0] = 'V';
    advertisedChain[6].extensionName[1] = 'X';
    advertisedChain[6].extensionName[2] = '\0';

    result = xud_instance_select_extensions(
        &withChain, advertisedChain, 7u, wantedFromDiagnostics, wantedCount);
    xud_check(result == XUD_OK, "instance/select-chain-ok",
        "the full diagnostics pair must be selectable");
    xud_check(withChain.enabledExtensionCount == 5u, "instance/select-chain-count",
        "three wanted plus both diagnostics extensions");
    xud_check(xud_name_equal(withChain.enabledExtensionNameStorage[4],
                  VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME)
            == true,
        "instance/select-chain-last",
        "the validation-feature extension is enabled, so its pNext chain is legal");

    /* The diagnostics struct's own parts, filled the way xud_instance_create() fills them. */
    xud_check(xud_diagnostics_fill_validation_features(&diag) == XUD_OK,
        "diagnostics/features-fill-for-instance", "the validation-feature request must assemble");
    strcpy(advertisedLayers[0].layerName, "VK_LAYER_TEST_validation");
    strcpy(advertisedLayers[1].layerName, "VK_LAYER_TEST_other");
    xud_check(xud_diagnostics_select_layers(&diag, advertisedLayers, 2u) == XUD_OK,
        "diagnostics/layers-fill-for-instance", "the advertised layers must be selected");
    xud_check(
        diag.enabledLayerCount == 2u, "diagnostics/layers-count-for-instance", "both of them");
    xud_check(diag.enabledLayerNamePointers[1] == diag.enabledLayerNameStorage[1],
        "diagnostics/layers-point-at-storage",
        "the pointer array points at the struct's own fixed buffers");
    xud_check(diag.enabledLayerNamePointers[2] == NULL, "diagnostics/layers-tail-null",
        "the unused tail of the pointer array is NULLed");

    result = xud_instance_fill_create_info(&withDiag, nameSlots, &diag, &createInfo);
    xud_check(result == XUD_OK, "instance/create-info-with-diagnostics-ok",
        "assembly with the diagnostics struct must succeed");
    xud_check(createInfo.enabledLayerCount == 2u, "instance/create-info-layer-count",
        "the diagnostics layer's selection reaches the create-info");
    xud_check(createInfo.ppEnabledLayerNames == diag.enabledLayerNamePointers,
        "instance/create-info-layer-names",
        "the create-info points at the diagnostics struct's own pointer array, not a copy");
    xud_check(xud_name_equal(createInfo.ppEnabledLayerNames[0], "VK_LAYER_TEST_validation") == true,
        "instance/create-info-layer-first", "advertised order reaches the driver");
    xud_check(createInfo.enabledExtensionCount == 4u,
        "instance/create-info-with-diagnostics-extension-count",
        "the diagnostics pair travels in the same create-info");
    xud_check(createInfo.pNext == NULL, "instance/create-info-chain-not-when-unadvertised",
        "the validation-feature request stays out while its extension is not enabled");

    result = xud_instance_fill_create_info(&withChain, nameSlots, &diag, &createInfoChain);
    xud_check(
        result == XUD_OK, "instance/create-info-chain-ok", "assembly with the chain must succeed");
    xud_check(createInfoChain.enabledExtensionCount == 5u,
        "instance/create-info-chain-extension-count", "all five names");
    xud_check(createInfoChain.enabledLayerCount == 2u, "instance/create-info-chain-layer-count",
        "the layers travel with the chain");
    xud_check(createInfoChain.pNext == (const void*)&diag.validationFeatures,
        "instance/create-info-chain-when-advertised",
        "the validation-feature request hangs off pNext once its extension is enabled");

    /* A layer count that does not fit the storage it came from is refused, not truncated. */
    diag.enabledLayerCount = XUD_MAX_DIAGNOSTIC_LAYERS + 1u;
    xud_check(
        xud_instance_fill_create_info(&withChain, nameSlots, &diag, &createInfo) == XUD_INVALID_ARG,
        "instance/create-info-layer-count-over-bound", "a layer list that does not fit is refused");
    diag.enabledLayerCount = 2u;

    /* The driver half refuses a loader it was not given open - checked before any dlopen. */
    {
        XudInstance* refused = NULL;
        XudLoader closed;
        XudDiagnostics* noneYet = NULL;

        memset(&closed, 0, sizeof(closed));

        xud_check(xud_instance_create(&refused, NULL, NULL, NULL, 0u) == XUD_INVALID_ARG,
            "instance/create-null-loader", "a null loader must be rejected");
        xud_check(refused == NULL, "instance/create-null-loader-out-untouched",
            "a rejected create must leave the out-param alone");
        xud_check(xud_instance_create(&refused, &closed, NULL, NULL, 0u) == XUD_INVALID_ARG,
            "instance/create-closed-loader", "a closed loader must be refused before any dlopen");
        xud_check(refused == NULL, "instance/create-closed-loader-out-untouched",
            "and the out-param must stay alone");
        xud_check(xud_instance_destroy(&refused, NULL, NULL) == XUD_INVALID_ARG,
            "instance/destroy-null-loader", "a null loader must be rejected");
        xud_check(refused == NULL, "instance/destroy-null-loader-out-untouched",
            "a rejected destroy must leave the pointer alone");
        xud_check(xud_instance_destroy(&refused, &closed, NULL) == XUD_INVALID_ARG,
            "instance/destroy-closed-loader", "a closed loader must be refused too");
        xud_check(refused == NULL, "instance/destroy-closed-loader-out-untouched",
            "and the pointer must stay alone");
        xud_check(xud_diagnostics_open(&noneYet, &closed) == XUD_UNSUPPORTED,
            "instance/diagnostics-open-closed-loader",
            "the diagnostics layer refuses the same closed loader");
        xud_check(noneYet == NULL, "instance/diagnostics-open-closed-out",
            "and allocates nothing behind it");
    }
}

/* The physical layer's pure half: the device policy, the feature intersection S6 hands
   vkCreateDevice, the queue-family rule, the apiVersion read-back and the free-only teardown.
   None of it calls the driver, so these checks run with no device present - the fixtures below
   are stand-ins for driver answers, built to the shapes xud_physical.h records. The driver half
   (xud_physical_query_devices, xud_physical_open) is S5.4's and is not defined yet, so nothing
   here is checked for it. */
static void xud_test_physical_pure(void) {
    /* A VkPhysicalDevice is a pointer to a driver-side object and xud_physical_select() never
       reads through one - it moves an index and checks the arrays are where they claim to be -
       so addresses of the test's own locals stand in for driver handles. Same handle assumption
       xud_loader.c asserts: VK_USE_64_BIT_PTR_DEFINES == 1, a handle is a pointer. */
    uint64_t fakeHandleStorage[3] = { 0xF00du, 0xF00du, 0xF00du };
    const VkPhysicalDevice candidates[3] = { (VkPhysicalDevice)&fakeHandleStorage[0],
        (VkPhysicalDevice)&fakeHandleStorage[1], (VkPhysicalDevice)&fakeHandleStorage[2] };
    VkPhysicalDeviceProperties2 props[3];
    VkPhysicalDeviceFeatures driverSaid;
    VkPhysicalDeviceFeatures enabled;
    XudDeviceCaps* caps = NULL;
    XudPhysicalDevice* device = NULL;
    XudPhysicalDevice* slot = NULL;
    uint32_t index = 0u;
    XudResult result;

    memset(props, 0, sizeof(props));
    memset(&driverSaid, 0, sizeof(driverSaid));

    /* A well-formed stand-in for the properties query, one row per candidate, and the discrete
       GPU deliberately at index 2 rather than index 0: the first-for-now policy must answer 0
       anyway, which is what proves the array is present but not ranked. */
    props[0].sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props[1].sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props[2].sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props[0].properties.deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    props[1].properties.deviceType = VK_PHYSICAL_DEVICE_TYPE_CPU;
    props[2].properties.deviceType = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;

    /* A poison index, so "the out-param was left untouched" is an observable answer - there is
       no no-device sentinel at this layer (the one in xud_physical.h is a queue-family index). */
    index = 0xA5A5u;
    xud_check(xud_physical_select(NULL, candidates, props, 3u) == XUD_INVALID_ARG,
        "physical/select-null-out", "a null out-index must be rejected");
    xud_check(xud_physical_select(&index, NULL, props, 3u) == XUD_INVALID_ARG,
        "physical/select-null-candidates",
        "a null candidates array with a non-zero count must be rejected");
    xud_check(xud_physical_select(&index, candidates, NULL, 3u) == XUD_INVALID_ARG,
        "physical/select-null-properties",
        "a null properties array with a non-zero count must be rejected");
    xud_check(index == 0xA5A5u, "physical/select-invalid-untouched",
        "a rejected select must leave the index alone");
    xud_check(xud_physical_select(&index, NULL, NULL, 0u) == XUD_UNSUPPORTED,
        "physical/select-no-device", "no candidate at all is unsupported, not invalid");
    xud_check(index == 0xA5A5u, "physical/select-unsupported-untouched",
        "out-index is written only on XUD_OK");

    result = xud_physical_select(&index, candidates, props, 3u);
    xud_check(result == XUD_OK, "physical/select-ok", "a non-empty candidate list selects");
    xud_check(
        index == 0u, "physical/select-first", "first in driver order, the whole policy today");
    xud_check(xud_physical_select(&index, candidates, props, 1u) == XUD_OK,
        "physical/select-one-candidate", "a one-candidate list is not an error");
    xud_check(index == 0u, "physical/select-one-index", "and answers index 0");

    /* The feature intersection: the driver has a feature the library does not want, lacks one
       it wants, and has one it wants; the wanted-but-missing one stays off. */
    caps = calloc(1u, sizeof(XudDeviceCaps));
    xud_check(
        caps != NULL, "physical/caps-alloc", "the caps are heap-allocated, they must allocate");
    caps->features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    caps->features.features.robustBufferAccess = VK_TRUE; /* wanted and present: stays on */
    caps->features.features.geometryShader = VK_TRUE; /* present, not wanted: stays off */
    caps->features.features.samplerAnisotropy = VK_FALSE; /* wanted, absent: stays off */
    caps->features.features.wideLines = VK_TRUE; /* wanted and present: stays on */

    /* The out-struct starts full of non-zero garbage: every field of VkPhysicalDeviceFeatures
       must be written by the call, so nothing the library does not want may survive it. */
    memset(&enabled, 0xFF, sizeof(enabled));
    result = xud_physical_record_features(&enabled, caps);
    xud_check(result == XUD_OK, "physical/record-features-ok", "the intersection must succeed");
    xud_check(enabled.robustBufferAccess == VK_TRUE, "physical/feature-wanted-present",
        "wanted and reported stays on");
    xud_check(enabled.wideLines == VK_TRUE, "physical/feature-wanted-present-2",
        "the second wanted and reported stays on");
    xud_check(enabled.geometryShader == VK_FALSE, "physical/feature-not-wanted",
        "a feature the library does not want stays off even when the driver has it");
    xud_check(enabled.samplerAnisotropy == VK_FALSE, "physical/feature-wanted-absent",
        "a wanted feature the driver lacks stays off, nothing is forced on");
    xud_check(enabled.inheritedQueries == VK_FALSE, "physical/feature-tail-written",
        "the last field is written too, the list cannot forget one");
    xud_check(enabled.robustBufferAccess != 0xFFFFFFFFu, "physical/feature-normalized",
        "the output is VK_TRUE/VK_FALSE, not a smuggled third value");

    /* The caps stay exactly what the driver said: the intersection is derived, never stored back.
     */
    xud_check(caps->features.features.geometryShader == VK_TRUE, "physical/caps-verbatim",
        "the driver's answer is not rewritten by the intersection");
    xud_check(xud_physical_record_features(NULL, caps) == XUD_INVALID_ARG,
        "physical/record-features-null-out", "a null out-struct must be rejected");
    xud_check(xud_physical_record_features(&enabled, NULL) == XUD_INVALID_ARG,
        "physical/record-features-null-caps", "null caps must be rejected");

    /* The apiVersion read-back: below the floor, at the floor, above it, and the zero a driver
       that answered nothing leaves behind (the four caps getters return void, so zero is the
       only failure signal they can give - see xud_physical.h). */
    xud_check(xud_physical_record_api_version(caps) == XUD_UNSUPPORTED, "physical/api-zero",
        "a driver that answered nothing fails the floor check");
    xud_check(caps->apiVersion == 0u, "physical/api-zero-recorded",
        "and what it said is still recorded verbatim");
    caps->properties.properties.apiVersion = VK_MAKE_API_VERSION(0u, 0u, 9u, 0u);
    xud_check(xud_physical_record_api_version(caps) == XUD_UNSUPPORTED, "physical/api-below-floor",
        "below the instance floor is unsupported");
    xud_check(caps->apiVersion == VK_MAKE_API_VERSION(0u, 0u, 9u, 0u),
        "physical/api-below-recorded", "the driver's number is recorded even when it fails");
    caps->properties.properties.apiVersion = XUD_INSTANCE_API_FLOOR;
    xud_check(xud_physical_record_api_version(caps) == XUD_OK, "physical/api-at-floor",
        "at the floor is usable");
    xud_check(caps->apiVersion == XUD_INSTANCE_API_FLOOR, "physical/api-at-floor-recorded",
        "and recorded");
    caps->properties.properties.apiVersion = VK_API_VERSION_1_4;
    xud_check(xud_physical_record_api_version(caps) == XUD_OK, "physical/api-above-floor",
        "above the floor is usable");
    xud_check(caps->apiVersion == VK_API_VERSION_1_4, "physical/api-above-recorded",
        "a driver above what these headers know is recorded as said");
    xud_check(xud_physical_record_api_version(NULL) == XUD_INVALID_ARG, "physical/api-null-caps",
        "null caps must be rejected");

    /* The queue-family rule: a compute-only family first, then the first one that carries the
       graphics bit, then another graphics family that must not win. */
    {
        VkQueueFamilyProperties2 families[3];
        VkQueueFamilyProperties2 chosen;

        memset(families, 0, sizeof(families));
        memset(&chosen, 0, sizeof(chosen));

        families[0].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
        families[0].queueFamilyProperties.queueFlags = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
        families[0].queueFamilyProperties.queueCount = 1u;
        families[0].queueFamilyProperties.timestampValidBits = 64u;

        families[1].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
        families[1].queueFamilyProperties.queueFlags
            = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT;
        families[1].queueFamilyProperties.queueCount = 8u;
        families[1].queueFamilyProperties.timestampValidBits = 36u;
        families[1].queueFamilyProperties.minImageTransferGranularity.width = 4u;

        families[2].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
        families[2].queueFamilyProperties.queueFlags = VK_QUEUE_GRAPHICS_BIT;
        families[2].queueFamilyProperties.queueCount = 1u;

        index = 0xA5A5u;
        result = xud_physical_select_queue_family(&index, &chosen, families, 3u);
        xud_check(result == XUD_OK, "physical/queue-family-ok", "a graphics family exists here");
        xud_check(index == 1u, "physical/queue-family-first-graphics",
            "the first family carrying the graphics bit in driver order wins");
        xud_check(chosen.sType == VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2,
            "physical/queue-family-s-type",
            "the row copied into the caps names its own struct type");
        xud_check(chosen.pNext == NULL, "physical/queue-family-p-next-null",
            "nothing is chained onto the row the caps keep");
        xud_check(chosen.queueFamilyProperties.queueFlags
                == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT),
            "physical/queue-family-flags", "the whole row is copied, flags included");
        xud_check(chosen.queueFamilyProperties.queueCount == 8u, "physical/queue-family-count",
            "S6's VkQueueCreateInfo input travels with the row");
        xud_check(chosen.queueFamilyProperties.timestampValidBits == 36u,
            "physical/queue-family-timestamp-bits", "and so do the timestamp bits");
        xud_check(chosen.queueFamilyProperties.minImageTransferGranularity.width == 4u,
            "physical/queue-family-granularity", "and the granularity, recorded not required");

        /* A compute-only driver: nothing here to draw with. */
        memset(&chosen, 0, sizeof(chosen));
        result = xud_physical_select_queue_family(&index, &chosen, families, 1u);
        xud_check(result == XUD_UNSUPPORTED, "physical/queue-family-no-graphics",
            "a compute-only family is refused");
        xud_check(index == XUD_PHYSICAL_NO_QUEUE_FAMILY, "physical/queue-family-sentinel",
            "the sentinel is written so an untouched index cannot look like family 0");
        xud_check(chosen.sType == 0u, "physical/queue-family-chosen-zeroed",
            "and no stale row survives the refusal");

        /* A graphics-only family (no transfer bit): transfer is recorded, not required. */
        memset(&chosen, 0, sizeof(chosen));
        result = xud_physical_select_queue_family(&index, &chosen, &families[2], 1u);
        xud_check(result == XUD_OK, "physical/queue-family-graphics-only",
            "a family without the transfer bit is still chosen");
        xud_check(
            index == 0u, "physical/queue-family-graphics-only-index", "the single row answers 0");

        /* No family at all, and the argument guards. */
        index = 0xA5A5u;
        result = xud_physical_select_queue_family(&index, &chosen, NULL, 0u);
        xud_check(result == XUD_UNSUPPORTED, "physical/queue-family-none",
            "no family at all is unsupported");
        xud_check(index == XUD_PHYSICAL_NO_QUEUE_FAMILY, "physical/queue-family-none-sentinel",
            "with the sentinel");
        index = 0xA5A5u;
        xud_check(xud_physical_select_queue_family(&index, &chosen, NULL, 3u) == XUD_INVALID_ARG,
            "physical/queue-family-null-families",
            "a null families array with a non-zero count must be rejected");
        xud_check(index == 0xA5A5u, "physical/queue-family-invalid-untouched",
            "a rejected choice must leave the index alone");
        xud_check(xud_physical_select_queue_family(NULL, &chosen, families, 3u) == XUD_INVALID_ARG,
            "physical/queue-family-null-out-index", "a null out-index must be rejected");
        xud_check(xud_physical_select_queue_family(&index, NULL, families, 3u) == XUD_INVALID_ARG,
            "physical/queue-family-null-out-row", "a null out-chosen row must be rejected");
    }

    /* The free-only rung of the teardown ladder: no driver call at all, because Vulkan has no
       vkDestroyPhysicalDevice. The heap array is the one thing it releases. */
    {
        XudPhysicalDevice* device2 = NULL;

        device = calloc(1u, sizeof(XudPhysicalDevice));
        xud_check(device != NULL, "physical/device-alloc", "the struct must allocate");
        device->caps.deviceExtensionProperties = calloc(3u, sizeof(VkExtensionProperties));
        xud_check(device->caps.deviceExtensionProperties != NULL, "physical/extension-array-alloc",
            "the caps' one heap array must allocate");
        device->caps.deviceExtensionCount = 1u;
        device->caps.deviceExtensionCapacity = 3u;
        slot = device;
        xud_check(xud_physical_close(&slot) == XUD_OK, "physical/close-ok",
            "the free-only rung must close a device");
        xud_check(slot == NULL, "physical/close-null", "and set the pointer NULL");

        /* A device whose extension query never ran: the array is NULL, close must still work
           (and under -DXUD_SANITIZERS=ON both closes above leak nothing). */
        device2 = calloc(1u, sizeof(XudPhysicalDevice));
        xud_check(device2 != NULL, "physical/device-alloc-2", "the second struct must allocate");
        slot = device2;
        xud_check(xud_physical_close(&slot) == XUD_OK, "physical/close-no-array",
            "a device with no query yet closes cleanly");
        xud_check(slot == NULL, "physical/close-no-array-null", "and its pointer is NULL");

        xud_check(xud_physical_close(NULL) == XUD_INVALID_ARG, "physical/close-null-ptr",
            "a null pointer-to-pointer must be rejected");
        slot = NULL;
        xud_check(xud_physical_close(&slot) == XUD_ALREADY, "physical/close-already",
            "an already-closed device answers XUD_ALREADY");
    }

    free(caps);
}

/*-----------------------------------------------------------------------------------------------
 * The device layer's pure half: the queue-row assembly, the device-extension selection, the
 * create-info assembly, the queue lookup and the argument guards. None of these calls the driv
 * er, so they run headless - which is what makes them testable at all: the driver half (xud_dev
 * ice_open, xud_device_wait_idle, xud_device_close with a live handle) is in the GPU-gated secti
 * on next to the physical layer's own. The handles below are the test's own locals standing in f
 * or driver handles - the same handle assumption xud_loader.c asserts (VK_USE_64_BIT_PTR_DEFINES
 *  == 1, a handle is a pointer), and nothing below reads through one.
 *-----------------------------------------------------------------------------------------------*/
static void xud_test_device_pure(void) {
    XudDeviceCaps caps;
    XudDevice* device = NULL;
    XudPhysicalDevice* fakePhysical = NULL;
    VkDeviceQueueCreateInfo row;
    VkDeviceCreateInfo createInfo;
    const char* nameSlots[XUD_DEVICE_MAX_DEVICE_EXTENSIONS];
    float priorities[XUD_DEVICE_QUEUE_MAX_COUNT];
    float frameLocalPriorities[XUD_DEVICE_QUEUE_MAX_COUNT];
    VkExtensionProperties advertised[3];
    XudQueue got;
    XudQueue graphics;
    XudQueue present;
    XudResult result;
    uint32_t index = 0u;
    uint64_t fakeHandleStorage[4] = { 0u, 0u, 0u, 0u };
    const VkDevice fakeDeviceHandle = (VkDevice)&fakeHandleStorage[0];
    const VkQueue fakeQueueHandles[2]
        = { (VkQueue)&fakeHandleStorage[1], (VkQueue)&fakeHandleStorage[2] };

    memset(&caps, 0, sizeof(caps));
    memset(&row, 0, sizeof(row));
    memset(&createInfo, 0, sizeof(createInfo));
    memset(advertised, 0, sizeof(advertised));
    memset(&got, 0, sizeof(got));
    for (index = 0u; index < XUD_DEVICE_QUEUE_MAX_COUNT; index++) {
        priorities[index] = XUD_DEVICE_QUEUE_PRIORITY_DEFAULT;
        frameLocalPriorities[index] = XUD_DEVICE_QUEUE_PRIORITY_DEFAULT;
    }

    /* ---- the one queue row, from a caps that stands in for a recorded one ---- */
    caps.queueFamilyIndex = 3u;
    caps.chosenQueueFamilyProperties.sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
    caps.chosenQueueFamilyProperties.queueFamilyProperties.queueFlags = VK_QUEUE_GRAPHICS_BIT;
    caps.chosenQueueFamilyProperties.queueFamilyProperties.queueCount = 8u;
    result = xud_device_fill_queue_create_info(&row, priorities, &caps);
    xud_check(result == XUD_OK, "device/row-ok",
        "a caps with a chosen family and queues in it fills a row");
    xud_check(row.sType == VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, "device/row-stamp",
        "the row is stamped, an unstamped row is the validation layer's first complaint");
    xud_check(row.pNext == NULL, "device/row-p-next", "nothing is chained onto it");
    xud_check(row.flags == 0, "device/row-flags",
        "no protected-memory flag: nothing above this layer asks for that today");
    xud_check(row.queueFamilyIndex == 3u, "device/row-family",
        "the family the physical layer chose is the family the row asks for");
    xud_check(row.queueCount == XUD_DEVICE_QUEUE_MAX_COUNT, "device/row-count-clamped",
        "a family with 8 queues is asked for at most XUD_DEVICE_QUEUE_MAX_COUNT");
    xud_check(row.pQueuePriorities == priorities, "device/row-priorities",
        "the row points at the caller's array, it does not copy one");

    memset(&row, 0, sizeof(row));
    caps.queueFamilyIndex = XUD_PHYSICAL_NO_QUEUE_FAMILY;
    xud_check(xud_device_fill_queue_create_info(&row, priorities, &caps) == XUD_INVALID_ARG,
        "device/row-no-family", "a caps with no chosen family is not a usable input");
    xud_check(row.sType == 0u, "device/row-rejected-untouched",
        "a rejected fill leaves the row at its zero state");
    caps.queueFamilyIndex = 0u;
    caps.chosenQueueFamilyProperties.queueFamilyProperties.queueCount = 0u;
    xud_check(xud_device_fill_queue_create_info(&row, priorities, &caps) == XUD_INVALID_ARG,
        "device/row-zero-queue-count", "a family with no queue in it is not a family to ask for");
    xud_check(xud_device_fill_queue_create_info(NULL, priorities, &caps) == XUD_INVALID_ARG,
        "device/row-null-out", "a null out-row must be rejected");
    xud_check(xud_device_fill_queue_create_info(&row, NULL, &caps) == XUD_INVALID_ARG,
        "device/row-null-priorities",
        "a row with no priorities array is the validation layer's pQueuePriorities violation");
    xud_check(xud_device_fill_queue_create_info(&row, priorities, NULL) == XUD_INVALID_ARG,
        "device/row-null-caps", "null caps must be rejected");
    xud_check(
        row.sType == 0u, "device/row-invalid-untouched", "a rejected fill leaves the row alone");
    caps.chosenQueueFamilyProperties.queueFamilyProperties.queueCount = 1u;
    xud_check(xud_device_fill_queue_create_info(&row, priorities, &caps) == XUD_OK,
        "device/row-one-queue", "the driver's own family here rowed one queue");
    xud_check(row.queueCount == 1u, "device/row-one-count",
        "the clamp is an upper bound only, one queue stays one");
    xud_check(row.queueFamilyIndex == 0u, "device/row-family-zero",
        "family 0 is a real choice, the sentinel is not");

    /* ---- the device struct, and the extension selection over it ---- */
    device = calloc(1u, sizeof(XudDevice));
    xud_check(device != NULL, "device/alloc", "the struct is heap-allocated, it must allocate");
    if (device == NULL)
        return;

    xud_check(xud_device_fill_queue_create_info(&device->queueRow, device->queuePriorities, &caps)
            == XUD_OK,
        "device/own-row", "the row filled into the struct");
    xud_check(device->queueRow.pQueuePriorities == device->queuePriorities,
        "device/own-row-priorities", "and it points at the struct's own array");

    xud_check(xud_name_copy(
                  &advertised[0].extensionName[0], VK_MAX_EXTENSION_NAME_SIZE, "VK_NOT_A_REAL_NAME")
            == XUD_OK,
        "device/advertised-first-name", "a name the library does not want");
    xud_check(xud_name_copy(&advertised[1].extensionName[0], VK_MAX_EXTENSION_NAME_SIZE,
                  VK_KHR_SWAPCHAIN_EXTENSION_NAME)
            == XUD_OK,
        "device/advertised-second-name", "the one name this layer wants");
    result = xud_device_select_extensions(device, advertised, 2u);
    xud_check(result == XUD_OK, "device/select-ok", "the selection runs on an advertised list");
    xud_check(device->enabledExtensionCount == 1u, "device/select-one-enabled",
        "only the advertised wanted name is enabled");
    xud_check(
        xud_name_equal(device->enabledExtensionNameStorage[0], VK_KHR_SWAPCHAIN_EXTENSION_NAME)
            == true,
        "device/select-name", "and it is that name");

    result = xud_device_select_extensions(device, advertised, 1u);
    xud_check(result == XUD_OK, "device/select-none-wanted-advertised",
        "a list without the wanted name still selects");
    xud_check(device->enabledExtensionCount == 0u, "device/select-empty",
        "the list is rewritten, a stale name cannot survive a re-selection");
    xud_check(xud_device_select_extensions(device, NULL, 0u) == XUD_OK, "device/select-empty-list",
        "no advertised names at all is not an error");
    xud_check(device->enabledExtensionCount == 0u, "device/select-empty-list-count",
        "...and the list stays empty");
    xud_check(xud_device_select_extensions(NULL, advertised, 2u) == XUD_INVALID_ARG,
        "device/select-null-out", "a null out-device must be rejected");
    xud_check(xud_device_select_extensions(device, NULL, 2u) == XUD_INVALID_ARG,
        "device/select-null-advertised", "a non-zero count with no array is an argument error");
    xud_check(xud_name_copy(&advertised[2].extensionName[0], VK_MAX_EXTENSION_NAME_SIZE,
                  VK_KHR_SWAPCHAIN_EXTENSION_NAME)
            == XUD_OK,
        "device/advertised-duplicate-name", "the same name twice in the driver's list");
    result = xud_device_select_extensions(device, advertised, 3u);
    xud_check(result == XUD_OK, "device/select-duplicate-ok",
        "a driver that repeats a name does not break the selection");
    xud_check(device->enabledExtensionCount == 1u, "device/select-duplicate-once",
        "...and the name is enabled once, the duplicate rule holds");

    /* ---- the create-info, assembled from the struct above ---- */
    device->device = fakeDeviceHandle;
    for (index = 0u; index < XUD_DEVICE_MAX_DEVICE_EXTENSIONS; index++)
        nameSlots[index] = "poison";
    result = xud_device_fill_create_info(&createInfo, nameSlots, device);
    xud_check(result == XUD_OK, "device/create-info-ok", "a struct this layer filled assembles");
    xud_check(createInfo.sType == VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, "device/create-info-stamp",
        "the create-info is stamped");
    xud_check(
        createInfo.pNext == NULL, "device/create-info-p-next", "and nothing is chained onto it");
    xud_check(
        createInfo.flags == 0, "device/create-info-flags", "no device-level flag is set today");
    xud_check(createInfo.queueCreateInfoCount == 1u, "device/create-info-one-row",
        "exactly one queue row, the one-family rule");
    xud_check(createInfo.pQueueCreateInfos == &device->queueRow, "device/create-info-row-address",
        "the row it names is the struct's own");
    xud_check(createInfo.enabledLayerCount == 0u, "device/create-info-legacy-layers-zero",
        "the legacy layer count is written, not left uninitialised");
    xud_check(createInfo.ppEnabledLayerNames == NULL, "device/create-info-legacy-layers-null",
        "...and so is the name array it would have pointed at");
    xud_check(createInfo.enabledExtensionCount == 1u, "device/create-info-ext-count",
        "the enabled list is the selection's");
    xud_check(createInfo.ppEnabledExtensionNames == nameSlots, "device/create-info-ext-names",
        "the names travel through the caller's storage, not a frame-local inside this function");
    xud_check(nameSlots[1] == NULL, "device/name-tail-cleared",
        "the tail of the name array is NULLed, no stale pointer survives");
    xud_check(nameSlots[XUD_DEVICE_MAX_DEVICE_EXTENSIONS - 1u] == NULL, "device/name-tail-last",
        "...including the last slot");
    xud_check(createInfo.pEnabledFeatures == &device->enabledFeatures,
        "device/create-info-features", "the features it carries are the struct's own record");

    device->queueRow.sType = 0u;
    xud_check(xud_device_fill_create_info(&createInfo, nameSlots, device) == XUD_INVALID_ARG,
        "device/create-info-unstamped-row",
        "an unstamped row is refused here, not left to a validation layer");
    xud_check(createInfo.sType == VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        "device/create-info-rejected-untouched",
        "a rejected assembly leaves the create-info alone");
    device->queueRow.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;

    device->queueRow.queueCount = 0u;
    xud_check(xud_device_fill_create_info(&createInfo, nameSlots, device) == XUD_INVALID_ARG,
        "device/create-info-zero-queue-count", "a row that asks for no queue is refused");
    device->queueRow.queueCount = XUD_DEVICE_QUEUE_MAX_COUNT + 1u;
    xud_check(xud_device_fill_create_info(&createInfo, nameSlots, device) == XUD_INVALID_ARG,
        "device/create-info-count-too-big",
        "a row longer than the priorities array is refused, the row would point past its end");
    device->queueRow.queueCount = 1u;

    device->queueRow.queueFamilyIndex = XUD_PHYSICAL_NO_QUEUE_FAMILY;
    xud_check(xud_device_fill_create_info(&createInfo, nameSlots, device) == XUD_INVALID_ARG,
        "device/create-info-sentinel-family", "the no-family sentinel is not a family to ask for");
    device->queueRow.queueFamilyIndex = 0u;

    device->queueRow.pQueuePriorities = frameLocalPriorities;
    xud_check(xud_device_fill_create_info(&createInfo, nameSlots, device) == XUD_INVALID_ARG,
        "device/create-info-foreign-priorities",
        "a row pointing at another array is refused - the dangling-pointer bug this struct layout "
        "exists to prevent");
    device->queueRow.pQueuePriorities = device->queuePriorities;

    device->queueRow.pNext = (void*)&caps;
    xud_check(xud_device_fill_create_info(&createInfo, nameSlots, device) == XUD_INVALID_ARG,
        "device/create-info-row-chained", "a chained row is refused, this layer chains nothing");
    device->queueRow.pNext = NULL;

    device->enabledExtensionCount = XUD_DEVICE_MAX_DEVICE_EXTENSIONS + 1u;
    xud_check(xud_device_fill_create_info(&createInfo, nameSlots, device) == XUD_INVALID_ARG,
        "device/create-info-count-too-big-ext",
        "an enabled count that does not fit the storage it came from is refused");
    device->enabledExtensionCount = 1u;
    xud_check(xud_device_fill_create_info(NULL, nameSlots, device) == XUD_INVALID_ARG,
        "device/create-info-null-out", "a null out-create-info must be rejected");
    xud_check(xud_device_fill_create_info(&createInfo, NULL, device) == XUD_INVALID_ARG,
        "device/create-info-null-names", "a null name array must be rejected");
    xud_check(xud_device_fill_create_info(&createInfo, nameSlots, NULL) == XUD_INVALID_ARG,
        "device/create-info-null-device", "a null device must be rejected");

    /* ---- the queue lookup, on a struct that stands in for one open produced ---- */
    device->graphicsQueue.queue = fakeQueueHandles[0];
    device->graphicsQueue.queueFamilyIndex = 0u;
    device->graphicsQueue.queueIndex = 0u;
    device->presentQueue.queue = fakeQueueHandles[1];
    device->presentQueue.queueFamilyIndex = 0u;
    device->presentQueue.queueIndex = 1u;
    device->queueRow.queueCount = 2u;
    got.queue = (VkQueue)&fakeHandleStorage[3];
    xud_check(xud_device_queue_get(&got, device, XUD_QUEUE_ROLE_GRAPHICS) == XUD_OK,
        "device/queue-get-graphics", "the graphics role resolves");
    xud_check(got.queue == fakeQueueHandles[0], "device/queue-get-graphics-handle",
        "...with the handle recorded for it");
    xud_check(got.queueFamilyIndex == 0u, "device/queue-get-graphics-family",
        "and the coordinates it was resolved at");
    xud_check(got.queueIndex == 0u, "device/queue-get-graphics-index", "...the index too");
    xud_check(xud_device_queue_get(&got, device, XUD_QUEUE_ROLE_PRESENT) == XUD_OK,
        "device/queue-get-present", "the present role resolves");
    xud_check(got.queue == fakeQueueHandles[1], "device/queue-get-present-handle",
        "a second queue in the family means a second handle");
    xud_check(got.queueIndex == 1u, "device/queue-get-present-index", "at index 1");
    got.queue = (VkQueue)&fakeHandleStorage[3];
    xud_check(xud_device_queue_get(&got, device, (XudQueueRole)XUD_DEVICE_QUEUE_ROLE_COUNT)
            == XUD_INVALID_ARG,
        "device/queue-get-role-range", "a role at the enum's end is refused");
    xud_check(xud_device_queue_get(&got, device, 7u) == XUD_INVALID_ARG,
        "device/queue-get-role-way-out", "a caller can still pass 7");
    xud_check(got.queue == (VkQueue)&fakeHandleStorage[3], "device/queue-get-rejected-untouched",
        "a rejected lookup leaves the out-record alone");
    xud_check(xud_device_queue_get(NULL, device, XUD_QUEUE_ROLE_GRAPHICS) == XUD_INVALID_ARG,
        "device/queue-get-null-out", "a null out-record must be rejected");
    xud_check(xud_device_queue_get(&got, NULL, XUD_QUEUE_ROLE_GRAPHICS) == XUD_INVALID_ARG,
        "device/queue-get-null-device", "a null device must be rejected");

    device->device = VK_NULL_HANDLE;
    xud_check(xud_device_queue_get(&got, device, XUD_QUEUE_ROLE_GRAPHICS) == XUD_INVALID_ARG,
        "device/queue-get-no-handle",
        "there is no queue to hand back for a device that does not exist");
    device->device = fakeDeviceHandle;

    device->presentQueue.queue = VK_NULL_HANDLE;
    xud_check(xud_device_queue_get(&got, device, XUD_QUEUE_ROLE_PRESENT) == XUD_INTERNAL_ERROR,
        "device/queue-get-unresolved",
        "a struct that claims a live device but has no queue in it was not produced here");
    device->presentQueue.queue = fakeQueueHandles[1];

    device->presentQueue.queueFamilyIndex = 4u;
    xud_check(xud_device_queue_get(&got, device, XUD_QUEUE_ROLE_PRESENT) == XUD_INTERNAL_ERROR,
        "device/queue-get-family-mismatch",
        "a pair whose family is not the row's is refused before it is ever asked");
    device->presentQueue.queueFamilyIndex = 0u;

    device->presentQueue.queueIndex = 2u;
    xud_check(xud_device_queue_get(&got, device, XUD_QUEUE_ROLE_PRESENT) == XUD_INTERNAL_ERROR,
        "device/queue-get-index-mismatch",
        "an index at or above the row's count is refused before it is ever asked");
    device->presentQueue.queueIndex = 1u;

    /* One queue in the row, both roles holding the same handle: the shape probed on this driver
       2026-10-18, where the one family rowed one queue and vkGetDeviceQueue answers the same han
       dle for both roles.
     */
    device->queueRow.queueCount = 1u;
    device->presentQueue.queue = fakeQueueHandles[0];
    device->presentQueue.queueIndex = 0u;
    xud_check(xud_device_queue_get(&graphics, device, XUD_QUEUE_ROLE_GRAPHICS) == XUD_OK,
        "device/queue-one-graphics", "the graphics role resolves");
    xud_check(xud_device_queue_get(&present, device, XUD_QUEUE_ROLE_PRESENT) == XUD_OK,
        "device/queue-one-present", "and so does the present role");
    xud_check(graphics.queue == present.queue, "device/queue-one-same-handle",
        "both roles answer the same handle when the family rowed one queue");
    xud_check(graphics.queueIndex == present.queueIndex, "device/queue-one-same-index",
        "...and the same coordinates");

    /* The open guards that need no driver at all: the argument checks and the caps check all fi
       re before a slot is touched, so the fake instance below is never dereferenced - the same s
       hape as xud_test_physical_negative()'s fake handle.
     */
    {
        XudDevice* held = NULL;
        XudDevice* synthetic = NULL;
        XudLoader* openLoader = NULL;
        const VkInstance fakeInstance = (VkInstance)(uintptr_t)1u;

        fakePhysical = calloc(1u, sizeof(XudPhysicalDevice));
        xud_check(fakePhysical != NULL, "device/fake-physical-alloc",
            "the synthetic physical struct must allocate");

        xud_check(xud_device_open(NULL, NULL, NULL, VK_NULL_HANDLE) == XUD_INVALID_ARG,
            "device/open-null-out", "no out-param to write into");
        xud_check(xud_device_open(&held, NULL, NULL, VK_NULL_HANDLE) == XUD_INVALID_ARG,
            "device/open-null-loader", "no table to call through");
        xud_check(xud_device_open(&held, NULL, NULL, fakeInstance) == XUD_INVALID_ARG,
            "device/open-null-physical", "no caps to create from");
        xud_check(xud_device_open(&held, NULL, fakePhysical, VK_NULL_HANDLE) == XUD_INVALID_ARG,
            "device/open-null-instance", "no handle to resolve the slots through");
        xud_check(
            held == NULL, "device/open-rejected-null", "a rejected open leaves nothing behind");

        synthetic = calloc(1u, sizeof(XudDevice));
        xud_check(
            synthetic != NULL, "device/synthetic-alloc", "the synthetic struct must allocate");
        if (synthetic != NULL) {
            xud_check(xud_device_open(&synthetic, NULL, NULL, VK_NULL_HANDLE) == XUD_INVALID_ARG,
                "device/open-already-owned",
                "a pointer that already owns a device is an argument error, not a leak");
            xud_check(synthetic != NULL, "device/open-already-owned-untouched",
                "...and the struct it already owns is not freed by the refusal");

            /* A handle-less struct: the guard and the rung both answer without touching the driv
               er, which is what makes these two paths testable headless.
             */
            xud_check(xud_device_wait_idle(synthetic, NULL) == XUD_INVALID_ARG,
                "device/wait-idle-null-loader", "a null loader is an argument error");
            xud_check(xud_device_wait_idle(NULL, NULL) == XUD_INVALID_ARG,
                "device/wait-idle-null-device", "a null device too");
            xud_check(xud_device_close(NULL, NULL) == XUD_INVALID_ARG, "device/close-null-ptr",
                "a null pointer-to-pointer must be rejected");
            xud_check(xud_device_close(&synthetic, NULL) == XUD_INVALID_ARG,
                "device/close-null-loader",
                "a null loader is an argument error even before the state");
            xud_check(synthetic != NULL, "device/close-arg-rejected-untouched",
                "a rejected close leaves the struct alone");

            if (xud_loader_open(&openLoader) == XUD_OK) {
                xud_check(openLoader->instanceTable.vkCreateDevice == NULL,
                    "device/create-row-unbound",
                    "the create row is instance-level: NULL until the instance layer fills it");
                xud_check(openLoader->deviceTable.vkGetDeviceQueue == NULL,
                    "device/get-row-unbound", "same for the queue lookup");
                xud_check(openLoader->deviceTable.vkDeviceWaitIdle == NULL,
                    "device/wait-row-unbound", "same for the guard");
                xud_check(openLoader->deviceTable.vkDestroyDevice == NULL,
                    "device/destroy-row-unbound", "same for the teardown call");

                xud_check(xud_device_wait_idle(synthetic, openLoader) == XUD_ALREADY,
                    "device/wait-idle-no-handle",
                    "nothing is in flight on a device that does not exist");
                xud_check(xud_device_close(&synthetic, openLoader) == XUD_OK,
                    "device/close-handleless", "no driver call, the struct is freed");
                xud_check(
                    synthetic == NULL, "device/close-handleless-null", "and the out-param cleared");

                /* Caps with no chosen family: the check fires before any slot is read, so the f
                   ake instance is never dereferenced (xud_physical_open() refuses that state, so
                   nothing above this layer produced these caps either).
                 */
                if (fakePhysical != NULL) {
                    fakePhysical->caps.queueFamilyIndex = XUD_PHYSICAL_NO_QUEUE_FAMILY;
                    xud_check(fakePhysical->caps.queueFamilyIndex == XUD_PHYSICAL_NO_QUEUE_FAMILY,
                        "device/fake-physical-sentinel",
                        "the no-family sentinel is written explicitly - a calloc-ed caps sits at "
                        "0, which is a real family index, which is why "
                        "xud_physical_select_queue_family writes the sentinel on refusal");
                    xud_check(xud_device_open(&held, openLoader, fakePhysical, fakeInstance)
                            == XUD_INVALID_ARG,
                        "device/open-no-family",
                        "caps with no chosen family are not a usable input, checked before any "
                        "slot is read");
                    xud_check(
                        held == NULL, "device/open-no-family-null", "...and nothing was created");
                }
                xud_check(xud_loader_close(&openLoader) == XUD_OK, "device/loader-close",
                    "the loader this block opened must close");
                xud_check(openLoader == NULL, "device/loader-close-null", "and be cleared");
            } else
                printf("note: loader unavailable, device open-guard checks skipped\n");

            if (synthetic != NULL)
                free(synthetic);
            if (fakePhysical != NULL)
                free(fakePhysical);
        }
    }

    free(device);
}

/* The public runtime query: it must answer ok or unsupported, never another code, and it
   must not cache - the answer is recomputed from a fresh dlopen each call. */
static void xud_test_vulkan_supported(void) {
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
static void xud_test_init_bringup_boundary(void) {
    XudContext* context = NULL;
    const XudResult result = xud_init_context(&context, 640, 480);

    xud_check(result == XUD_UNSUPPORTED, "init/bringup-incomplete",
        "valid args must yield XUD_UNSUPPORTED until the layers below the instance land");
    xud_check(context == NULL, "init/no-context-on-unsupported",
        "an incomplete bring-up must not return a context");

    /* The same must hold for the smallest and the largest accepted extents. */
    xud_check(xud_init_context(&context, 1, 1) == XUD_UNSUPPORTED, "init/min-extent",
        "1x1 must be accepted then reported unsupported");
    xud_check(context == NULL, "init/min-extent-null", "out-param must stay NULL");
    xud_check(
        xud_init_context(&context, XUD_MAX_WINDOW_EXTENT, XUD_MAX_WINDOW_EXTENT) == XUD_UNSUPPORTED,
        "init/extent-at-limit", "the limit itself must be accepted");
    xud_check(context == NULL, "init/limit-extent-null", "out-param must stay NULL");
}

static void xud_test_close(void) {
    XudContext* context = NULL;

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
static void xud_test_no_leak(void) {
    for (int i = 0; i < 1000; i++) {
        XudContext* context = NULL;
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
static void xud_test_diagnostics_gpu(void) {
    XudLoader* loader = NULL;
    XudInstance* instance = NULL;
    XudInstance* other = NULL;
    XudDiagnostics* diagnostics = NULL;
    XudResult result;

    result = xud_loader_open(&loader);
    xud_check(result == XUD_OK, "gpu/diagnostics-loader-open",
        "the diagnostics run needs an open loader");
    if (loader == NULL)
        return;

    /* The layer query is loader-mediated and instance-level: no handle in hand, but the
       loader's global slot answers, and the count is read without an array. */
    xud_check(xud_diagnostics_query_layers(loader, NULL, NULL) == XUD_INVALID_ARG,
        "gpu/diagnostics-query-null-out",
        "a null out-param must be rejected before the driver is asked");

    result = xud_diagnostics_open(&diagnostics, loader);
    xud_check(result == XUD_OK, "gpu/diagnostics-open",
        "the struct must open with the advertised layers selected");
    if (diagnostics == NULL) {
        (void)xud_loader_close(&loader);
        return;
    }
    xud_check(diagnostics->enabledLayerCount == 3u, "gpu/diagnostics-layer-count",
        "three instance layers are advertised here");
    xud_check(diagnostics->enabledLayerNameStorage[0][0] != '\0', "gpu/diagnostics-layer-name-set",
        "the names are copied into the struct's own buffers");
    xud_check(diagnostics->enabledLayerNamePointers[0] == diagnostics->enabledLayerNameStorage[0],
        "gpu/diagnostics-layer-name-points-at-storage", "and the pointer array points at them");
    xud_check(diagnostics->messenger == VK_NULL_HANDLE, "gpu/diagnostics-messenger-not-yet",
        "no messenger before an instance exists");
    xud_check(diagnostics->instance == VK_NULL_HANDLE, "gpu/diagnostics-instance-not-yet",
        "and no borrowed owner yet");
    xud_check(diagnostics->messagesReceived == 0u, "gpu/diagnostics-no-messages-yet",
        "the counters start empty");
    xud_check(diagnostics->errorMessages == 0u, "gpu/diagnostics-no-errors-yet",
        "and the validation-clean signal starts clean");

    /* The instance layer asks the diagnostics layer for its wanted extensions, so the
       messenger extension is enabled before the instance exists. */
    result = xud_instance_create(&instance, loader, diagnostics, NULL, 0u);
    xud_check(result == XUD_OK, "gpu/diagnostics-instance-create",
        "vkCreateInstance with the validation layers enabled must succeed");
    if (instance == NULL) {
        (void)xud_diagnostics_close(&diagnostics, loader);
        (void)xud_loader_close(&loader);
        return;
    }
    xud_check(instance->instance != VK_NULL_HANDLE, "gpu/diagnostics-instance-live",
        "the instance must be live before the messenger is installed");
    xud_check(instance->enabledExtensionCount == 4u, "gpu/diagnostics-extension-count",
        "the three wanted plus VK_EXT_debug_utils, both advertised here");
    xud_check(
        xud_name_equal(instance->enabledExtensionNameStorage[3], VK_EXT_DEBUG_UTILS_EXTENSION_NAME)
            == true,
        "gpu/diagnostics-debug-utils-enabled",
        "the messenger extension is enabled in the diagnostics layer's order");

    /* The messenger row is instance-level: it is a row of the table the instance layer filled,
       and the bound check names the instance the rows belong to - a table filled for one instan
       ce is not a table for another, which is what makes a handle kept across a recreation a ca
       ught error rather than a pointer into a layer that is gone. */
    xud_check(xud_loader_instance_bound(loader, VK_NULL_HANDLE) == false,
        "gpu/diagnostics-messenger-not-null-instance", "the rows are not the null handle's rows");
    xud_check(xud_loader_instance_bound(loader, instance->instance) == true,
        "gpu/diagnostics-messenger-table-bound",
        "the instance layer filled the table for this instance");
    xud_check(loader->instanceTable.vkCreateDebugUtilsMessengerEXT != NULL,
        "gpu/diagnostics-messenger-row-resolved",
        "VK_EXT_debug_utils is enabled, so the row answers");

    result = xud_diagnostics_create_messenger(diagnostics, loader, instance->instance);
    xud_check(result == XUD_OK, "gpu/diagnostics-messenger-create",
        "the messenger must install through the live instance");
    xud_check(diagnostics->messenger != VK_NULL_HANDLE, "gpu/diagnostics-messenger-live",
        "the handle must be live");
    xud_check(diagnostics->instance == instance->instance, "gpu/diagnostics-messenger-owner",
        "the owning instance is borrowed for teardown, not copied");

    /* Asking again on a struct that already has one is XUD_ALREADY, not a second handle:
       the struct owns exactly one messenger, and a second handle would be uncloseable. */
    xud_check(
        xud_diagnostics_create_messenger(diagnostics, loader, instance->instance) == XUD_ALREADY,
        "gpu/diagnostics-messenger-twice",
        "a struct that already has a messenger is XUD_ALREADY, not a second handle");
    xud_check(diagnostics->messenger != VK_NULL_HANDLE, "gpu/diagnostics-messenger-unchanged",
        "and the handle it has is untouched");

    /* A messenger that belongs to a different VkInstance than the one being destroyed is a
       misuse: destroying it through this one would be a lie, so it is rejected before anything
       is torn down - and the second instance is torn down cleanly behind that refusal. */
    xud_check(xud_instance_create(&other, loader, diagnostics, NULL, 0u) == XUD_OK,
        "gpu/diagnostics-second-instance", "a second instance must create as cleanly as the first");
    if (other != NULL) {
        xud_check(xud_instance_destroy(&other, loader, diagnostics) == XUD_INVALID_ARG,
            "gpu/diagnostics-messenger-owner-mismatch",
            "the messenger belongs to the first instance, not this one");
        xud_check(other != NULL, "gpu/diagnostics-mismatch-leaves-instance-alive",
            "a refused destroy must not tear anything down");
        xud_check(diagnostics->messenger != VK_NULL_HANDLE,
            "gpu/diagnostics-mismatch-keeps-messenger",
            "and must not silence the messenger either");
        xud_check(xud_instance_destroy(&other, loader, NULL) == XUD_OK,
            "gpu/diagnostics-second-instance-destroy",
            "torn down without the diagnostics struct it does not own");
        xud_check(other == NULL, "gpu/diagnostics-second-instance-null", "and cleared");
    }

    /* Teardown order: the messenger dies through the instance it belongs to, inside
       xud_instance_destroy(), before that instance goes away. */
    result = xud_instance_destroy(&instance, loader, diagnostics);
    xud_check(result == XUD_OK, "gpu/diagnostics-instance-destroy",
        "the instance must be destroyed with its messenger");
    xud_check(
        instance == NULL, "gpu/diagnostics-instance-null", "destroy must clear the out-param");
    xud_check(diagnostics->messenger == VK_NULL_HANDLE, "gpu/diagnostics-messenger-gone",
        "the messenger was destroyed first");
    xud_check(diagnostics->instance == VK_NULL_HANDLE, "gpu/diagnostics-owner-cleared",
        "and the borrowed owner is cleared with it");

    /* The validation-clean signal: the messenger was installed for the whole instance
       lifetime, so a validation complaint would have reached the callback and counted here. */
    xud_check(diagnostics->messagesReceived == 0u, "gpu/diagnostics-no-messages",
        "the messenger was live for the whole instance lifetime, so a validation complaint would "
        "have counted here");
    xud_check(diagnostics->errorMessages == 0u, "gpu/diagnostics-validation-clean",
        "the run is validation-clean, not just crash-free");

    /* Closing with no messenger left is the free-and-clear half: no driver call. */
    result = xud_diagnostics_close(&diagnostics, loader);
    xud_check(result == XUD_OK, "gpu/diagnostics-close",
        "the struct must close once the messenger is gone");
    xud_check(diagnostics == NULL, "gpu/diagnostics-close-null", "and be cleared");

    xud_check(xud_loader_close(&loader) == XUD_OK, "gpu/diagnostics-loader-close",
        "the loader must close");
    xud_check(loader == NULL, "gpu/diagnostics-loader-close-null", "and be cleared");
}

/*-----------------------------------------------------------------------------------------------
 * The physical layer's argument paths, and the slot rule that must answer before any driver
 * call
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_physical_negative(void) {
    XudPhysicalDevice* physical = NULL;
    XudPhysicalDevice* second = NULL;
    XudLoader* loader = NULL;
    VkPhysicalDevice* candidates = NULL;
    uint32_t count = 0u;
    XudResult result;

    xud_check(xud_physical_open(NULL, NULL, VK_NULL_HANDLE) == XUD_INVALID_ARG,
        "physical/open-out-null", "no out-param to write into");
    xud_check(xud_physical_open(&physical, NULL, VK_NULL_HANDLE) == XUD_INVALID_ARG,
        "physical/open-loader-null", "no table to call through");
    xud_check(
        xud_physical_query_devices(NULL, &candidates, &count, VK_NULL_HANDLE) == XUD_INVALID_ARG,
        "physical/query-loader-null", "no table to call through");
    xud_check(
        xud_physical_query_devices(loader, &candidates, &count, VK_NULL_HANDLE) == XUD_INVALID_ARG,
        "physical/query-instance-null", "a NULL loader is an argument error, not a slot answer");
    xud_check(xud_physical_query_devices(loader, NULL, &count, VK_NULL_HANDLE) == XUD_INVALID_ARG,
        "physical/query-out-null", "no out-array to write into");
    xud_check(
        xud_physical_query_devices(loader, &candidates, NULL, VK_NULL_HANDLE) == XUD_INVALID_ARG,
        "physical/query-count-null", "no place to put the count");
    xud_check(
        physical == NULL, "physical/open-out-stays-null", "a rejected open leaves nothing behind");
    xud_check(
        candidates == NULL, "physical/query-out-stays-null", "a rejected query leaves no array");

    /* The loader opens on this machine with no device present (dlopen needs no GPU), so the
       instance-level slots exist but are not bound yet. The handle below is NOT a real VkInsta
       nce and must never be dereferenced: the point of the check is that the nil-slot rule ans
       wers XUD_UNSUPPORTED before the call, and a missing check would call through a NULL slo
       t and crash the run.
     */
    result = xud_loader_open(&loader);
    xud_check(result == XUD_OK, "physical/loader-open", "the loader half needs no device");
    if (loader != NULL) {
        VkInstance fake = (VkInstance)(uintptr_t)1u;

        xud_check(loader->instanceTable.vkEnumeratePhysicalDevices == NULL, "physical/row-unbound",
            "the device-list row is instance-level: NULL until the instance layer fills it");
        xud_check(xud_physical_query_devices(loader, &candidates, &count, fake) == XUD_UNSUPPORTED,
            "physical/query-slot-nil",
            "a name the table does not know is reported, never called through");
        xud_check(
            candidates == NULL, "physical/query-slot-nil-out", "and the out pair stays empty");
        xud_check(count == 0u, "physical/query-slot-nil-count", "...including the count");

        /* A rejected open with a live loader: the argument paths still come first. */
        xud_check(xud_physical_open(&physical, loader, VK_NULL_HANDLE) == XUD_INVALID_ARG,
            "physical/open-instance-null", "no handle to resolve the slots through");
        xud_check(xud_physical_open(NULL, loader, VK_NULL_HANDLE) == XUD_INVALID_ARG,
            "physical/open-out-null-live", "...and no out-param");
        xud_check(physical == NULL, "physical/open-rejected-null",
            "a rejected open leaves the out-param NULL");

        xud_check(xud_loader_close(&loader) == XUD_OK, "physical/loader-close",
            "the loader the negative run opened must close");
        xud_check(loader == NULL, "physical/loader-close-null", "and be cleared");
    }

    /* The free-only rung, on a struct this test allocated rather than one the driver handed
       over: the caps' extension array is NULL here, so the close frees the struct only. */
    physical = calloc(1u, sizeof(XudPhysicalDevice));
    xud_check(physical != NULL, "physical/synthetic-alloc", "the synthetic struct must allocate");
    if (physical != NULL) {
        xud_check(xud_physical_close(&physical) == XUD_OK, "physical/close-synthetic",
            "no driver call, the struct is freed");
        xud_check(physical == NULL, "physical/close-synthetic-null", "and the out-param cleared");
    }
    xud_check(xud_physical_close(&second) == XUD_ALREADY, "physical/close-null-already",
        "a NULL-in-handling pointer is XUD_ALREADY");
}

/*-----------------------------------------------------------------------------------------------
 * GPU-gated: the device layer against the real driver - vkCreateDevice from the recorded caps,
 * the two role queues, the wait-idle guard, the teardown rung. Called by xud_test_gpu_physical()
 * while the physical device that owns this one is still alive: a VkDevice is created from a VkPh
 * ysicalDevice handle (PFN_vkCreateDevice's first argument in these headers), so its teardown mus
 * t happen inside the owner's lifetime, the order xud_close_context() uses (device before physica
 * l). The checks below that insist on machine-specific numbers (one queue family, one queue in it,
 *  VK_KHR_swapchain advertised) are marked as such.
 *-----------------------------------------------------------------------------------------------*/
static void xud_test_device_gpu(
    XudLoader* loader, const XudPhysicalDevice* physical, VkInstance instance) {
    XudDevice* device = NULL;
    XudDevice* second = NULL;
    XudDevice* synthetic = NULL;
    XudQueue graphics;
    XudQueue present;
    XudResult result;
    uint32_t index = 0u;
    uint32_t advertisedIndex = 0u;
    bool enabledAdvertised = true;
    bool prioritiesInRange = true;
    uint32_t expectedQueueCount = 0u;

    xud_check(xud_device_open(NULL, loader, physical, instance) == XUD_INVALID_ARG,
        "device-gpu/open-null-out", "no out-param to write into");
    xud_check(xud_device_open(&device, loader, physical, VK_NULL_HANDLE) == XUD_INVALID_ARG,
        "device-gpu/open-null-instance", "no handle to resolve the slots through");
    xud_check(xud_device_open(&device, loader, NULL, instance) == XUD_INVALID_ARG,
        "device-gpu/open-null-physical", "no caps to create from");
    xud_check(
        device == NULL, "device-gpu/open-rejected-null", "a rejected open leaves nothing behind");

    result = xud_device_open(&device, loader, physical, instance);
    xud_check(result == XUD_OK, "device-gpu/open",
        "vkCreateDevice from the recorded caps must succeed on this machine");
    if (device == NULL)
        return;

    xud_check(device->device != VK_NULL_HANDLE, "device-gpu-handle", "the handle must be live");
    xud_check(loader->instanceTable.vkCreateDevice != NULL, "device-gpu-row-create",
        "the create row is an instance-level row, resolved at the instance bind");
    xud_check(xud_loader_device_bound(loader, device->device) == true, "device-gpu-table-bound",
        "the device layer filled the device table for this device");
    xud_check(loader->deviceTable.vkGetDeviceQueue != NULL, "device-gpu-row-get",
        "...and so is the queue lookup");
    xud_check(
        loader->deviceTable.vkDeviceWaitIdle != NULL, "device-gpu-row-wait", "...and the guard");
    xud_check(loader->deviceTable.vkDestroyDevice != NULL, "device-gpu-row-destroy",
        "...and the teardown call, bound before it is needed");

    xud_check(device->queueRow.sType == VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        "device-gpu-row-stamp", "the row the driver was handed is stamped");
    xud_check(
        device->queueRow.pNext == NULL, "device-gpu-row-p-next", "and nothing is chained onto it");
    xud_check(device->queueRow.flags == 0, "device-gpu-row-flags", "no protected-memory flag");
    xud_check(device->queueRow.queueFamilyIndex == physical->caps.queueFamilyIndex,
        "device-gpu-row-family", "the row asks the family the physical layer chose");
    xud_check(device->queueRow.queueCount >= 1u, "device-gpu-row-count-nonzero",
        "a row that asks for no queue is refused before the call");
    xud_check(device->queueRow.queueCount <= XUD_DEVICE_QUEUE_MAX_COUNT,
        "device-gpu-row-count-ceiling", "and never for more than this layer's ceiling");
    expectedQueueCount
        = (physical->caps.chosenQueueFamilyProperties.queueFamilyProperties.queueCount
              < XUD_DEVICE_QUEUE_MAX_COUNT)
        ? physical->caps.chosenQueueFamilyProperties.queueFamilyProperties.queueCount
        : XUD_DEVICE_QUEUE_MAX_COUNT;
    xud_check(device->queueRow.queueCount == expectedQueueCount, "device-gpu-row-count-min",
        "the row asks for min(what the family rowed, the ceiling)");
    xud_check(device->queueRow.pQueuePriorities == device->queuePriorities,
        "device-gpu-row-own-array",
        "the row points at the struct's own array, so it cannot dangle past the call");
    for (index = 0u; index < XUD_DEVICE_QUEUE_MAX_COUNT; index++) {
        if ((device->queuePriorities[index] < XUD_DEVICE_QUEUE_PRIORITY_MIN)
            || (device->queuePriorities[index] > XUD_DEVICE_QUEUE_PRIORITY_MAX))
            prioritiesInRange = false;
    }
    xud_check(prioritiesInRange == true, "device-gpu-priorities-in-range",
        "every priority is inside the spec's 0..1 range");

    /* The features the device was created with are the intersection, not the driver's verbatim
       answer: for a wanted name the AND is the driver's bit, and a name the library does not wan
       t stays off whatever the driver says about it.
     */
    xud_check(device->enabledFeatures.robustBufferAccess
            == physical->caps.features.features.robustBufferAccess,
        "device-gpu-feature-robust", "the intersection is the driver's answer for a wanted name");
    xud_check(device->enabledFeatures.samplerAnisotropy
            == physical->caps.features.features.samplerAnisotropy,
        "device-gpu-feature-anisotropy", "same");
    xud_check(device->enabledFeatures.wideLines == physical->caps.features.features.wideLines,
        "device-gpu-feature-wide-lines", "same");
    xud_check(device->enabledFeatures.geometryShader == VK_FALSE, "device-gpu-feature-not-wanted",
        "a feature the library does not want stays off, even on a driver that has it");

    /* The check-before-you-use rule, observed at the driver: every name that got enabled was in
       the recorded device-extension list. The count and the name below are machine-specific (lavk
       vk advertises VK_KHR_swapchain, probed 2026-10-18 at row 84 of 196).
     */
    for (index = 0u; index < device->enabledExtensionCount; index++) {
        bool found = false;

        for (advertisedIndex = 0u; advertisedIndex < physical->caps.deviceExtensionCount;
            advertisedIndex++) {
            if (xud_name_equal(device->enabledExtensionNameStorage[index],
                    physical->caps.deviceExtensionProperties[advertisedIndex].extensionName)
                == true)
                found = true;
        }
        if (found == false)
            enabledAdvertised = false;
    }
    xud_check(enabledAdvertised == true, "device-gpu-ext-advertised",
        "every enabled name was in the recorded list, nothing was forced on");
    xud_check(device->enabledExtensionCount == 1u, "device-gpu-ext-count",
        "machine-specific: VK_KHR_swapchain is advertised here, so exactly one wanted name is "
        "enabled");
    xud_check(
        xud_name_equal(device->enabledExtensionNameStorage[0], VK_KHR_SWAPCHAIN_EXTENSION_NAME)
            == true,
        "device-gpu-ext-swapchain", "machine-specific: and it is that name");

    xud_check(xud_device_queue_get(&graphics, device, XUD_QUEUE_ROLE_GRAPHICS) == XUD_OK,
        "device-gpu-queue-graphics", "the graphics role resolves");
    xud_check(graphics.queue != VK_NULL_HANDLE, "device-gpu-queue-graphics-handle",
        "the handle must be live");
    xud_check(graphics.queueFamilyIndex == device->queueRow.queueFamilyIndex,
        "device-gpu-queue-graphics-family", "the pair the lookup hands back is the row's own");
    xud_check(graphics.queueIndex < device->queueRow.queueCount, "device-gpu-queue-graphics-index",
        "...and the index is inside the count asked for");
    xud_check(xud_device_queue_get(&present, device, XUD_QUEUE_ROLE_PRESENT) == XUD_OK,
        "device-gpu-queue-present", "the present role resolves");
    xud_check(present.queue != VK_NULL_HANDLE, "device-gpu-queue-present-handle",
        "...with a live handle");
    xud_check(present.queueFamilyIndex == device->queueRow.queueFamilyIndex,
        "device-gpu-queue-present-family", "from the same family");
    xud_check(present.queueIndex < device->queueRow.queueCount, "device-gpu-queue-present-index",
        "at an index inside the row");
    if (device->queueRow.queueCount == 1u)
        xud_check(graphics.queue == present.queue, "device-gpu-queue-same-handle",
            "machine-specific: one queue in the family, both roles answer the same handle - "
            "presentation is not a queue flag (see xud_physical.h)");

    xud_check(xud_device_wait_idle(device, loader) == XUD_OK, "device-gpu-wait-idle",
        "a device with nothing in flight is already idle");
    xud_check(xud_device_close(&device, loader) == XUD_OK, "device-gpu-close",
        "the guard ran, the device was destroyed, the struct freed");
    xud_check(device == NULL, "device-gpu-close-null", "and the out-param cleared");
    xud_check(xud_device_close(&device, loader) == XUD_ALREADY, "device-gpu-close-twice",
        "a second close is XUD_ALREADY, not a double free");

    result = xud_device_open(&second, loader, physical, instance);
    xud_check(result == XUD_OK, "device-gpu-open-second",
        "the four cached slots survive a teardown, a recreation path works");
    if (second != NULL) {
        xud_check(second->device != VK_NULL_HANDLE, "device-gpu-second-handle",
            "the second device is live");
        xud_check(second->queueRow.queueFamilyIndex == physical->caps.queueFamilyIndex,
            "device-gpu-second-family", "...from the same recorded family");
        xud_check(xud_device_close(&second, loader) == XUD_OK, "device-gpu-close-second",
            "and tears down");
        xud_check(second == NULL, "device-gpu-second-null", "...cleared");
    }

    /* A struct that never got a handle: the guard answers XUD_ALREADY and the rung frees with no
       driver call, which is the same path the headless run covers.
     */
    synthetic = calloc(1u, sizeof(XudDevice));
    xud_check(
        synthetic != NULL, "device-gpu-synthetic-alloc", "the synthetic struct must allocate");
    if (synthetic != NULL) {
        synthetic->device = VK_NULL_HANDLE;
        xud_check(xud_device_wait_idle(synthetic, loader) == XUD_ALREADY,
            "device-gpu-wait-idle-no-handle",
            "nothing is in flight on a device that does not exist");
        xud_check(xud_device_close(&synthetic, loader) == XUD_OK, "device-gpu-close-no-handle",
            "no driver call, the struct is freed");
        xud_check(
            synthetic == NULL, "device-gpu-close-no-handle-null", "and the out-param cleared");
    }
}

/*-----------------------------------------------------------------------------------------------
 * GPU-gated: the physical layer against the real driver
 *-----------------------------------------------------------------------------------------------*/

/* Called by xud_test_gpu_instance() with the loader and instance it already has, and torn
   down before that instance goes away - the order xud_close_context() uses. The numbers the
   checks below insist on are the ones that hold on any driver (a recorded caps that says som
   ething, a chosen family that carries the graphics bit, a pNext this layer never asked for,
   an intersection that matches the driver's own answer); the machine-specific ones - lavvk /
   vulkan_intel, TigerLake-LP GT2: one device, apiVersion 1.4.354, 196 device extensions with
   VK_KHR_swapchain among them, one queue family with flags 0x1f - are marked as such.
*/
static void xud_test_gpu_physical(XudLoader* loader, VkInstance instance) {
    XudPhysicalDevice* physical = NULL;
    XudPhysicalDevice* second = NULL;
    VkPhysicalDevice* candidates = NULL;
    VkPhysicalDevice fakeHandles[1];
    VkPhysicalDeviceProperties2 fakeRows[1];
    VkQueueFamilyProperties2 families[1];
    VkQueueFamilyProperties2 chosenRow;
    VkPhysicalDeviceFeatures requested;
    XudResult result;
    uint32_t candidateCount = 0u;
    uint32_t chosen = 0u;
    uint32_t index = 0u;
    bool swapchainAdvertised = false;

    result = xud_physical_open(&physical, loader, instance);
    xud_check(result == XUD_OK, "gpu/physical-open", "the driver must hand us a device and caps");
    xud_check(physical != NULL, "gpu/physical-non-null", "the out-param must be set");
    if (physical == NULL)
        return;

    xud_check(physical->physicalDevice != VK_NULL_HANDLE, "gpu/physical-handle",
        "the handle must be live");
    xud_check(physical->caps.properties.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        "gpu/physical-stamp-properties",
        "this layer stamps the wrapper, the driver leaves it alone");
    xud_check(physical->caps.features.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        "gpu/physical-stamp-features", "same for the features row");
    xud_check(physical->caps.memoryProperties.sType
            == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
        "gpu/physical-stamp-memory", "same for the memory row");
    xud_check(physical->caps.properties.pNext == NULL, "gpu/physical-chain-properties",
        "this layer asked for no chained struct and got none");
    xud_check(physical->caps.features.pNext == NULL, "gpu/physical-chain-features",
        "same for the features row");
    xud_check(physical->caps.memoryProperties.pNext == NULL, "gpu/physical-chain-memory",
        "same for the memory row");

    xud_check(xud_physical_record_api_version(&physical->caps) == XUD_OK, "gpu/physical-api-read",
        "the read-back is idempotent on a recorded caps");
    xud_check(physical->caps.properties.properties.apiVersion >= XUD_INSTANCE_API_FLOOR,
        "gpu/physical-floor", "the recorded apiVersion clears the floor");
    xud_check(physical->caps.deviceExtensionProperties != NULL, "gpu/physical-ext-array",
        "the caps' one heap array exists");
    xud_check(physical->caps.deviceExtensionCount > 0u, "gpu/physical-ext-count",
        "the driver advertises device extensions");
    xud_check(physical->caps.deviceExtensionCapacity >= physical->caps.deviceExtensionCount,
        "gpu/physical-ext-capacity",
        "capacity is a step of the doubling sequence, not a tight fit");
    for (index = 0u; index < physical->caps.deviceExtensionCount; index++) {
        if (xud_name_equal(physical->caps.deviceExtensionProperties[index].extensionName,
                VK_KHR_SWAPCHAIN_EXTENSION_NAME)
            == true)
            swapchainAdvertised = true;
    }
    /* The swapchain layer (S7) needs this name, and the check-before-you-use rule says it has
       to be found in the recorded list rather than assumed - probed here: present, row 84 of
       196.
     */
    xud_check(swapchainAdvertised == true, "gpu/physical-swapchain-advertised",
        "VK_KHR_swapchain is in the recorded device-extension list");

    xud_check(physical->caps.queueFamilyIndex != XUD_PHYSICAL_NO_QUEUE_FAMILY,
        "gpu/physical-family-chosen", "a family carrying the graphics bit was found");
    xud_check(physical->caps.queueFamilyCount >= 1u, "gpu/physical-family-count",
        "the driver rowed at least one family");
    xud_check((physical->caps.chosenQueueFamilyProperties.queueFamilyProperties.queueFlags
                  & VK_QUEUE_GRAPHICS_BIT)
            != 0u,
        "gpu/physical-family-graphics",
        "the recorded row is the one the rule picked, and it carries the bit");
    xud_check(physical->caps.chosenQueueFamilyProperties.queueFamilyProperties.queueCount >= 1u,
        "gpu/physical-queue-count",
        "a family with no queue in it is not a family this layer can hand on");

    /* The intersection against the driver's own answer: for a wanted name the AND is the driv
       er's bit, so the recorded caps and the request must agree. The three names below are the
       layer's wanted list, whose one owner is xud_physical.c (xud_physical.h says so, and the l
       ist is not exported); spelling them here is what the test asks for, and the check below i
       s that the layer's answer matches the driver's for exactly those names.
     */
    memset(&requested, 0, sizeof(requested));
    requested.robustBufferAccess = VK_TRUE;
    requested.samplerAnisotropy = VK_TRUE;
    requested.wideLines = VK_TRUE;
    xud_check(xud_physical_record_features(&requested, &physical->caps) == XUD_OK,
        "gpu/physical-features-record", "the intersection runs on a recorded caps");
    xud_check(requested.robustBufferAccess == physical->caps.features.features.robustBufferAccess,
        "gpu/physical-feature-robust", "the AND is the driver's answer for a wanted name");
    xud_check(requested.samplerAnisotropy == physical->caps.features.features.samplerAnisotropy,
        "gpu/physical-feature-anisotropy", "same");
    xud_check(requested.wideLines == physical->caps.features.features.wideLines,
        "gpu/physical-feature-wide-lines", "same");

    /* The query helper, called the way the layer calls it. */
    result = xud_physical_query_devices(loader, &candidates, &candidateCount, instance);
    xud_check(result == XUD_OK, "gpu/physical-query-devices",
        "the device list answers through the bound slot");
    xud_check(candidateCount >= 1u, "gpu/physical-query-count",
        "this machine enumerates at least one device");
    if (candidates != NULL) {
        free(candidates);
        candidates = NULL;
    }

    /* The pure policy, run against the handle the layer just took: the row the caps carry must
       be the row the policy picks, which is what ties the driver half to the pure half. The
       handle and the row are the layer's own values, not fakes - the policy reads neither throu
       gh, it only reads the rows.
     */
    fakeHandles[0] = physical->physicalDevice;
    fakeRows[0] = physical->caps.properties;
    xud_check(xud_physical_select(&chosen, fakeHandles, fakeRows, 1u) == XUD_OK,
        "gpu/physical-select-agrees", "the policy picks the row the layer recorded");
    xud_check(chosen == 0u, "gpu/physical-select-index", "...and says so");

    families[0] = physical->caps.chosenQueueFamilyProperties;
    memset(&chosenRow, 0, sizeof(chosenRow));
    xud_check(xud_physical_select_queue_family(&chosen, &chosenRow, families, 1u) == XUD_OK,
        "gpu/physical-family-rule-agrees", "the pure rule accepts the row the driver gave");
    xud_check(chosen == 0u, "gpu/physical-family-rule-index", "...at the only position there is");
    xud_check(chosenRow.queueFamilyProperties.queueFlags
            == physical->caps.chosenQueueFamilyProperties.queueFamilyProperties.queueFlags,
        "gpu/physical-family-rule-row", "and copies the whole row it accepted");

    /* Teardown, then a second bring-up through the same table: the six slots are cached, the
       way the instance-level ones are, so a recreation path works - and the close is the free-
       only one, with no driver call in it. The device layer's own GPU section runs above this
       teardown: a VkDevice is created from a VkPhysicalDevice handle, so it must be torn down
       while its owner is alive, the order xud_close_context() uses.
     */
    xud_test_device_gpu(loader, physical, instance);

    xud_check(xud_physical_close(&physical) == XUD_OK, "gpu/physical-close",
        "the caps' array and the struct are freed, no driver call");
    xud_check(physical == NULL, "gpu/physical-close-null", "and the out-param cleared");
    xud_check(xud_physical_close(&physical) == XUD_ALREADY, "gpu/physical-close-twice",
        "a second close is XUD_ALREADY, not a double free");

    result = xud_physical_open(&second, loader, instance);
    xud_check(result == XUD_OK, "gpu/physical-open-second", "the cached slots survive a teardown");
    xud_check(second != NULL, "gpu/physical-second-non-null", "the out-param must be set");
    if (second != NULL) {
        xud_check(second->caps.deviceExtensionCount > 0u, "gpu/physical-second-count",
            "the second caps carry the list again");
        xud_check(
            xud_physical_close(&second) == XUD_OK, "gpu/physical-close-second", "and tear down");
        xud_check(second == NULL, "gpu/physical-second-null", "...cleared");
    }

    xud_check(xud_physical_open(&second, loader, VK_NULL_HANDLE) == XUD_INVALID_ARG,
        "gpu/physical-open-null-instance",
        "a NULL handle is an argument error even with a live table");
    xud_check(second == NULL, "gpu/physical-open-rejected-null", "and nothing was created");
}

/*-----------------------------------------------------------------------------------------------
 * GPU-gated: the instance layer against the real driver. This is the first test that needs
 * a device, and the run must be validation-clean, not just crash-free. It hands its loader
 * and instance to xud_test_gpu_physical() below, which is the physical layer's own GPU secti
 * on (the plan's xud_test_gpu_physical(), S5.7) - both need the same loader+instance pair,
 * and the physical device must be torn down before the instance goes away.
 *-----------------------------------------------------------------------------------------------*/
static void xud_test_gpu_instance(void) {
    XudLoader* loader = NULL;
    XudInstance* instance = NULL;
    XudResult result;

    result = xud_loader_open(&loader);
    xud_check(result == XUD_OK, "gpu/loader-open", "the GPU run needs an open loader");
    if (loader == NULL)
        return;

    /* vkDestroyInstance is instance-level: it is a row of the instance table, and that table i
       s filled only once an instance exists - the bind is a rung, not a lookup per call (see x
       ud_instance.c). */
    xud_check(loader->instanceTable.vkDestroyInstance == NULL, "gpu/destroy-row-null",
        "the row must stay NULL before the instance exists");
    xud_check(xud_loader_instance_bound(loader, VK_NULL_HANDLE) == false, "gpu/destroy-not-bound",
        "and the loader is bound to no instance yet");

    result = xud_instance_create(&instance, loader, NULL, NULL, 0u);
    xud_check(result == XUD_OK, "gpu/instance-create",
        "vkCreateInstance at the floor must succeed on this machine");
    if (instance == NULL) {
        (void)xud_loader_close(&loader);
        return;
    }
    xud_check(
        instance->instance != VK_NULL_HANDLE, "gpu/instance-handle", "the handle must be live");
    xud_check(instance->applicationInfo.apiVersion == XUD_INSTANCE_API_FLOOR, "gpu/instance-floor",
        "the floor travels to the driver");
    /* lavvk advertises all three wanted extensions here (verified 2026-10-16), so all three
       are enabled in wanted order; a driver that advertises fewer would enable fewer. */
    xud_check(instance->enabledExtensionCount == 3u, "gpu/instance-extension-count",
        "all three wanted extensions are advertised here");
    xud_check(xud_name_equal(instance->enabledExtensionNameStorage[0],
                  VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)
            == true,
        "gpu/name-first", "wanted order reaches the driver");
    xud_check(
        xud_name_equal(instance->enabledExtensionNameStorage[1], VK_KHR_SURFACE_EXTENSION_NAME)
            == true,
        "gpu/name-second", "wanted order reaches the driver");
    xud_check(xud_name_equal(instance->enabledExtensionNameStorage[2],
                  VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)
            == true,
        "gpu/name-third", "wanted order reaches the driver");
    xud_check(xud_loader_instance_bound(loader, instance->instance) == true,
        "gpu/table-bound-after-create", "the instance layer filled the table for this instance");

    /* The physical layer, opened through the live instance and torn down inside xud_test_gpu_
       physical() below before this function destroys the instance - the order xud_close_context(
       ) uses, and the reason the physical section runs here rather than after the destroy.
     */
    xud_test_gpu_physical(loader, instance->instance);

    result = xud_instance_destroy(&instance, loader, NULL);
    xud_check(result == XUD_OK, "gpu/instance-destroy",
        "the instance must be destroyed through the live handle");
    xud_check(instance == NULL, "gpu/destroy-nulls-out", "destroy must clear the out-param");
    xud_check(loader->instanceTable.vkDestroyInstance != NULL, "gpu/destroy-row-bound",
        "the instance-level teardown row was resolved by the bind");

    xud_check(xud_instance_destroy(&instance, loader, NULL) == XUD_ALREADY, "gpu/destroy-twice",
        "a second destroy is XUD_ALREADY, not a double free");

    /* A second create/destroy pair on the same table: the binding is reusable and nothing
       accumulates in the loader. */
    result = xud_instance_create(&instance, loader, NULL, NULL, 0u);
    xud_check(result == XUD_OK, "gpu/instance-create-second",
        "a second instance must create as cleanly as the first");
    xud_check(instance != NULL, "gpu/instance-second-non-null", "the out-param must be set");
    xud_check(xud_instance_destroy(&instance, loader, NULL) == XUD_OK,
        "gpu/instance-destroy-second", "and tear down");
    xud_check(instance == NULL, "gpu/instance-second-null", "destroy must clear it");

    /* Teardown of an instance that never got a handle: no driver call, struct freed. */
    instance = calloc(1, sizeof(XudInstance));
    xud_check(
        instance != NULL, "gpu/synthetic-instance-alloc", "the synthetic struct must allocate");
    if (instance != NULL) {
        instance->instance = VK_NULL_HANDLE;
        xud_check(xud_instance_destroy(&instance, loader, NULL) == XUD_OK, "gpu/destroy-handleless",
            "no driver call, the struct is freed");
        xud_check(instance == NULL, "gpu/destroy-handleless-null", "and the out-param cleared");
    }

    xud_check(xud_loader_close(&loader) == XUD_OK, "gpu/loader-close", "the loader must close");
    xud_check(loader == NULL, "gpu/loader-close-null", "and be cleared");
}

/*-----------------------------------------------------------------------------------------------
 * The window layer, headless half: every guard that runs before GLFW is called, plus the pure
 * accessors and the loader-row checks. xud_window_open() is only ever called here with bad argum
 * ents: this machine has a Wayland session, so a valid call would create a real window and the
 * default run would stop being device-free. The window that is allocated here is the synthetic st
 * ate every other layer tests - no GLFW window, no surface - which is what makes the handleless t
 * eardown rule of this layer testable without a window system.
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_window_pure(void) {
    XudLoader loader;
    XudInstance instance;
    XudPhysicalDevice physical;
    XudWindow* window = NULL;
    XudWindow* synthetic = NULL;
    const VkInstance fake = (VkInstance)(uintptr_t)1u;
    const VkInstance otherFake = (VkInstance)(uintptr_t)2u;
    const char* names[XUD_INSTANCE_EXTRA_MAX] = { NULL };
    uint32_t count = 0u;
    bool supports = false;

    memset(&loader, 0, sizeof(loader));
    loader.open = true;
    loader.boundInstance = VK_NULL_HANDLE;
    memset(&instance, 0, sizeof(instance));
    instance.instance = VK_NULL_HANDLE;
    instance.enabledExtensionCount = 0u;
    memset(&physical, 0, sizeof(physical));
    physical.physicalDevice = VK_NULL_HANDLE;
    physical.caps.queueFamilyIndex = XUD_PHYSICAL_NO_QUEUE_FAMILY;

    synthetic = calloc(1, sizeof(XudWindow));
    xud_check(synthetic != NULL, "window/synthetic-alloc", "the synthetic struct must allocate");
    if (synthetic == NULL)
        return;
    synthetic->surface = VK_NULL_HANDLE;
    synthetic->surfaceInstance = VK_NULL_HANDLE;
    synthetic->glfwWindow = NULL;

    /* open: the argument guards, all of them ahead of any GLFW call. */
    xud_check(xud_window_open(NULL, 640u, 480u) == XUD_INVALID_ARG, "window/open-null-out",
        "a null out-param must be rejected");
    xud_check(xud_window_open(&window, 0u, 480u) == XUD_INVALID_ARG, "window/open-zero-width",
        "an extent of 0 is not a window");
    xud_check(xud_window_open(&window, 640u, 0u) == XUD_INVALID_ARG, "window/open-zero-height",
        "an extent of 0 is not a window");
    xud_check(xud_window_open(&window, XUD_WINDOW_MAX_EXTENT + 1u, 480u) == XUD_INVALID_ARG,
        "window/open-over-int-width",
        "an extent above glfwCreateWindow's int bound is a bad argument, not a wrap");
    xud_check(xud_window_open(&window, 640u, XUD_WINDOW_MAX_EXTENT + 1u) == XUD_INVALID_ARG,
        "window/open-over-int-height", "the same bound on the other axis");
    xud_check(window == NULL, "window/open-refusals-null", "a refused open leaves the slot empty");
    xud_check(xud_window_open(&synthetic, 640u, 480u) == XUD_INVALID_ARG, "window/open-slot-taken",
        "a slot that already owns a window is not filled twice");
    xud_check(synthetic != NULL, "window/open-slot-taken-leaves-it",
        "the refusal must not have freed or cleared the window in hand");

    /* The accessor over an empty required list: GLFW had no Vulkan answer at open time, so the
       window asks for nothing and the GLFW call below is the one that refuses. */
    xud_check(xud_window_required_instance_extensions(synthetic, names, &count) == XUD_OK,
        "window/required-empty-ok", "an empty required list is not an error");
    xud_check(count == 0u, "window/required-empty-count", "nothing is claimed");
    xud_check(
        names[0] == NULL, "window/required-empty-untouched", "nothing beyond the count is written");
    xud_check(xud_window_required_instance_extensions(NULL, names, &count) == XUD_INVALID_ARG,
        "window/required-null-window", "a null window must be rejected");
    xud_check(xud_window_required_instance_extensions(synthetic, NULL, &count) == XUD_INVALID_ARG,
        "window/required-null-out", "a null out-array must be rejected");
    xud_check(xud_window_required_instance_extensions(synthetic, names, NULL) == XUD_INVALID_ARG,
        "window/required-null-count-out", "a null out-count must be rejected");

    /* The pure support check, both ways: one required name, enabled or not. */
    xud_check(xud_window_check_instance_support(NULL, synthetic, &instance) == XUD_INVALID_ARG,
        "window/support-null-out", "a null out-param must be rejected");
    xud_check(xud_window_check_instance_support(&supports, NULL, &instance) == XUD_INVALID_ARG,
        "window/support-null-window", "a null window must be rejected");
    xud_check(xud_window_check_instance_support(&supports, synthetic, NULL) == XUD_INVALID_ARG,
        "window/support-null-instance", "a null instance must be rejected");
    xud_check(xud_window_check_instance_support(&supports, synthetic, &instance) == XUD_OK,
        "window/support-empty-ok", "an empty required list asks for nothing");
    xud_check(supports == true, "window/support-empty-true", "so the instance supports it");

    strcpy(synthetic->requiredInstanceExtensionNameStorage[0], VK_KHR_SURFACE_EXTENSION_NAME);
    synthetic->requiredInstanceExtensionCount = 1u;
    xud_check(xud_window_check_instance_support(&supports, synthetic, &instance) == XUD_OK,
        "window/support-missing-ok", "the check itself is not an error");
    xud_check(supports == false, "window/support-missing-false",
        "a required name the instance does not have is not supported");
    strcpy(instance.enabledExtensionNameStorage[0], VK_KHR_SURFACE_EXTENSION_NAME);
    instance.enabledExtensionCount = 1u;
    xud_check(xud_window_check_instance_support(&supports, synthetic, &instance) == XUD_OK,
        "window/support-enabled-ok", "the same check with the name enabled");
    xud_check(
        supports == true, "window/support-enabled-true", "the name being enabled is what flips it");

    /* presentation_support: every guard, and the refused call leaves the answer alone. The fiel
       d guards are reached one at a time, with the guards above them already satisfied. */
    supports = true;
    xud_check(
        xud_window_presentation_support(NULL, synthetic, &instance, &physical) == XUD_INVALID_ARG,
        "window/presentation-null-out", "a null out-param must be rejected");
    xud_check(
        xud_window_presentation_support(&supports, NULL, &instance, &physical) == XUD_INVALID_ARG,
        "window/presentation-null-window", "a null window must be rejected");
    xud_check(
        xud_window_presentation_support(&supports, synthetic, NULL, &physical) == XUD_INVALID_ARG,
        "window/presentation-null-instance", "a null instance must be rejected");
    xud_check(
        xud_window_presentation_support(&supports, synthetic, &instance, NULL) == XUD_INVALID_ARG,
        "window/presentation-null-physical", "a null physical device must be rejected");
    synthetic->glfwWindow = (GLFWwindow*)(uintptr_t)1u;
    xud_check(xud_window_presentation_support(&supports, synthetic, &instance, &physical)
            == XUD_INVALID_ARG,
        "window/presentation-handleless-instance",
        "GLFW asserts the instance is live, so a handle-less one is a bad argument");
    instance.instance = fake;
    xud_check(xud_window_presentation_support(&supports, synthetic, &instance, &physical)
            == XUD_INVALID_ARG,
        "window/presentation-handleless-physical", "and the same for the physical device");
    physical.physicalDevice = (VkPhysicalDevice)(uintptr_t)1u;
    xud_check(xud_window_presentation_support(&supports, synthetic, &instance, &physical)
            == XUD_INVALID_ARG,
        "window/presentation-no-family",
        "a physical device whose queue family was never chosen has nothing to ask about");
    physical.caps.queueFamilyIndex = 0u;
    instance.enabledExtensionCount = 0u;
    xud_check(xud_window_presentation_support(&supports, synthetic, &instance, &physical)
            == XUD_UNSUPPORTED,
        "window/presentation-refused",
        "an instance that cannot have a surface for this window is refused without a call");
    xud_check(supports == true, "window/presentation-refused-leaves-answer",
        "a refused call must not have written an answer: it was true above and stayed true");

    /* surface_create: the loader and instance guards, then the two row checks. No GLFW call is
       reached on any of these paths - the fake window handle is never dereferenced. */
    xud_check(xud_window_surface_create(NULL, &loader, &instance) == XUD_INVALID_ARG,
        "window/surface-null-window", "a null window must be rejected");
    xud_check(xud_window_surface_create(synthetic, NULL, &instance) == XUD_INVALID_ARG,
        "window/surface-null-loader", "a null loader must be rejected");
    xud_check(xud_window_surface_create(synthetic, &loader, NULL) == XUD_INVALID_ARG,
        "window/surface-null-instance", "a null instance must be rejected");
    loader.open = false;
    xud_check(xud_window_surface_create(synthetic, &loader, &instance) == XUD_INVALID_ARG,
        "window/surface-closed-loader", "a closed loader has no rows to call through");
    loader.open = true;
    strcpy(instance.enabledExtensionNameStorage[0], VK_KHR_SURFACE_EXTENSION_NAME);
    instance.enabledExtensionCount = 1u;
    xud_check(xud_window_surface_create(synthetic, &loader, &instance) == XUD_UNSUPPORTED,
        "window/surface-not-bound",
        "rows that are not this instance's rows cannot create its surface");
    loader.boundInstance = fake;
    xud_check(xud_window_surface_create(synthetic, &loader, &instance) == XUD_UNSUPPORTED,
        "window/surface-no-destroy-row",
        "a surface this layer cannot destroy is a surface it must not create");
    xud_check(synthetic->surface == VK_NULL_HANDLE, "window/surface-refusals-stay-empty",
        "a refused create leaves the surface at its zero state");

    /* surface_close: no surface is already-closed, and a surface whose instance rows are gone,
       or whose destroy row is gone, is refused without a call. */
    xud_check(xud_window_surface_close(NULL, &loader) == XUD_INVALID_ARG,
        "window/surface-close-null-window", "a null window must be rejected");
    xud_check(xud_window_surface_close(synthetic, NULL) == XUD_INVALID_ARG,
        "window/surface-close-null-loader", "a null loader must be rejected");
    loader.open = false;
    xud_check(xud_window_surface_close(synthetic, &loader) == XUD_INVALID_ARG,
        "window/surface-close-closed-loader", "a closed loader has no rows to call through");
    loader.open = true;
    xud_check(xud_window_surface_close(synthetic, &loader) == XUD_ALREADY,
        "window/surface-close-nothing", "no surface is already closed, not an error");
    synthetic->surface = (VkSurfaceKHR)(uintptr_t)1u;
    synthetic->surfaceInstance = otherFake;
    xud_check(xud_window_surface_close(synthetic, &loader) == XUD_UNSUPPORTED,
        "window/surface-close-other-instance-rows",
        "a surface must not be destroyed through another instance's rows");
    synthetic->surfaceInstance = fake;
    xud_check(xud_window_surface_close(synthetic, &loader) == XUD_UNSUPPORTED,
        "window/surface-close-no-destroy-row",
        "the row this layer needs is not in the table, so the surface stays");
    xud_check(synthetic->surface != VK_NULL_HANDLE, "window/surface-close-refusal-keeps-it",
        "a refused teardown must not clear a live surface");

    /* window_close: the surface gate, and the handleless teardown. */
    xud_check(xud_window_close(NULL) == XUD_INVALID_ARG, "window/close-null-out",
        "a null out-param must be rejected");
    xud_check(xud_window_close(&window) == XUD_ALREADY, "window/close-nothing",
        "no struct is already closed, not an error");
    xud_check(xud_window_close(&synthetic) == XUD_INVALID_ARG, "window/close-under-surface",
        "a window whose surface is still live must not be destroyed under it");
    xud_check(synthetic != NULL, "window/close-refusal-keeps-window",
        "the refusal must not have freed the window");
    synthetic->surface = VK_NULL_HANDLE;
    synthetic->glfwWindow = NULL;
    xud_check(xud_window_close(&synthetic) == XUD_OK, "window/close-handleless",
        "a window that never got a GLFW window frees without a GLFW call");
    xud_check(synthetic == NULL, "window/close-clears-out", "close clears the out-param");
}

/*-----------------------------------------------------------------------------------------------
 * The window layer against the driver: a real GLFW window on this machine's Wayland session, an
 * instance that carries GLFW's required names, the window system's presentation answer, and a su
 * rface created and destroyed through that instance. The teardown order is the one xud_close_con
 * text() will use: surface before window, window before instance.
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_gpu_window(void) {
    XudLoader* loader = NULL;
    XudInstance* instance = NULL;
    XudPhysicalDevice* physical = NULL;
    XudWindow* window = NULL;
    XudWindow* first = NULL;
    XudWindow* second = NULL;
    const char* required[XUD_INSTANCE_EXTRA_MAX] = { NULL };
    uint32_t requiredCount = 0u;
    bool supports = false;
    XudResult result;

    result = xud_loader_open(&loader);
    xud_check(result == XUD_OK, "gpu-window/loader-open", "the GPU run needs an open loader");
    if (loader == NULL)
        return;

    result = xud_window_open(&window, 320u, 240u);
    xud_check(result == XUD_OK, "gpu-window/open",
        "this machine has a Wayland session, so GLFW must create a window");
    if (window == NULL) {
        (void)xud_loader_close(&loader);
        return;
    }

    xud_check(window->glfwWindow != NULL, "gpu-window/glfw-handle",
        "the window half of the struct is live");
    xud_check((window->width == 320u) && (window->height == 240u), "gpu-window/extent-recorded",
        "the extent asked for is the extent recorded");
    xud_check(window->surface == VK_NULL_HANDLE, "gpu-window/surface-null-at-open",
        "opening a window creates no surface");
    xud_check(window->surfaceInstance == VK_NULL_HANDLE, "gpu-window/surface-instance-null",
        "and records no instance for one");

    xud_check(xud_window_required_instance_extensions(window, required, &requiredCount) == XUD_OK,
        "gpu-window/required-accessor", "the recorded list is readable");
    xud_check(requiredCount == 2u, "gpu-window/required-count",
        "GLFW's Wayland backend asks for exactly two instance-level names");
    xud_check(xud_name_equal(required[0], VK_KHR_SURFACE_EXTENSION_NAME) == true,
        "gpu-window/required-first", "the surface extension, which this layer already wants");
    xud_check(xud_name_equal(required[1], "VK_KHR_wayland_surface") == true,
        "gpu-window/required-second",
        "the window system's own name, spelled out here: VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME "
        "is in vulkan_wayland.h, which this tree does not include");

    /* GLFW's names have to be part of THIS instance's enabled list: glfwCreateWindowSurface resol
       ves vkCreateWaylandSurfaceKHR through vkGetInstanceProcAddr against the instance it is hand
       ed, and the row answers NULL for an instance that does not have the extension enabled. VK_KH
       R_surface is wanted by the instance layer already, so it is not enabled twice. */
    result = xud_instance_create(&instance, loader, NULL, required, requiredCount);
    xud_check(
        result == XUD_OK, "gpu-window/instance-create-merged", "the merged instance must create");
    if (instance == NULL) {
        (void)xud_window_close(&window);
        (void)xud_loader_close(&loader);
        return;
    }
    xud_check(instance->enabledExtensionCount == 4u, "gpu-window/enabled-count",
        "three wanted names plus GLFW's wayland name, VK_KHR_surface not twice");
    xud_check(xud_window_check_instance_support(&supports, window, instance) == XUD_OK,
        "gpu-window/support-check", "the pure check runs against the merged instance");
    xud_check(supports == true, "gpu-window/support-true",
        "the merge worked: GLFW's required names are enabled here");

    result = xud_physical_open(&physical, loader, instance->instance);
    xud_check(result == XUD_OK, "gpu-window/physical-open", "the device is queried through it");
    if (physical == NULL) {
        (void)xud_window_close(&window);
        (void)xud_instance_destroy(&instance, loader, NULL);
        (void)xud_loader_close(&loader);
        return;
    }

    xud_check(xud_window_presentation_support(&supports, window, instance, physical) == XUD_OK,
        "gpu-window/presentation-call", "the query itself is not an error");
    xud_check(supports == true, "gpu-window/support-presented",
        "the Wayland window system presents to this physical device, through the chosen family");

    xud_check(loader->instanceTable.vkDestroySurfaceKHR != NULL, "gpu-window/destroy-row-live",
        "the row that tears a surface down exists here, so a surface may be created");

    result = xud_window_surface_create(window, loader, instance);
    xud_check(result == XUD_OK, "gpu-window/surface-create",
        "glfwCreateWindowSurface hands this window a surface for that instance");
    xud_check(window->surface != VK_NULL_HANDLE, "gpu-window/surface-live", "the surface is live");
    xud_check(window->surfaceInstance == instance->instance, "gpu-window/surface-instance-recorded",
        "and the instance it belongs to is recorded, for the teardown call that needs it");
    xud_check(xud_window_surface_create(window, loader, instance) == XUD_INVALID_ARG,
        "gpu-window/surface-create-twice", "a window does not get two surfaces");

    xud_check(xud_window_close(&window) == XUD_INVALID_ARG,
        "gpu-window/close-refused-under-surface",
        "the window must not be destroyed while its surface is still live");
    xud_check(
        window != NULL, "gpu-window/close-refusal-leaves-window", "and the refusal left it alone");

    xud_check(xud_window_surface_close(window, loader) == XUD_OK, "gpu-window/surface-close",
        "vkDestroySurfaceKHR through the instance it belongs to, while that instance is alive");
    xud_check(
        window->surface == VK_NULL_HANDLE, "gpu-window/surface-cleared", "the handle cleared");
    xud_check(window->surfaceInstance == VK_NULL_HANDLE, "gpu-window/surface-instance-cleared",
        "and so did the borrowed instance");
    xud_check(xud_window_surface_close(window, loader) == XUD_ALREADY,
        "gpu-window/surface-close-twice", "a second teardown is reported, not a double free");

    xud_check(
        xud_window_close(&window) == XUD_OK, "gpu-window/window-close", "the window half now");
    xud_check(window == NULL, "gpu-window/window-null-after-close", "and the slot is cleared");
    xud_check(xud_window_close(&window) == XUD_ALREADY, "gpu-window/window-close-twice",
        "a second close is reported too");

    /* Two windows alive at once: GLFW's state is process-level, and the count that decides when g
       lwfTerminate runs is the one thing that makes the second window's teardown safe. */
    result = xud_window_open(&first, 64u, 64u);
    xud_check(result == XUD_OK, "gpu-window/open-first", "a second run opens one more window");
    result = xud_window_open(&second, 64u, 64u);
    xud_check(result == XUD_OK, "gpu-window/open-second", "and a third at the same time");
    xud_check((first != NULL) && (second != NULL), "gpu-window/two-live",
        "both windows are alive together");
    if ((first != NULL) && (second != NULL)) {
        xud_check(first->glfwWindow != second->glfwWindow, "gpu-window/two-distinct",
            "they are two windows, not one window twice");
        xud_check(xud_window_close(&first) == XUD_OK, "gpu-window/close-first-of-pair",
            "closing one of the pair leaves GLFW alive for the other");
        xud_check(xud_window_close(&second) == XUD_OK, "gpu-window/close-second-of-pair",
            "and the last one runs GLFW's own teardown");
    }

    xud_check(xud_physical_close(&physical) == XUD_OK, "gpu-window/physical-close", "free-only");
    xud_check(physical == NULL, "gpu-window/physical-null", "and cleared");
    xud_check(xud_instance_destroy(&instance, loader, NULL) == XUD_OK,
        "gpu-window/instance-destroy",
        "the instance the surface was created for goes after the surface went");
    xud_check(instance == NULL, "gpu-window/instance-null", "and cleared");
    xud_check(xud_loader_close(&loader) == XUD_OK, "gpu-window/loader-close", "the loader closes");
    xud_check(loader == NULL, "gpu-window/loader-null", "and cleared");
}

int main(void) {
    const int gpu = xud_gpu_tests_enabled();

    printf("xudlibc %s test run: gpu gate %s (compile-time default %d)\n", XUD_VERSION,
        XUD_ENV_GPU_TESTS, XUD_GPU_TESTS);

    xud_test_version();
    xud_test_result_names();
    xud_test_result_mapping();
    xud_test_name_helpers();
    xud_test_loader_nil_guard();
    xud_test_loader_table();
    xud_test_diagnostics_pure();
    xud_test_instance_pure();
    xud_test_physical_pure();
    xud_test_device_pure();
    xud_test_window_pure();
    xud_test_physical_negative();
    xud_test_vulkan_supported();
    xud_test_init_arg_validation();
    xud_test_init_bringup_boundary();
    xud_test_close();
    xud_test_no_leak();

    /* GPU-gated section: the instance layer and the physical layer are the things that need a
       device (the physical layer needs an instance to enumerate through), and the diagnostics l
       ayer is the one that puts a validation layer behind the instance. */
    if (gpu) {
        xud_test_gpu_instance();
        xud_test_diagnostics_gpu();
        xud_test_gpu_window();
    } else
        printf("gpu checks: skipped (set XUD_GPU_TESTS=1 to run them)\n");

    printf("run complete: %d checks, %d failures\n", xudChecks, xudFailures);

    return xudFailures == 0 ? 0 : 1;
}
