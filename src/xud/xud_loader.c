/***********************************************************************************************
 *
 *   xud_loader.c - Vulkan loader bring-up: dlopen the ICD loader, resolve the PFN table
 *
 *   Owns : the loader layer. dlopen(XUD_VULKAN_LIBRARY_SONAME), dlsym("vkGetInstanceProcAddr")
 *          as the seed, the global-level entry points resolved through that seed, and teardown.
 *   Layer: loader. The only place Vulkan is loaded from, and it is loaded by name at runtime:
 *          no Vulkan symbol is linked at build time, so this file compiles and the tests pass on
 *          a machine with no driver. Every layer above calls Vulkan through the slots in XudLoader
 *          and binds its own slots through xud_loader_bind() once it holds the handle the command
 *          needs (vkGetInstanceProcAddr answers NULL for lower-level commands without one -
 *          verified here: resolving vkDestroyInstance with VK_NULL_HANDLE returns NULL, resolving
 *          it through a live instance returns the address).
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud_loader.h"
#include "xud.h"

#include <assert.h> /* Required for: static_assert, the compile-time PFN slot check */
#include <dlfcn.h> /* Required for: dlopen, dlsym, dlclose */
#include <stdlib.h> /* Required for: calloc, free */
#include <string.h> /* Required for: memcpy, the object->function address transfer */

/* The PFN slots hold function addresses and xud_pfn_copy moves a pointer-sized word, so a
   slot must be pointer-sized. Checked here for the slots filled at loader bring-up; each later
   layer adds the checks for the slots it fills (xud_instance.c checks the instance-level ones). */
static_assert(sizeof(PFN_vkGetInstanceProcAddr) == sizeof(void*), "PFN slot must be pointer-sized");
static_assert(sizeof(PFN_vkEnumerateInstanceLayerProperties) == sizeof(void*),
    "PFN slot must be pointer-sized");
static_assert(sizeof(PFN_vkEnumerateInstanceExtensionProperties) == sizeof(void*),
    "PFN slot must be pointer-sized");

/* xud_loader_resolve() takes a handle as void * and casts it to VkInstance at the call, which
   is only sound while every Vulkan handle is a pointer. Where this define is 0 the
   non-dispatchable handles are uint64_t and the cast would have to go through a union. */
static_assert(VK_USE_64_BIT_PTR_DEFINES == 1, "xudlibc assumes every Vulkan handle is a pointer");

/* Internal: move one function address into one typed PFN slot. `dstSlot` is the address of
   the slot, `srcBits` the address of a variable holding the function address. No cast: ISO C
   forbids converting between object and function pointers (-Wpedantic says so). */
static void xud_pfn_copy(void* dstSlot, const void* srcBits) {
    memcpy(dstSlot, srcBits, sizeof(void*));
}

/* Public (xud_loader.h): resolve one name into its slot, bumping resolvedCount only when the
   slot went from empty to filled - so resolvedCount is a property of the table, not of the
   calls, and re-binding a live name is a no-op. `handle` selects the level: VK_NULL_HANDLE for
   the global-level names, the instance/device handle for the lower-level ones. The address
   moves by copy (xud_pfn_copy), never by cast: casting a function pointer to void * is a
   hard error under these flags. */
XudResult xud_loader_bind(XudLoader* loader, const void* handle, const char* name, void* dstSlot) {
    XudPfnBits bits;
    void* previous = NULL;
    XudResult result = xud_loader_resolve(loader, handle, name, &bits);

    if (result != XUD_OK)
        return result;

    memcpy(&previous, dstSlot, sizeof(void*)); /* the slot is pointer sized (static_assert) */
    xud_pfn_copy(dstSlot, &bits.function);
    if (previous == NULL)
        loader->resolvedCount += 1u;
    return XUD_OK;
}

XudResult xud_loader_resolve(
    const XudLoader* loader, const void* handle, const char* name, XudPfnBits* outFunction) {
    XudPfnBits bits;

    if ((loader == NULL) || (outFunction == NULL) || (name == NULL) || (name[0] == '\0'))
        return XUD_INVALID_ARG;

    bits.function = NULL;

    /* The guard every call site relies on: a nil PFN is reported, never dereferenced. */
    if (loader->getInstanceProcAddr == NULL)
        return XUD_UNSUPPORTED;

    /* The cast back to VkInstance is the one place a handle changes type: the argument arrives
       as void * on purpose (see xud_loader.h) so the layers below can pass a VkPhysicalDevice or
       VkDevice without an incompatible-pointer argument. Object-pointer to object-pointer casts
       are fine; only object <-> function is forbidden. */
    bits.function = loader->getInstanceProcAddr((VkInstance)handle, name);
    if (bits.function == NULL)
        return XUD_UNSUPPORTED;

    *outFunction = bits;
    return XUD_OK;
}

XudResult xud_loader_open(XudLoader** outLoader) {
    XudLoader* loader = NULL;
    XudResult result = XUD_INVALID_ARG;
    XudPfnBits bits;
    void* seedBits = NULL;

    if ((outLoader == NULL) || (*outLoader != NULL))
        return XUD_INVALID_ARG;

    loader = calloc(1, sizeof(XudLoader));
    if (loader == NULL)
        return XUD_OUT_OF_MEMORY;

    loader->library = dlopen(XUD_VULKAN_LIBRARY_SONAME, RTLD_NOW | RTLD_GLOBAL);
    if (loader->library == NULL) {
        /* No Vulkan loader on this machine: the honest answer is unsupported, and there is
           nothing yet to tear down. The dlerror() string is dropped here - the diagnostics
           layer that surfaces it arrives at step S8. */
        free(loader);
        return XUD_UNSUPPORTED;
    }

    seedBits = dlsym(loader->library, "vkGetInstanceProcAddr");
    if (seedBits == NULL)
        goto fail;
    xud_pfn_copy(&loader->getInstanceProcAddr, &seedBits);
    loader->resolvedCount += 1u;

    /* Loader sanity: the seed must answer for its own name. This is a real driver call, but
       it needs no device, so the default headless test run still exercises it. */
    result = xud_loader_resolve(loader, VK_NULL_HANDLE, "vkGetInstanceProcAddr", &bits);
    if ((result != XUD_OK) || (bits.function == NULL))
        goto fail;

    result = xud_loader_bind(loader, VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties",
        &loader->enumerateInstanceLayerProperties);
    if (result != XUD_OK)
        goto fail;

    result = xud_loader_bind(loader, VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties",
        &loader->enumerateInstanceExtensionProperties);
    if (result != XUD_OK)
        goto fail;

    if (loader->resolvedCount != XUD_LOADER_GLOBAL_SLOT_COUNT) {
        result = XUD_INTERNAL_ERROR;
        goto fail;
    }

    *outLoader = loader;
    return XUD_OK;

fail:
    (void)xud_loader_close(&loader);
    return (result != XUD_OK) ? result : XUD_INTERNAL_ERROR;
}

XudResult xud_loader_close(XudLoader** loader) {
    XudLoader* victim = NULL;
    int dlResult = 0;

    if (loader == NULL)
        return XUD_INVALID_ARG;
    if (*loader == NULL)
        return XUD_ALREADY;

    victim = *loader;
    if (victim->library != NULL) {
        dlResult = dlclose(victim->library);
        victim->library = NULL;
    }

    free(victim);
    *loader = NULL;

    /* The loader refused to unload: an invariant broke, and the reason (a dlerror() string)
       is surfaced by the diagnostics layer at step S8. */
    if (dlResult != 0)
        return XUD_INTERNAL_ERROR;
    return XUD_OK;
}

/* The open invariant every layer above needs before it calls through the table: the handle is
   loaded and the seed is resolved. resolvedCount alone is not enough - it counts slots, and a
   table can hold slots resolved against a loader that never opened. */
bool xud_loader_is_open(const XudLoader* loader) {
    return (loader != NULL) && (loader->library != NULL) && (loader->getInstanceProcAddr != NULL);
}
