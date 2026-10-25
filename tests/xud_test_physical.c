/***********************************************************************************************
 *
 *   xud_test_physical.c - the physical layer: selection, recorded caps, the queue-family rule
 *
 *   Owns : the device policy, the feature intersection the device layer hands to VkDeviceCreateInfo, the graphics-queue rule, the
 * apiVersion read-back, the free-only teardown - and the negative half, every bad argument and nil row of this layer refused with no driver
 * call. Layer: tests. Sees xud_physical.h and xud_loader.h: the layer is a query result of an instance, so the negative half drives it with
 * synthetic rows rather than with a device.
 *
 *   Deterministic: fixed loop counts, no time, no randomness, no filesystem.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud.h" /* Required for: XudResult, XUD_VERSION, the XUD_GPU_TESTS gate */
#include "xud_loader.h" /* Required for: XudLoader, the rows the query and the open rung call through */
#include "xud_physical.h" /* Required for: XudPhysicalDevice, XudDeviceCaps, the select/record rungs, the query and the close */
#include "xud_test_harness.h" /* Required for: xud_check, xud_gpu_tests_enabled, the area entry points */
#include <stdlib.h> /* Required for: calloc and free, the synthetic records this area builds and frees */
#include <string.h> /* Required for: memset, the synthetic rows this area zeroes and compares */

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
    const VkPhysicalDevice candidates[3]
        = { (VkPhysicalDevice)&fakeHandleStorage[0], (VkPhysicalDevice)&fakeHandleStorage[1], (VkPhysicalDevice)&fakeHandleStorage[2] };
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
    xud_check(xud_physical_select(NULL, candidates, props, 3u) == XUD_INVALID_ARG, "physical/select-null-out",
        "a null out-index must be rejected");
    xud_check(xud_physical_select(&index, NULL, props, 3u) == XUD_INVALID_ARG, "physical/select-null-candidates",
        "a null candidates array with a non-zero count must be rejected");
    xud_check(xud_physical_select(&index, candidates, NULL, 3u) == XUD_INVALID_ARG, "physical/select-null-properties",
        "a null properties array with a non-zero count must be rejected");
    xud_check(index == 0xA5A5u, "physical/select-invalid-untouched", "a rejected select must leave the index alone");
    xud_check(xud_physical_select(&index, NULL, NULL, 0u) == XUD_UNSUPPORTED, "physical/select-no-device",
        "no candidate at all is unsupported, not invalid");
    xud_check(index == 0xA5A5u, "physical/select-unsupported-untouched", "out-index is written only on XUD_OK");

    result = xud_physical_select(&index, candidates, props, 3u);
    xud_check(result == XUD_OK, "physical/select-ok", "a non-empty candidate list selects");
    xud_check(index == 0u, "physical/select-first", "first in driver order, the whole policy today");
    xud_check(xud_physical_select(&index, candidates, props, 1u) == XUD_OK, "physical/select-one-candidate",
        "a one-candidate list is not an error");
    xud_check(index == 0u, "physical/select-one-index", "and answers index 0");

    /* The feature intersection: the driver has a feature the library does not want, lacks one
       it wants, and has one it wants; the wanted-but-missing one stays off. */
    caps = calloc(1u, sizeof(XudDeviceCaps));
    xud_check(caps != NULL, "physical/caps-alloc", "the caps are heap-allocated, they must allocate");
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
    xud_check(enabled.robustBufferAccess == VK_TRUE, "physical/feature-wanted-present", "wanted and reported stays on");
    xud_check(enabled.wideLines == VK_TRUE, "physical/feature-wanted-present-2", "the second wanted and reported stays on");
    xud_check(enabled.geometryShader == VK_FALSE, "physical/feature-not-wanted",
        "a feature the library does not want stays off even when the driver has it");
    xud_check(enabled.samplerAnisotropy == VK_FALSE, "physical/feature-wanted-absent",
        "a wanted feature the driver lacks stays off, nothing is forced on");
    xud_check(
        enabled.inheritedQueries == VK_FALSE, "physical/feature-tail-written", "the last field is written too, the list cannot forget one");
    xud_check(enabled.robustBufferAccess != 0xFFFFFFFFu, "physical/feature-normalized",
        "the output is VK_TRUE/VK_FALSE, not a smuggled third value");

    /* The caps stay exactly what the driver said: the intersection is derived, never stored back.
     */
    xud_check(caps->features.features.geometryShader == VK_TRUE, "physical/caps-verbatim",
        "the driver's answer is not rewritten by the intersection");
    xud_check(xud_physical_record_features(NULL, caps) == XUD_INVALID_ARG, "physical/record-features-null-out",
        "a null out-struct must be rejected");
    xud_check(xud_physical_record_features(&enabled, NULL) == XUD_INVALID_ARG, "physical/record-features-null-caps",
        "null caps must be rejected");

    /* The apiVersion read-back: below the floor, at the floor, above it, and the zero a driver
       that answered nothing leaves behind (the four caps getters return void, so zero is the
       only failure signal they can give - see xud_physical.h). */
    xud_check(xud_physical_record_api_version(caps) == XUD_UNSUPPORTED, "physical/api-zero",
        "a driver that answered nothing fails the floor check");
    xud_check(caps->apiVersion == 0u, "physical/api-zero-recorded", "and what it said is still recorded verbatim");
    caps->properties.properties.apiVersion = VK_MAKE_API_VERSION(0u, 0u, 9u, 0u);
    xud_check(
        xud_physical_record_api_version(caps) == XUD_UNSUPPORTED, "physical/api-below-floor", "below the instance floor is unsupported");
    xud_check(caps->apiVersion == VK_MAKE_API_VERSION(0u, 0u, 9u, 0u), "physical/api-below-recorded",
        "the driver's number is recorded even when it fails");
    caps->properties.properties.apiVersion = XUD_INSTANCE_API_FLOOR;
    xud_check(xud_physical_record_api_version(caps) == XUD_OK, "physical/api-at-floor", "at the floor is usable");
    xud_check(caps->apiVersion == XUD_INSTANCE_API_FLOOR, "physical/api-at-floor-recorded", "and recorded");
    caps->properties.properties.apiVersion = VK_API_VERSION_1_4;
    xud_check(xud_physical_record_api_version(caps) == XUD_OK, "physical/api-above-floor", "above the floor is usable");
    xud_check(caps->apiVersion == VK_API_VERSION_1_4, "physical/api-above-recorded",
        "a driver above what these headers know is recorded as said");
    xud_check(xud_physical_record_api_version(NULL) == XUD_INVALID_ARG, "physical/api-null-caps", "null caps must be rejected");

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
        families[1].queueFamilyProperties.queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT;
        families[1].queueFamilyProperties.queueCount = 8u;
        families[1].queueFamilyProperties.timestampValidBits = 36u;
        families[1].queueFamilyProperties.minImageTransferGranularity.width = 4u;

        families[2].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
        families[2].queueFamilyProperties.queueFlags = VK_QUEUE_GRAPHICS_BIT;
        families[2].queueFamilyProperties.queueCount = 1u;

        index = 0xA5A5u;
        result = xud_physical_select_queue_family(&index, &chosen, families, 3u);
        xud_check(result == XUD_OK, "physical/queue-family-ok", "a graphics family exists here");
        xud_check(index == 1u, "physical/queue-family-first-graphics", "the first family carrying the graphics bit in driver order wins");
        xud_check(chosen.sType == VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2, "physical/queue-family-s-type",
            "the row copied into the caps names its own struct type");
        xud_check(chosen.pNext == NULL, "physical/queue-family-p-next-null", "nothing is chained onto the row the caps keep");
        xud_check(chosen.queueFamilyProperties.queueFlags == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT), "physical/queue-family-flags",
            "the whole row is copied, flags included");
        xud_check(chosen.queueFamilyProperties.queueCount == 8u, "physical/queue-family-count",
            "S6's VkQueueCreateInfo input travels with the row");
        xud_check(
            chosen.queueFamilyProperties.timestampValidBits == 36u, "physical/queue-family-timestamp-bits", "and so do the timestamp bits");
        xud_check(chosen.queueFamilyProperties.minImageTransferGranularity.width == 4u, "physical/queue-family-granularity",
            "and the granularity, recorded not required");

        /* A compute-only driver: nothing here to draw with. */
        memset(&chosen, 0, sizeof(chosen));
        result = xud_physical_select_queue_family(&index, &chosen, families, 1u);
        xud_check(result == XUD_UNSUPPORTED, "physical/queue-family-no-graphics", "a compute-only family is refused");
        xud_check(index == XUD_PHYSICAL_NO_QUEUE_FAMILY, "physical/queue-family-sentinel",
            "the sentinel is written so an untouched index cannot look like family 0");
        xud_check(chosen.sType == 0u, "physical/queue-family-chosen-zeroed", "and no stale row survives the refusal");

        /* A graphics-only family (no transfer bit): transfer is recorded, not required. */
        memset(&chosen, 0, sizeof(chosen));
        result = xud_physical_select_queue_family(&index, &chosen, &families[2], 1u);
        xud_check(result == XUD_OK, "physical/queue-family-graphics-only", "a family without the transfer bit is still chosen");
        xud_check(index == 0u, "physical/queue-family-graphics-only-index", "the single row answers 0");

        /* No family at all, and the argument guards. */
        index = 0xA5A5u;
        result = xud_physical_select_queue_family(&index, &chosen, NULL, 0u);
        xud_check(result == XUD_UNSUPPORTED, "physical/queue-family-none", "no family at all is unsupported");
        xud_check(index == XUD_PHYSICAL_NO_QUEUE_FAMILY, "physical/queue-family-none-sentinel", "with the sentinel");
        index = 0xA5A5u;
        xud_check(xud_physical_select_queue_family(&index, &chosen, NULL, 3u) == XUD_INVALID_ARG, "physical/queue-family-null-families",
            "a null families array with a non-zero count must be rejected");
        xud_check(index == 0xA5A5u, "physical/queue-family-invalid-untouched", "a rejected choice must leave the index alone");
        xud_check(xud_physical_select_queue_family(NULL, &chosen, families, 3u) == XUD_INVALID_ARG, "physical/queue-family-null-out-index",
            "a null out-index must be rejected");
        xud_check(xud_physical_select_queue_family(&index, NULL, families, 3u) == XUD_INVALID_ARG, "physical/queue-family-null-out-row",
            "a null out-chosen row must be rejected");
    }

    /* The free-only rung of the teardown ladder: no driver call at all, because Vulkan has no
       vkDestroyPhysicalDevice. The heap array is the one thing it releases. */
    {
        XudPhysicalDevice* device2 = NULL;

        device = calloc(1u, sizeof(XudPhysicalDevice));
        xud_check(device != NULL, "physical/device-alloc", "the struct must allocate");
        device->caps.deviceExtensionProperties = calloc(3u, sizeof(VkExtensionProperties));
        xud_check(
            device->caps.deviceExtensionProperties != NULL, "physical/extension-array-alloc", "the caps' one heap array must allocate");
        device->caps.deviceExtensionCount = 1u;
        device->caps.deviceExtensionCapacity = 3u;
        slot = device;
        xud_check(xud_physical_close(&slot) == XUD_OK, "physical/close-ok", "the free-only rung must close a device");
        xud_check(slot == NULL, "physical/close-null", "and set the pointer NULL");

        /* A device whose extension query never ran: the array is NULL, close must still work
           (and under -DXUD_SANITIZERS=ON both closes above leak nothing). */
        device2 = calloc(1u, sizeof(XudPhysicalDevice));
        xud_check(device2 != NULL, "physical/device-alloc-2", "the second struct must allocate");
        slot = device2;
        xud_check(xud_physical_close(&slot) == XUD_OK, "physical/close-no-array", "a device with no query yet closes cleanly");
        xud_check(slot == NULL, "physical/close-no-array-null", "and its pointer is NULL");

        xud_check(xud_physical_close(NULL) == XUD_INVALID_ARG, "physical/close-null-ptr", "a null pointer-to-pointer must be rejected");
        slot = NULL;
        xud_check(xud_physical_close(&slot) == XUD_ALREADY, "physical/close-already", "an already-closed device answers XUD_ALREADY");
    }

    free(caps);
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

    xud_check(xud_physical_open(NULL, NULL, VK_NULL_HANDLE) == XUD_INVALID_ARG, "physical/open-out-null", "no out-param to write into");
    xud_check(
        xud_physical_open(&physical, NULL, VK_NULL_HANDLE) == XUD_INVALID_ARG, "physical/open-loader-null", "no table to call through");
    xud_check(xud_physical_query_devices(NULL, &candidates, &count, VK_NULL_HANDLE) == XUD_INVALID_ARG, "physical/query-loader-null",
        "no table to call through");
    xud_check(xud_physical_query_devices(loader, &candidates, &count, VK_NULL_HANDLE) == XUD_INVALID_ARG, "physical/query-instance-null",
        "a NULL loader is an argument error, not a slot answer");
    xud_check(xud_physical_query_devices(loader, NULL, &count, VK_NULL_HANDLE) == XUD_INVALID_ARG, "physical/query-out-null",
        "no out-array to write into");
    xud_check(xud_physical_query_devices(loader, &candidates, NULL, VK_NULL_HANDLE) == XUD_INVALID_ARG, "physical/query-count-null",
        "no place to put the count");
    xud_check(physical == NULL, "physical/open-out-stays-null", "a rejected open leaves nothing behind");
    xud_check(candidates == NULL, "physical/query-out-stays-null", "a rejected query leaves no array");

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
        xud_check(xud_physical_query_devices(loader, &candidates, &count, fake) == XUD_UNSUPPORTED, "physical/query-slot-nil",
            "a name the table does not know is reported, never called through");
        xud_check(candidates == NULL, "physical/query-slot-nil-out", "and the out pair stays empty");
        xud_check(count == 0u, "physical/query-slot-nil-count", "...including the count");

        /* A rejected open with a live loader: the argument paths still come first. */
        xud_check(xud_physical_open(&physical, loader, VK_NULL_HANDLE) == XUD_INVALID_ARG, "physical/open-instance-null",
            "no handle to resolve the slots through");
        xud_check(xud_physical_open(NULL, loader, VK_NULL_HANDLE) == XUD_INVALID_ARG, "physical/open-out-null-live", "...and no out-param");
        xud_check(physical == NULL, "physical/open-rejected-null", "a rejected open leaves the out-param NULL");

        xud_check(xud_loader_close(&loader) == XUD_OK, "physical/loader-close", "the loader the negative run opened must close");
        xud_check(loader == NULL, "physical/loader-close-null", "and be cleared");
    }

    /* The free-only rung, on a struct this test allocated rather than one the driver handed
       over: the caps' extension array is NULL here, so the close frees the struct only. */
    physical = calloc(1u, sizeof(XudPhysicalDevice));
    xud_check(physical != NULL, "physical/synthetic-alloc", "the synthetic struct must allocate");
    if (physical != NULL) {
        xud_check(xud_physical_close(&physical) == XUD_OK, "physical/close-synthetic", "no driver call, the struct is freed");
        xud_check(physical == NULL, "physical/close-synthetic-null", "and the out-param cleared");
    }
    xud_check(xud_physical_close(&second) == XUD_ALREADY, "physical/close-null-already", "a NULL-in-handling pointer is XUD_ALREADY");
}

void xud_test_physical(void) {
    xud_test_physical_pure();
    xud_test_physical_negative();
}
