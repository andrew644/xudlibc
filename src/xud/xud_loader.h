/***********************************************************************************************
 *
 *   xud_loader.h - Vulkan loader bring-up: dlopen handle + resolved entry-point table
 *
 *   Owns : the XudLoader type - one dlopen() handle for the Vulkan ICD loader plus the
 *          internal function table every layer below calls through - and the contract of
 *          the loader layer. Internal module header: user code includes xud.h only.
 *   Layer: loader, above GLFW and below the instance. Sees <vulkan/vulkan.h> for
 *          declarations only (PFN_vk* types, VkResult, VK_NULL_HANDLE); nothing here links
 *          a Vulkan symbol. Verified on this machine: `ldd` shows no libvulkan/libdl
 *          dependency and `nm -u` shows no undefined vk* symbol (see AGENTS.md commands).
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#ifndef XUD_XUD_LOADER_H_
#define XUD_XUD_LOADER_H_

#include "xud.h"
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h> /* Required for: PFN_vk* types, VkResult, VK_NULL_HANDLE - declarations only, never linked */

/* The one shared library the loader layer dlopen()s. No Vulkan symbol is linked at build
   time, so the library still builds and runs on a machine with no driver. */
#define XUD_VULKAN_LIBRARY_SONAME "libvulkan.so.1"

/* Table slots: XudLoader has XUD_LOADER_SLOT_COUNT entry-point fields, of which
   XUD_LOADER_GLOBAL_SLOT_COUNT are resolvable with no handle in hand (global level,
   vkGetInstanceProcAddr(NULL, name)) and are the only ones filled at bring-up step S2. */
#define XUD_LOADER_GLOBAL_SLOT_COUNT 3u
#define XUD_LOADER_SLOT_COUNT 18u

/* Addresses move between the object world (dlsym) and the function world (the PFN_vk*
   slots) by copy, never by cast: ISO C forbids converting between object and function
   pointers, and -Wpedantic -Werror says so. Verified clean under gcc 16.2.1 and clang
   22.1.8; the slot size is checked at compile time in xud_loader.c. */
typedef union XudPfnBits {
    void *object;
    void (*function)(void);
} XudPfnBits;

typedef struct XudLoader {
    void *library;                                        /* dlopen() handle for XUD_VULKAN_LIBRARY_SONAME, NULL = not loaded */
    uint32_t resolvedCount;                               /* how many PFN slots are filled - the "table populated" flag */

    /* Global level: resolvable with no handle in hand, filled at step S2. */
    PFN_vkGetInstanceProcAddr getInstanceProcAddr;        /* the seed: every other slot is resolved through it */
    PFN_vkEnumerateInstanceLayerProperties enumerateInstanceLayerProperties;
    PFN_vkEnumerateInstanceExtensionProperties enumerateInstanceExtensionProperties;

    /* Instance level: NULL until the instance layer (xud_instance.c) has a live VkInstance
       to resolve through - vkGetInstanceProcAddr answers NULL for these without one. */
    PFN_vkCreateInstance createInstance;
    PFN_vkDestroyInstance destroyInstance;
    PFN_vkEnumeratePhysicalDevices enumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceProperties getPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceFeatures2 getPhysicalDeviceFeatures2;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties2 getPhysicalDeviceQueueFamilyProperties2;
    PFN_vkEnumerateDeviceExtensionProperties enumerateDeviceExtensionProperties;

    /* Device level: NULL until the device layer has a live VkDevice to resolve through. */
    PFN_vkCreateDevice createDevice;
    PFN_vkDestroyDevice destroyDevice;
    PFN_vkGetDeviceQueue getDeviceQueue;
    PFN_vkDeviceWaitIdle deviceWaitIdle;

    /* Swapchain/surface teardown: NULL until the swapchain/surface layers create those
       objects (they are created through GLFW's own entry points, see xud_core.c). */
    PFN_vkCreateSwapchainKHR createSwapchainKHR;
    PFN_vkGetSwapchainImagesKHR getSwapchainImagesKHR;
    PFN_vkDestroySwapchainKHR destroySwapchainKHR;
    PFN_vkDestroySurfaceKHR destroySurfaceKHR;
} XudLoader;

/* Loader-layer contract. Every function returns an XudResult; nothing here is ever called
   through a NULL PFN slot - the guard lives in xud_loader_resolve. */

/// Opens the loader: dlopen(XUD_VULKAN_LIBRARY_SONAME), resolve the seed entry point with
/// dlsym, then resolve the global-level entry points through the seed. XUD_UNSUPPORTED if
/// the loader is absent or does not answer - that is the honest "no driver" answer, not an
/// error. On XUD_OK *outLoader owns a table with XUD_LOADER_GLOBAL_SLOT_COUNT slots filled.
XudResult xud_loader_open(XudLoader **outLoader);

/// Closes the loader: dlclose the handle, zero the table, free it, set the pointer NULL.
XudResult xud_loader_close(XudLoader **loader);

/// The open invariant, for the layers above: true when the loader handle is loaded and the
/// seed entry point is resolved, which is what every call through the table needs. A layer
/// that finds this false reports XUD_INVALID_ARG and never calls through a NULL PFN slot.
bool xud_loader_is_open(const XudLoader *loader);

/// Resolves one entry point through the seed. `handle` selects the level: VK_NULL_HANDLE
/// for the global-level names the ICD loader answers before any instance exists, the
/// instance/device handle this layer created for the lower-level ones (vkGetInstanceProcAddr
/// answers NULL for those without a live handle). It is `void *` because the Vulkan C
/// headers type that parameter as VkInstance and nothing else - passing a VkPhysicalDevice
/// or VkDevice into a VkInstance slot would be an incompatible-pointer argument - so the
/// call casts it back, which is legal because every handle is a pointer here
/// (VK_USE_64_BIT_PTR_DEFINES == 1, static_assert-ed in xud_loader.c).
/// XUD_UNSUPPORTED when the seed slot is NULL (a nil PFN is reported, never called) or when
/// the loader does not know `name`; XUD_INVALID_ARG on a NULL loader/name/out-function.
/// `outFunction` is filled only on XUD_OK.
XudResult xud_loader_resolve(const XudLoader *loader, const void *handle, const char *name, XudPfnBits *outFunction);

/// Resolves one entry point and stores it into a slot of the loader's table, bumping
/// resolvedCount only the first time that slot goes from empty to filled - so resolvedCount
/// is a property of the table, not of the calls, and re-binding a live name is a no-op.
/// `dstSlot` is the address of the slot (`&loader->vkX`); it must be pointer-sized, which
/// every layer static_asserts for the PFN types it binds (xud_loader.c checks the global
/// ones). The address moves by copy, never by cast - ISO C forbids converting between
/// object and function pointers and -Wpedantic -Werror says so.
XudResult xud_loader_bind(XudLoader *loader, const void *handle, const char *name, void *dstSlot);

#endif /* XUD_XUD_LOADER_H_ */
