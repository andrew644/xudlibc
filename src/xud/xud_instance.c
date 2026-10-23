/***********************************************************************************************
 *
 *   xud_instance.c - the Vulkan instance layer: one VkInstance per context
 *
 *   Owns : instance bring-up. The pure parts (application info, extension selection, create-info
 *          assembly) are separate functions so the headless test run can cover them with no
 *          driver present; the driver parts are vkEnumerateInstanceExtensionProperties, vkCreate
 *          Instance and vkDestroyInstance, every one of them called through a PFN slot of the
 *          loader's table - this file never names a vk* function directly. The one exception is
 *          vkDestroyDebugUtilsMessengerEXT, which teardown calls through xud_loader_resolve() into
 *          a local: the diagnostics layer's messenger must die through the VkInstance that owns it,
 *          while that instance is still alive, and a resolve cached in a table slot would be stale
 *          across an instance recreation (see xud_loader.h for the same rule).
 *   Layer: above the loader, below the physical device. Verified against lavvk here (2026-10-16):
 *          26 instance-level extensions advertised, all three wanted ones present, and vkCreate
 *          Instance at apiVersion 1.0 with those three enabled returns VK_SUCCESS.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud_instance.h"
#include "xud.h"
#include "xud_loader.h"
#include "xud_vulkan.h"

#include <assert.h> /* Required for: static_assert, the compile-time checks below */
#include <stdlib.h> /* Required for: calloc, free */
#include <string.h> /* Required for: memcpy, memcpy-based comparison (xud_name_equal) */

/* The slots this layer binds through xud_loader_bind() must be pointer-sized, the same rule
   xud_loader.c checks for the global-level slots (xud_pfn_copy moves one word). */
static_assert(sizeof(PFN_vkCreateInstance) == sizeof(void*), "PFN slot must be pointer-sized");
static_assert(sizeof(PFN_vkDestroyInstance) == sizeof(void*), "PFN slot must be pointer-sized");
static_assert(sizeof(PFN_vkEnumerateInstanceExtensionProperties) == sizeof(void*),
    "PFN slot must be pointer-sized");
static_assert(sizeof(PFN_vkDestroyDebugUtilsMessengerEXT) == sizeof(void*),
    "resolved messenger teardown must fit one word");

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
static const char* const XUD_INSTANCE_WANTED_EXTENSIONS[] = {
    VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    VK_KHR_SURFACE_EXTENSION_NAME,
    VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
};
#define XUD_INSTANCE_WANTED_COUNT                                                                  \
    ((uint32_t)(sizeof(XUD_INSTANCE_WANTED_EXTENSIONS) / sizeof(XUD_INSTANCE_WANTED_EXTENSIONS[0])))

static_assert(XUD_INSTANCE_WANTED_COUNT <= XUD_MAX_INSTANCE_EXTENSIONS,
    "the wanted list must fit the instance struct");

/* Both wanted lists share the instance's fixed storage, so the union has to fit - the diagnostics
   layer's list (VK_EXT_debug_utils, VK_EXT_validation_features) is documented in xud_diagnostics.c,
   this layer's own list is the one just above. The size of the diagnostics list is that layer's own
   macro, XUD_DIAGNOSTICS_WANTED_MAX, seen through the include above: there is no second macro here
   for the same number, so the arrays below and the accessor's out-array cannot drift apart, and
   these two asserts are what keeps the union inside XUD_MAX_INSTANCE_EXTENSIONS. They also sit
   below the #define: XUD_INSTANCE_WANTED_COUNT is a macro, and a static_assert that mentions it
   before the define sees an undeclared identifier inside a constant expression, which is two
   errors, not one (verified with gcc 16.2.1 under the project flags: "XUD_INSTANCE_WANTED_COUNT
   undeclared here" plus "expression in static assertion is not an integer"). */
static_assert(XUD_DIAGNOSTICS_WANTED_MAX <= XUD_MAX_INSTANCE_EXTENSIONS,
    "the diagnostics wanted list must fit the instance struct");
static_assert(XUD_INSTANCE_WANTED_COUNT + XUD_DIAGNOSTICS_WANTED_MAX <= XUD_MAX_INSTANCE_EXTENSIONS,
    "both wanted lists must fit the instance struct");

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
static XudResult xud_instance_query_extensions(
    const XudLoader* loader, VkExtensionProperties** outProperties, uint32_t* outCount) {
    VkExtensionProperties* items = NULL;
    uint32_t capacity = XUD_INSTANCE_QUERY_MIN_CAPACITY;
    uint32_t count = 0u;
    VkResult vk;

    if ((loader == NULL) || (outProperties == NULL) || (outCount == NULL))
        return XUD_INVALID_ARG;
    if (loader->enumerateInstanceExtensionProperties == NULL)
        return XUD_UNSUPPORTED;

    for (;;) {
        items = calloc(capacity, sizeof(VkExtensionProperties));
        if (items == NULL)
            return XUD_OUT_OF_MEMORY;

        count = capacity;
        vk = loader->enumerateInstanceExtensionProperties(NULL, &count, items);

        if ((vk == VK_SUCCESS) && (count <= capacity)) {
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
            return xud_result_from_vk(vk); /* a real failure: mapped, not retried */
        if (capacity >= XUD_INSTANCE_QUERY_MAX_CAPACITY)
            return XUD_VULKAN_ERROR; /* the list is longer than the bound we allow */
        capacity *= 2u;
    }
}

XudResult xud_instance_fill_application_info(VkApplicationInfo* outApplicationInfo) {
    if (outApplicationInfo == NULL)
        return XUD_INVALID_ARG;

    outApplicationInfo->sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    outApplicationInfo->pNext = NULL;
    outApplicationInfo->pApplicationName = XUD_INSTANCE_NAME;
    outApplicationInfo->pEngineName = XUD_INSTANCE_NAME;

    /* One version for both slots: this library is the engine and the application. The Vulkan
       header macro composes the same three numbers xud.h publishes. */
    outApplicationInfo->applicationVersion
        = VK_MAKE_API_VERSION(0u, XUD_VERSION_MAJOR, XUD_VERSION_MINOR, XUD_VERSION_PATCH);
    outApplicationInfo->engineVersion = outApplicationInfo->applicationVersion;
    outApplicationInfo->apiVersion = XUD_INSTANCE_API_FLOOR;
    return XUD_OK;
}

/* Internal: append one extension name to the instance's enabled list, but only when the driver
   advertised it and it is not already there. Nothing is ever forced on: a name that was not
   advertised simply stays out of the list, and the feature that needed it fails later with
   XUD_UNSUPPORTED. The duplicate check is what lets the diagnostics layer's list repeat a name
   this layer also wants - the two wanted lists are owned by two different files and neither may
   assume what the other contains. */
static XudResult xud_instance_enable_if_advertised(XudInstance* outInstance,
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

    for (enabledIndex = 0u; enabledIndex < outInstance->enabledExtensionCount; enabledIndex++) {
        if (xud_name_equal(outInstance->enabledExtensionNameStorage[enabledIndex], name)) {
            *outAppended = false;
            return XUD_OK; /* already in the list */
        }
    }
    if (outInstance->enabledExtensionCount >= XUD_MAX_INSTANCE_EXTENSIONS)
        return XUD_OUT_OF_RESOURCES;

    if (xud_name_copy(outInstance->enabledExtensionNameStorage[outInstance->enabledExtensionCount],
            VK_MAX_EXTENSION_NAME_SIZE, name)
        != XUD_OK)
        return XUD_INTERNAL_ERROR; /* a driver name longer than the buffer: reported, not truncated
                                    */

    outInstance->enabledExtensionCount += 1u;
    *outAppended = true;
    return XUD_OK;
}

XudResult xud_instance_select_extensions(XudInstance* outInstance,
    const VkExtensionProperties* advertised, uint32_t advertisedCount,
    const char* const extraNames[XUD_DIAGNOSTICS_WANTED_MAX], uint32_t extraCount) {
    uint32_t wantedIndex = 0u;
    uint32_t extraIndex = 0u;
    XudResult result = XUD_INVALID_ARG;
    bool appended = false;

    if ((outInstance == NULL) || ((advertised == NULL) && (advertisedCount != 0u))
        || ((extraNames == NULL) && (extraCount != 0u)))
        return XUD_INVALID_ARG;

    /* The selection is rewritten on every call, so a reused struct cannot keep a stale list. */
    outInstance->enabledExtensionCount = 0u;

    for (wantedIndex = 0u; wantedIndex < XUD_INSTANCE_WANTED_COUNT; wantedIndex++) {
        result = xud_instance_enable_if_advertised(outInstance, advertised, advertisedCount,
            XUD_INSTANCE_WANTED_EXTENSIONS[wantedIndex], &appended);
        if (result != XUD_OK) {
            outInstance->enabledExtensionCount = 0u;
            return result;
        }
    }
    for (extraIndex = 0u; extraIndex < extraCount; extraIndex++) {
        result = xud_instance_enable_if_advertised(
            outInstance, advertised, advertisedCount, extraNames[extraIndex], &appended);
        if (result != XUD_OK) {
            outInstance->enabledExtensionCount = 0u;
            return result;
        }
    }
    return XUD_OK;
}

/* Internal: is every name in `names` enabled in this instance? This is what decides whether the
diagnostics layer's validation-feature request may hang off VkInstanceCreateInfo::pNext at all:
a pNext chain may carry a struct for an *enabled* extension and for nothing else, and VK_EXT_valid
ation_features is the extension that gates VkValidationFeaturesEXT. It is NOT advertised here
(verified 2026-10-16: 26 instance-level extensions from this loader, VK_EXT_debug_utils among them,
VK_EXT_validation_features not - with or without VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation in
the environment), so on this machine the chain stays empty and the validation layer runs on its own
defaults. Checking the whole diagnostics wanted set instead of only that one name is the
conservative form of the same check: it can leave a legal request out, it can never put an illegal
one on the chain, and it keeps this file free of extension names it does not own. */
static bool xud_instance_all_names_enabled(const XudInstance* instance,
    const char* const names[XUD_DIAGNOSTICS_WANTED_MAX], uint32_t count) {
    uint32_t wanted = 0u;
    uint32_t row = 0u;
    bool found = false;

    if ((names == NULL) && (count != 0u))
        return false;
    if (instance == NULL)
        return false;

    for (wanted = 0u; wanted < count; wanted++) {
        found = false;
        for (row = 0u; row < instance->enabledExtensionCount; row++)
            if (xud_name_equal(names[wanted], instance->enabledExtensionNameStorage[row]))
                found = true;
        if (!found)
            return false;
    }
    return true;
}

XudResult xud_instance_fill_create_info(const XudInstance* instance,
    const char* nameSlots[XUD_MAX_INSTANCE_EXTENSIONS], const XudDiagnostics* diagnostics,
    VkInstanceCreateInfo* outCreateInfo) {
    uint32_t index = 0u;
    const char* diagnosticsWanted[XUD_DIAGNOSTICS_WANTED_MAX] = { NULL };
    uint32_t diagnosticsWantedCount = 0u;
    XudResult result = XUD_INVALID_ARG;

    if ((instance == NULL) || (nameSlots == NULL) || (outCreateInfo == NULL))
        return XUD_INVALID_ARG;
    if ((diagnostics != NULL) && (diagnostics->enabledLayerCount > XUD_MAX_DIAGNOSTIC_LAYERS))
        return XUD_INVALID_ARG; /* the layer count does not fit the storage it came from */

    outCreateInfo->sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    outCreateInfo->flags = 0; /* see the wanted-list comment: the portability flag is not set */
    outCreateInfo->pApplicationInfo = &instance->applicationInfo;

    /* The diagnostics layer's validation-feature request hangs off pNext, but only when the
       extensions that gate it are enabled - see xud_instance_all_names_enabled above, which is
       that check and says why (VK_EXT_validation_features is not advertised by this driver, so
       the chain stays empty here). A release build (no diagnostics struct) has no pNext, no
       validation request, and no layer at all.
       What is deliberately NOT on this chain: VkDebugUtilsMessengerCreateInfoEXT. Probed twice
       here (2026-10-16): with this loader (1.4.357) plus the validation layer, vkCreateInstance
       segfaults when a messenger create-info hangs off pNext, while the standalone
       vkCreateDebugUtilsMessengerEXT/vkDestroyDebugUtilsMessengerEXT pair through a live instance
       works cleanly. Do not re-attempt the chain without re-probing. */
    outCreateInfo->pNext = NULL;
    if (diagnostics != NULL) {
        result = xud_diagnostics_wanted_extensions(diagnosticsWanted, &diagnosticsWantedCount);
        if (result != XUD_OK)
            return result;
        if (xud_instance_all_names_enabled(instance, diagnosticsWanted, diagnosticsWantedCount))
            outCreateInfo->pNext = &diagnostics->validationFeatures;
    }

    /* The validation layers are the diagnostics step's answer: it enumerated what the loader
       advertises and picked what is advertised, never a name this layer knows. The pointer array
       lives in the diagnostics struct, so it outlives the vkCreateInstance call the caller makes,
       exactly like the extension-name slots below. No diagnostics struct (a release build) means
       no layer requested at all: 0 + NULL, which is the "no layers" shape. */
    if (diagnostics != NULL) {
        outCreateInfo->enabledLayerCount = diagnostics->enabledLayerCount;
        outCreateInfo->ppEnabledLayerNames
            = (diagnostics->enabledLayerCount != 0u) ? diagnostics->enabledLayerNamePointers : NULL;
    } else {
        outCreateInfo->enabledLayerCount = 0u;
        outCreateInfo->ppEnabledLayerNames = NULL;
    }

    for (index = 0u; index < instance->enabledExtensionCount; index++)
        nameSlots[index] = instance->enabledExtensionNameStorage[index];
    for (; index < XUD_MAX_INSTANCE_EXTENSIONS; index++)
        nameSlots[index] = NULL; /* the tail is always NULLed so a stale pointer cannot survive */

    outCreateInfo->enabledExtensionCount = instance->enabledExtensionCount;
    outCreateInfo->ppEnabledExtensionNames
        = (instance->enabledExtensionCount != 0u) ? nameSlots : NULL;
    return XUD_OK;
}

XudResult xud_instance_create(
    XudInstance** outInstance, XudLoader* loader, XudDiagnostics* diagnostics) {
    XudInstance* instance = NULL;
    VkExtensionProperties* advertised = NULL;
    VkInstanceCreateInfo createInfo;
    const char* nameSlots[XUD_MAX_INSTANCE_EXTENSIONS];
    const char* diagnosticsWanted[XUD_DIAGNOSTICS_WANTED_MAX] = { NULL };
    XudResult result = XUD_INVALID_ARG;
    uint32_t advertisedCount = 0u;
    uint32_t diagnosticsWantedCount = 0u;
    VkResult vk;

    if ((outInstance == NULL) || (*outInstance != NULL) || (loader == NULL)
        || (!xud_loader_is_open(loader)))
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

    /* The diagnostics layer owns its wanted-extension list; this layer asks for it through the
       accessor, which is the one mechanism there is. An `extern const char *const XUD_INSTANCE
       _DIAGNOSTICS_WANTED_EXTENSIONS[]` for the same list was deleted from xud_instance.h rather
       than left unused: a declaration nothing defines compiles and fails at link time, and the
       accessor is the one that has a definition (see the comment over the two lists there). A
       release build with no diagnostics struct adds nothing to the wanted set: the debug-utils
       /validation-feature pair is a debug-only request, and with no diagnostics struct there is
       no messenger to ask for and no validation request to carry, so the pair stays out of the
       enabled list. */
    if (diagnostics != NULL) {
        result = xud_diagnostics_wanted_extensions(diagnosticsWanted, &diagnosticsWantedCount);
        if (result != XUD_OK)
            goto fail;
    }

    result = xud_instance_select_extensions(
        instance, advertised, advertisedCount, diagnosticsWanted, diagnosticsWantedCount);
    if (result != XUD_OK)
        goto fail;

    result = xud_instance_fill_create_info(instance, nameSlots, diagnostics, &createInfo);
    if (result != XUD_OK)
        goto fail;

    /* vkCreateInstance is a global-level command: the loader answers for it with no handle in
       hand, so binding it into the table is the same shape as loader bring-up used. */
    result = xud_loader_bind(loader, VK_NULL_HANDLE, "vkCreateInstance", &loader->createInstance);
    if (result != XUD_OK)
        goto fail;
    if (loader->createInstance == NULL) {
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
       and xud_instance_destroy() clears the handle before freeing the struct. NULL goes in the
       diagnostics slot on purpose: the diagnostics struct belongs to the context, not to this
       layer, and no messenger can exist here - creating one needs a live VkInstance, which is
       exactly what failed. */
    if (instance != NULL)
        (void)xud_instance_destroy(&instance, loader, NULL);
    if (advertised != NULL)
        free(advertised);
    return (result != XUD_OK) ? result : XUD_INTERNAL_ERROR;
}

/* Internal: destroy the diagnostics layer's messenger through a VkInstance that is still alive,
   then clear the struct's own handles so a later xud_diagnostics_close() frees the struct exactly
   once and never calls through a messenger that is gone. The entry point is resolved here, into a
   local, rather than cached in a slot of the loader's table: vkGetInstanceProcAddr answers NULL for
   it unless VK_EXT_debug_utils is enabled on the instance in hand, and a resolve cached across an
   instance recreation would be a stale pointer into a layer that is gone (xud_loader.h says the
   same for the diagnostics layer). */
static XudResult xud_instance_close_messenger(
    const XudLoader* loader, VkInstance instance, XudDiagnostics* diagnostics) {
    XudPfnBits bits;
    XudResult result;

    result = xud_loader_resolve(loader, instance, "vkDestroyDebugUtilsMessengerEXT", &bits);
    if (result != XUD_OK)
        return result;
    if (bits.function == NULL)
        return XUD_INTERNAL_ERROR;

    ((PFN_vkDestroyDebugUtilsMessengerEXT)bits.function)(instance, diagnostics->messenger, NULL);
    diagnostics->messenger = VK_NULL_HANDLE;
    diagnostics->instance = VK_NULL_HANDLE;
    return XUD_OK;
}

XudResult xud_instance_destroy(
    XudInstance** instance, XudLoader* loader, XudDiagnostics* diagnostics) {
    XudResult result;

    if ((instance == NULL) || (loader == NULL) || (!xud_loader_is_open(loader)))
        return XUD_INVALID_ARG;
    if (*instance == NULL)
        return XUD_ALREADY;

    /* Messenger first, and through the instance that is about to go away:
       vkDestroyDebugUtilsMessengerEXT takes the owning VkInstance and the extension requires that
       instance to still be alive, so this call must precede the vkDestroyInstance below. The struct
       itself is not freed here - this layer only has a copy of that pointer, and
       xud_close_context() frees it through xud_diagnostics_close() once the messenger is gone;
       freeing it here would leave the caller holding a dangling pointer. A messenger that belongs
       to a different VkInstance than the one being destroyed is a misuse: destroying it through
       this one would be a lie, so it is rejected before anything is torn down. When the messenger
       cannot be destroyed the instance is left alive and the failure is returned - the messenger
       still references it, so tearing the instance down anyway would be a lie too. */
    if ((diagnostics != NULL) && (diagnostics->messenger != VK_NULL_HANDLE)) {
        if (diagnostics->instance != (*instance)->instance)
            return XUD_INVALID_ARG;
        result = xud_instance_close_messenger(loader, (*instance)->instance, diagnostics);
        if (result != XUD_OK)
            return result;
    }

    if ((*instance)->instance == VK_NULL_HANDLE) {
        free(*instance);
        *instance = NULL;
        return XUD_OK;
    }

    /* vkDestroyInstance is an instance-level command: vkGetInstanceProcAddr answers NULL for it
       while no instance exists (verified here), so it is resolved here, through the instance
       being freed, and cached in the loader's table for the next context. If the loader cannot
       resolve it the instance leaks and that is returned - a loader that cannot destroy what it
       created is broken, and this layer does not paper over that. */
    if (loader->destroyInstance == NULL) {
        result = xud_loader_bind(
            loader, (*instance)->instance, "vkDestroyInstance", &loader->destroyInstance);
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
