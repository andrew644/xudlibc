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

    /* Instance level: NULL until step S3 has an instance to resolve through. */
    PFN_vkCreateInstance createInstance;
    PFN_vkDestroyInstance destroyInstance;
    PFN_vkEnumeratePhysicalDevices enumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceProperties getPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceFeatures2 getPhysicalDeviceFeatures2;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties2 getPhysicalDeviceQueueFamilyProperties2;
    PFN_vkEnumerateDeviceExtensionProperties enumerateDeviceExtensionProperties;

    /* Device level: NULL until step S6 has a device to resolve through. */
    PFN_vkCreateDevice createDevice;
    PFN_vkDestroyDevice destroyDevice;
    PFN_vkGetDeviceQueue getDeviceQueue;
    PFN_vkDeviceWaitIdle deviceWaitIdle;

    /* Device-level swapchain/surface teardown: NULL until step S7. */
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

/// Resolves one entry point through the seed. `instance` is VK_NULL_HANDLE for global-level
/// names (the ICD loader answers for nothing else at that level); later steps pass the
/// instance they created. XUD_UNSUPPORTED when the seed slot is NULL - a nil PFN is
/// reported, never called - or when the loader does not know `name`.
XudResult xud_loader_resolve(const XudLoader *loader, VkInstance instance, const char *name, XudPfnBits *outFunction);

#endif /* XUD_XUD_LOADER_H_ */
