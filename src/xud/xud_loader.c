/***********************************************************************************************
 *
 *   xud_loader.c - Vulkan loader bring-up: Volk's tables, opened and closed once per context
 *
 *   Owns : the XudLoader lifecycle (volkInitialize/volkFinalize), the copy of Volk's three loader
 *          level rows into the per-context struct, and the two bulk-fill rungs that resolve a ha
 *          ndle's whole entry-point table (volkLoadInstanceTable/volkLoadDeviceTable). Internal m
 *          odule source: it pairs with xud_loader.h, which owns the type and the contract.
 *   Layer: loader. This is the one place Vulkan is entered from, and it is entered by name at ru
 *          ntime: volk.c dlopen()s libvulkan.so.1 (RTLD_NOW|RTLD_LOCAL, volk.c 114, with a libvu
 *          lkan.so fallback at 116) and volk.h declares every Vulkan entry point as `extern PFN_
 *          vkXxx vkXxx;` - a data pointer, not a prototype - so under VK_NO_PROTOTYPES a direct
 *          vkCreateInstance(...) call compiles and links against that pointer. No Vulkan symbol i
 *          s linked at build time and no -lvulkan appears anywhere: verified on this machine, ld
 *          di lists no libvulkan and nm -u lists no vk* function symbol.
 *
 *   What this file no longer does (2026-10-23, the loader rework): it no longer dlopens by hand,
 *   no longer dlsyms a seed entry point, no longer keeps a name table, and no longer works aroun
 *   d dlsym's void*-to-function-pointer cast with a union and a memcpy (AGENTS.md's strict-mode c
 *   ast rule). Volk does all four, and it hands back typed PFN slots, so the copy below is a pl
 *   ain assignment between two identical function-pointer types.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud_loader.h"
#include "xud.h"

#include <stdlib.h> /* Required for: calloc, free - the loader struct itself */
#include <string.h> /* Required for: memset - clearing the table pair at teardown and at re-bind */

/*-----------------------------------------------------------------------------------------------
 * Bring-up and teardown
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_loader_open(XudLoader** outLoader) {
    XudLoader* loader = NULL;
    VkResult vk;
    uint32_t globalSlots = 0u;

    if ((outLoader == NULL) || (*outLoader != NULL))
        return XUD_INVALID_ARG;

    loader = calloc(1u, sizeof(XudLoader));
    if (loader == NULL)
        return XUD_OUT_OF_MEMORY;

    /* volkInitialize() is the whole of the loader bring-up: dlopen by name, then dlsym the loader
       -level names into Volk's globals (volk.c 141-149). It answers VK_ERROR_INITIALIZATION_FAIL
       ED when there is no loader to open or no seed to resolve, and this layer maps that to XUD_U
       NSUPPORTED rather than through xud_result_from_vk: "no Vulkan loader on this machine" is th
       e reason every layer above refuses, and AGENTS.md's xud_vulkan_supported() rule says it mus
       t be a real runtime query that stays green with no device present. A Vulkan failure of a c
       all that had a driver behind it would be XUD_VULKAN_ERROR; here there was no call. */
    vk = volkInitialize();
    if (vk != VK_SUCCESS) {
        free(loader);
        return XUD_UNSUPPORTED;
    }

    /* Volk's globals are the loader-level rows: commands with no handle in hand, so a VkInstance
       is not needed to find them. They are copied into this context's struct rather than read of
       f Volk's globals at every call site, for the one reason xud_loader.h states: Volk's global
       table is process-wide, so a second context's xud_loader_close() would otherwise clear rows
       the first context is still calling through. A plain assignment, not a cast - both sides ar
       e the same PFN type, which is what the hand-rolled bring-up this replaced needed the XudPfn
       Bits union for (dlsym hands back void*, Volk does not). */
    loader->createInstance = vkCreateInstance;
    loader->enumerateInstanceExtensionProperties = vkEnumerateInstanceExtensionProperties;
    loader->enumerateInstanceLayerProperties = vkEnumerateInstanceLayerProperties;

    /* All three are core 1.0 names, so a loader that does not answer them is not a loader this
       library can use: the loader is left closed rather than half-open, and the reason is XUD_U
       NSUPPORTED, the same answer as "no loader at all" - the three are counted rather than chec
       ked one by one so the count is the thing the invariant is stated against (XUD_LOADER_GLOBA
       L_SLOT_COUNT, the field count this rung fills). */
    if (loader->createInstance != NULL)
        globalSlots++;
    if (loader->enumerateInstanceExtensionProperties != NULL)
        globalSlots++;
    if (loader->enumerateInstanceLayerProperties != NULL)
        globalSlots++;
    if (globalSlots != XUD_LOADER_GLOBAL_SLOT_COUNT) {
        free(loader);
        return XUD_UNSUPPORTED;
    }

    /* Both tables are still all-NULL: nothing has been resolved through a handle yet, and a NULL
       slot is the state every layer above reads as "not available, report XUD_UNSUPPORTED rather
       than call through it". calloc made that state, and the two handles below say the same. */
    loader->open = true;
    loader->boundInstance = VK_NULL_HANDLE; /* no instance yet, so no instance-level rows */
    loader->boundDevice = VK_NULL_HANDLE; /* no device yet, so no device-level rows    */

    *outLoader = loader;
    return XUD_OK;
}

XudResult xud_loader_close(XudLoader** loader) {
    if (loader == NULL)
        return XUD_INVALID_ARG;
    if (*loader == NULL)
        return XUD_ALREADY;

    /* volkFinalize() dlclose()s the loader and clears Volk's global rows (volk.c 151-167). It re
       turns void, so unlike the hand-rolled bring-up this replaced - which checked dlclose()'s in
       t answer and answered XUD_INTERNAL_ERROR for a non-zero one - a failed unload is not obser
       vable here. The rung's answer is about the struct it freed, and the tables go with it: the
       rows of a table whose owner is gone name code in a library that is being unloaded, so clea
       ring them before freeing the struct is the honest state even though the struct is about to
       be unreachable. */
    volkFinalize();

    memset(&(*loader)->instanceTable, 0, sizeof((*loader)->instanceTable));
    memset(&(*loader)->deviceTable, 0, sizeof((*loader)->deviceTable));
    (*loader)->open = false;
    (*loader)->boundInstance = VK_NULL_HANDLE;
    (*loader)->boundDevice = VK_NULL_HANDLE;

    free(*loader);
    *loader = NULL;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The three invariants the layers above check
 *-----------------------------------------------------------------------------------------------*/

bool xud_loader_is_open(const XudLoader* loader) {
    return (loader != NULL) && (loader->open == true);
}

bool xud_loader_instance_bound(const XudLoader* loader, VkInstance instance) {
    if (loader == NULL)
        return false;
    /* The table is bound *for this instance*: its rows came from vkGetInstanceProcAddr(instance,
       name), so a table filled for another instance is not a table for this one, and a handle k
       ept across a recreation fails here rather than calling through rows of a layer that is go
       ne. VK_HANDLE equality is pointer equality here, which is the same fact the handle-level c
       asts in this file rest on (VK_USE_64_BIT_PTR_DEFINES == 1, asserted in this header). */
    return loader->open && (loader->boundInstance == instance) && (instance != VK_NULL_HANDLE);
}

bool xud_loader_device_bound(const XudLoader* loader, VkDevice device) {
    if (loader == NULL)
        return false;
    return loader->open && (loader->boundDevice == device) && (device != VK_NULL_HANDLE);
}

/*-----------------------------------------------------------------------------------------------
 * The two bulk-fill rungs
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_loader_bind_instance(XudLoader* loader, VkInstance instance) {
    if ((loader == NULL) || (!loader->open) || (instance == VK_NULL_HANDLE))
        return XUD_INVALID_ARG;

    /* volkLoadInstanceTable() memsets the table and fills every instance-level row through vkGe
       tInstanceProcAddr(instance, name) (volk.c 171-183, volk.h 114). It returns void, so the on
       ly answer available is which rows came back NULL, and a NULL row is the state the layers a
       bove read as XUD_UNSUPPORTED - the check-before-you-use rule, one rung lower. A row can on
       ly name a layer that this instance instantiated, which is why the bind is a rung and not a
       lookup on every call: a table filled for one instance is not a table for another. */
    volkLoadInstanceTable(&loader->instanceTable, instance);
    loader->boundInstance = instance;

    /* A device table belongs to a device of the instance it was bound under, so re-binding the
       instance invalidates it: clearing it here is what makes xud_loader_device_bound() false an
       d every device-level call site refuse, rather than keep rows that name a layer of an inst
       ance that is gone. The handle goes with the rows. */
    if (loader->boundDevice != VK_NULL_HANDLE) {
        memset(&loader->deviceTable, 0, sizeof(loader->deviceTable));
        loader->boundDevice = VK_NULL_HANDLE;
    }

    return XUD_OK;
}

XudResult xud_loader_bind_device(XudLoader* loader, VkDevice device) {
    if ((loader == NULL) || (!loader->open) || (device == VK_NULL_HANDLE))
        return XUD_INVALID_ARG;

    /* volkLoadDeviceTable() resolves through Volk's GLOBAL vkGetDeviceProcAddr (volk.c 185-197,
       volk.h 120), which volkLoadInstanceTable set for the instance loaded last - not for the in
       stance this loader was built around. Handing it this loader's own row is what keeps the ta
       ble filled here the rows of this loader's instance when a second context has loaded its ow
       n in between; vkGetDeviceProcAddr is device-level for resolution (it needs the VkDevice) b
       ut instance-level for lookup, which is why Volk puts it in VolkInstanceTable. A NULL row m
       eans the instance above did not answer it, and no device table can be filled without it. */
    if ((loader->boundInstance == VK_NULL_HANDLE)
        || (loader->instanceTable.vkGetDeviceProcAddr == NULL))
        return XUD_UNSUPPORTED;
    vkGetDeviceProcAddr = loader->instanceTable.vkGetDeviceProcAddr;

    volkLoadDeviceTable(&loader->deviceTable, device);
    loader->boundDevice = device;
    return XUD_OK;
}
