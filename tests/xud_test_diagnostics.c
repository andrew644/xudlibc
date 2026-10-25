/***********************************************************************************************
 *
 *   xud_test_diagnostics.c - the diagnostics layer: advertised layers, validation features, the debug messenger
 *
 *   Owns : the layer query, the wanted-extension accessor, the severity names, the messenger create-info assembly, the log callback and its
 * counters - plus the GPU half that installs a real messenger through a real instance. Layer: tests. Sees xud_diagnostics.h and
 * xud_instance.h: the messenger pair is instance-level, so the GPU half needs an instance of its own, opened and destroyed inside this
 * area.
 *
 *   Deterministic: fixed loop counts, no time, no randomness, no filesystem.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud.h" /* Required for: XudResult, XUD_VERSION, the XUD_GPU_TESTS gate */
#include "xud_diagnostics.h" /* Required for: XudDiagnostics, the layer query, the messenger rungs, the log callback */
#include "xud_instance.h" /* Required for: XudInstance, xud_instance_create/destroy - the GPU half needs an instance to install the messenger through */
#include "xud_loader.h" /* Required for: XudLoader, xud_loader_instance_bound */
#include "xud_test_harness.h" /* Required for: xud_check, xud_gpu_tests_enabled, the area entry points */
#include <string.h> /* Required for: memset, the synthetic rows this area zeroes and compares */

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
    xud_check(wantedCount == XUD_DIAGNOSTICS_WANTED_MAX, "diagnostics/wanted-count", "exactly its cap");
    xud_check(
        xud_name_equal(wanted[0], VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == true, "diagnostics/wanted-first", "the messenger extension first");
    xud_check(xud_name_equal(wanted[1], VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME) == true, "diagnostics/wanted-second",
        "the validation-feature extension second");
    xud_check(xud_diagnostics_wanted_extensions(NULL, &wantedCount) == XUD_INVALID_ARG, "diagnostics/wanted-null-names",
        "a null name array must be rejected");
    xud_check(xud_diagnostics_wanted_extensions(wanted, NULL) == XUD_INVALID_ARG, "diagnostics/wanted-null-count",
        "a null count must be rejected");

    /* The validation-feature request is a pNext chain member, so it must be complete before
       the instance layer assembles VkInstanceCreateInfo. Only two enables are asked for -
       best practices and synchronization validation - and nothing is disabled, because the
       driver's default set is already the one this library wants. */
    result = xud_diagnostics_fill_validation_features(&diag);
    xud_check(result == XUD_OK, "diagnostics/features-ok", "the request must assemble");
    xud_check(diag.validationFeatures.sType == VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT, "diagnostics/features-s-type",
        "sType must be the validation-features struct type");
    xud_check(diag.validationFeatures.pNext == NULL, "diagnostics/features-p-next-null",
        "nothing is chained onto the validation-feature request");
    xud_check(diag.validationFeatures.enabledValidationFeatureCount == 2u, "diagnostics/features-enabled-count",
        "best practices plus synchronization validation");
    xud_check(diag.validationFeatures.pEnabledValidationFeatures == diag.enabledValidationFeatures,
        "diagnostics/features-points-at-own-storage", "the request points at the struct's own fixed array");
    xud_check(diag.validationFeatures.pEnabledValidationFeatures[0] == VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT,
        "diagnostics/features-best-practices", "best practices first");
    xud_check(diag.validationFeatures.pEnabledValidationFeatures[1] == VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT,
        "diagnostics/features-sync-validation", "synchronization validation second");
    xud_check(diag.validationFeatures.disabledValidationFeatureCount == 0u, "diagnostics/features-disabled-count",
        "nothing is disabled: the default set is already right");
    xud_check(
        diag.validationFeatures.pDisabledValidationFeatures == NULL, "diagnostics/features-disabled-null", "no array without a count");
    xud_check(xud_diagnostics_fill_validation_features(NULL) == XUD_INVALID_ARG, "diagnostics/features-null-out",
        "a null struct must be rejected");

    /* The messenger create-info: what the driver is asked to install. VERBOSE is deliberately
       out of the severity mask - it is chatter, not something a caller acts on - and the
       ERROR bit is deliberately in, because the validation-clean signal of the GPU run is
       built on the callback seeing every error. */
    result = xud_diagnostics_fill_messenger_create_info(&diag);
    xud_check(result == XUD_OK, "diagnostics/messenger-info-ok", "the create-info must assemble");
    xud_check(diag.messengerCreateInfo.sType == VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT, "diagnostics/messenger-s-type",
        "sType must be the messenger-create struct type");
    xud_check(
        diag.messengerCreateInfo.pNext == NULL, "diagnostics/messenger-p-next-null", "nothing is chained onto the messenger create-info");
    xud_check(diag.messengerCreateInfo.flags == 0u, "diagnostics/messenger-flags", "no flags on the messenger create-info");
    xud_check((diag.messengerCreateInfo.messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) != 0u, "diagnostics/severity-info",
        "INFO is reported");
    xud_check((diag.messengerCreateInfo.messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0u,
        "diagnostics/severity-warning", "WARNING is reported");
    xud_check((diag.messengerCreateInfo.messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0u,
        "diagnostics/severity-error", "ERROR is reported");
    xud_check((diag.messengerCreateInfo.messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) == 0u,
        "diagnostics/severity-no-verbose", "VERBOSE stays out");
    xud_check((diag.messengerCreateInfo.messageType & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0u, "diagnostics/type-validation",
        "validation complaints are reported");
    xud_check((diag.messengerCreateInfo.messageType & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) != 0u,
        "diagnostics/type-performance", "performance warnings are reported");
    xud_check((diag.messengerCreateInfo.messageType & VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT) != 0u, "diagnostics/type-general",
        "everything else the layer says is reported");
    xud_check(diag.messengerCreateInfo.pfnUserCallback == xud_diagnostics_message_callback, "diagnostics/callback-bound",
        "the one log callback is the one installed");
    xud_check(diag.messengerCreateInfo.pUserData == (void*)&diag, "diagnostics/user-data-is-struct",
        "the callback counts into the struct it was given");
    xud_check(xud_diagnostics_fill_messenger_create_info(NULL) == XUD_INVALID_ARG, "diagnostics/messenger-info-null-out",
        "a null struct must be rejected");

    /* Severity naming: the exact single bits, and nothing else - a combination is not one
       severity, so it has no name. */
    severityName = xud_diagnostics_severity_name(VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT);
    xud_check((severityName != NULL) && xud_name_equal(severityName, "ERROR"), "diagnostics/severity-name-error", "ERROR names as ERROR");
    severityName = xud_diagnostics_severity_name(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT);
    xud_check(
        (severityName != NULL) && xud_name_equal(severityName, "WARNING"), "diagnostics/severity-name-warning", "WARNING names as WARNING");
    severityName = xud_diagnostics_severity_name(VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT);
    xud_check((severityName != NULL) && xud_name_equal(severityName, "INFO"), "diagnostics/severity-name-info", "INFO names as INFO");
    severityName = xud_diagnostics_severity_name(VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT);
    xud_check(
        (severityName != NULL) && xud_name_equal(severityName, "VERBOSE"), "diagnostics/severity-name-verbose", "VERBOSE names as VERBOSE");
    xud_check(xud_diagnostics_severity_name(0) == NULL, "diagnostics/severity-name-zero", "no severity names nothing");
    xud_check(xud_diagnostics_severity_name(VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
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

    xud_check(xud_diagnostics_message_callback(
                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, &synthetic, &diag)
            == VK_FALSE,
        "diagnostics/callback-error", "the callback must never abort a driver");
    xud_check(diag.messagesReceived == 1u, "diagnostics/callback-counts-one", "one message counted");
    xud_check(diag.errorMessages == 1u, "diagnostics/callback-counts-error", "an ERROR severity counts as an error");
    xud_check(xud_diagnostics_message_callback(
                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, &synthetic, &diag)
            == VK_FALSE,
        "diagnostics/callback-warning", "WARNING is logged too, and still does not abort");
    xud_check(diag.messagesReceived == 2u, "diagnostics/callback-counts-two", "the running total travels");
    xud_check(diag.errorMessages == 1u, "diagnostics/callback-warning-not-error", "WARNING is not an error");
    xud_check(xud_diagnostics_message_callback(
                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, NULL, NULL)
            == VK_FALSE,
        "diagnostics/callback-null-data", "a null callbackData and null userData must not crash it");
    xud_check(
        diag.messagesReceived == 2u, "diagnostics/callback-null-data-not-counted", "with no struct in hand there is nothing to count into");
    xud_check(diag.errorMessages == 1u, "diagnostics/callback-null-data-not-error", "INFO is not an error");
    xud_check(xud_diagnostics_message_callback(
                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, NULL, &diag)
            == VK_FALSE,
        "diagnostics/callback-null-data-live-user", "a null callbackData with the struct in hand is the shape a driver can send");
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
    xud_check(xud_name_equal(diag.enabledLayerNameStorage[0], "VK_LAYER_TEST_one") == true, "diagnostics/layers-first",
        "advertised order, not sorted");
    xud_check(xud_name_equal(diag.enabledLayerNameStorage[2], "VK_LAYER_TEST_three") == true, "diagnostics/layers-last",
        "the last advertised layer is the last enabled one");
    xud_check(diag.enabledLayerNamePointers[0] == diag.enabledLayerNameStorage[0], "diagnostics/layers-point-at-storage",
        "the pointer array points at the struct's own buffers");
    xud_check(diag.enabledLayerNamePointers[2] == diag.enabledLayerNameStorage[2], "diagnostics/layers-point-at-storage-last",
        "the rule holds for the last row too");
    xud_check(diag.enabledLayerNamePointers[3] == NULL, "diagnostics/layers-tail-null", "the unused tail of the pointer array is NULLed");
    xud_check(xud_name_equal(diag.enabledLayerNameStorage[3], "") == true, "diagnostics/layers-storage-tail-untouched",
        "nothing beyond the count is written");

    /* The cap: a list that does not fit is an error, never a silent truncation - and the
       struct must not be left half-written. */
    xud_check(xud_diagnostics_select_layers(&diag, advertised, XUD_MAX_DIAGNOSTIC_LAYERS + 1u) == XUD_OUT_OF_RESOURCES,
        "diagnostics/layers-over-cap", "a layer list that does not fit is an error, not a truncation");
    xud_check(diag.enabledLayerCount == 0u, "diagnostics/layers-over-cap-cleared", "and it leaves nothing half-written");

    /* No advertised layers is a valid empty selection; a count without an array is a lie. */
    xud_check(xud_diagnostics_select_layers(&diag, NULL, 0u) == XUD_OK, "diagnostics/layers-none", "no advertised layers is not an error");
    xud_check(diag.enabledLayerCount == 0u, "diagnostics/layers-none-count", "and nothing is enabled");
    xud_check(xud_diagnostics_select_layers(&diag, NULL, 3u) == XUD_INVALID_ARG, "diagnostics/layers-null-array",
        "a count without an array must be rejected");
    xud_check(xud_diagnostics_select_layers(NULL, advertised, 3u) == XUD_INVALID_ARG, "diagnostics/layers-null-out",
        "a null struct must be rejected");

    /* The driver half, guarded headlessly: a loader with no layer slot cannot answer, and a
       closed loader cannot open or close a diagnostics struct. */
    xud_check(xud_diagnostics_query_layers(&closed, &queried, &queriedCount) == XUD_UNSUPPORTED, "diagnostics/query-closed-loader",
        "a loader with no layer slot cannot answer");
    xud_check((queried == NULL) && (queriedCount == 0u), "diagnostics/query-closed-out", "a failed query must leave both out-params empty");
    xud_check(xud_diagnostics_query_layers(&closed, NULL, NULL) == XUD_INVALID_ARG, "diagnostics/query-null-out",
        "a null out-param must be rejected before the driver is asked");
    xud_check(xud_diagnostics_open(&noneYet, &closed) == XUD_UNSUPPORTED, "diagnostics/open-closed-loader",
        "a loader with no layer slot cannot answer the query the struct is built from");
    xud_check(noneYet == NULL, "diagnostics/open-closed-out", "and nothing is allocated behind a closed loader");
    xud_check(xud_diagnostics_open(NULL, &closed) == XUD_INVALID_ARG, "diagnostics/open-null-out", "a null out-param must be rejected");
    xud_check(xud_diagnostics_close(&noneYet, &closed) == XUD_ALREADY, "diagnostics/close-already", "closing nothing is XUD_ALREADY");
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
    xud_check(result == XUD_OK, "gpu/diagnostics-loader-open", "the diagnostics run needs an open loader");
    if (loader == NULL)
        return;

    /* The layer query is loader-mediated and instance-level: no handle in hand, but the
       loader's global slot answers, and the count is read without an array. */
    xud_check(xud_diagnostics_query_layers(loader, NULL, NULL) == XUD_INVALID_ARG, "gpu/diagnostics-query-null-out",
        "a null out-param must be rejected before the driver is asked");

    result = xud_diagnostics_open(&diagnostics, loader);
    xud_check(result == XUD_OK, "gpu/diagnostics-open", "the struct must open with the advertised layers selected");
    if (diagnostics == NULL) {
        (void)xud_loader_close(&loader);
        return;
    }
    xud_check(diagnostics->enabledLayerCount == 3u, "gpu/diagnostics-layer-count", "three instance layers are advertised here");
    xud_check(diagnostics->enabledLayerNameStorage[0][0] != '\0', "gpu/diagnostics-layer-name-set",
        "the names are copied into the struct's own buffers");
    xud_check(diagnostics->enabledLayerNamePointers[0] == diagnostics->enabledLayerNameStorage[0],
        "gpu/diagnostics-layer-name-points-at-storage", "and the pointer array points at them");
    xud_check(diagnostics->messenger == VK_NULL_HANDLE, "gpu/diagnostics-messenger-not-yet", "no messenger before an instance exists");
    xud_check(diagnostics->instance == VK_NULL_HANDLE, "gpu/diagnostics-instance-not-yet", "and no borrowed owner yet");
    xud_check(diagnostics->messagesReceived == 0u, "gpu/diagnostics-no-messages-yet", "the counters start empty");
    xud_check(diagnostics->errorMessages == 0u, "gpu/diagnostics-no-errors-yet", "and the validation-clean signal starts clean");

    /* The instance layer asks the diagnostics layer for its wanted extensions, so the
       messenger extension is enabled before the instance exists. */
    result = xud_instance_create(&instance, loader, diagnostics, NULL, 0u);
    xud_check(result == XUD_OK, "gpu/diagnostics-instance-create", "vkCreateInstance with the validation layers enabled must succeed");
    if (instance == NULL) {
        (void)xud_diagnostics_close(&diagnostics, loader);
        (void)xud_loader_close(&loader);
        return;
    }
    xud_check(instance->instance != VK_NULL_HANDLE, "gpu/diagnostics-instance-live",
        "the instance must be live before the messenger is installed");
    xud_check(instance->enabledExtensionCount == 4u, "gpu/diagnostics-extension-count",
        "the three wanted plus VK_EXT_debug_utils, both advertised here");
    xud_check(xud_name_equal(instance->enabledExtensionNameStorage[3], VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == true,
        "gpu/diagnostics-debug-utils-enabled", "the messenger extension is enabled in the diagnostics layer's order");

    /* The messenger row is instance-level: it is a row of the table the instance layer filled,
       and the bound check names the instance the rows belong to - a table filled for one instan
       ce is not a table for another, which is what makes a handle kept across a recreation a ca
       ught error rather than a pointer into a layer that is gone. */
    xud_check(xud_loader_instance_bound(loader, VK_NULL_HANDLE) == false, "gpu/diagnostics-messenger-not-null-instance",
        "the rows are not the null handle's rows");
    xud_check(xud_loader_instance_bound(loader, instance->instance) == true, "gpu/diagnostics-messenger-table-bound",
        "the instance layer filled the table for this instance");
    xud_check(loader->instanceTable.vkCreateDebugUtilsMessengerEXT != NULL, "gpu/diagnostics-messenger-row-resolved",
        "VK_EXT_debug_utils is enabled, so the row answers");

    result = xud_diagnostics_create_messenger(diagnostics, loader, instance->instance);
    xud_check(result == XUD_OK, "gpu/diagnostics-messenger-create", "the messenger must install through the live instance");
    xud_check(diagnostics->messenger != VK_NULL_HANDLE, "gpu/diagnostics-messenger-live", "the handle must be live");
    xud_check(diagnostics->instance == instance->instance, "gpu/diagnostics-messenger-owner",
        "the owning instance is borrowed for teardown, not copied");

    /* Asking again on a struct that already has one is XUD_ALREADY, not a second handle:
       the struct owns exactly one messenger, and a second handle would be uncloseable. */
    xud_check(xud_diagnostics_create_messenger(diagnostics, loader, instance->instance) == XUD_ALREADY, "gpu/diagnostics-messenger-twice",
        "a struct that already has a messenger is XUD_ALREADY, not a second handle");
    xud_check(diagnostics->messenger != VK_NULL_HANDLE, "gpu/diagnostics-messenger-unchanged", "and the handle it has is untouched");

    /* A messenger that belongs to a different VkInstance than the one being destroyed is a
       misuse: destroying it through this one would be a lie, so it is rejected before anything
       is torn down - and the second instance is torn down cleanly behind that refusal. */
    xud_check(xud_instance_create(&other, loader, diagnostics, NULL, 0u) == XUD_OK, "gpu/diagnostics-second-instance",
        "a second instance must create as cleanly as the first");
    if (other != NULL) {
        xud_check(xud_instance_destroy(&other, loader, diagnostics) == XUD_INVALID_ARG, "gpu/diagnostics-messenger-owner-mismatch",
            "the messenger belongs to the first instance, not this one");
        xud_check(other != NULL, "gpu/diagnostics-mismatch-leaves-instance-alive", "a refused destroy must not tear anything down");
        xud_check(diagnostics->messenger != VK_NULL_HANDLE, "gpu/diagnostics-mismatch-keeps-messenger",
            "and must not silence the messenger either");
        xud_check(xud_instance_destroy(&other, loader, NULL) == XUD_OK, "gpu/diagnostics-second-instance-destroy",
            "torn down without the diagnostics struct it does not own");
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
    xud_check(diagnostics->messagesReceived == 0u, "gpu/diagnostics-no-messages",
        "the messenger was live for the whole instance lifetime, so a validation complaint would "
        "have counted here");
    xud_check(diagnostics->errorMessages == 0u, "gpu/diagnostics-validation-clean", "the run is validation-clean, not just crash-free");

    /* Closing with no messenger left is the free-and-clear half: no driver call. */
    result = xud_diagnostics_close(&diagnostics, loader);
    xud_check(result == XUD_OK, "gpu/diagnostics-close", "the struct must close once the messenger is gone");
    xud_check(diagnostics == NULL, "gpu/diagnostics-close-null", "and be cleared");

    xud_check(xud_loader_close(&loader) == XUD_OK, "gpu/diagnostics-loader-close", "the loader must close");
    xud_check(loader == NULL, "gpu/diagnostics-loader-close-null", "and be cleared");
}

void xud_test_diagnostics(void) {
    xud_test_diagnostics_pure();
    if (xud_gpu_tests_enabled()) {
        xud_test_diagnostics_gpu();
    }
}
