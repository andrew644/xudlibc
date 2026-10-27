/***********************************************************************************************
 *
 *   xud_test_vocabulary.c - the shared vocabulary: xud.h's own facts and xud_vulkan.h's two funnels
 *
 *   Owns : the version stamp, the error-code names, the VkResult-to-XudResult mapping, the swapchain-
 *   boundary mapping that differs from it in exactly the two codes a swapchain answers with, and the
 *   fixed-buffer name helpers - the halves every layer above leans on, checked with no driver present.
 *   Layer: tests. Sees xud.h and xud_vulkan.h only: nothing here needs a Vulkan handle, so this is the smallest area of the run.
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

/* The SECOND funnel, and the whole point of this area: the two funnels differ in exactly the two
   codes a swapchain answers with, and they must differ in nothing else. The general one above cal
   ls both a driver fault; the swapchain-boundary one calls both a frame that ran and a recreate wan
   ted, because for vkAcquireNextImageKHR and vkQueuePresentKHR a positive VkResult is a verdict ab
   out the swapchain, not about the call. Pure, so the headless run covers it completely - which is
   what makes "a resize is not a refused frame" a checkable fact rather than one that only ever sh
   ows up on a machine whose compositor moved the window.
*/
static void xud_test_swapchain_mapping(void) {
    uint32_t wanted = 0u;

    wanted = 0u;
    xud_check(xud_swapchain_result_from_vk(VK_SUCCESS, &wanted) == XUD_OK, "swapchain-vk/ok", "a clean acquire is OK");
    xud_check(wanted == 0u, "swapchain-vk/ok-wants-nothing", "and it wants no recreate");

    wanted = 0u;
    xud_check(xud_swapchain_result_from_vk(VK_SUBOPTIMAL_KHR, &wanted) == XUD_OK, "swapchain-vk/suboptimal-is-a-frame",
        "the swapchain works, so the frame that used it is a frame");
    xud_check(wanted == 1u, "swapchain-vk/suboptimal-wants-a-recreate", "and a better swapchain is wanted");

    wanted = 0u;
    xud_check(xud_swapchain_result_from_vk(VK_ERROR_OUT_OF_DATE_KHR, &wanted) == XUD_OK, "swapchain-vk/out-of-date-is-a-frame",
        "the image this answer names is still valid and presentable, so the frame is still a frame");
    xud_check(wanted == 1u, "swapchain-vk/out-of-date-wants-a-recreate", "and the new swapchain is wanted before the next acquire");

    /* The general funnel still calls both of them a fault - the two funnels are two tables and the
       difference between them is the pair of rows, not a caller's judgement about which row to use.
    */
    xud_check(xud_result_from_vk(VK_SUBOPTIMAL_KHR) == XUD_VULKAN_ERROR, "swapchain-vk/general-still-calls-suboptimal-a-fault",
        "outside the acquire and the present a positive code is a driver fault");
    xud_check(xud_result_from_vk(VK_ERROR_OUT_OF_DATE_KHR) == XUD_VULKAN_ERROR, "swapchain-vk/general-still-calls-out-of-date-a-fault",
        "and the general funnel keeps saying so for every other call in the tree");

    /* Everything else delegates: the code and the flag together must be the general mapping plus
       "nothing wanted", including for the two swapchain-adjacent codes that are NOT of this pair
       (a lost surface and an unusable display mode are refusals, not a swapchain shape a recrea
       te answers).
    */
    wanted = 1u;
    xud_check(xud_swapchain_result_from_vk(VK_ERROR_DEVICE_LOST, &wanted) == XUD_LOST, "swapchain-vk/delegates-lost",
        "a lost device still maps to LOST through the second funnel");
    xud_check(
        wanted == 0u, "swapchain-vk/lost-wants-nothing", "and the flag is written on the error path, not left at whatever the caller had");
    wanted = 1u;
    xud_check(xud_swapchain_result_from_vk(VK_ERROR_OUT_OF_HOST_MEMORY, &wanted) == XUD_OUT_OF_MEMORY, "swapchain-vk/delegates-host-memory",
        "host exhaustion still maps to OUT_OF_MEMORY");
    xud_check(wanted == 0u, "swapchain-vk/host-memory-wants-nothing", "and wants no recreate");
    wanted = 1u;
    xud_check(xud_swapchain_result_from_vk(VK_ERROR_INCOMPATIBLE_DRIVER, &wanted) == XUD_UNSUPPORTED, "swapchain-vk/delegates-unsupported",
        "an incompatible driver still maps to UNSUPPORTED");
    xud_check(wanted == 0u, "swapchain-vk/incompatible-wants-nothing", "and wants no recreate");
    xud_check(xud_swapchain_result_from_vk(VK_ERROR_SURFACE_LOST_KHR, &wanted) == XUD_VULKAN_ERROR, "swapchain-vk/surface-lost-is-a-fault",
        "a lost surface is not a swapchain shape a recreate answers");
    xud_check(wanted == 0u, "swapchain-vk/surface-lost-wants-nothing", "and nothing is wanted for it");
    xud_check(xud_swapchain_result_from_vk(VK_ERROR_INCOMPATIBLE_DISPLAY_KHR, &wanted) == XUD_VULKAN_ERROR,
        "swapchain-vk/incompatible-display-is-a-fault", "nor is an unusable display mode");
    xud_check(wanted == 0u, "swapchain-vk/display-wants-nothing", "and nothing is wanted for it either");
    xud_check(xud_swapchain_result_from_vk(VK_TIMEOUT, &wanted) == XUD_VULKAN_ERROR, "swapchain-vk/timeout-is-a-fault",
        "a positive code that is not one of the pair is still a fault");
    xud_check(wanted == 0u, "swapchain-vk/timeout-wants-nothing", "and it wants no recreate");
    xud_check(xud_swapchain_result_from_vk(-4242, &wanted) == XUD_VULKAN_ERROR, "swapchain-vk/unknown-code",
        "a code outside the enum is a fault too");
    xud_check(wanted == 0u, "swapchain-vk/unknown-wants-nothing", "and wants no recreate");

    xud_check(xud_swapchain_result_from_vk(VK_SUBOPTIMAL_KHR, NULL) == XUD_INVALID_ARG, "swapchain-vk/null-flag",
        "a flag nowhere to write is a bad argument, and it is refused before any mapping is made");
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
    xud_test_swapchain_mapping();
    xud_test_name_helpers();
}
