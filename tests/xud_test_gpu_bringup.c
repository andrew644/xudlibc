/***********************************************************************************************
 *
 *   xud_test_gpu_bringup.c - the GPU bring-up ladder: instance, physical, device against the real driver
 *
 *   Owns : the three rungs that need a device and nest on the way in - the instance the physical device is queried through, the physical
 * device the VkDevice is created from, the device whose two role queues are resolved - and tear back down while their owner is alive, in
 * the order xud_close_context() uses. Layer: tests. Entirely under the GPU gate. The three functions stay in one TU because the inner two
 * take the loader and the handles the outer one owns: xud_test_gpu_instance() calls xud_test_gpu_physical() with the instance it just
 * created, which calls xud_test_device_gpu() with the physical device it just queried. That nesting is the ownership proof, so it is not
 * broken by a header that hands Vulkan handles across TU boundaries.
 *
 *   Deterministic: fixed loop counts, no time, no randomness, no filesystem.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud.h" /* Required for: XudResult, XUD_VERSION, the XUD_GPU_TESTS gate */
#include "xud_device.h" /* Required for: XudDevice, XudQueue, the queue lookup, the guard and the close */
#include "xud_instance.h" /* Required for: XudInstance, xud_instance_create/destroy */
#include "xud_loader.h" /* Required for: XudLoader, the rows every rung here calls through */
#include "xud_physical.h" /* Required for: XudPhysicalDevice, XudDeviceCaps, the query and the record rungs */
#include "xud_test_harness.h" /* Required for: xud_check, xud_gpu_tests_enabled, the area entry points */
#include <stdlib.h> /* Required for: calloc and free, the synthetic records this area builds and frees */
#include <string.h> /* Required for: memset, the synthetic rows this area zeroes and compares */

/*-----------------------------------------------------------------------------------------------
 * GPU-gated: the device layer against the real driver - vkCreateDevice from the recorded caps,
 * the two role queues, the wait-idle guard, the teardown rung. Called by xud_test_gpu_physical()
 * while the physical device that owns this one is still alive: a VkDevice is created from a VkPh
 * ysicalDevice handle (PFN_vkCreateDevice's first argument in these headers), so its teardown mus
 * t happen inside the owner's lifetime, the order xud_close_context() uses (device before physica
 * l). The checks below that insist on machine-specific numbers (one queue family, one queue in it,
 *  VK_KHR_swapchain advertised) are marked as such.
 *-----------------------------------------------------------------------------------------------*/
static void xud_test_device_gpu(XudLoader* loader, const XudPhysicalDevice* physical, VkInstance instance) {
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

    xud_check(
        xud_device_open(NULL, loader, physical, instance) == XUD_INVALID_ARG, "device-gpu/open-null-out", "no out-param to write into");
    xud_check(xud_device_open(&device, loader, physical, VK_NULL_HANDLE) == XUD_INVALID_ARG, "device-gpu/open-null-instance",
        "no handle to resolve the slots through");
    xud_check(
        xud_device_open(&device, loader, NULL, instance) == XUD_INVALID_ARG, "device-gpu/open-null-physical", "no caps to create from");
    xud_check(device == NULL, "device-gpu/open-rejected-null", "a rejected open leaves nothing behind");

    result = xud_device_open(&device, loader, physical, instance);
    xud_check(result == XUD_OK, "device-gpu/open", "vkCreateDevice from the recorded caps must succeed on this machine");
    if (device == NULL)
        return;

    xud_check(device->device != VK_NULL_HANDLE, "device-gpu-handle", "the handle must be live");
    xud_check(loader->instanceTable.vkCreateDevice != NULL, "device-gpu-row-create",
        "the create row is an instance-level row, resolved at the instance bind");
    xud_check(xud_loader_device_bound(loader, device->device) == true, "device-gpu-table-bound",
        "the device layer filled the device table for this device");
    xud_check(loader->deviceTable.vkGetDeviceQueue != NULL, "device-gpu-row-get", "...and so is the queue lookup");
    xud_check(loader->deviceTable.vkDeviceWaitIdle != NULL, "device-gpu-row-wait", "...and the guard");
    xud_check(loader->deviceTable.vkDestroyDevice != NULL, "device-gpu-row-destroy", "...and the teardown call, bound before it is needed");

    xud_check(device->queueRow.sType == VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, "device-gpu-row-stamp",
        "the row the driver was handed is stamped");
    xud_check(device->queueRow.pNext == NULL, "device-gpu-row-p-next", "and nothing is chained onto it");
    xud_check(device->queueRow.flags == 0, "device-gpu-row-flags", "no protected-memory flag");
    xud_check(device->queueRow.queueFamilyIndex == physical->caps.queueFamilyIndex, "device-gpu-row-family",
        "the row asks the family the physical layer chose");
    xud_check(device->queueRow.queueCount >= 1u, "device-gpu-row-count-nonzero", "a row that asks for no queue is refused before the call");
    xud_check(device->queueRow.queueCount <= XUD_DEVICE_QUEUE_MAX_COUNT, "device-gpu-row-count-ceiling",
        "and never for more than this layer's ceiling");
    expectedQueueCount = (physical->caps.chosenQueueFamilyProperties.queueFamilyProperties.queueCount < XUD_DEVICE_QUEUE_MAX_COUNT)
        ? physical->caps.chosenQueueFamilyProperties.queueFamilyProperties.queueCount
        : XUD_DEVICE_QUEUE_MAX_COUNT;
    xud_check(device->queueRow.queueCount == expectedQueueCount, "device-gpu-row-count-min",
        "the row asks for min(what the family rowed, the ceiling)");
    xud_check(device->queueRow.pQueuePriorities == device->queuePriorities, "device-gpu-row-own-array",
        "the row points at the struct's own array, so it cannot dangle past the call");
    for (index = 0u; index < XUD_DEVICE_QUEUE_MAX_COUNT; index++) {
        if ((device->queuePriorities[index] < XUD_DEVICE_QUEUE_PRIORITY_MIN)
            || (device->queuePriorities[index] > XUD_DEVICE_QUEUE_PRIORITY_MAX))
            prioritiesInRange = false;
    }
    xud_check(prioritiesInRange == true, "device-gpu-priorities-in-range", "every priority is inside the spec's 0..1 range");

    /* The features the device was created with are the intersection, not the driver's verbatim
       answer: for a wanted name the AND is the driver's bit, and a name the library does not wan
       t stays off whatever the driver says about it.
     */
    xud_check(device->enabledFeatures.robustBufferAccess == physical->caps.features.features.robustBufferAccess,
        "device-gpu-feature-robust", "the intersection is the driver's answer for a wanted name");
    xud_check(device->enabledFeatures.samplerAnisotropy == physical->caps.features.features.samplerAnisotropy,
        "device-gpu-feature-anisotropy", "same");
    xud_check(device->enabledFeatures.wideLines == physical->caps.features.features.wideLines, "device-gpu-feature-wide-lines", "same");
    xud_check(device->enabledFeatures.geometryShader == VK_FALSE, "device-gpu-feature-not-wanted",
        "a feature the library does not want stays off, even on a driver that has it");

    /* The check-before-you-use rule, observed at the driver: every name that got enabled was in
       the recorded device-extension list. The count and the name below are machine-specific (lavk
       vk advertises VK_KHR_swapchain, probed 2026-10-18 at row 84 of 196).
     */
    for (index = 0u; index < device->enabledExtensionCount; index++) {
        bool found = false;

        for (advertisedIndex = 0u; advertisedIndex < physical->caps.deviceExtensionCount; advertisedIndex++) {
            if (xud_name_equal(
                    device->enabledExtensionNameStorage[index], physical->caps.deviceExtensionProperties[advertisedIndex].extensionName)
                == true)
                found = true;
        }
        if (found == false)
            enabledAdvertised = false;
    }
    xud_check(enabledAdvertised == true, "device-gpu-ext-advertised", "every enabled name was in the recorded list, nothing was forced on");
    xud_check(device->enabledExtensionCount == 1u, "device-gpu-ext-count",
        "machine-specific: VK_KHR_swapchain is advertised here, so exactly one wanted name is "
        "enabled");
    xud_check(xud_name_equal(device->enabledExtensionNameStorage[0], VK_KHR_SWAPCHAIN_EXTENSION_NAME) == true, "device-gpu-ext-swapchain",
        "machine-specific: and it is that name");

    xud_check(xud_device_queue_get(&graphics, device, XUD_QUEUE_ROLE_GRAPHICS) == XUD_OK, "device-gpu-queue-graphics",
        "the graphics role resolves");
    xud_check(graphics.queue != VK_NULL_HANDLE, "device-gpu-queue-graphics-handle", "the handle must be live");
    xud_check(graphics.queueFamilyIndex == device->queueRow.queueFamilyIndex, "device-gpu-queue-graphics-family",
        "the pair the lookup hands back is the row's own");
    xud_check(graphics.queueIndex < device->queueRow.queueCount, "device-gpu-queue-graphics-index",
        "...and the index is inside the count asked for");
    xud_check(
        xud_device_queue_get(&present, device, XUD_QUEUE_ROLE_PRESENT) == XUD_OK, "device-gpu-queue-present", "the present role resolves");
    xud_check(present.queue != VK_NULL_HANDLE, "device-gpu-queue-present-handle", "...with a live handle");
    xud_check(present.queueFamilyIndex == device->queueRow.queueFamilyIndex, "device-gpu-queue-present-family", "from the same family");
    xud_check(present.queueIndex < device->queueRow.queueCount, "device-gpu-queue-present-index", "at an index inside the row");
    if (device->queueRow.queueCount == 1u)
        xud_check(graphics.queue == present.queue, "device-gpu-queue-same-handle",
            "machine-specific: one queue in the family, both roles answer the same handle - "
            "presentation is not a queue flag (see xud_physical.h)");

    xud_check(xud_device_wait_idle(device, loader) == XUD_OK, "device-gpu-wait-idle", "a device with nothing in flight is already idle");
    xud_check(xud_device_close(&device, loader) == XUD_OK, "device-gpu-close", "the guard ran, the device was destroyed, the struct freed");
    xud_check(device == NULL, "device-gpu-close-null", "and the out-param cleared");
    xud_check(
        xud_device_close(&device, loader) == XUD_ALREADY, "device-gpu-close-twice", "a second close is XUD_ALREADY, not a double free");

    result = xud_device_open(&second, loader, physical, instance);
    xud_check(result == XUD_OK, "device-gpu-open-second", "the four cached slots survive a teardown, a recreation path works");
    if (second != NULL) {
        xud_check(second->device != VK_NULL_HANDLE, "device-gpu-second-handle", "the second device is live");
        xud_check(second->queueRow.queueFamilyIndex == physical->caps.queueFamilyIndex, "device-gpu-second-family",
            "...from the same recorded family");
        xud_check(xud_device_close(&second, loader) == XUD_OK, "device-gpu-close-second", "and tears down");
        xud_check(second == NULL, "device-gpu-second-null", "...cleared");
    }

    /* A struct that never got a handle: the guard answers XUD_ALREADY and the rung frees with no
       driver call, which is the same path the headless run covers.
     */
    synthetic = calloc(1u, sizeof(XudDevice));
    xud_check(synthetic != NULL, "device-gpu-synthetic-alloc", "the synthetic struct must allocate");
    if (synthetic != NULL) {
        synthetic->device = VK_NULL_HANDLE;
        xud_check(xud_device_wait_idle(synthetic, loader) == XUD_ALREADY, "device-gpu-wait-idle-no-handle",
            "nothing is in flight on a device that does not exist");
        xud_check(xud_device_close(&synthetic, loader) == XUD_OK, "device-gpu-close-no-handle", "no driver call, the struct is freed");
        xud_check(synthetic == NULL, "device-gpu-close-no-handle-null", "and the out-param cleared");
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

    xud_check(physical->physicalDevice != VK_NULL_HANDLE, "gpu/physical-handle", "the handle must be live");
    xud_check(physical->caps.properties.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, "gpu/physical-stamp-properties",
        "this layer stamps the wrapper, the driver leaves it alone");
    xud_check(physical->caps.features.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, "gpu/physical-stamp-features",
        "same for the features row");
    xud_check(physical->caps.memoryProperties.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, "gpu/physical-stamp-memory",
        "same for the memory row");
    xud_check(
        physical->caps.properties.pNext == NULL, "gpu/physical-chain-properties", "this layer asked for no chained struct and got none");
    xud_check(physical->caps.features.pNext == NULL, "gpu/physical-chain-features", "same for the features row");
    xud_check(physical->caps.memoryProperties.pNext == NULL, "gpu/physical-chain-memory", "same for the memory row");

    xud_check(xud_physical_record_api_version(&physical->caps) == XUD_OK, "gpu/physical-api-read",
        "the read-back is idempotent on a recorded caps");
    xud_check(physical->caps.properties.properties.apiVersion >= XUD_INSTANCE_API_FLOOR, "gpu/physical-floor",
        "the recorded apiVersion clears the floor");
    xud_check(physical->caps.deviceExtensionProperties != NULL, "gpu/physical-ext-array", "the caps' one heap array exists");
    xud_check(physical->caps.deviceExtensionCount > 0u, "gpu/physical-ext-count", "the driver advertises device extensions");
    xud_check(physical->caps.deviceExtensionCapacity >= physical->caps.deviceExtensionCount, "gpu/physical-ext-capacity",
        "capacity is a step of the doubling sequence, not a tight fit");
    for (index = 0u; index < physical->caps.deviceExtensionCount; index++) {
        if (xud_name_equal(physical->caps.deviceExtensionProperties[index].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == true)
            swapchainAdvertised = true;
    }
    /* The swapchain layer (S7) needs this name, and the check-before-you-use rule says it has
       to be found in the recorded list rather than assumed - probed here: present, row 84 of
       196.
     */
    xud_check(
        swapchainAdvertised == true, "gpu/physical-swapchain-advertised", "VK_KHR_swapchain is in the recorded device-extension list");

    xud_check(physical->caps.queueFamilyIndex != XUD_PHYSICAL_NO_QUEUE_FAMILY, "gpu/physical-family-chosen",
        "a family carrying the graphics bit was found");
    xud_check(physical->caps.queueFamilyCount >= 1u, "gpu/physical-family-count", "the driver rowed at least one family");
    xud_check((physical->caps.chosenQueueFamilyProperties.queueFamilyProperties.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0u,
        "gpu/physical-family-graphics", "the recorded row is the one the rule picked, and it carries the bit");
    xud_check(physical->caps.chosenQueueFamilyProperties.queueFamilyProperties.queueCount >= 1u, "gpu/physical-queue-count",
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
    xud_check(xud_physical_record_features(&requested, &physical->caps) == XUD_OK, "gpu/physical-features-record",
        "the intersection runs on a recorded caps");
    xud_check(requested.robustBufferAccess == physical->caps.features.features.robustBufferAccess, "gpu/physical-feature-robust",
        "the AND is the driver's answer for a wanted name");
    xud_check(requested.samplerAnisotropy == physical->caps.features.features.samplerAnisotropy, "gpu/physical-feature-anisotropy", "same");
    xud_check(requested.wideLines == physical->caps.features.features.wideLines, "gpu/physical-feature-wide-lines", "same");

    /* The query helper, called the way the layer calls it. */
    result = xud_physical_query_devices(loader, &candidates, &candidateCount, instance);
    xud_check(result == XUD_OK, "gpu/physical-query-devices", "the device list answers through the bound slot");
    xud_check(candidateCount >= 1u, "gpu/physical-query-count", "this machine enumerates at least one device");
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
    xud_check(xud_physical_select(&chosen, fakeHandles, fakeRows, 1u) == XUD_OK, "gpu/physical-select-agrees",
        "the policy picks the row the layer recorded");
    xud_check(chosen == 0u, "gpu/physical-select-index", "...and says so");

    families[0] = physical->caps.chosenQueueFamilyProperties;
    memset(&chosenRow, 0, sizeof(chosenRow));
    xud_check(xud_physical_select_queue_family(&chosen, &chosenRow, families, 1u) == XUD_OK, "gpu/physical-family-rule-agrees",
        "the pure rule accepts the row the driver gave");
    xud_check(chosen == 0u, "gpu/physical-family-rule-index", "...at the only position there is");
    xud_check(chosenRow.queueFamilyProperties.queueFlags == physical->caps.chosenQueueFamilyProperties.queueFamilyProperties.queueFlags,
        "gpu/physical-family-rule-row", "and copies the whole row it accepted");

    /* Teardown, then a second bring-up through the same table: the six slots are cached, the
       way the instance-level ones are, so a recreation path works - and the close is the free-
       only one, with no driver call in it. The device layer's own GPU section runs above this
       teardown: a VkDevice is created from a VkPhysicalDevice handle, so it must be torn down
       while its owner is alive, the order xud_close_context() uses.
     */
    xud_test_device_gpu(loader, physical, instance);

    xud_check(xud_physical_close(&physical) == XUD_OK, "gpu/physical-close", "the caps' array and the struct are freed, no driver call");
    xud_check(physical == NULL, "gpu/physical-close-null", "and the out-param cleared");
    xud_check(xud_physical_close(&physical) == XUD_ALREADY, "gpu/physical-close-twice", "a second close is XUD_ALREADY, not a double free");

    result = xud_physical_open(&second, loader, instance);
    xud_check(result == XUD_OK, "gpu/physical-open-second", "the cached slots survive a teardown");
    xud_check(second != NULL, "gpu/physical-second-non-null", "the out-param must be set");
    if (second != NULL) {
        xud_check(second->caps.deviceExtensionCount > 0u, "gpu/physical-second-count", "the second caps carry the list again");
        xud_check(xud_physical_close(&second) == XUD_OK, "gpu/physical-close-second", "and tear down");
        xud_check(second == NULL, "gpu/physical-second-null", "...cleared");
    }

    xud_check(xud_physical_open(&second, loader, VK_NULL_HANDLE) == XUD_INVALID_ARG, "gpu/physical-open-null-instance",
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
    xud_check(loader->instanceTable.vkDestroyInstance == NULL, "gpu/destroy-row-null", "the row must stay NULL before the instance exists");
    xud_check(
        xud_loader_instance_bound(loader, VK_NULL_HANDLE) == false, "gpu/destroy-not-bound", "and the loader is bound to no instance yet");

    result = xud_instance_create(&instance, loader, NULL, NULL, 0u);
    xud_check(result == XUD_OK, "gpu/instance-create", "vkCreateInstance at the floor must succeed on this machine");
    if (instance == NULL) {
        (void)xud_loader_close(&loader);
        return;
    }
    xud_check(instance->instance != VK_NULL_HANDLE, "gpu/instance-handle", "the handle must be live");
    xud_check(instance->applicationInfo.apiVersion == XUD_INSTANCE_API_FLOOR, "gpu/instance-floor", "the floor travels to the driver");
    /* lavvk advertises all three wanted extensions here (verified 2026-10-16), so all three
       are enabled in wanted order; a driver that advertises fewer would enable fewer. */
    xud_check(instance->enabledExtensionCount == 3u, "gpu/instance-extension-count", "all three wanted extensions are advertised here");
    xud_check(xud_name_equal(instance->enabledExtensionNameStorage[0], VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) == true,
        "gpu/name-first", "wanted order reaches the driver");
    xud_check(xud_name_equal(instance->enabledExtensionNameStorage[1], VK_KHR_SURFACE_EXTENSION_NAME) == true, "gpu/name-second",
        "wanted order reaches the driver");
    xud_check(xud_name_equal(instance->enabledExtensionNameStorage[2], VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == true,
        "gpu/name-third", "wanted order reaches the driver");
    xud_check(xud_loader_instance_bound(loader, instance->instance) == true, "gpu/table-bound-after-create",
        "the instance layer filled the table for this instance");

    /* The physical layer, opened through the live instance and torn down inside xud_test_gpu_
       physical() below before this function destroys the instance - the order xud_close_context(
       ) uses, and the reason the physical section runs here rather than after the destroy.
     */
    xud_test_gpu_physical(loader, instance->instance);

    result = xud_instance_destroy(&instance, loader, NULL);
    xud_check(result == XUD_OK, "gpu/instance-destroy", "the instance must be destroyed through the live handle");
    xud_check(instance == NULL, "gpu/destroy-nulls-out", "destroy must clear the out-param");
    xud_check(loader->instanceTable.vkDestroyInstance != NULL, "gpu/destroy-row-bound",
        "the instance-level teardown row was resolved by the bind");

    xud_check(xud_instance_destroy(&instance, loader, NULL) == XUD_ALREADY, "gpu/destroy-twice",
        "a second destroy is XUD_ALREADY, not a double free");

    /* A second create/destroy pair on the same table: the binding is reusable and nothing
       accumulates in the loader. */
    result = xud_instance_create(&instance, loader, NULL, NULL, 0u);
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

void xud_test_gpu_bringup(void) {
    if (!xud_gpu_tests_enabled()) {
        return;
    }
    xud_test_gpu_instance();
}
