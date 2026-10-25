/***********************************************************************************************
 *
 *   xud_test_device.c - the device layer: the queue row, the enabled features, the two role queues
 *
 *   Owns : the queue-family row assembly, the device-extension selection, the create-info assembly, the queue lookup, the wait-idle guard
 * and the teardown - all of it against synthetic caps, so the headless run covers the whole pure half. Layer: tests. Sees xud_device.h,
 * xud_physical.h and xud_loader.h: the layer's whole input is the physical layer's recorded caps.
 *
 *   Deterministic: fixed loop counts, no time, no randomness, no filesystem.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud.h" /* Required for: XudResult, XUD_VERSION, the XUD_GPU_TESTS gate */
#include "xud_device.h" /* Required for: XudDevice, XudQueue, XudQueueRole, the fill/select rungs, the guard and the close */
#include "xud_loader.h" /* Required for: XudLoader, the rows the driver half calls through */
#include "xud_physical.h" /* Required for: XudPhysicalDevice, XudDeviceCaps - the caps the create-info is built from */
#include "xud_test_harness.h" /* Required for: xud_check, xud_gpu_tests_enabled, the area entry points */
#include <stdio.h> /* Required for: printf, the note this area prints when a guard has nothing to check */
#include <stdlib.h> /* Required for: calloc and free, the synthetic records this area builds and frees */
#include <string.h> /* Required for: memset, the synthetic rows this area zeroes and compares */

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
    const VkQueue fakeQueueHandles[2] = { (VkQueue)&fakeHandleStorage[1], (VkQueue)&fakeHandleStorage[2] };

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
    xud_check(result == XUD_OK, "device/row-ok", "a caps with a chosen family and queues in it fills a row");
    xud_check(row.sType == VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, "device/row-stamp",
        "the row is stamped, an unstamped row is the validation layer's first complaint");
    xud_check(row.pNext == NULL, "device/row-p-next", "nothing is chained onto it");
    xud_check(row.flags == 0, "device/row-flags", "no protected-memory flag: nothing above this layer asks for that today");
    xud_check(row.queueFamilyIndex == 3u, "device/row-family", "the family the physical layer chose is the family the row asks for");
    xud_check(row.queueCount == XUD_DEVICE_QUEUE_MAX_COUNT, "device/row-count-clamped",
        "a family with 8 queues is asked for at most XUD_DEVICE_QUEUE_MAX_COUNT");
    xud_check(row.pQueuePriorities == priorities, "device/row-priorities", "the row points at the caller's array, it does not copy one");

    memset(&row, 0, sizeof(row));
    caps.queueFamilyIndex = XUD_PHYSICAL_NO_QUEUE_FAMILY;
    xud_check(xud_device_fill_queue_create_info(&row, priorities, &caps) == XUD_INVALID_ARG, "device/row-no-family",
        "a caps with no chosen family is not a usable input");
    xud_check(row.sType == 0u, "device/row-rejected-untouched", "a rejected fill leaves the row at its zero state");
    caps.queueFamilyIndex = 0u;
    caps.chosenQueueFamilyProperties.queueFamilyProperties.queueCount = 0u;
    xud_check(xud_device_fill_queue_create_info(&row, priorities, &caps) == XUD_INVALID_ARG, "device/row-zero-queue-count",
        "a family with no queue in it is not a family to ask for");
    xud_check(xud_device_fill_queue_create_info(NULL, priorities, &caps) == XUD_INVALID_ARG, "device/row-null-out",
        "a null out-row must be rejected");
    xud_check(xud_device_fill_queue_create_info(&row, NULL, &caps) == XUD_INVALID_ARG, "device/row-null-priorities",
        "a row with no priorities array is the validation layer's pQueuePriorities violation");
    xud_check(
        xud_device_fill_queue_create_info(&row, priorities, NULL) == XUD_INVALID_ARG, "device/row-null-caps", "null caps must be rejected");
    xud_check(row.sType == 0u, "device/row-invalid-untouched", "a rejected fill leaves the row alone");
    caps.chosenQueueFamilyProperties.queueFamilyProperties.queueCount = 1u;
    xud_check(xud_device_fill_queue_create_info(&row, priorities, &caps) == XUD_OK, "device/row-one-queue",
        "the driver's own family here rowed one queue");
    xud_check(row.queueCount == 1u, "device/row-one-count", "the clamp is an upper bound only, one queue stays one");
    xud_check(row.queueFamilyIndex == 0u, "device/row-family-zero", "family 0 is a real choice, the sentinel is not");

    /* ---- the device struct, and the extension selection over it ---- */
    device = calloc(1u, sizeof(XudDevice));
    xud_check(device != NULL, "device/alloc", "the struct is heap-allocated, it must allocate");
    if (device == NULL)
        return;

    xud_check(xud_device_fill_queue_create_info(&device->queueRow, device->queuePriorities, &caps) == XUD_OK, "device/own-row",
        "the row filled into the struct");
    xud_check(device->queueRow.pQueuePriorities == device->queuePriorities, "device/own-row-priorities",
        "and it points at the struct's own array");

    xud_check(xud_name_copy(&advertised[0].extensionName[0], VK_MAX_EXTENSION_NAME_SIZE, "VK_NOT_A_REAL_NAME") == XUD_OK,
        "device/advertised-first-name", "a name the library does not want");
    xud_check(xud_name_copy(&advertised[1].extensionName[0], VK_MAX_EXTENSION_NAME_SIZE, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == XUD_OK,
        "device/advertised-second-name", "the one name this layer wants");
    result = xud_device_select_extensions(device, advertised, 2u);
    xud_check(result == XUD_OK, "device/select-ok", "the selection runs on an advertised list");
    xud_check(device->enabledExtensionCount == 1u, "device/select-one-enabled", "only the advertised wanted name is enabled");
    xud_check(xud_name_equal(device->enabledExtensionNameStorage[0], VK_KHR_SWAPCHAIN_EXTENSION_NAME) == true, "device/select-name",
        "and it is that name");

    result = xud_device_select_extensions(device, advertised, 1u);
    xud_check(result == XUD_OK, "device/select-none-wanted-advertised", "a list without the wanted name still selects");
    xud_check(
        device->enabledExtensionCount == 0u, "device/select-empty", "the list is rewritten, a stale name cannot survive a re-selection");
    xud_check(
        xud_device_select_extensions(device, NULL, 0u) == XUD_OK, "device/select-empty-list", "no advertised names at all is not an error");
    xud_check(device->enabledExtensionCount == 0u, "device/select-empty-list-count", "...and the list stays empty");
    xud_check(xud_device_select_extensions(NULL, advertised, 2u) == XUD_INVALID_ARG, "device/select-null-out",
        "a null out-device must be rejected");
    xud_check(xud_device_select_extensions(device, NULL, 2u) == XUD_INVALID_ARG, "device/select-null-advertised",
        "a non-zero count with no array is an argument error");
    xud_check(xud_name_copy(&advertised[2].extensionName[0], VK_MAX_EXTENSION_NAME_SIZE, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == XUD_OK,
        "device/advertised-duplicate-name", "the same name twice in the driver's list");
    result = xud_device_select_extensions(device, advertised, 3u);
    xud_check(result == XUD_OK, "device/select-duplicate-ok", "a driver that repeats a name does not break the selection");
    xud_check(
        device->enabledExtensionCount == 1u, "device/select-duplicate-once", "...and the name is enabled once, the duplicate rule holds");

    /* ---- the create-info, assembled from the struct above ---- */
    device->device = fakeDeviceHandle;
    for (index = 0u; index < XUD_DEVICE_MAX_DEVICE_EXTENSIONS; index++)
        nameSlots[index] = "poison";
    result = xud_device_fill_create_info(&createInfo, nameSlots, device);
    xud_check(result == XUD_OK, "device/create-info-ok", "a struct this layer filled assembles");
    xud_check(createInfo.sType == VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, "device/create-info-stamp", "the create-info is stamped");
    xud_check(createInfo.pNext == NULL, "device/create-info-p-next", "and nothing is chained onto it");
    xud_check(createInfo.flags == 0, "device/create-info-flags", "no device-level flag is set today");
    xud_check(createInfo.queueCreateInfoCount == 1u, "device/create-info-one-row", "exactly one queue row, the one-family rule");
    xud_check(createInfo.pQueueCreateInfos == &device->queueRow, "device/create-info-row-address", "the row it names is the struct's own");
    xud_check(createInfo.enabledLayerCount == 0u, "device/create-info-legacy-layers-zero",
        "the legacy layer count is written, not left uninitialised");
    xud_check(createInfo.ppEnabledLayerNames == NULL, "device/create-info-legacy-layers-null",
        "...and so is the name array it would have pointed at");
    xud_check(createInfo.enabledExtensionCount == 1u, "device/create-info-ext-count", "the enabled list is the selection's");
    xud_check(createInfo.ppEnabledExtensionNames == nameSlots, "device/create-info-ext-names",
        "the names travel through the caller's storage, not a frame-local inside this function");
    xud_check(nameSlots[1] == NULL, "device/name-tail-cleared", "the tail of the name array is NULLed, no stale pointer survives");
    xud_check(nameSlots[XUD_DEVICE_MAX_DEVICE_EXTENSIONS - 1u] == NULL, "device/name-tail-last", "...including the last slot");
    xud_check(createInfo.pEnabledFeatures == &device->enabledFeatures, "device/create-info-features",
        "the features it carries are the struct's own record");

    device->queueRow.sType = 0u;
    xud_check(xud_device_fill_create_info(&createInfo, nameSlots, device) == XUD_INVALID_ARG, "device/create-info-unstamped-row",
        "an unstamped row is refused here, not left to a validation layer");
    xud_check(createInfo.sType == VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, "device/create-info-rejected-untouched",
        "a rejected assembly leaves the create-info alone");
    device->queueRow.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;

    device->queueRow.queueCount = 0u;
    xud_check(xud_device_fill_create_info(&createInfo, nameSlots, device) == XUD_INVALID_ARG, "device/create-info-zero-queue-count",
        "a row that asks for no queue is refused");
    device->queueRow.queueCount = XUD_DEVICE_QUEUE_MAX_COUNT + 1u;
    xud_check(xud_device_fill_create_info(&createInfo, nameSlots, device) == XUD_INVALID_ARG, "device/create-info-count-too-big",
        "a row longer than the priorities array is refused, the row would point past its end");
    device->queueRow.queueCount = 1u;

    device->queueRow.queueFamilyIndex = XUD_PHYSICAL_NO_QUEUE_FAMILY;
    xud_check(xud_device_fill_create_info(&createInfo, nameSlots, device) == XUD_INVALID_ARG, "device/create-info-sentinel-family",
        "the no-family sentinel is not a family to ask for");
    device->queueRow.queueFamilyIndex = 0u;

    device->queueRow.pQueuePriorities = frameLocalPriorities;
    xud_check(xud_device_fill_create_info(&createInfo, nameSlots, device) == XUD_INVALID_ARG, "device/create-info-foreign-priorities",
        "a row pointing at another array is refused - the dangling-pointer bug this struct layout "
        "exists to prevent");
    device->queueRow.pQueuePriorities = device->queuePriorities;

    device->queueRow.pNext = (void*)&caps;
    xud_check(xud_device_fill_create_info(&createInfo, nameSlots, device) == XUD_INVALID_ARG, "device/create-info-row-chained",
        "a chained row is refused, this layer chains nothing");
    device->queueRow.pNext = NULL;

    device->enabledExtensionCount = XUD_DEVICE_MAX_DEVICE_EXTENSIONS + 1u;
    xud_check(xud_device_fill_create_info(&createInfo, nameSlots, device) == XUD_INVALID_ARG, "device/create-info-count-too-big-ext",
        "an enabled count that does not fit the storage it came from is refused");
    device->enabledExtensionCount = 1u;
    xud_check(xud_device_fill_create_info(NULL, nameSlots, device) == XUD_INVALID_ARG, "device/create-info-null-out",
        "a null out-create-info must be rejected");
    xud_check(xud_device_fill_create_info(&createInfo, NULL, device) == XUD_INVALID_ARG, "device/create-info-null-names",
        "a null name array must be rejected");
    xud_check(xud_device_fill_create_info(&createInfo, nameSlots, NULL) == XUD_INVALID_ARG, "device/create-info-null-device",
        "a null device must be rejected");

    /* ---- the queue lookup, on a struct that stands in for one open produced ---- */
    device->graphicsQueue.queue = fakeQueueHandles[0];
    device->graphicsQueue.queueFamilyIndex = 0u;
    device->graphicsQueue.queueIndex = 0u;
    device->presentQueue.queue = fakeQueueHandles[1];
    device->presentQueue.queueFamilyIndex = 0u;
    device->presentQueue.queueIndex = 1u;
    device->queueRow.queueCount = 2u;
    got.queue = (VkQueue)&fakeHandleStorage[3];
    xud_check(
        xud_device_queue_get(&got, device, XUD_QUEUE_ROLE_GRAPHICS) == XUD_OK, "device/queue-get-graphics", "the graphics role resolves");
    xud_check(got.queue == fakeQueueHandles[0], "device/queue-get-graphics-handle", "...with the handle recorded for it");
    xud_check(got.queueFamilyIndex == 0u, "device/queue-get-graphics-family", "and the coordinates it was resolved at");
    xud_check(got.queueIndex == 0u, "device/queue-get-graphics-index", "...the index too");
    xud_check(
        xud_device_queue_get(&got, device, XUD_QUEUE_ROLE_PRESENT) == XUD_OK, "device/queue-get-present", "the present role resolves");
    xud_check(got.queue == fakeQueueHandles[1], "device/queue-get-present-handle", "a second queue in the family means a second handle");
    xud_check(got.queueIndex == 1u, "device/queue-get-present-index", "at index 1");
    got.queue = (VkQueue)&fakeHandleStorage[3];
    xud_check(xud_device_queue_get(&got, device, (XudQueueRole)XUD_DEVICE_QUEUE_ROLE_COUNT) == XUD_INVALID_ARG,
        "device/queue-get-role-range", "a role at the enum's end is refused");
    xud_check(xud_device_queue_get(&got, device, 7u) == XUD_INVALID_ARG, "device/queue-get-role-way-out", "a caller can still pass 7");
    xud_check(got.queue == (VkQueue)&fakeHandleStorage[3], "device/queue-get-rejected-untouched",
        "a rejected lookup leaves the out-record alone");
    xud_check(xud_device_queue_get(NULL, device, XUD_QUEUE_ROLE_GRAPHICS) == XUD_INVALID_ARG, "device/queue-get-null-out",
        "a null out-record must be rejected");
    xud_check(xud_device_queue_get(&got, NULL, XUD_QUEUE_ROLE_GRAPHICS) == XUD_INVALID_ARG, "device/queue-get-null-device",
        "a null device must be rejected");

    device->device = VK_NULL_HANDLE;
    xud_check(xud_device_queue_get(&got, device, XUD_QUEUE_ROLE_GRAPHICS) == XUD_INVALID_ARG, "device/queue-get-no-handle",
        "there is no queue to hand back for a device that does not exist");
    device->device = fakeDeviceHandle;

    device->presentQueue.queue = VK_NULL_HANDLE;
    xud_check(xud_device_queue_get(&got, device, XUD_QUEUE_ROLE_PRESENT) == XUD_INTERNAL_ERROR, "device/queue-get-unresolved",
        "a struct that claims a live device but has no queue in it was not produced here");
    device->presentQueue.queue = fakeQueueHandles[1];

    device->presentQueue.queueFamilyIndex = 4u;
    xud_check(xud_device_queue_get(&got, device, XUD_QUEUE_ROLE_PRESENT) == XUD_INTERNAL_ERROR, "device/queue-get-family-mismatch",
        "a pair whose family is not the row's is refused before it is ever asked");
    device->presentQueue.queueFamilyIndex = 0u;

    device->presentQueue.queueIndex = 2u;
    xud_check(xud_device_queue_get(&got, device, XUD_QUEUE_ROLE_PRESENT) == XUD_INTERNAL_ERROR, "device/queue-get-index-mismatch",
        "an index at or above the row's count is refused before it is ever asked");
    device->presentQueue.queueIndex = 1u;

    /* One queue in the row, both roles holding the same handle: the shape probed on this driver
       2026-10-18, where the one family rowed one queue and vkGetDeviceQueue answers the same han
       dle for both roles.
     */
    device->queueRow.queueCount = 1u;
    device->presentQueue.queue = fakeQueueHandles[0];
    device->presentQueue.queueIndex = 0u;
    xud_check(xud_device_queue_get(&graphics, device, XUD_QUEUE_ROLE_GRAPHICS) == XUD_OK, "device/queue-one-graphics",
        "the graphics role resolves");
    xud_check(xud_device_queue_get(&present, device, XUD_QUEUE_ROLE_PRESENT) == XUD_OK, "device/queue-one-present",
        "and so does the present role");
    xud_check(graphics.queue == present.queue, "device/queue-one-same-handle",
        "both roles answer the same handle when the family rowed one queue");
    xud_check(graphics.queueIndex == present.queueIndex, "device/queue-one-same-index", "...and the same coordinates");

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
        xud_check(fakePhysical != NULL, "device/fake-physical-alloc", "the synthetic physical struct must allocate");

        xud_check(
            xud_device_open(NULL, NULL, NULL, VK_NULL_HANDLE) == XUD_INVALID_ARG, "device/open-null-out", "no out-param to write into");
        xud_check(
            xud_device_open(&held, NULL, NULL, VK_NULL_HANDLE) == XUD_INVALID_ARG, "device/open-null-loader", "no table to call through");
        xud_check(
            xud_device_open(&held, NULL, NULL, fakeInstance) == XUD_INVALID_ARG, "device/open-null-physical", "no caps to create from");
        xud_check(xud_device_open(&held, NULL, fakePhysical, VK_NULL_HANDLE) == XUD_INVALID_ARG, "device/open-null-instance",
            "no handle to resolve the slots through");
        xud_check(held == NULL, "device/open-rejected-null", "a rejected open leaves nothing behind");

        synthetic = calloc(1u, sizeof(XudDevice));
        xud_check(synthetic != NULL, "device/synthetic-alloc", "the synthetic struct must allocate");
        if (synthetic != NULL) {
            xud_check(xud_device_open(&synthetic, NULL, NULL, VK_NULL_HANDLE) == XUD_INVALID_ARG, "device/open-already-owned",
                "a pointer that already owns a device is an argument error, not a leak");
            xud_check(
                synthetic != NULL, "device/open-already-owned-untouched", "...and the struct it already owns is not freed by the refusal");

            /* A handle-less struct: the guard and the rung both answer without touching the driv
               er, which is what makes these two paths testable headless.
             */
            xud_check(xud_device_wait_idle(synthetic, NULL) == XUD_INVALID_ARG, "device/wait-idle-null-loader",
                "a null loader is an argument error");
            xud_check(xud_device_wait_idle(NULL, NULL) == XUD_INVALID_ARG, "device/wait-idle-null-device", "a null device too");
            xud_check(
                xud_device_close(NULL, NULL) == XUD_INVALID_ARG, "device/close-null-ptr", "a null pointer-to-pointer must be rejected");
            xud_check(xud_device_close(&synthetic, NULL) == XUD_INVALID_ARG, "device/close-null-loader",
                "a null loader is an argument error even before the state");
            xud_check(synthetic != NULL, "device/close-arg-rejected-untouched", "a rejected close leaves the struct alone");

            if (xud_loader_open(&openLoader) == XUD_OK) {
                xud_check(openLoader->instanceTable.vkCreateDevice == NULL, "device/create-row-unbound",
                    "the create row is instance-level: NULL until the instance layer fills it");
                xud_check(openLoader->deviceTable.vkGetDeviceQueue == NULL, "device/get-row-unbound", "same for the queue lookup");
                xud_check(openLoader->deviceTable.vkDeviceWaitIdle == NULL, "device/wait-row-unbound", "same for the guard");
                xud_check(openLoader->deviceTable.vkDestroyDevice == NULL, "device/destroy-row-unbound", "same for the teardown call");

                xud_check(xud_device_wait_idle(synthetic, openLoader) == XUD_ALREADY, "device/wait-idle-no-handle",
                    "nothing is in flight on a device that does not exist");
                xud_check(
                    xud_device_close(&synthetic, openLoader) == XUD_OK, "device/close-handleless", "no driver call, the struct is freed");
                xud_check(synthetic == NULL, "device/close-handleless-null", "and the out-param cleared");

                /* Caps with no chosen family: the check fires before any slot is read, so the f
                   ake instance is never dereferenced (xud_physical_open() refuses that state, so
                   nothing above this layer produced these caps either).
                 */
                if (fakePhysical != NULL) {
                    fakePhysical->caps.queueFamilyIndex = XUD_PHYSICAL_NO_QUEUE_FAMILY;
                    xud_check(fakePhysical->caps.queueFamilyIndex == XUD_PHYSICAL_NO_QUEUE_FAMILY, "device/fake-physical-sentinel",
                        "the no-family sentinel is written explicitly - a calloc-ed caps sits at "
                        "0, which is a real family index, which is why "
                        "xud_physical_select_queue_family writes the sentinel on refusal");
                    xud_check(xud_device_open(&held, openLoader, fakePhysical, fakeInstance) == XUD_INVALID_ARG, "device/open-no-family",
                        "caps with no chosen family are not a usable input, checked before any "
                        "slot is read");
                    xud_check(held == NULL, "device/open-no-family-null", "...and nothing was created");
                }
                xud_check(xud_loader_close(&openLoader) == XUD_OK, "device/loader-close", "the loader this block opened must close");
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

void xud_test_device(void) { xud_test_device_pure(); }
