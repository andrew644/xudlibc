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

/* The rows this layer calls through must be pointer-sized, the same rule xud_loader.h checks
   for the table pair as a whole. Volk's rows are PFN typedefs, so this is a property of the hea
   ders rather than of a copy that moves one word - the hand-rolled bring-up this replaced had su
   ch a helper to check against, Volk has nothing here to move. */
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
static_assert(sizeof(((XudDevice*)0)->queuePriorities) / sizeof(float) >= XUD_DEVICE_QUEUE_MAX_COUNT,
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
   ady, enabled by xud_instance.c), and every other 1.1+ device-level name: AGENTS.md's minimum-core
   decision is now the 1.3 floor, and VK_KHR_swapchain is core from 1.1 up, so the one name this
   layer asks for is a courtesy rather than a requirement - it is still advertised at a 1.3 instance
   (probed 2026-11-01: 196 device-level extensions, VK_KHR_swapchain at row 84, the same numbers the
   1.0 floor answered on 2026-10-18), and asking for a second name would be asking for a path that
   nothing above this layer needs yet. */
static const char* const XUD_DEVICE_WANTED_DEVICE_EXTENSIONS[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
};
#define XUD_DEVICE_WANTED_DEVICE_COUNT                                                                                                     \
    ((uint32_t)(sizeof(XUD_DEVICE_WANTED_DEVICE_EXTENSIONS) / sizeof(XUD_DEVICE_WANTED_DEVICE_EXTENSIONS[0])))

static_assert(XUD_DEVICE_WANTED_DEVICE_COUNT <= XUD_DEVICE_MAX_DEVICE_EXTENSIONS,
    "the wanted device-extension list must fit the device struct's storage");

/*-----------------------------------------------------------------------------------------------
 * The queue rows: the graphics family's, and the dedicated transfer family's when there is one
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_device_fill_queue_create_info(VkDeviceQueueCreateInfo* outQueueRow, const float* priorities, const XudDeviceCaps* caps) {
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
    outQueueRow->flags = 0; /* VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT would put the protected-memory discipline above
                               this layer, and nothing above it asks for that today - see xud_device.h */
    outQueueRow->queueFamilyIndex = caps->queueFamilyIndex;
    outQueueRow->queueCount = (familyQueues < XUD_DEVICE_QUEUE_MAX_COUNT) ? familyQueues : XUD_DEVICE_QUEUE_MAX_COUNT;
    outQueueRow->pQueuePriorities = priorities; /* the parameter is const float * and the field is
                                                   const float *, so the two match with no cast */
    return XUD_OK;
}

XudResult xud_device_fill_transfer_queue_create_info(
    VkDeviceQueueCreateInfo* outQueueRow, const float* priorities, const XudDeviceCaps* caps) {
    uint32_t familyQueues = 0u;

    if ((outQueueRow == NULL) || (priorities == NULL) || (caps == NULL))
        return XUD_INVALID_ARG;

    if (caps->transferQueueFamilyIndex == XUD_PHYSICAL_NO_QUEUE_FAMILY)
        return XUD_INVALID_ARG; /* the physical layer chooses the transfer family beside the graphics
                                   one; caps without it are caps its open never produced */
    familyQueues = caps->chosenTransferQueueFamilyProperties.queueFamilyProperties.queueCount;
    if (familyQueues == 0u)
        return XUD_INVALID_ARG; /* a family with no queue in it is not a family this layer can ask
                                   for */

    outQueueRow->sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    outQueueRow->pNext = NULL;
    outQueueRow->flags = 0; /* the same protected-memory decision as row 0 */
    outQueueRow->queueFamilyIndex = caps->transferQueueFamilyIndex;
    outQueueRow->queueCount = (familyQueues < XUD_DEVICE_QUEUE_MAX_COUNT) ? familyQueues : XUD_DEVICE_QUEUE_MAX_COUNT;
    outQueueRow->pQueuePriorities = priorities;
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
static XudResult xud_device_enable_if_advertised(
    XudDevice* outDevice, const VkExtensionProperties* advertised, uint32_t advertisedCount, const char* name, bool* outAppended) {
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

    if (xud_name_copy(outDevice->enabledExtensionNameStorage[outDevice->enabledExtensionCount], VK_MAX_EXTENSION_NAME_SIZE, name) != XUD_OK)
        return XUD_INTERNAL_ERROR; /* a driver name longer than the buffer: reported, not truncated
                                    */

    outDevice->enabledExtensionCount += 1u;
    *outAppended = true;
    return XUD_OK;
}

XudResult xud_device_select_extensions(XudDevice* outDevice, const VkExtensionProperties* advertised, uint32_t advertisedCount) {
    uint32_t wantedIndex = 0u;
    XudResult result = XUD_INVALID_ARG;
    bool appended = false;

    if ((outDevice == NULL) || ((advertised == NULL) && (advertisedCount != 0u)))
        return XUD_INVALID_ARG;

    /* The selection is rewritten on every call, so a reused struct cannot keep a stale list. */
    outDevice->enabledExtensionCount = 0u;

    for (wantedIndex = 0u; wantedIndex < XUD_DEVICE_WANTED_DEVICE_COUNT; wantedIndex++) {
        result = xud_device_enable_if_advertised(
            outDevice, advertised, advertisedCount, XUD_DEVICE_WANTED_DEVICE_EXTENSIONS[wantedIndex], &appended);
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

XudResult xud_device_fill_create_info(
    VkDeviceCreateInfo* outCreateInfo, const char* nameSlots[XUD_DEVICE_MAX_DEVICE_EXTENSIONS], const XudDevice* device) {
    uint32_t index = 0u;

    if ((outCreateInfo == NULL) || (nameSlots == NULL) || (device == NULL))
        return XUD_INVALID_ARG;

    /* Every row named by the count must be one this layer's own fill helpers produced, and it
       must still point at the struct's own priorities array: a row assembled from a frame-local
       array is exactly the dangling-pointer bug the struct layout in xud_device.h exists to pre
       vent, and lavvk accepts the row without checking (probed 2026-10-18), so the check is her
       e rather than in the driver. */
    if ((device->queueRowCount < 1u) || (device->queueRowCount > XUD_DEVICE_QUEUE_ROW_MAX_COUNT))
        return XUD_INVALID_ARG;
    if ((device->queueRows[0].sType != VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO) || (device->queueRows[0].pNext != NULL))
        return XUD_INVALID_ARG;
    if ((device->queueRows[0].queueCount < 1u) || (device->queueRows[0].queueCount > XUD_DEVICE_QUEUE_MAX_COUNT))
        return XUD_INVALID_ARG;
    if (device->queueRows[0].queueFamilyIndex == XUD_PHYSICAL_NO_QUEUE_FAMILY)
        return XUD_INVALID_ARG;
    if (device->queueRows[0].pQueuePriorities != device->queuePriorities)
        return XUD_INVALID_ARG;
    if (device->queueRowCount == 2u) {
        if ((device->queueRows[1].sType != VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO) || (device->queueRows[1].pNext != NULL))
            return XUD_INVALID_ARG;
        if ((device->queueRows[1].queueCount < 1u) || (device->queueRows[1].queueCount > XUD_DEVICE_QUEUE_MAX_COUNT))
            return XUD_INVALID_ARG;
        if (device->queueRows[1].queueFamilyIndex == XUD_PHYSICAL_NO_QUEUE_FAMILY)
            return XUD_INVALID_ARG;
        if (device->queueRows[1].queueFamilyIndex == device->queueRows[0].queueFamilyIndex)
            return XUD_INVALID_ARG; /* a second row for the SAME family is the shape the validation
                                       layer calls out ("pCreateInfo->pQueueCreateInfos[1] uses the
                                       same queueFamilyIndex"), so the count and the families may
                                       not disagree */
        if (device->queueRows[1].pQueuePriorities != device->transferQueuePriorities)
            return XUD_INVALID_ARG;
    }
    if (device->enabledExtensionCount > XUD_DEVICE_MAX_DEVICE_EXTENSIONS)
        return XUD_INVALID_ARG; /* the count does not fit the storage it came from */

    if ((device->enabledDescriptorIndexing.sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES)
        || (device->enabledDescriptorIndexing.pNext != NULL)) {
        return XUD_UNSUPPORTED; /* the chained row is not the one xud_physical_record_descriptor_indexing
                                   stamps, or it carries a chain this layer cannot model */
    }

    outCreateInfo->sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    outCreateInfo->pNext = &device->enabledDescriptorIndexing; /* the one chained row: the core-promoted
                                                                  bindless knobs, which ride the pNext
                                                                  without an extension name and share no
                                                                  field with pEnabledFeatures below */
    outCreateInfo->flags = 0;
    outCreateInfo->queueCreateInfoCount = device->queueRowCount;
    outCreateInfo->pQueueCreateInfos = device->queueRows;

    /* The row rule, carried from the families the physical layer chose: row 0 is always the gra
       phics family, and row 1 joins the count only when the transfer family is a DIFFERENT fami
       ly - the two rows above are what make a same-family pair a refusal rather than a driver m
       essage. */

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
    outCreateInfo->ppEnabledExtensionNames = (device->enabledExtensionCount != 0u) ? nameSlots : NULL;

    /* The features the device enables are the physical layer's wanted-intersection, already norm
       alised to VK_TRUE/VK_FALSE by xud_physical_record_features (see xud_physical.c: a driver an
       swering 42 for a feature does not get 42 handed to vkCreateDevice), and they are copied int
       o this struct so the record says what the device was created with rather than pointing at
       caps that belong to another layer. */
    outCreateInfo->pEnabledFeatures = &device->enabledFeatures; /* the field is const VkPhysicalDeviceFeatures *, so the address
                                                                   of this struct's own copy needs no cast */
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The three role queues, looked up in what open resolved
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_device_queue_get(XudQueue* outQueue, const XudDevice* device, XudQueueRole role) {
    const XudQueue* stored = NULL;
    const VkDeviceQueueCreateInfo* row = NULL;
    uint32_t index = 0u;

    if ((outQueue == NULL) || (device == NULL))
        return XUD_INVALID_ARG;
    if (role >= (XudQueueRole)XUD_DEVICE_QUEUE_ROLE_COUNT)
        return XUD_INVALID_ARG; /* the enum has three values and a caller can still pass 7 */
    if (device->device == VK_NULL_HANDLE)
        return XUD_INVALID_ARG; /* no device: there is no queue to hand back for a device that does
                                   not exist */

    stored = (role == XUD_QUEUE_ROLE_PRESENT) ? &device->presentQueue
        : (role == XUD_QUEUE_ROLE_TRANSFER)   ? &device->transferQueue
                                              : &device->graphicsQueue;
    if (stored->queue == VK_NULL_HANDLE)
        return XUD_INTERNAL_ERROR; /* a struct that claims a live device but has no queue in it:
                                      xud_device_open() resolves every role before it hands the
                                      struct out, so this state was not produced here */

    /* The pair is checked against the row that names this queue's family before it is ever hand
       ed to vkGetDeviceQueue above - the same two rules the validation layer states (VUID-vkGetD
       eviceQueue-queueFamilyIndex-00384, VUID-vkGetDeviceQueue-queueIndex-00385, both probed 2026
       -10-18 by asking with family 99 and with index 1 against a row that asked for one queue).
       The loop is what lets the transfer queue name row 1's family while graphics and present na
       me row 0's: a queue whose family no row names is a record open never resolved. Catching i
       t here is an answer rather than a message stream, and lavvk accepts both wrong pairs witho
       ut the layer.
     */
    for (index = 0u; index < device->queueRowCount; index++) {
        if (device->queueRows[index].queueFamilyIndex == stored->queueFamilyIndex) {
            row = &device->queueRows[index];
            break;
        }
    }
    if (row == NULL)
        return XUD_INTERNAL_ERROR;
    if (stored->queueIndex >= row->queueCount)
        return XUD_INTERNAL_ERROR;

    *outQueue = *stored;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * Bring-up: bind, assemble, create, resolve the three queues
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_device_open(XudDevice** outDevice, XudLoader* loader, const XudPhysicalDevice* physical, VkInstance instance) {
    XudDevice* device = NULL;
    VkDeviceCreateInfo createInfo;
    const char* nameSlots[XUD_DEVICE_MAX_DEVICE_EXTENSIONS];
    XudResult result = XUD_INVALID_ARG;
    VkResult vk = VK_SUCCESS;
    uint32_t role = 0u;
    uint32_t queueIndex = 0u;

    if ((outDevice == NULL) || (*outDevice != NULL) || (loader == NULL) || (physical == NULL) || (instance == VK_NULL_HANDLE))
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
       writes it (and a failed create must leave the fail ladder nothing to destroy), all three qu
       eue records are empty until the three lookups write them, and the count starts at the fall
       back shape - one row - because the second row is earned by the caps, not assumed. */
    device->device = VK_NULL_HANDLE;
    device->graphicsQueue.queue = VK_NULL_HANDLE;
    device->presentQueue.queue = VK_NULL_HANDLE;
    device->transferQueue.queue = VK_NULL_HANDLE;
    device->queueRowCount = 1u;
    for (role = 0u; role < XUD_DEVICE_QUEUE_MAX_COUNT; role++) {
        device->queuePriorities[role] = XUD_DEVICE_QUEUE_PRIORITY_DEFAULT;
        device->transferQueuePriorities[role] = XUD_DEVICE_QUEUE_PRIORITY_DEFAULT;
    }

    /* The row this layer creates the device through is instance-level: vkCreateDevice is resolv
       ed through vkGetInstanceProcAddr, which the instance layer resolved in bulk when it bound
       the instance table (xud_loader.h's tier rule, probed 2026-10-18: non-NULL through a live
       instance, NULL with VK_NULL_HANDLE in hand). The three rows this layer reads the device t
       hrough - vkGetDeviceQueue, vkDeviceWaitIdle, vkDestroyDevice - are device-level and cann
       ot exist before the device does, so they are filled by xud_loader_bind_device immediatel
       y after vkCreateDevice answered, resolving through vkGetDeviceProcAddr; that entry point i
       s looked up through the instance, which is why this layer still needs an instance in han
       d and never a VkDevice to resolve through, and why the loader parameter is non-const: x
       ud_loader_bind_device writes the device table, and both compilers reject a const XudLoad
       er * here. Both checks below are the check-before-you-use rule, one rung lower: a row the
       driver did not answer is reported, never called through. vkGetDeviceProcAddr is checked he
       re rather than at the bind because a device created without it could not be destroyed by
       the fail ladder below - leaking it silently is not an option this layer may take. */
    if (!xud_loader_instance_bound(loader, instance)) {
        result = XUD_UNSUPPORTED; /* no rows resolved for any instance, so nothing here can be
                                      called */
        goto fail;
    }
    if ((loader->instanceTable.vkCreateDevice == NULL) || (loader->instanceTable.vkGetDeviceProcAddr == NULL)) {
        result = XUD_UNSUPPORTED; /* a row the driver did not answer - never called through */
        goto fail;
    }

    /* The names to enable, chosen against the caps' advertised list - the check-before-you-use
       rule, and the reason a driver without VK_KHR_swapchain still gets a device (the swapchain
       layer above is the one that refuses). The array is the caps' own: a query result the physi
       cal layer owns, never freed here. */
    result = xud_device_select_extensions(device, physical->caps.deviceExtensionProperties, physical->caps.deviceExtensionCount);
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

    /* The bindless knobs, same one-owner rule: the intersection the physical layer derived, and
       then the gate - the descriptor rung's sampler array needs all four, so a driver missing any
       one is refused here rather than half-supported, and the refusal says so at this rung where
       the caps are still in hand. */
    result = xud_physical_record_descriptor_indexing(&device->enabledDescriptorIndexing, &physical->caps);
    if (result != XUD_OK)
        goto fail;
    if ((device->enabledDescriptorIndexing.runtimeDescriptorArray != VK_TRUE)
        || (device->enabledDescriptorIndexing.shaderSampledImageArrayNonUniformIndexing != VK_TRUE)
        || (device->enabledDescriptorIndexing.descriptorBindingPartiallyBound != VK_TRUE)
        || (device->enabledDescriptorIndexing.descriptorBindingSampledImageUpdateAfterBind != VK_TRUE)) {
        result = XUD_UNSUPPORTED; /* no bindless sampler array on this device, and every context
                                      shape above the descriptor rung is a bindless context */
        goto fail;
    }

    result = xud_device_fill_queue_create_info(&device->queueRows[0], device->queuePriorities, &physical->caps);
    if (result != XUD_OK)
        goto fail;

    /* The transfer row is filled either way; whether it JOINS the create-info is the count's deci
       sion below, because a second row naming the same family as row 0 is the shape the validatio
       n layer refuses. When the caps' transfer family IS the graphics family the device rows one
       family and the transfer role resolves from row 0's queues - the fallback the physical layer
       chose, carried through untouched. */
    result = xud_device_fill_transfer_queue_create_info(&device->queueRows[1], device->transferQueuePriorities, &physical->caps);
    if (result != XUD_OK)
        goto fail;
    device->queueRowCount = (physical->caps.transferQueueFamilyIndex != physical->caps.queueFamilyIndex) ? 2u : 1u;

    result = xud_device_fill_create_info(&createInfo, nameSlots, device);
    if (result != XUD_OK)
        goto fail;

    vk = loader->instanceTable.vkCreateDevice(physical->physicalDevice, &createInfo, NULL, &device->device);
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

    /* The device-level table, filled in bulk now that the device exists: volkLoadDeviceTable res
       olves every row through vkGetDeviceProcAddr(device, name), so a row can only name code the
       device this layer just created backs. It is filled before the three queue lookups below and
       before anything can call xud_device_wait_idle()/xud_device_close() on this device, which i
       s what makes xud_loader_device_bound() the invariant those two rungs check. The row was ch
       ecked non-NULL above, so the bind cannot refuse here; if it ever does, the device leaks a
       nd the reason travels - the fail ladder destroys through a row of this very table. */
    result = xud_loader_bind_device(loader, device->device);
    if (result != XUD_OK) {
        result = XUD_INTERNAL_ERROR;
        goto fail;
    }
    if ((loader->deviceTable.vkGetDeviceQueue == NULL) || (loader->deviceTable.vkDeviceWaitIdle == NULL)
        || (loader->deviceTable.vkDestroyDevice == NULL)) {
        result = XUD_UNSUPPORTED;
        goto fail;
    }

    /* The three role queues, resolved once here rather than on every frame: a queue handle lives a
       s long as the device that owns it does, and there is no vkDestroyQueue, so there is nothing
       above this layer can do with a re-resolve. The family index is the row the role belongs to -
       row 0 for graphics and present, row 1 for the transfer role when the driver rowed a dedicat
       ed transfer family and row 0 when it did not - and the queue index is the role clamped into
       the count that row asked for: the two rules the validation layer's queue VUIDs state, satis
       fied by construction rather than by hoping. */
    for (role = 0u; role < XUD_DEVICE_QUEUE_ROLE_COUNT; role++) {
        XudQueue* record = (role == XUD_QUEUE_ROLE_PRESENT) ? &device->presentQueue
            : (role == XUD_QUEUE_ROLE_TRANSFER)             ? &device->transferQueue
                                                            : &device->graphicsQueue;
        const VkDeviceQueueCreateInfo* row
            = (role == XUD_QUEUE_ROLE_TRANSFER && device->queueRowCount == 2u) ? &device->queueRows[1] : &device->queueRows[0];

        queueIndex = (role == XUD_QUEUE_ROLE_TRANSFER) ? 0u : ((role < row->queueCount) ? role : (row->queueCount - 1u));
        record->queue = VK_NULL_HANDLE; /* the zero state before the call: PFN_vkGetDeviceQueue returns void
                                           in these headers (vulkan_core.h:4547), so there is no VkResult to
                                           map and the out handle is the only answer available */
        loader->deviceTable.vkGetDeviceQueue(device->device, row->queueFamilyIndex, queueIndex, &record->queue);
        if (record->queue == VK_NULL_HANDLE) {
            /* A driver that answers nothing for a family/index pair the row itself asked for: the
               caps and the driver disagree, and a device handed out with no queue in it would be a
               device nothing above can submit to. Refused here, and the fail ladder below destroys
               the device this step made. */
            result = XUD_VULKAN_ERROR;
            goto fail;
        }
        record->queueFamilyIndex = row->queueFamilyIndex;
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
    if ((device != NULL) && (device->device != VK_NULL_HANDLE) && (loader->deviceTable.vkDestroyDevice != NULL)) {
        (void)loader->deviceTable.vkDestroyDevice(device->device, NULL);
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
    if (device->device == VK_NULL_HANDLE)
        return XUD_ALREADY; /* nothing is in flight on a device that does not exist, which i
                               s already the state asked for - and this answer needs no table, w
                               hich is why it comes before the bound check */
    if (!xud_loader_device_bound(loader, device->device))
        return XUD_UNSUPPORTED; /* the device-level rows are the loader's, and an unbound tabl
                                   e means they were never resolved for any device - a nil row i
                                   s reported, never called through, the rule every call site i
                                   n this tree follows */
    if (loader->deviceTable.vkDeviceWaitIdle == NULL)
        return XUD_UNSUPPORTED; /* a nil PFN is reported, never called through - the rule every call
                                   site in this tree follows */

    vk = loader->deviceTable.vkDeviceWaitIdle(device->device);
    return xud_result_from_vk(vk);
}

XudResult xud_device_close(XudDevice** device, const XudLoader* loader) {
    XudDevice* victim = NULL;
    XudResult guard = XUD_OK;

    if ((device == NULL) || (loader == NULL))
        return XUD_INVALID_ARG; /* the arguments are checked before the state, the rule
                                   xud_physical_close() follows */
    if (*device == NULL)
        return XUD_ALREADY;

    victim = *device;

    /* The guard first, and it is this layer's own guarantee rather than a promise about the cal
       ler above: the spec requires that nothing is in flight when a device is destroyed, and a
       caller that had queued submissions without submitting them would otherwise be racing the
       m. A refusal here (a nil row) leaves the device alive and the struct un-freed - refusing i
       s the honest answer, leaking a handle silently is not. XUD_ALREADY (the struct has no han
       dle) and XUD_LOST (the driver says the device is gone) both continue below - a lost device
       is still destroyed, vkDestroyDevice is this layer's teardown call and skipping it would l
       eak an object the driver may still hold. */
    guard = xud_device_wait_idle(victim, loader);
    if (guard == XUD_UNSUPPORTED)
        return guard;

    if (victim->device != VK_NULL_HANDLE) {
        if (!xud_loader_device_bound(loader, victim->device))
            return XUD_UNSUPPORTED; /* no rows resolved for this device, so there is no teardow
                                      n row to call through and the handle would leak silently */
        if (loader->deviceTable.vkDestroyDevice == NULL)
            return XUD_UNSUPPORTED; /* the handle would leak silently, so this rung refuses instead
                                     */

        /* PFN_vkDestroyDevice returns void in these headers (vulkan_core.h:4542) - there is no
           VkResult for xud_result_from_vk to map here, so the destroy is a call whose answer is not
           available and the layer's answer is the guard's. The handle is cleared after the call so
           a struct that survi ves this (it does not, it is freed below) cannot be torn down twice.
         */
        loader->deviceTable.vkDestroyDevice(victim->device, NULL);
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
