/***********************************************************************************************
 *
 *   xud_instance.h - the Vulkan instance layer: one VkInstance per context
 *
 *   Owns : the XudInstance type (the handle plus the create-info that produced it), the pure
 *          parts of instance bring-up (application info, extension selection, create-info
 *          assembly), and the two driver calls that create/destroy the instance.
 *   Layer: above the loader - every Vulkan call here goes through a PFN slot of XudLoader -
 *          and below the physical device (which resolves through the instance this layer
 *          fills) and the swapchain. Sees <vulkan/vulkan.h> for declarations only; no Vulkan
 *          symbol is linked (AGENTS.md: there is no -lvulkan anywhere in the build).
 *          Internal like xud_loader.h: tests may include it, user code must not.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#ifndef XUD_XUD_INSTANCE_H_
#define XUD_XUD_INSTANCE_H_

#include <assert.h>
#include "xud.h"
#include "xud_loader.h"
#include "xud_vulkan.h"
#include <vulkan/vulkan.h>

/* The API floor every xudlibc instance asks for. The library minimum is Vulkan 1.0 +
   VK_KHR_surface (AGENTS.md, settled decisions): lavvk here reports Vulkan 1.4.354, so the
   higher paths exist, but the library must still work at the floor, and the *2 queries and
   the surface creation that the rest of the layers need are only reachable at the floor as
   instance-level extensions - which is exactly what the extension selection below enables. */
#define XUD_INSTANCE_API_FLOOR VK_API_VERSION_1_0

/* pApplicationName and pEngineName of every instance this library creates. */
#define XUD_INSTANCE_NAME "xudlibc"

/* Instance-level extensions enabled at once, and the bounds of the advertised-list query
   (vkEnumerateInstanceExtensionProperties has no count-only form: see xud_instance.c). */
#define XUD_MAX_INSTANCE_EXTENSIONS 8u
#define XUD_INSTANCE_QUERY_MIN_CAPACITY 16u
#define XUD_INSTANCE_QUERY_MAX_CAPACITY 512u

typedef struct XudInstance {
    VkInstance instance;                                                 /* VK_NULL_HANDLE until xud_instance_create() fills it */
    VkApplicationInfo applicationInfo;                                   /* what was asked for: names, versions, apiVersion floor */
    uint32_t enabledExtensionCount;                                      /* how many rows of enabledExtensionNameStorage are live */
    char enabledExtensionNameStorage[XUD_MAX_INSTANCE_EXTENSIONS][VK_MAX_EXTENSION_NAME_SIZE]; /* the names, copied into fixed buffers */
} XudInstance;

/// The VkApplicationInfo every xudlibc instance carries: XUD_INSTANCE_NAME in both name
/// slots, the library version in both version slots, XUD_INSTANCE_API_FLOOR as the floor.
/// Pure - no driver call - so tests assert the floor and the names with no device present.
/// XUD_INVALID_ARG on a NULL out-param.
XudResult xud_instance_fill_application_info(VkApplicationInfo *outApplicationInfo);

/// Choose which of the advertised instance-level extensions to enable: the ones xudlibc
/// wants that the driver actually advertised, in wanted order (the wanted list itself lives
/// in xud_instance.c, documented there). Nothing is required - an extension that was not
/// advertised simply stays out of the list, and the feature that needed it fails later with
/// XUD_UNSUPPORTED.
/// Pure: `advertised` is just an array, so tests feed a synthetic one and the selection is
/// covered with no device present. Rewrites enabledExtensionCount/Storage on every call,
/// including when nothing matches. XUD_INVALID_ARG when `advertised` is NULL while
/// advertisedCount is not 0.
XudResult xud_instance_select_extensions(XudInstance *outInstance, const VkExtensionProperties *advertised, uint32_t advertisedCount);

/// Assemble the VkInstanceCreateInfo for an XudInstance: sType, pNext NULL, flags 0
/// (VK_INSTANCE_CREATE_FLAG_PORTABILITY_BIT is not set here - the extension is requested
/// explicitly and the flag only changes what a driver is allowed to *hide*, see the comment
/// in xud_instance.c), pApplicationInfo -> &instance->applicationInfo, zero enabled layers
/// (the validation layers are added there by the diagnostics step), and the selected
/// extension names.
/// `nameSlots` is a caller-provided array of XUD_MAX_INSTANCE_EXTENSIONS slots: it is filled
/// with pointers into instance->enabledExtensionNameStorage and outCreateInfo->ppEnabledExtensionNames
/// points at it, so both must outlive the vkCreateInstance call.
/// Pure. XUD_INVALID_ARG on a NULL instance, NULL nameSlots array, or NULL out-param.
XudResult xud_instance_fill_create_info(const XudInstance *instance, const char *nameSlots[XUD_MAX_INSTANCE_EXTENSIONS], VkInstanceCreateInfo *outCreateInfo);

/// Create the instance. `loader` must be open (xud_loader_open filled the global-level slots
/// this needs). On XUD_OK `outInstance` owns a live VkInstance plus the create-info that
/// produced it; on any error it stays NULL and everything created so far is destroyed, in
/// reverse creation order. XUD_INVALID_ARG on a NULL out-pointer, an out-pointer that already
/// owns an instance (nothing is created twice into the same slot), or a missing/unopen loader.
/// Driver calls, in order: vkEnumerateInstanceExtensionProperties (twice at least - pCount is
/// input/output, see xud_instance_query_extensions), vkCreateInstance through the slot the
/// loader table binds here. vkDestroyInstance is *not* resolved here: it is an instance-level
/// command, vkGetInstanceProcAddr answers NULL for it without a live handle (verified here),
/// and a failure to resolve it after a successful create would leave an instance nothing could
/// free - so it is resolved by xud_instance_destroy(), through the instance it frees.
XudResult xud_instance_create(XudInstance **outInstance, XudLoader *loader);

/// Destroy the instance and clear the out pointer. The arguments are checked before the state:
/// XUD_INVALID_ARG when the pointer itself is NULL or the loader is missing/not open, and
/// XUD_ALREADY only when those are sound and the instance they point at is already NULL - a
/// second destroy is reported, never a double free.
/// vkDestroyInstance is resolved on demand through the live instance and cached in the
/// loader's table, so a context that never draws still tears down without ever having
/// resolved it at create time. If the loader cannot resolve it, the instance leaks and the
/// failure is returned - that is a broken loader, not something this layer papers over.
XudResult xud_instance_destroy(XudInstance **instance, XudLoader *loader);

#endif /* XUD_XUD_INSTANCE_H_ */
