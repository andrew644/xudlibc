/***********************************************************************************************
 *
 *   xud_test_instance.c - the instance layer: application info, extension selection, create/destroy
 *
 *   Owns : the application-info row, the extension selection that enables nothing unadvertised, the merged extra-name list the window layer
 * asks for, the create-info assembly and the destroy rung. Layer: tests. Sees xud_instance.h and xud_diagnostics.h: the validation layers
 * this layer requests come from the diagnostics struct, so that struct is an input to the assembly being checked here.
 *
 *   Deterministic: fixed loop counts, no time, no randomness, no filesystem.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud.h" /* Required for: XudResult, XUD_VERSION, the XUD_GPU_TESTS gate */
#include "xud_diagnostics.h" /* Required for: XudDiagnostics, the layer selection the create-info carries */
#include "xud_instance.h" /* Required for: XudInstance, xud_instance_create/destroy, the fill/select rungs, XUD_INSTANCE_EXTRA_MAX */
#include "xud_loader.h" /* Required for: XudLoader, the rows xud_instance_create() calls through */
#include "xud_test_harness.h" /* Required for: xud_check, xud_gpu_tests_enabled, the area entry points */
#include <string.h> /* Required for: memset, the synthetic rows this area zeroes and compares */

/* The instance layer's pure parts: application info, extension selection (including what the
   diagnostics layer contributes to it), create-info assembly. No driver call, so the headless
   run covers them completely. */
static void xud_test_instance_pure(void) {
    VkInstanceCreateInfo createInfo;
    VkInstanceCreateInfo createInfoChain;
    const char* nameSlots[XUD_MAX_INSTANCE_EXTENSIONS];
    const char* noExtras[XUD_DIAGNOSTICS_WANTED_MAX + XUD_INSTANCE_EXTRA_MAX] = { NULL };
    const char* wantedFromDiagnostics[XUD_DIAGNOSTICS_WANTED_MAX + XUD_INSTANCE_EXTRA_MAX] = { NULL };
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
    xud_check(instance.applicationInfo.sType == VK_STRUCTURE_TYPE_APPLICATION_INFO, "instance/app-info-s-type",
        "sType must be the application-info struct type");
    xud_check(instance.applicationInfo.pNext == NULL, "instance/app-info-p-next-null", "nothing is chained onto the application info");
    xud_check(xud_name_equal(instance.applicationInfo.pApplicationName, XUD_INSTANCE_NAME) == true, "instance/app-info-name",
        "the application name must be the library name");
    xud_check(xud_name_equal(instance.applicationInfo.pEngineName, XUD_INSTANCE_NAME) == true, "instance/app-info-engine-name",
        "this library is the engine");
    xud_check(
        instance.applicationInfo.applicationVersion == VK_MAKE_API_VERSION(0u, XUD_VERSION_MAJOR, XUD_VERSION_MINOR, XUD_VERSION_PATCH),
        "instance/app-version", "the version must be the library version");
    xud_check(instance.applicationInfo.engineVersion == instance.applicationInfo.applicationVersion, "instance/engine-version",
        "both version slots must agree");
    xud_check(instance.applicationInfo.apiVersion == XUD_INSTANCE_API_FLOOR, "instance/api-floor", "the floor must be VK_API_VERSION_1_0");
    xud_check(
        xud_instance_fill_application_info(NULL) == XUD_INVALID_ARG, "instance/app-info-null-out", "a null out-param must be rejected");

    /* Two of the three wanted extensions, one extension the library does not want at this
       level (VK_KHR_swapchain is device-level), and the wanted pair in the reverse of wanted
       order: the selection must follow wanted order and ignore what is not wanted. */
    strcpy(advertised[0].extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    strcpy(advertised[1].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    strcpy(advertised[2].extensionName, VK_KHR_SURFACE_EXTENSION_NAME);

    result = xud_instance_select_extensions(&instance, advertised, 3u, noExtras, 0u);
    xud_check(result == XUD_OK, "instance/select-ok", "selection must succeed");
    xud_check(instance.enabledExtensionCount == 2u, "instance/select-count", "only wanted-and-advertised names are enabled");
    xud_check(xud_name_equal(instance.enabledExtensionNameStorage[0], VK_KHR_SURFACE_EXTENSION_NAME) == true, "instance/select-order",
        "wanted order, not advertised order");
    xud_check(xud_name_equal(instance.enabledExtensionNameStorage[1], VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == true,
        "instance/select-second", "the second wanted-and-advertised name");
    xud_check(xud_name_equal(instance.enabledExtensionNameStorage[2], "") == true, "instance/select-tail-untouched",
        "nothing beyond the count is written");

    /* The selection is rewritten on every call: a reused struct cannot keep a stale list. */
    result = xud_instance_select_extensions(&instance, NULL, 0u, noExtras, 0u);
    xud_check(result == XUD_OK, "instance/select-none-ok", "an empty advertised list is not an error");
    xud_check(instance.enabledExtensionCount == 0u, "instance/select-none-count", "the previous list must not survive");
    xud_check(xud_instance_select_extensions(NULL, advertised, 3u, noExtras, 0u) == XUD_INVALID_ARG, "instance/select-null-out",
        "a null out-param must be rejected");
    xud_check(xud_instance_select_extensions(&instance, NULL, 3u, noExtras, 0u) == XUD_INVALID_ARG, "instance/select-null-advertised",
        "a null advertised array with a non-zero count must be rejected");
    xud_check(xud_instance_select_extensions(&instance, advertised, 3u, NULL, 0u) == XUD_OK, "instance/select-no-extras",
        "a null extra list with a zero count is the no-diagnostics shape");
    xud_check(xud_instance_select_extensions(&instance, advertised, 3u, NULL, 2u) == XUD_INVALID_ARG, "instance/select-null-extras",
        "a null extra list with a non-zero count must be rejected");

    /* Re-select, then assemble: the create-info must point at the caller's nameSlots array,
       which is what keeps it alive across the vkCreateInstance call. */
    result = xud_instance_select_extensions(&instance, advertised, 3u, noExtras, 0u);
    xud_check(result == XUD_OK, "instance/select-again-ok", "selection must be repeatable");
    result = xud_instance_fill_create_info(&instance, nameSlots, NULL, &createInfo);
    xud_check(result == XUD_OK, "instance/create-info-ok", "assembly must succeed");
    xud_check(createInfo.sType == VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, "instance/create-info-s-type",
        "sType must be the instance-create struct type");
    xud_check(createInfo.pNext == NULL, "instance/create-info-p-next-null", "nothing is chained yet - validation is the diagnostics step");
    xud_check(createInfo.flags == 0, "instance/create-info-flags", "the portability flag is deliberately not set");
    xud_check(createInfo.pApplicationInfo == &instance.applicationInfo, "instance/create-info-app-info",
        "the create-info carries the application info");
    xud_check(createInfo.pApplicationInfo->apiVersion == XUD_INSTANCE_API_FLOOR, "instance/create-info-floor", "the floor travels with it");
    xud_check(createInfo.enabledLayerCount == 0u, "instance/create-info-no-layers", "validation layers are the diagnostics step's job");
    xud_check(createInfo.ppEnabledLayerNames == NULL, "instance/create-info-no-layer-names", "no layer names without a count");
    xud_check(createInfo.enabledExtensionCount == 2u, "instance/create-info-extension-count", "the selected count travels");
    xud_check(createInfo.ppEnabledExtensionNames != NULL, "instance/create-info-extension-names", "a non-zero count must carry names");
    xud_check(nameSlots[0] == createInfo.ppEnabledExtensionNames[0], "instance/create-info-aliases-caller-array",
        "the create-info points at the caller's array, not a copy");
    xud_check(xud_name_equal(createInfo.ppEnabledExtensionNames[0], VK_KHR_SURFACE_EXTENSION_NAME) == true,
        "instance/create-info-first-name", "wanted order reaches the driver");
    xud_check(xud_name_equal(createInfo.ppEnabledExtensionNames[1], VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == true,
        "instance/create-info-second-name", "wanted order reaches the driver");
    xud_check(nameSlots[2] == NULL, "instance/create-info-name-tail-null",
        "the tail of the caller's array is NULLed so no stale pointer survives");
    xud_check(xud_instance_fill_create_info(NULL, nameSlots, NULL, &createInfo) == XUD_INVALID_ARG, "instance/create-info-null-instance",
        "a null instance must be rejected");
    xud_check(xud_instance_fill_create_info(&instance, NULL, NULL, &createInfo) == XUD_INVALID_ARG, "instance/create-info-null-name-slots",
        "a null name-slot array must be rejected");
    xud_check(xud_instance_fill_create_info(&instance, nameSlots, NULL, NULL) == XUD_INVALID_ARG, "instance/create-info-null-out",
        "a null out-param must be rejected");

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

    xud_check(xud_diagnostics_wanted_extensions(wantedFromDiagnostics, &wantedCount) == XUD_OK, "diagnostics/wanted-accessor-answers",
        "the instance layer's only mechanism for the wanted pair");
    xud_check(wantedCount == XUD_DIAGNOSTICS_WANTED_MAX, "diagnostics/wanted-accessor-count", "and it hands over exactly its cap");

    result = xud_instance_select_extensions(&withDiag, advertisedDiag, 6u, wantedFromDiagnostics, wantedCount);
    xud_check(result == XUD_OK, "instance/select-with-diagnostics-ok", "the diagnostics extras must be selectable");
    xud_check(withDiag.enabledExtensionCount == 4u, "instance/select-with-diagnostics-count",
        "three wanted plus debug utils, the validation-feature extension is not advertised here");
    xud_check(xud_name_equal(withDiag.enabledExtensionNameStorage[3], VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == true,
        "instance/select-diagnostics-tail",
        "the diagnostics pair is appended after this layer's own list, in the diagnostics layer's "
        "order");

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
    xud_check(xud_name_equal(withChain.enabledExtensionNameStorage[4], VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME) == true,
        "instance/select-chain-last", "the validation-feature extension is enabled, so its pNext chain is legal");

    /* The diagnostics struct's own parts, filled the way xud_instance_create() fills them. */
    xud_check(xud_diagnostics_fill_validation_features(&diag) == XUD_OK, "diagnostics/features-fill-for-instance",
        "the validation-feature request must assemble");
    strcpy(advertisedLayers[0].layerName, "VK_LAYER_TEST_validation");
    strcpy(advertisedLayers[1].layerName, "VK_LAYER_TEST_other");
    xud_check(xud_diagnostics_select_layers(&diag, advertisedLayers, 2u) == XUD_OK, "diagnostics/layers-fill-for-instance",
        "the advertised layers must be selected");
    xud_check(diag.enabledLayerCount == 2u, "diagnostics/layers-count-for-instance", "both of them");
    xud_check(diag.enabledLayerNamePointers[1] == diag.enabledLayerNameStorage[1], "diagnostics/layers-point-at-storage",
        "the pointer array points at the struct's own fixed buffers");
    xud_check(diag.enabledLayerNamePointers[2] == NULL, "diagnostics/layers-tail-null", "the unused tail of the pointer array is NULLed");

    result = xud_instance_fill_create_info(&withDiag, nameSlots, &diag, &createInfo);
    xud_check(result == XUD_OK, "instance/create-info-with-diagnostics-ok", "assembly with the diagnostics struct must succeed");
    xud_check(createInfo.enabledLayerCount == 2u, "instance/create-info-layer-count",
        "the diagnostics layer's selection reaches the create-info");
    xud_check(createInfo.ppEnabledLayerNames == diag.enabledLayerNamePointers, "instance/create-info-layer-names",
        "the create-info points at the diagnostics struct's own pointer array, not a copy");
    xud_check(xud_name_equal(createInfo.ppEnabledLayerNames[0], "VK_LAYER_TEST_validation") == true, "instance/create-info-layer-first",
        "advertised order reaches the driver");
    xud_check(createInfo.enabledExtensionCount == 4u, "instance/create-info-with-diagnostics-extension-count",
        "the diagnostics pair travels in the same create-info");
    xud_check(createInfo.pNext == NULL, "instance/create-info-chain-not-when-unadvertised",
        "the validation-feature request stays out while its extension is not enabled");

    result = xud_instance_fill_create_info(&withChain, nameSlots, &diag, &createInfoChain);
    xud_check(result == XUD_OK, "instance/create-info-chain-ok", "assembly with the chain must succeed");
    xud_check(createInfoChain.enabledExtensionCount == 5u, "instance/create-info-chain-extension-count", "all five names");
    xud_check(createInfoChain.enabledLayerCount == 2u, "instance/create-info-chain-layer-count", "the layers travel with the chain");
    xud_check(createInfoChain.pNext == (const void*)&diag.validationFeatures, "instance/create-info-chain-when-advertised",
        "the validation-feature request hangs off pNext once its extension is enabled");

    /* A layer count that does not fit the storage it came from is refused, not truncated. */
    diag.enabledLayerCount = XUD_MAX_DIAGNOSTIC_LAYERS + 1u;
    xud_check(xud_instance_fill_create_info(&withChain, nameSlots, &diag, &createInfo) == XUD_INVALID_ARG,
        "instance/create-info-layer-count-over-bound", "a layer list that does not fit is refused");
    diag.enabledLayerCount = 2u;

    /* The driver half refuses a loader it was not given open - checked before any dlopen. */
    {
        XudInstance* refused = NULL;
        XudLoader closed;
        XudDiagnostics* noneYet = NULL;

        memset(&closed, 0, sizeof(closed));

        xud_check(xud_instance_create(&refused, NULL, NULL, NULL, 0u) == XUD_INVALID_ARG, "instance/create-null-loader",
            "a null loader must be rejected");
        xud_check(refused == NULL, "instance/create-null-loader-out-untouched", "a rejected create must leave the out-param alone");
        xud_check(xud_instance_create(&refused, &closed, NULL, NULL, 0u) == XUD_INVALID_ARG, "instance/create-closed-loader",
            "a closed loader must be refused before any dlopen");
        xud_check(refused == NULL, "instance/create-closed-loader-out-untouched", "and the out-param must stay alone");
        xud_check(xud_instance_destroy(&refused, NULL, NULL) == XUD_INVALID_ARG, "instance/destroy-null-loader",
            "a null loader must be rejected");
        xud_check(refused == NULL, "instance/destroy-null-loader-out-untouched", "a rejected destroy must leave the pointer alone");
        xud_check(xud_instance_destroy(&refused, &closed, NULL) == XUD_INVALID_ARG, "instance/destroy-closed-loader",
            "a closed loader must be refused too");
        xud_check(refused == NULL, "instance/destroy-closed-loader-out-untouched", "and the pointer must stay alone");
        xud_check(xud_diagnostics_open(&noneYet, &closed) == XUD_UNSUPPORTED, "instance/diagnostics-open-closed-loader",
            "the diagnostics layer refuses the same closed loader");
        xud_check(noneYet == NULL, "instance/diagnostics-open-closed-out", "and allocates nothing behind it");
    }
}

void xud_test_instance(void) { xud_test_instance_pure(); }
