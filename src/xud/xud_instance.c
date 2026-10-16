/***********************************************************************************************
 *
 *   xud_instance.c - the Vulkan instance layer: one VkInstance per context
 *
 *   Owns : instance bring-up. The pure parts (application info, extension selection, create-info
 *          assembly) are separate functions so the headless test run can cover them with no
 *          driver present; the driver parts are vkEnumerateInstanceExtensionProperties, vkCreate
 *          Instance and vkDestroyInstance, every one of them called through a PFN slot of the
 *          loader's table - this file never names a vk* function directly.
 *   Layer: above the loader, below the physical device. Verified against lavvk here (2026-10-16):
 *          26 instance-level extensions advertised, all three wanted ones present, and vkCreate
 *          Instance at apiVersion 1.0 with those three enabled returns VK_SUCCESS.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud.h"
#include "xud_loader.h"
#include "xud_vulkan.h"
#include "xud_instance.h"

#include <assert.h> /* Required for: static_assert, the compile-time checks below */
#include <stdlib.h> /* Required for: calloc, free */
#include <string.h> /* Required for: memcpy, memcpy-based comparison (xud_name_equal) */

/* The slots this layer binds through xud_loader_bind() must be pointer-sized, the same rule
   xud_loader.c checks for the global-level slots (xud_pfn_copy moves one word). */
static_assert(sizeof(PFN_vkCreateInstance) == sizeof(void *), "PFN slot must be pointer-sized");
static_assert(sizeof(PFN_vkDestroyInstance) == sizeof(void *), "PFN slot must be pointer-sized");
static_assert(sizeof(PFN_vkEnumerateInstanceExtensionProperties) == sizeof(void *), "PFN slot must be pointer-sized");

/* The instance-level extensions xudlibc asks for, in the order it asks. None is required: each
   is checked against what the driver advertised before it goes into VkInstanceCreateInfo, an
   absent one simply stays out of the list, and the feature that needed it fails later with
   XUD_UNSUPPORTED.
   - VK_KHR_get_physical_device_properties2: the *2 queries (vkGetPhysicalDeviceFeatures2,
     vkGetPhysicalDeviceProperties2, vkGetPhysicalDeviceMemoryProperties2) are core in Vulkan 1.1
     and only reachable as an extension at the 1.0 floor this library asks for; the device layer
     records all three of them, and the *2 forms are the only ones that chain pNext cleanly.
   - VK_KHR_surface: vkCreateSurfaceKHR and the surface queries GLFW's Vulkan path uses (it hands
     us a VkSurfaceKHR, and vkDestroySurfaceKHR is ours). Core in 1.1, an extension at the floor.
   - VK_KHR_portability_enumeration: without it a driver may hide the portability-gated *device*
     extensions, so the caps recorded at the device layer would be a lie. Requesting it is the
     honest thing to do; VK_INSTANCE_CREATE_FLAG_PORTABILITY_BIT is deliberately not set, because
     that flag only tells the driver "you may hide these", which is the opposite of what this
     library wants.
   Verified advertised here (lavvk, 2026-10-16): all three, out of 26 instance-level extensions. */
static const char *const XUD_INSTANCE_WANTED_EXTENSIONS[] = {
    VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    VK_KHR_SURFACE_EXTENSION_NAME,
    VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
};
#define XUD_INSTANCE_WANTED_COUNT \
    ((uint32_t)(sizeof(XUD_INSTANCE_WANTED_EXTENSIONS) / sizeof(XUD_INSTANCE_WANTED_EXTENSIONS[0])))

static_assert(XUD_INSTANCE_WANTED_COUNT <= XUD_MAX_INSTANCE_EXTENSIONS, "the wanted list must fit the instance struct");

/* Internal: the instance-level extensions the loader advertises, as a heap array the caller
   frees. vkEnumerateInstanceExtensionProperties has no count-only form: pCount is input/output,
   on entry it is the capacity of the caller's array and on exit the number written, and VK_IN
   COMPLETE means "there were more". Verified here: a zero-capacity call writes 0 and returns
   VK_INCOMPLETE, so the count cannot be read without an array. This asks with
   XUD_INSTANCE_QUERY_MIN_CAPACITY entries and doubles until the driver stops reporting
   VK_INCOMPLETE, up to XUD_INSTANCE_QUERY_MAX_CAPACITY. A driver that reports VK_SUCCESS
   while writing more than the capacity is the same shape - its count is a lie, so the array
   may be short - and retries the same way; success is only ever claimed for a list the driver
   said fits.
   The slot is guaranteed by the loader contract (xud_loader_open fills the global-level slots);
   a NULL slot means the loader is not open, which is XUD_UNSUPPORTED, not a bind. */
static XudResult xud_instance_query_extensions(const XudLoader *loader, VkExtensionProperties **outProperties, uint32_t *outCount)
{
    VkExtensionProperties *items = NULL;
    uint32_t capacity = XUD_INSTANCE_QUERY_MIN_CAPACITY;
    uint32_t count = 0u;
    VkResult vk;

    if ((loader == NULL) || (outProperties == NULL) || (outCount == NULL))
        return XUD_INVALID_ARG;
    if (loader->enumerateInstanceExtensionProperties == NULL)
        return XUD_UNSUPPORTED;

    for (;;)
    {
        items = calloc(capacity, sizeof(VkExtensionProperties));
        if (items == NULL)
            return XUD_OUT_OF_MEMORY;

        count = capacity;
        vk = loader->enumerateInstanceExtensionProperties(NULL, &count, items);

        if ((vk == VK_SUCCESS) && (count <= capacity))
        {
            *outProperties = items;
            *outCount = count;
            return XUD_OK;
        }

        free(items);

        /* Two shapes mean "the list you asked for was cut": VK_INCOMPLETE (the driver says so)
           and VK_SUCCESS with count > capacity (it wrote more than we allowed, so its count is
           a lie and the array may be short). Both retry at a bigger capacity; anything else is
           a real failure, mapped once and never retried. */
        if ((vk != VK_INCOMPLETE) && ((vk != VK_SUCCESS) || (count <= capacity)))
            return xud_result_from_vk(vk);          /* a real failure: mapped, not retried */
        if (capacity >= XUD_INSTANCE_QUERY_MAX_CAPACITY)
            return XUD_VULKAN_ERROR;                /* the list is longer than the bound we allow */
        capacity *= 2u;
    }
}

XudResult xud_instance_fill_application_info(VkApplicationInfo *outApplicationInfo)
{
    if (outApplicationInfo == NULL)
        return XUD_INVALID_ARG;

    outApplicationInfo->sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    outApplicationInfo->pNext = NULL;
    outApplicationInfo->pApplicationName = XUD_INSTANCE_NAME;
    outApplicationInfo->pEngineName = XUD_INSTANCE_NAME;

    /* One version for both slots: this library is the engine and the application. The Vulkan
       header macro composes the same three numbers xud.h publishes. */
    outApplicationInfo->applicationVersion = VK_MAKE_API_VERSION(0u, XUD_VERSION_MAJOR, XUD_VERSION_MINOR, XUD_VERSION_PATCH);
    outApplicationInfo->engineVersion = outApplicationInfo->applicationVersion;
    outApplicationInfo->apiVersion = XUD_INSTANCE_API_FLOOR;
    return XUD_OK;
}

XudResult xud_instance_select_extensions(XudInstance *outInstance, const VkExtensionProperties *advertised, uint32_t advertisedCount)
{
    uint32_t wantedIndex = 0u;
    uint32_t enabledIndex = 0u;

    if (outInstance == NULL)
        return XUD_INVALID_ARG;
    if ((advertised == NULL) && (advertisedCount != 0u))
        return XUD_INVALID_ARG;

    /* The selection is rewritten on every call, so a reused struct cannot keep a stale list. */
    outInstance->enabledExtensionCount = 0u;

    for (wantedIndex = 0u; wantedIndex < XUD_INSTANCE_WANTED_COUNT; wantedIndex++)
    {
        uint32_t advertisedIndex = 0u;
        bool isAdvertised = false;

        for (advertisedIndex = 0u; advertisedIndex < advertisedCount; advertisedIndex++)
        {
            if (xud_name_equal(advertised[advertisedIndex].extensionName, XUD_INSTANCE_WANTED_EXTENSIONS[wantedIndex]))
            {
                isAdvertised = true;
                break;
            }
        }
        if (isAdvertised == false)
            continue;   /* wanted but not there: stays out of the list, nothing is forced on */

        if (xud_name_copy(outInstance->enabledExtensionNameStorage[enabledIndex],
                          VK_MAX_EXTENSION_NAME_SIZE,
                          XUD_INSTANCE_WANTED_EXTENSIONS[wantedIndex]) != XUD_OK)
            return XUD_INTERNAL_ERROR;   /* a driver name longer than the buffer: reported, not truncated */

        enabledIndex++;
    }

    outInstance->enabledExtensionCount = enabledIndex;
    return XUD_OK;
}

XudResult xud_instance_fill_create_info(const XudInstance *instance, const char *nameSlots[XUD_MAX_INSTANCE_EXTENSIONS], VkInstanceCreateInfo *outCreateInfo)
{
    uint32_t index = 0u;

    if ((instance == NULL) || (nameSlots == NULL) || (outCreateInfo == NULL))
        return XUD_INVALID_ARG;

    outCreateInfo->sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    outCreateInfo->pNext = NULL;
    outCreateInfo->flags = 0;   /* see the wanted-list comment: the portability flag is not set */
    outCreateInfo->pApplicationInfo = &instance->applicationInfo;

    /* The validation layers are not here yet: that is the diagnostics step's job, and it
       requests them by enumerating what the loader advertises, never by name. */
    outCreateInfo->enabledLayerCount = 0u;
    outCreateInfo->ppEnabledLayerNames = NULL;

    for (index = 0u; index < instance->enabledExtensionCount; index++)
        nameSlots[index] = instance->enabledExtensionNameStorage[index];
    for (; index < XUD_MAX_INSTANCE_EXTENSIONS; index++)
        nameSlots[index] = NULL;   /* the tail is always NULLed so a stale pointer cannot survive */

    outCreateInfo->enabledExtensionCount = instance->enabledExtensionCount;
    outCreateInfo->ppEnabledExtensionNames = (instance->enabledExtensionCount != 0u) ? nameSlots : NULL;
    return XUD_OK;
}

XudResult xud_instance_create(XudInstance **outInstance, XudLoader *loader)
{
    XudInstance *instance = NULL;
    VkExtensionProperties *advertised = NULL;
    VkInstanceCreateInfo createInfo;
    const char *nameSlots[XUD_MAX_INSTANCE_EXTENSIONS];
    XudResult result = XUD_INVALID_ARG;
    uint32_t advertisedCount = 0u;
    VkResult vk;

    if ((outInstance == NULL) || (*outInstance != NULL) || (loader == NULL) || (!xud_loader_is_open(loader)))
        return XUD_INVALID_ARG;

    instance = calloc(1, sizeof(XudInstance));
    if (instance == NULL)
        return XUD_OUT_OF_MEMORY;
    instance->instance = VK_NULL_HANDLE;

    result = xud_instance_fill_application_info(&instance->applicationInfo);
    if (result != XUD_OK)
        goto fail;

    result = xud_instance_query_extensions(loader, &advertised, &advertisedCount);
    if (result != XUD_OK)
        goto fail;

    result = xud_instance_select_extensions(instance, advertised, advertisedCount);
    if (result != XUD_OK)
        goto fail;

    result = xud_instance_fill_create_info(instance, nameSlots, &createInfo);
    if (result != XUD_OK)
        goto fail;

    /* vkCreateInstance is a global-level command: the loader answers for it with no handle in
       hand, so binding it into the table is the same shape as loader bring-up used. */
    result = xud_loader_bind(loader, VK_NULL_HANDLE, "vkCreateInstance", &loader->createInstance);
    if (result != XUD_OK)
        goto fail;
    if (loader->createInstance == NULL)
    {
        result = XUD_INTERNAL_ERROR;
        goto fail;
    }

    vk = loader->createInstance(&createInfo, NULL, &instance->instance);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK)
        goto fail;

    *outInstance = instance;
    /* The advertised list was a query result: the selection was already copied into the
       instance's own storage, so nothing keeps it past here. */
    free(advertised);
    return XUD_OK;

fail:
    /* Reverse creation order: the only Vulkan object this step can have made is the instance,
       and xud_instance_destroy() clears the handle before freeing the struct. */
    if (instance != NULL)
        (void)xud_instance_destroy(&instance, loader);
    if (advertised != NULL)
        free(advertised);
    return (result != XUD_OK) ? result : XUD_INTERNAL_ERROR;
}

XudResult xud_instance_destroy(XudInstance **instance, XudLoader *loader)
{
    XudResult result;

    if ((instance == NULL) || (loader == NULL) || (!xud_loader_is_open(loader)))
        return XUD_INVALID_ARG;
    if (*instance == NULL)
        return XUD_ALREADY;

    if ((*instance)->instance == VK_NULL_HANDLE)
    {
        free(*instance);
        *instance = NULL;
        return XUD_OK;
    }

    /* vkDestroyInstance is an instance-level command: vkGetInstanceProcAddr answers NULL for it
       while no instance exists (verified here), so it is resolved here, through the instance
       being freed, and cached in the loader's table for the next context. If the loader cannot
       resolve it the instance leaks and that is returned - a loader that cannot destroy what it
       created is broken, and this layer does not paper over that. */
    if (loader->destroyInstance == NULL)
    {
        result = xud_loader_bind(loader, (*instance)->instance, "vkDestroyInstance", &loader->destroyInstance);
        if (result != XUD_OK)
            return result;
    }
    if (loader->destroyInstance == NULL)
        return XUD_INTERNAL_ERROR;

    loader->destroyInstance((*instance)->instance, NULL);
    (*instance)->instance = VK_NULL_HANDLE;
    free(*instance);
    *instance = NULL;
    return XUD_OK;
}
