/***********************************************************************************************
 *
 *   xud_device.c - the device layer: vkCreateDevice from the recorded caps, the two role
 *          queues, and the teardown guard
 *
 *   Owns : the device layer. Binds the four slots it calls (vkCreateDevice, vkDestroyDevice,
 *          vkGetDeviceQueue, vkDeviceWaitIdle) through the live VkInstance it was handed, as
 *          sembles the one queue row and the one create-info from the physical layer's caps, c
 *          reates the device, resolves the two role queues, and tears all of it back down with
 *          vkDeviceWaitIdle ahead of vkDestroyDevice.
 *   Layer: above the physical device, below the swapchain. Nothing in this file calls a Vulkan
 *          entry point directly - every one is reached through a slot of XudLoader, so the arch
 *          ive holds no vk* symbol and the link line has no -lvulkan anywhere (AGENTS.md). The
 *          four names this layer calls are already slots of the table and they are instance-le
 *          vel, not device-level: probed 2026-10-18 through the instance xud_instance.c create
 *          s and through a bare one at apiVersion 1.0 with no extension and no layer, vkGetIns
 *          tanceProcAddr answers non-NULL for all four and NULL for all four with VK_NULL_HAND
 *          LE in hand, which is why the layer takes an instance and never resolves anything th
 *          rough the VkDevice it just made.
 *          What the validation layer turned into hard rules here (probed 2026-10-18, VK_INSTA
 *          NCE_LAYERS=VK_LAYER_KHRONOS_validation): pQueuePriorities must not be NULL when queu
 *          eCount is non-zero, both create-info structs must be stamped, queueCount must be gre
 *          ater than 0, a family may not be asked for twice, and more queues may not be request
 *          ed than the family has. lavvk accepts every one of those violations without the laye
 *          r, so the rules are enforced by this file's own assembly (one row, one family, count
 *          = min(family count, XUD_DEVICE_QUEUE_MAX_COUNT), priorities array always present) rat
 *          her than by a driver that says no.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud_device.h"
#include "xud.h"
#include "xud_loader.h"
#include "xud_physical.h"
#include "xud_vulkan.h"

#include <assert.h> /* Required for: static_assert, the PFN slot and array-size checks */
#include <stdlib.h> /* Required for: calloc, free */

/* The slots xud_device_open() binds through xud_loader_bind() must be pointer-sized, the same
   rule xud_loader.c checks for the global-level ones and xud_instance.c/xud_physical.c add for
   the tiers above them (xud_pfn_copy moves one word). */
static_assert(sizeof(PFN_vkCreateDevice) == sizeof(void*), "PFN slot must be pointer-sized");
static_assert(sizeof(PFN_vkDestroyDevice) == sizeof(void*), "PFN slot must be pointer-sized");
static_assert(sizeof(PFN_vkGetDeviceQueue) == sizeof(void*), "PFN slot must be pointer-sized");
static_assert(sizeof(PFN_vkDeviceWaitIdle) == sizeof(void*), "PFN slot must be pointer-sized");

/* The row's pQueuePriorities points at the struct's own array (see xud_device.h's note on why the
   array lives inside the struct rather than on a frame), so the array must be at least as long as
   the biggest count this layer ever asks a row for. Checked, not trusted: the row's queueCount is
   clamped to XUD_DEVICE_QUEUE_MAX_COUNT in xud_device_fill_queue_create_info, and an array shorter
   than that clamp would make the row point past its end. The member access inside sizeof is not e
   valuated - sizeof of a non-VLA array member is a compile-time computation - so no object is eve
   r dereferenced here. */
static_assert(
    sizeof(((XudDevice*)0)->queuePriorities) / sizeof(float) >= XUD_DEVICE_QUEUE_MAX_COUNT,
    "the row's priorities array must be at least as long as the biggest count this layer asks a "
    "row for");

/* The device-level extension names this layer wants, in request order. One owner per wanted list
   - the rule xud_instance.c follows for its instance-level list and xud_diagnostics.c follows for
   the diagnostics layer's: this layer's list is here, documented here, and everything else reads
   what the struct ended up with. Device-level means vkEnumerateDeviceExtensionProperties territory
   (the caps' advertised list), not vkEnumerateInstanceExtensionProperties territory: VK_KHR_swapch
   ain is the one name in this list, and it is the name the swapchain layer above needs - the caps a
   dvertise it here (probed 2026-10-18: 196 device-level extensions, VK_KHR_swapchain at row 84), so
   it is enabled, and on a driver that does not advertise it the list stays empty and that layer ans
   wers XUD_UNSUPPORTED rather than calling vkCreateSwapchainKHR on a device that was not created fo
   r one.
   Deliberately not wanted today: VK_KHR_debug (its device-level form is a per-device request, and
   the diagnostics layer's messenger is instance-level and already gives the log side - a per-devic
   e debug request is a diagnostics decision, not this layer's), VK_KHR_surface (instance-level alre
   ady, enabled by xud_instance.c), and every 1.1+ device-level name (AGENTS.md's minimum-core deci
   sion is the 1.0 floor plus VK_KHR_SWAPCHAIN; the layers above this one ask for higher paths gate
   d on recorded caps, and asking here would be asking for a path nothing above needs yet). */
static const char* const XUD_DEVICE_WANTED_DEVICE_EXTENSIONS[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
};
#define XUD_DEVICE_WANTED_DEVICE_COUNT                                                             \
    ((uint32_t)(sizeof(XUD_DEVICE_WANTED_DEVICE_EXTENSIONS)                                        \
        / sizeof(XUD_DEVICE_WANTED_DEVICE_EXTENSIONS[0])))

static_assert(XUD_DEVICE_WANTED_DEVICE_COUNT <= XUD_DEVICE_MAX_DEVICE_EXTENSIONS,
    "the wanted device-extension list must fit the device struct's storage");

/*-----------------------------------------------------------------------------------------------
 * The one queue row, from the physical layer's chosen family
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_device_fill_queue_create_info(
    VkDeviceQueueCreateInfo* outQueueRow, const float* priorities, const XudDeviceCaps* caps) {
    uint32_t familyQueues = 0u;

    if ((outQueueRow == NULL) || (priorities == NULL) || (caps == NULL))
        return XUD_INVALID_ARG;

    /* The caps must be a usable input: the physical layer's own open refuses both shapes below,
       so caps in either shape are not caps this layer was handed by the layer below - they are a
       struct somebody else built, and a row built from it would name a family the driver never
       chose (the validation layer's VUID-VkDeviceQueueCreateInfo-queueFamilyIndex-00383 would be
       the only thing catching that later). */
    if (caps->queueFamilyIndex == XUD_PHYSICAL_NO_QUEUE_FAMILY)
        return XUD_INVALID_ARG;
    familyQueues = caps->chosenQueueFamilyProperties.queueFamilyProperties.queueCount;
    if (familyQueues == 0u)
        return XUD_INVALID_ARG; /* a family with no queue in it is not a family this layer can ask
                                   for */

    outQueueRow->sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    outQueueRow->pNext = NULL;
    outQueueRow->flags
        = 0; /* VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT would put the protected-memory discipline above
                this layer, and nothing above it asks for that today - see xud_device.h */
    outQueueRow->queueFamilyIndex = caps->queueFamilyIndex;
    outQueueRow->queueCount
        = (familyQueues < XUD_DEVICE_QUEUE_MAX_COUNT) ? familyQueues : XUD_DEVICE_QUEUE_MAX_COUNT;
    outQueueRow->pQueuePriorities = priorities; /* the parameter is const float * and the field is
                                                   const float *, so the two match with no cast */
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * Which advertised device-level extensions get enabled
 *-----------------------------------------------------------------------------------------------*/

/* Internal: append one device-level extension name to the device's enabled list, but only when t
   he caps advertised it and it is not already there. The mirror of xud_instance_enable_if_adverti
   sed one tier up: nothing is ever forced on, a name the caps do not advertise simply stays out,
   and the duplicate rule is what lets a wanted list repeat a name without the list needing to kno
   w what the struct already holds. */
static XudResult xud_device_enable_if_advertised(XudDevice* outDevice,
    const VkExtensionProperties* advertised, uint32_t advertisedCount, const char* name,
    bool* outAppended) {
    uint32_t advertisedIndex = 0u;
    uint32_t enabledIndex = 0u;

    *outAppended = false;

    for (advertisedIndex = 0u; advertisedIndex < advertisedCount; advertisedIndex++) {
        if (xud_name_equal(advertised[advertisedIndex].extensionName, name)) {
            *outAppended = true;
            break;
        }
    }
    if (*outAppended == false)
        return XUD_OK; /* wanted but not advertised: stays out, nothing is forced on */

    for (enabledIndex = 0u; enabledIndex < outDevice->enabledExtensionCount; enabledIndex++) {
        if (xud_name_equal(outDevice->enabledExtensionNameStorage[enabledIndex], name)) {
            *outAppended = false;
            return XUD_OK; /* already in the list */
        }
    }
    if (outDevice->enabledExtensionCount >= XUD_DEVICE_MAX_DEVICE_EXTENSIONS)
        return XUD_OUT_OF_RESOURCES;

    if (xud_name_copy(outDevice->enabledExtensionNameStorage[outDevice->enabledExtensionCount],
            VK_MAX_EXTENSION_NAME_SIZE, name)
        != XUD_OK)
        return XUD_INTERNAL_ERROR; /* a driver name longer than the buffer: reported, not truncated
                                    */

    outDevice->enabledExtensionCount += 1u;
    *outAppended = true;
    return XUD_OK;
}

XudResult xud_device_select_extensions(
    XudDevice* outDevice, const VkExtensionProperties* advertised, uint32_t advertisedCount) {
    uint32_t wantedIndex = 0u;
    XudResult result = XUD_INVALID_ARG;
    bool appended = false;

    if ((outDevice == NULL) || ((advertised == NULL) && (advertisedCount != 0u)))
        return XUD_INVALID_ARG;

    /* The selection is rewritten on every call, so a reused struct cannot keep a stale list. */
    outDevice->enabledExtensionCount = 0u;

    for (wantedIndex = 0u; wantedIndex < XUD_DEVICE_WANTED_DEVICE_COUNT; wantedIndex++) {
        result = xud_device_enable_if_advertised(outDevice, advertised, advertisedCount,
            XUD_DEVICE_WANTED_DEVICE_EXTENSIONS[wantedIndex], &appended);
        if (result != XUD_OK) {
            outDevice->enabledExtensionCount = 0u;
            return result;
        }
    }
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The create-info, assembled from this layer's own record
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_device_fill_create_info(VkDeviceCreateInfo* outCreateInfo,
    const char* nameSlots[XUD_DEVICE_MAX_DEVICE_EXTENSIONS], const XudDevice* device) {
    uint32_t index = 0u;

    if ((outCreateInfo == NULL) || (nameSlots == NULL) || (device == NULL))
        return XUD_INVALID_ARG;

    /* The row must be the one this layer's own fill helper produced, and it must still point at t
       he struct's own priorities array: a row assembled from a frame-local array is exactly the d
      angling-pointer bug the struct layout in xud_device.h exists to prevent, and lavvk accepts t
       he row without checking (probed 2026-10-18), so the check is here rather than in the drive
       r. */
    if ((device->queueRow.sType != VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO)
        || (device->queueRow.pNext != NULL))
        return XUD_INVALID_ARG;
    if ((device->queueRow.queueCount < 1u)
        || (device->queueRow.queueCount > XUD_DEVICE_QUEUE_MAX_COUNT))
        return XUD_INVALID_ARG;
    if (device->queueRow.queueFamilyIndex == XUD_PHYSICAL_NO_QUEUE_FAMILY)
        return XUD_INVALID_ARG;
    if (device->queueRow.pQueuePriorities != device->queuePriorities)
        return XUD_INVALID_ARG;
    if (device->enabledExtensionCount > XUD_DEVICE_MAX_DEVICE_EXTENSIONS)
        return XUD_INVALID_ARG; /* the count does not fit the storage it came from */

    outCreateInfo->sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    outCreateInfo->pNext = NULL;
    outCreateInfo->flags = 0;
    outCreateInfo->queueCreateInfoCount = 1u;
    outCreateInfo->pQueueCreateInfos = &device->queueRow;

    /* AGENTS.md's one-family rule, carried: exactly one row, the family the physical layer chose,
       and the validation layer's complaint about a second row for the same family (probed 2026-1
       0-18: "pCreateInfo->pQueueCreateInfos[1] uses the same queueFamilyIndex (0) and flags ... a
       s pCreateInfo->pQueueCreateInfos[0]") is a shape this layer cannot produce. */

    /* Legacy in these headers - vulkan_core.h:3747/:3749 carry the comment "legacy and not used"
       above both fields from Vulkan-Headers 1.4.363 here. They are still fields, so they are wri
       tten: the layers this library enables are instance-level and travel on VkInstanceCreateInf
       o with the diagnostics layer's answer, never here. */
    outCreateInfo->enabledLayerCount = 0u;
    outCreateInfo->ppEnabledLayerNames = NULL;

    for (index = 0u; index < device->enabledExtensionCount; index++)
        nameSlots[index] = device->enabledExtensionNameStorage[index];
    for (; index < XUD_DEVICE_MAX_DEVICE_EXTENSIONS; index++)
        nameSlots[index] = NULL; /* the tail is always NULLed so a stale pointer cannot survive */

    outCreateInfo->enabledExtensionCount = device->enabledExtensionCount;
    outCreateInfo->ppEnabledExtensionNames
        = (device->enabledExtensionCount != 0u) ? nameSlots : NULL;

    /* The features the device enables are the physical layer's wanted-intersection, already norm
       alised to VK_TRUE/VK_FALSE by xud_physical_record_features (see xud_physical.c: a driver an
       swering 42 for a feature does not get 42 handed to vkCreateDevice), and they are copied int
       o this struct so the record says what the device was created with rather than pointing at
       caps that belong to another layer. */
    outCreateInfo->pEnabledFeatures
        = &device->enabledFeatures; /* the field is const VkPhysicalDeviceFeatures *, so the address
                                       of this struct's own copy needs no cast */
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The two role queues, looked up in what open resolved
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_device_queue_get(XudQueue* outQueue, const XudDevice* device, XudQueueRole role) {
    const XudQueue* stored = NULL;

    if ((outQueue == NULL) || (device == NULL))
        return XUD_INVALID_ARG;
    if (role >= (XudQueueRole)XUD_DEVICE_QUEUE_ROLE_COUNT)
        return XUD_INVALID_ARG; /* the enum has two values and a caller can still pass 7 */
    if (device->device == VK_NULL_HANDLE)
        return XUD_INVALID_ARG; /* no device: there is no queue to hand back for a device that does
                                   not exist */

    stored = (role == XUD_QUEUE_ROLE_PRESENT) ? &device->presentQueue : &device->graphicsQueue;
    if (stored->queue == VK_NULL_HANDLE)
        return XUD_INTERNAL_ERROR; /* a struct that claims a live device but has no queue in it:
                                      xud_device_open() resolves both roles before it hands the
                                      struct out, so this state was not produced here */

    /* The pair is checked against this layer's own row before it is ever handed to vkGetDeviceQue
       ue above - the same two rules the validation layer states (VUID-vkGetDeviceQueue-queueFamil
       yIndex-00384, VUID-vkGetDeviceQueue-queueIndex-00385, both probed 2026-10-18 by asking with
       family 99 and with index 1 against a row that asked for one queue). Catching it here is an
       answer rather than a message stream, and lavvk accepts both wrong pairs without the layer.
     */
    if (stored->queueFamilyIndex != device->queueRow.queueFamilyIndex)
        return XUD_INTERNAL_ERROR;
    if (stored->queueIndex >= device->queueRow.queueCount)
        return XUD_INTERNAL_ERROR;

    *outQueue = *stored;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * Bring-up: bind, assemble, create, resolve the two queues
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_device_open(XudDevice** outDevice, XudLoader* loader,
    const XudPhysicalDevice* physical, VkInstance instance) {
    XudDevice* device = NULL;
    VkDeviceCreateInfo createInfo;
    const char* nameSlots[XUD_DEVICE_MAX_DEVICE_EXTENSIONS];
    XudResult result = XUD_INVALID_ARG;
    VkResult vk = VK_SUCCESS;
    uint32_t role = 0u;
    uint32_t queueIndex = 0u;

    if ((outDevice == NULL) || (*outDevice != NULL) || (loader == NULL) || (physical == NULL)
        || (instance == VK_NULL_HANDLE))
        return XUD_INVALID_ARG;
    if (!xud_loader_is_open(loader))
        return XUD_UNSUPPORTED;
    if (physical->caps.queueFamilyIndex == XUD_PHYSICAL_NO_QUEUE_FAMILY)
        return XUD_INVALID_ARG; /* the caps handed in are not a usable input - xud_physical_open()
                                   refuses that state, so nothing above produced these caps */
    if (physical->physicalDevice == VK_NULL_HANDLE)
        return XUD_INVALID_ARG; /* vkCreateDevice's first argument in these headers
                                   (PFN_vkCreateDevice, vulkan_core.h:4541): a device cannot be
                                   created from no physical device */

    device = calloc(1u, sizeof(XudDevice));
    if (device == NULL)
        return XUD_OUT_OF_MEMORY;

    /* The zero state the driver needs before it answers: the handle is NULL until vkCreateDevice
       writes it (and a failed create must leave the fail ladder nothing to destroy), and both que
       ue records are empty until the two lookups write them. */
    device->device = VK_NULL_HANDLE;
    device->graphicsQueue.queue = VK_NULL_HANDLE;
    device->presentQueue.queue = VK_NULL_HANDLE;
    for (role = 0u; role < XUD_DEVICE_QUEUE_MAX_COUNT; role++)
        device->queuePriorities[role] = XUD_DEVICE_QUEUE_PRIORITY_DEFAULT;

    /* The four names this layer calls, resolved through the live instance and cached in the table
       - they name the driver's own code, not a layer instantiated per instance, so a slot stays v
       alid across an instance recreation the way a cached messenger pointer would not (xud_loader
       .h's rule, and the reason the loader parameter is non-const: xud_loader_bind writes slots, a
       nd both compilers reject a const XudLoader * here, the same rule xud_physical.c states). A n
       ame the loader does not know answers NULL and xud_loader_bind reports XUD_UNSUPPORTED - neve
       r called through. */
    result = xud_loader_bind(loader, instance, "vkCreateDevice", &loader->createDevice);
    if (result != XUD_OK)
        goto fail;
    result = xud_loader_bind(loader, instance, "vkGetDeviceQueue", &loader->getDeviceQueue);
    if (result != XUD_OK)
        goto fail;
    result = xud_loader_bind(loader, instance, "vkDeviceWaitIdle", &loader->deviceWaitIdle);
    if (result != XUD_OK)
        goto fail;
    result = xud_loader_bind(loader, instance, "vkDestroyDevice", &loader->destroyDevice);
    if (result != XUD_OK)
        goto fail;

    /* The nil-slot rule xud_device.h states, for the four calls this layer makes. xud_loader_bind
       filled each slot on XUD_OK; this is the check the contract promises, not a second opinion a
       bout the bind. */
    if ((loader->createDevice == NULL) || (loader->getDeviceQueue == NULL)
        || (loader->deviceWaitIdle == NULL) || (loader->destroyDevice == NULL)) {
        result = XUD_UNSUPPORTED;
        goto fail;
    }

    /* The names to enable, chosen against the caps' advertised list - the check-before-you-use
       rule, and the reason a driver without VK_KHR_swapchain still gets a device (the swapchain
       layer above is the one that refuses). The array is the caps' own: a query result the physi
       cal layer owns, never freed here. */
    result = xud_device_select_extensions(
        device, physical->caps.deviceExtensionProperties, physical->caps.deviceExtensionCount);
    if (result != XUD_OK)
        goto fail;

    /* The features to enable: the physical layer's wanted-intersection, not the driver's verbat
       im answer. xud_physical_record_features is that intersection's one owner (xud_physical.c), s
       o this layer asks it for the answer rather than spelling the wanted list a second time here -
       the rule xud_instance.c follows for its extension lists, one owner per wanted list. Copying
       ca ps.features.features instead would hand vkCreateDevice every feature this driver happens
       to ha ve (geometryShader, tessellation, everything), which is the opposite of the
       check-before-you-us e rule in AGENTS.md: VkDeviceCreateInfo must carry only what was verified
       as wanted. A driver th at reported a feature absent stays off in this struct, and nothing
       here forces it on. */
    result = xud_physical_record_features(&device->enabledFeatures, &physical->caps);
    if (result != XUD_OK)
        goto fail;

    result = xud_device_fill_queue_create_info(
        &device->queueRow, device->queuePriorities, &physical->caps);
    if (result != XUD_OK)
        goto fail;

    result = xud_device_fill_create_info(&createInfo, nameSlots, device);
    if (result != XUD_OK)
        goto fail;

    vk = loader->createDevice(physical->physicalDevice, &createInfo, NULL, &device->device);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK) {
        /* A driver that refuses for a feature or a name we asked for is unsupported, not broken
           (xud_result_from_vk maps VK_ERROR_FEATURE_NOT_PRESENT and VK_ERROR_EXTENSION_NOT_PRESE
           NT to XUD_UNSUPPORTED); reaching that arm means the caps and the driver disagree, and
           this layer asked only for what the caps verified. The handle is untouched on failure in
           these headers, and it was set to VK_NULL_HANDLE above, so the fail ladder below has not
           hing to destroy. */
        device->device = VK_NULL_HANDLE;
        goto fail;
    }

    /* The two role queues, resolved once here rather than on every frame: a queue handle lives a
       s long as the device that owns it does, and there is no vkDestroyQueue, so there is nothing
       above this layer can do with a re-resolve. The family index is the row's own and the index
       is the role clamped into the count the row asked for - the two rules the validation layer's
       queue VUIDs state, satisfied by construction rather than by hoping. */
    for (role = 0u; role < XUD_DEVICE_QUEUE_ROLE_COUNT; role++) {
        XudQueue* record
            = (role == XUD_QUEUE_ROLE_PRESENT) ? &device->presentQueue : &device->graphicsQueue;

        queueIndex
            = (role < device->queueRow.queueCount) ? role : (device->queueRow.queueCount - 1u);
        record->queue
            = VK_NULL_HANDLE; /* the zero state before the call: PFN_vkGetDeviceQueue returns void
                                 in these headers (vulkan_core.h:4547), so there is no VkResult to
                                 map and the out handle is the only answer available */
        loader->getDeviceQueue(
            device->device, device->queueRow.queueFamilyIndex, queueIndex, &record->queue);
        if (record->queue == VK_NULL_HANDLE) {
            /* A driver that answers nothing for a family/index pair the row itself asked for: the
               caps and the driver disagree, and a device handed out with no queue in it would be a
               device nothing above can submit to. Refused here, and the fail ladder below destroys
               the device this step made. */
            result = XUD_VULKAN_ERROR;
            goto fail;
        }
        record->queueFamilyIndex = device->queueRow.queueFamilyIndex;
        record->queueIndex = queueIndex;
    }

    *outDevice = device;
    return XUD_OK;

fail:
    /* Reverse creation order: the queues are projections of the device and have nothing of their
       own to release, so the device is the only Vulkan object this step can have made, and it is
       destroyed through the slot this layer just bound (PFN_vkDestroyDevice returns void in these
       headers, so there is no answer to check for the teardown call itself). The struct's memory is
       freed on every path, so a partial bring-up leaks nothing. */
    if ((device != NULL) && (device->device != VK_NULL_HANDLE) && (loader->destroyDevice != NULL)) {
        (void)loader->destroyDevice(device->device, NULL);
        device->device = VK_NULL_HANDLE;
    }
    if (device != NULL)
        free(device);
    *outDevice = NULL;
    return (result != XUD_OK) ? result : XUD_INTERNAL_ERROR;
}

/*-----------------------------------------------------------------------------------------------
 * The teardown guard, and the rung that uses it
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_device_wait_idle(const XudDevice* device, const XudLoader* loader) {
    VkResult vk = VK_SUCCESS;

    if ((loader == NULL) || (device == NULL))
        return XUD_INVALID_ARG;
    if (!xud_loader_is_open(loader))
        return XUD_UNSUPPORTED;
    if (device->device == VK_NULL_HANDLE)
        return XUD_ALREADY; /* nothing is in flight on a device that does not exist, which is
                               already the state asked for */
    if (loader->deviceWaitIdle == NULL)
        return XUD_UNSUPPORTED; /* a nil PFN is reported, never called through - the rule every call
                                   site in this tree follows */

    vk = loader->deviceWaitIdle(device->device);
    return xud_result_from_vk(vk);
}

XudResult xud_device_close(XudDevice** device, const XudLoader* loader) {
    XudDevice* victim = NULL;
    XudResult guard = XUD_OK;

    if ((device == NULL) || (loader == NULL))
        return XUD_INVALID_ARG; /* the arguments are checked before the state, the rule
                                   xud_physical_close() follows */
    if (!xud_loader_is_open(loader))
        return XUD_UNSUPPORTED;
    if (*device == NULL)
        return XUD_ALREADY;

    victim = *device;

    /* The guard first, and it is this layer's own guarantee rather than a promise about the caller
       above: the spec requires that nothing is in flight when a device is destroyed, and a caller
       that had queued submissions without submitting them would otherwise be racing them. A refus
       al here (a nil slot) leaves the device alive and the struct un-freed - refusing is the honest
       answer, leaking a handle silently is not. XUD_ALREADY (the struct has no handle) and XUD_LOST
       (the driver says the device is gone) both continue below - a lost device is still destroyed,
       v kDestroyDevice is this layer's teardown call and skipping it would leak an object the
       driver m ay still hold. */
    guard = xud_device_wait_idle(victim, loader);
    if (guard == XUD_UNSUPPORTED)
        return guard;

    if (victim->device != VK_NULL_HANDLE) {
        if (loader->destroyDevice == NULL)
            return XUD_UNSUPPORTED; /* the handle would leak silently, so this rung refuses instead
                                     */

        /* PFN_vkDestroyDevice returns void in these headers (vulkan_core.h:4542) - there is no
           VkResult for xud_result_from_vk to map here, so the destroy is a call whose answer is not
           available and the layer's answer is the guard's. The handle is cleared after the call so
           a struct that survi ves this (it does not, it is freed below) cannot be torn down twice.
         */
        loader->destroyDevice(victim->device, NULL);
        victim->device = VK_NULL_HANDLE;
    }

    free(victim);
    *device = NULL;

    /* A lost device is still destroyed above and still freed here - vkDestroyDevice is this layer's
       teardown call and skipping it would leak an object the driver may still hold - and the answer
       th at travels is the guard's, so a caller sees XUD_LOST rather than a clean OK. XUD_ALREADY
       from the guard means this rung had nothing to guard and nothing to destroy: the struct was
       freed below, wh ich is what the rung did, so the answer is XUD_OK. */
    if (guard == XUD_LOST)
        return XUD_LOST;
    return XUD_OK;
}
