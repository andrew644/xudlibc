/***********************************************************************************************
 *
 *   xud_test_vocabulary.c - the shared vocabulary: xud.h's own facts and xud_vulkan.h's funnel
 *
 *   Owns : the version stamp, the error-code names, the VkResult-to-XudResult mapping and the fixed-buffer name helpers - the two halves
 * every layer above leans on, checked with no driver present. Layer: tests. Sees xud.h and xud_vulkan.h only: nothing here needs a Vulkan
 * handle, so this is the smallest area of the run.
 *
 *   Deterministic: fixed loop counts, no time, no randomness, no filesystem.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud.h" /* Required for: XudResult, XUD_VERSION, the XUD_GPU_TESTS gate */
#include "xud_test_harness.h" /* Required for: xud_check, xud_gpu_tests_enabled, the area entry points */
#include "xud_vulkan.h" /* Required for: xud_result_from_vk, xud_result_name, the name helpers, and the Vulkan header they are spelled with */
#include <string.h> /* Required for: memset, strlen, strcmp, the synthetic rows this area zeroes and compares */

static void xud_test_version(void) {
    xud_check(XUD_VERSION_MAJOR == 0, "version/major", "XUD_VERSION_MAJOR is not 0");
    xud_check(XUD_VERSION_MINOR == 1, "version/minor", "XUD_VERSION_MINOR is not 1");
    xud_check(XUD_VERSION_PATCH == 0, "version/patch", "XUD_VERSION_PATCH is not 0");
    xud_check(strlen(XUD_VERSION) > 0, "version/string", "XUD_VERSION is empty");
    xud_check(strcmp(XUD_VERSION, "0.1") == 0, "version/consistent", "XUD_VERSION does not match the major.minor macros");
}

static void xud_test_result_names(void) {
    const XudResult all[] = { XUD_OK, XUD_UNSUPPORTED, XUD_INVALID_ARG, XUD_OUT_OF_MEMORY, XUD_OUT_OF_RESOURCES, XUD_ALREADY, XUD_LOST,
        XUD_VULKAN_ERROR, XUD_INTERNAL_ERROR };
    const size_t count = sizeof(all) / sizeof(all[0]);

    xud_check(XUD_OK == 0, "result/ok-is-zero", "XUD_OK is not 0");

    for (size_t i = 0; i < count; i++) {
        const char* name = xud_result_name(all[i]);
        xud_check(name != NULL && name[0] != '\0', "result/name-non-null", "empty name");
        xud_check(strcmp(name, "XUD_OUT_OF_RANGE") != 0, "result/name-known", "an enum member falls through to the out-of-range name");
        for (size_t j = i + 1; j < count; j++) {
            xud_check(strcmp(name, xud_result_name(all[j])) != 0, "result/names-unique", "two error codes share a name");
        }
    }
}

/* The shared Vulkan vocabulary: VkResult -> XudResult, and the fixed-buffer name helpers.
   Pure, so the headless run covers them completely. */
static void xud_test_result_mapping(void) {
    xud_check(xud_result_from_vk(VK_SUCCESS) == XUD_OK, "vk/ok", "VK_SUCCESS must map to XUD_OK");
    xud_check(xud_result_from_vk(VK_ERROR_DEVICE_LOST) == XUD_LOST, "vk/lost", "device lost must map to XUD_LOST");
    xud_check(
        xud_result_from_vk(VK_ERROR_OUT_OF_HOST_MEMORY) == XUD_OUT_OF_MEMORY, "vk/host-memory", "host OOM must map to XUD_OUT_OF_MEMORY");
    xud_check(xud_result_from_vk(VK_ERROR_OUT_OF_DEVICE_MEMORY) == XUD_OUT_OF_RESOURCES, "vk/device-memory",
        "device OOM must map to XUD_OUT_OF_RESOURCES");
    xud_check(xud_result_from_vk(VK_ERROR_TOO_MANY_OBJECTS) == XUD_OUT_OF_RESOURCES, "vk/too-many-objects",
        "object exhaustion must map to XUD_OUT_OF_RESOURCES");
    xud_check(xud_result_from_vk(VK_ERROR_INCOMPATIBLE_DRIVER) == XUD_UNSUPPORTED, "vk/incompatible-driver",
        "an incompatible driver must map to XUD_UNSUPPORTED");
    xud_check(xud_result_from_vk(VK_ERROR_LAYER_NOT_PRESENT) == XUD_UNSUPPORTED, "vk/layer-not-present",
        "a missing layer must map to XUD_UNSUPPORTED");
    xud_check(xud_result_from_vk(VK_ERROR_EXTENSION_NOT_PRESENT) == XUD_UNSUPPORTED, "vk/extension-not-present",
        "a missing extension must map to XUD_UNSUPPORTED");
    xud_check(xud_result_from_vk(VK_ERROR_FEATURE_NOT_PRESENT) == XUD_UNSUPPORTED, "vk/feature-not-present",
        "a missing feature must map to XUD_UNSUPPORTED");
    /* Everything else is a Vulkan failure this library cannot classify: one code for all of it. */
    xud_check(xud_result_from_vk(VK_ERROR_INITIALIZATION_FAILED) == XUD_VULKAN_ERROR, "vk/init-failed",
        "init failure must map to XUD_VULKAN_ERROR");
    xud_check(
        xud_result_from_vk(VK_ERROR_MEMORY_MAP_FAILED) == XUD_VULKAN_ERROR, "vk/map-failed", "a map failure must map to XUD_VULKAN_ERROR");
    xud_check(xud_result_from_vk(VK_ERROR_FORMAT_NOT_SUPPORTED) == XUD_VULKAN_ERROR, "vk/format-unsupported",
        "an unsupported format must map to XUD_VULKAN_ERROR");
    xud_check(
        xud_result_from_vk(VK_ERROR_NOT_PERMITTED) == XUD_VULKAN_ERROR, "vk/not-permitted", "not-permitted must map to XUD_VULKAN_ERROR");
    /* VK_INCOMPLETE is not an error in Vulkan, but it means the list was truncated: a caller
       that wants the whole list grows its array and calls again before it maps anything. */
    xud_check(xud_result_from_vk(VK_INCOMPLETE) == XUD_VULKAN_ERROR, "vk/incomplete", "a truncated list must not map to XUD_OK");
    xud_check(xud_result_from_vk(VK_NOT_READY) == XUD_VULKAN_ERROR, "vk/not-ready", "not-ready must map to XUD_VULKAN_ERROR");
    xud_check(xud_result_from_vk(VK_TIMEOUT) == XUD_VULKAN_ERROR, "vk/timeout", "timeout must map to XUD_VULKAN_ERROR");
    xud_check(xud_result_from_vk(VK_EVENT_SET) == XUD_VULKAN_ERROR, "vk/event-set", "event-set must map to XUD_VULKAN_ERROR");
    xud_check(xud_result_from_vk(VK_EVENT_RESET) == XUD_VULKAN_ERROR, "vk/event-reset", "event-reset must map to XUD_VULKAN_ERROR");
    xud_check(xud_result_from_vk(-4242) == XUD_VULKAN_ERROR, "vk/unknown-code", "a code outside the enum must map to XUD_VULKAN_ERROR");
}

static void xud_test_name_helpers(void) {
    char buffer[VK_MAX_EXTENSION_NAME_SIZE];

    memset(buffer, 0, sizeof(buffer));
    xud_check(xud_name_copy(buffer, sizeof(buffer), VK_KHR_SURFACE_EXTENSION_NAME) == XUD_OK, "name/copy-fits",
        "a name that fits must be copied");
    xud_check(xud_name_equal(buffer, VK_KHR_SURFACE_EXTENSION_NAME) == true, "name/copy-roundtrip", "the copy must be readable");

    /* The boundary: a name that fills the buffer exactly (size-1 characters plus NUL) fits. */
    xud_check(xud_name_copy(buffer, 3u, "ab") == XUD_OK, "name/copy-exact-fit", "a name that fills the buffer exactly must be accepted");
    xud_check(xud_name_equal(buffer, "ab") == true, "name/copy-exact-roundtrip", "the exact fit must round-trip");

    /* One character too many is rejected, not truncated: the buffer still holds "ab". */
    xud_check(xud_name_copy(buffer, 3u, "abc") == XUD_INVALID_ARG, "name/copy-too-long",
        "a name that does not fit must be rejected, not truncated");
    xud_check(xud_name_equal(buffer, "abc") == false, "name/no-truncate", "a rejected copy must not have written the name");

    xud_check(
        xud_name_copy(NULL, sizeof(buffer), "x") == XUD_INVALID_ARG, "name/copy-null-destination", "a null destination must be rejected");
    xud_check(xud_name_copy(buffer, 0u, "x") == XUD_INVALID_ARG, "name/copy-zero-size", "a zero-size buffer must be rejected");
    xud_check(xud_name_copy(buffer, sizeof(buffer), NULL) == XUD_INVALID_ARG, "name/copy-null-source", "a null source must be rejected");

    xud_check(xud_name_equal(NULL, NULL) == true, "name/equal-both-null", "two absent names are equal");
    xud_check(xud_name_equal(NULL, "x") == false, "name/equal-one-null", "an absent name is not equal to a present one");
    xud_check(xud_name_equal("x", NULL) == false, "name/equal-other-side-null", "the rule is symmetric");
    xud_check(xud_name_equal("VK_KHR_surface", VK_KHR_SURFACE_EXTENSION_NAME) == true, "name/equal-literal",
        "a literal matches the header macro");
}

void xud_test_vocabulary(void) {
    xud_test_version();
    xud_test_result_names();
    xud_test_result_mapping();
    xud_test_name_helpers();
}
