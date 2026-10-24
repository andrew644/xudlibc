/***********************************************************************************************
 *
 *   xud_loader.h - Vulkan loader bring-up: Volk's entry-point tables, one pair per context
 *
 *   Owns : the XudLoader type - the per-context pair of Vulkan entry-point tables Volk fills
 *          (struct VolkInstanceTable, resolved through the VkInstance this context created, and
 *          struct VolkDeviceTable, resolved through the VkDevice it created), plus the three
 *          loader-level entry points volkInitialize() fills with no handle in hand, copied into
 *          this struct so the invariant below is per-context rather than process-wide - and the
 *          contract of the loader layer: volkInitialize()/volkFinalize() bring-up and teardown,
 *          the two bind rungs that fill the tables, and the readiness checks every layer above
 *          makes before it calls through them. Internal module header: user code includes xud.h
 *          only.
 *   Layer: loader, above GLFW and below the instance. This is the only place Vulkan is loaded
 *          from, and it is loaded by name at runtime: volk.c dlopen()s libvulkan.so.1 (RTLD_NOW
 *          |RTLD_LOCAL at volk.c 114, with a libvulkan.so fallback at 116) and volk.h declares
 *          every Vulkan entry point as `extern PFN_vkXxx vkXxx;` - a data pointer, not a protot
 *          ype - so under VK_NO_PROTOTYPES a direct vkCreateInstance(...) call compiles and link
 *          s against that pointer. No Vulkan symbol is linked at build time: the rule in AGENTS.
 *          md is now a link-time fact rather than a convention, because forgetting volk.c from t
 *          he link leaves those pointers undefined. Verified on this machine: ldd lists no libvu
 *          lkan, nm -u lists no vk* function symbol.
 *
 *   What replaced what (2026-10-23, the loader rework): the hand-maintained 20-slot XudLoader of
 *   PFN_vkX fields, filled one name at a time through xud_loader_resolve()/xud_loader_bind() by
 *   a hand-rolled dlopen/dlsym bring-up, is gone. Volk's tables are the same mechanism one rung
 *   up - a struct of PFN slots per handle level, filled in bulk by volkLoadInstanceTable()/volu
 *   nkLoadDeviceTable() - so the per-name resolve calls, the XUD_LOADER_SLOT_COUNT count, the
 *   XUD_LOADER_NAMES table and the XudPfnBits union that worked around dlsym's void*-to-functio
 *   n-pointer cast (AGENTS.md's strict-mode cast rule) all disappear with it - XUD_LOADER_GLOBAL
 *   _SLOT_COUNT survives, because it is the count of loader-level rows xud_loader_open() insis
 *   ts on before it calls itself open: Volk hands back typed PFN slots, so no cast is needed
 * anywhere. The XudLoader s urface - open/close/is_open plus the two bind rungs - is kept, because
 * the layers above nee d a per-context object to own the tables and to state which levels are
 * actually usable, and that is what the loader layer's contract has always been.
 *
 *   Include order (the volk.h trap, verified here): volk.h errors `To use volk, you need to def
 *   ine VK_NO_PROTOTYPES before including vulkan.h` when VULKAN_H_ is already in (volk.h 17-18)
 *   and defines VK_NO_PROTOTYPES itself when it is not (25-26). So in every TU that wants Vulkan
 *   volk.h comes BEFORE <vulkan/vulkan.h>; every xud header that includes the Vulkan header does
 *   the volk.h include immediately above it, which is what makes the order hold whichever xud h
 *   eader a TU happens to include first. volk.h includes vulkan/vulkan_core.h and vulkan/vulkan
 *   _platform.h itself, so the <vulkan/vulkan.h> include that follows is a no-op once volk.h has
 *   been seen - the guard is there for the TU that includes a Vulkan-bearing xud header without
 *   this one.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#ifndef XUD_LOADER_H_
#define XUD_LOADER_H_

#include "volk.h" /* Required for: VK_NO_PROTOTYPES, the extern PFN rows, VolkInstanceTable/VolkDeviceTable */
#include "xud.h" /* Required for: XudResult - the loader's own error codes */
#include <vulkan/vulkan.h> /* Required for: VkResult, VK_NULL_HANDLE, PFN_vk* - declarations only, never linked */

#include <assert.h> /* Required for: static_assert, the handle-size and table-shape checks below */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*-----------------------------------------------------------------------------------------------
 * The two facts this layer's contract rests on, both compile-time
 *-----------------------------------------------------------------------------------------------
 *
 *   1. Vulkan handles are pointers here, which is what makes the bound-state checks in this fi
 *      le exact rather than a layout coincidence: `loader->boundInstance == instance` is pointer
 *      equality, so a handle kept across an instance recreation compares unequal to the handle t
 *      he rows were resolved for instead of aliasing by luck. VK_USE_64_BIT_PTR_DEFINES defaults
 *      to 1 in these headers (vulkan_core.h 29-33) and VK_DEFINE_HANDLE is `typedef struct objec
 *      t##_T* object;` (vulkan_core.h 26), so every handle in this tree is an 8-byte pointer.
 *   2. A PFN slot is one pointer wide, which is what makes Volk's tables a plain array of slots
 *      as compiled here: sizeof(struct VolkInstanceTable) is 1016 B = 127 entries and
 *      sizeof(struct VolkDeviceTable) is 5680 B = 710 entries, both with VK_USE_64_BIT_PTR_DEF
 *      INES == 1 and VK_EXT ray-tracing off (the feature macros that would grow the tables are
 *      not defined by <vulkan/vulkan.h>). The counts are measured, not assumed: the asserts below
 *      are the ones that hold without them.
 */

static_assert(VK_USE_64_BIT_PTR_DEFINES == 1,
    "xud_loader.h: the bound-handle compares below are pointer compares only while "
    "VK_DEFINE_HANDLE is a pointer");
static_assert(sizeof(PFN_vkCreateInstance) == sizeof(void*), "xud_loader.h: an entry-point slot is one pointer wide");
static_assert(
    sizeof(struct VolkInstanceTable) % sizeof(void*) == 0u, "xud_loader.h: Volk's instance table is a whole number of pointer-wide slots");
static_assert(
    sizeof(struct VolkDeviceTable) % sizeof(void*) == 0u, "xud_loader.h: Volk's device table is a whole number of pointer-wide slots");

/*-----------------------------------------------------------------------------------------------
 * The loader's own vocabulary: one per-context table pair, plus the loader-level rows
 *-----------------------------------------------------------------------------------------------
 */

/* The loader-level entry points: commands with no handle in hand, which volkInitialize() fills
   as Volk's globals (volk.c 141-149) and volkFinalize() clears (volk.c 151-167). They are cop
   ied into the per-context struct at xud_loader_open() for one reason: Volk's global table is
   process-wide, so a second context's xud_loader_close() would otherwise clear the rows the f
   irst context is still using. AGENTS.md's "multiple windows must work" rule is what makes the
   copy worth the three fields it costs. A dlclose of libvulkan.so.1 while another context is
   still alive remains a hazard either way - Volk's loader handle is one dlopen per process -
   and that hazard is not new here: the hand-rolled bring-up this replaced dlclose()d the same
   soname. */
#define XUD_LOADER_GLOBAL_SLOT_COUNT 3u

typedef struct XudLoader {
    /* --- loader-level (no handle in hand), copied from Volk's globals at open time --- */
    PFN_vkCreateInstance createInstance;
    PFN_vkEnumerateInstanceExtensionProperties enumerateInstanceExtensionProperties;
    PFN_vkEnumerateInstanceLayerProperties enumerateInstanceLayerProperties;

    /* --- instance-level: Volk's own table, filled in bulk by volkLoadInstanceTable() ---
       127 entries as compiled here. The rows this tree calls through it are the instance's own
       teardown (destroyInstance), the physical layer's whole query surface (enumeratePhysicalDe
       vices, the three *2 property getters, the queue-family getter, enumerateDeviceExtensionPr
       operties), the diagnostics layer's messenger pair (which only answers non-NULL when VK_EX
       T_debug_utils is enabled on the instance in hand), and vkGetDeviceProcAddr - device-level
       for resolution but instance-level for lookup, which is why Volk puts it here. The struct
       is Volk's, so it carries every instance-level entry point the headers declare, not a hand
       picked subset: there is no list here to keep in step with the layers above. */
    struct VolkInstanceTable instanceTable;

    /* --- device-level: Volk's own table, filled in bulk by volkLoadDeviceTable() ---
       710 entries as compiled here, and every row of it answers NULL unless the extension that
       owns it was enabled on the instance the device was created from - probed here with an ins
       tance that did not enable VK_KHR_swapchain, where every swapchain row came back NULL whil
       e destroyDevice/deviceWaitIdle/getDeviceQueue came back set. That is the check-before-you
       -use rule working through the table rather than around it, and it is why the device laye
       r's enabled-extension list still has to be built from the caps' advertised names. */
    struct VolkDeviceTable deviceTable;

    /* --- the three states of this struct --- */
    bool open; /* volkInitialize() answered VK_SUCCESS and volkFinalize() has not run */

    /* The handle each table was resolved for, VK_NULL_HANDLE until it was. They are the bound
       state rather than a flag beside it, and the reason is the handle-level rule every layer a
       bove obeys: a table is only usable for the handle it was resolved through, and naming tha
       t handle is what lets a layer say "these rows are this instance's rows" instead of "the
       loader bound some instance at some point". Re-binding overwrites the handle and the rows
       together, which is what makes a stale handle a caught error rather than a pointer into a
       layer that is gone - the rule xud_loader.h used to state about the messenger pair, now en
       forced for every row of the table. */
    VkInstance boundInstance; /* whose rows struct VolkInstanceTable holds */
    VkDevice boundDevice; /* whose rows struct VolkDeviceTable holds  */
} XudLoader;

/*-----------------------------------------------------------------------------------------------
 * The loader layer's contract
 *-----------------------------------------------------------------------------------------------
 *
 *   xud_loader_open() is the bring-up rung: volkInitialize() dlopens libvulkan.so.1 by name and
 *   fills the loader-level rows. It answers XUD_UNSUPPORTED when the machine has no Vulkan loader
 *   at all - volkInitialize() answers VK_ERROR_INITIALIZATION_FAILED there, and this layer maps t
 *   hat to XUD_UNSUPPORTED rather than through xud_result_from_vk because "no driver" is the rea
 *   son the whole library above refuses, not a Vulkan failure of a call that had a driver behind
 *   it. The three loader-level rows are then checked for non-NULL: they are all core 1.0 names,
 *   so a loader that does not answer them is not a loader this library can use, and the loader is
 *   left closed rather than half-open.
 *
 *   xud_loader_close() is the teardown rung: volkFinalize() dlclose()s the loader and clears Volk
 *   's global rows (volk.c 151-167). It returns void, so unlike the hand-rolled rung this replace
 *   d there is no dlclose answer to check - a failed unload is not observable here, and the runt
 *   g's answer is about the struct it freed.
 *
 *   xud_loader_bind_instance() / xud_loader_bind_device() are the bulk-fill rungs: one call per
 *   handle, filling every row of that level's table at once. They replace the per-name xud_loade
 *   r_resolve()/xud_loader_bind() pair the layers above used to call before each of their calls.
 *   A name the driver does not answer stays NULL in the table and the layer that wants it reports
 *   XUD_UNSUPPORTED rather than calling through it - the check-before-you-use rule, one rung low
 *   er, unchanged. A bind with no handle in hand is XUD_INVALID_ARG; a device bind with no
 *   instance bound above it, or with a NULL vkGetDeviceProcAddr row to resolve through, is
 *   XUD_UNSUPPORTED - the rung cannot resolve, and that is not a bad argument. A refused bind
 *   leaves the table alone.
 *
 *   xud_loader_is_open() is the bring-up invariant. xud_loader_instance_bound() / xud_loader_de
 *   vice_bound() are the two table invariants, and they take the handle a layer is about to cal
 *   l through: a bound table is a table whose rows belong to the handle in hand, because volkLoa
 *   dInstanceTable() resolves through vkGetInstanceProcAddr(instance, name) and a row can only n
 *   ame a layer that instance instantiated. A table filled for one instance is not a table for a
 *   nother, and saying so is what makes the bind a rung rather than a lookup on every call.
 */

XudResult xud_loader_open(XudLoader** outLoader);
XudResult xud_loader_close(XudLoader** loader);

bool xud_loader_is_open(const XudLoader* loader);
bool xud_loader_instance_bound(const XudLoader* loader, VkInstance instance);
bool xud_loader_device_bound(const XudLoader* loader, VkDevice device);

XudResult xud_loader_bind_instance(XudLoader* loader, VkInstance instance);
XudResult xud_loader_bind_device(XudLoader* loader, VkDevice device);

/*-----------------------------------------------------------------------------------------------
 * Two facts about Volk's tables, verified on this machine by running them (2026-10-23):
 *   - volkLoadInstanceTable() fills the instance-level rows AND sets Volk's global vkGetDevice
 *     ProcAddr for the instance loaded last; vkGetDeviceProcAddr is device-level for *resolut
 *     ion* (it needs the VkDevice) but instance-level for *lookup*, which is why Volk puts it i
 *     n VolkInstanceTable.
 *   - volkLoadDeviceTable() resolves through that global, so xud_loader_bind_device() hands it
 *     this loader's own row first - otherwise a second context loading its own instance would
 *     silently fill the first context's device table with the second instance's rows.
 *   - A name whose extension is not enabled on the instance answers NULL: probed here, with an
 *     instance that did not enable VK_KHR_swapchain, every swapchain row of the device table ca
 *     me back NULL while destroyDevice/deviceWaitIdle/getDeviceQueue/cmdDraw/allocateMemory cam
 *     e back set. That is the check-before-you-use rule working through the table, not around i
 *     t, and it is why the device layer's enabled-extension list still has to be built from the
 *     caps' advertised names.
 */

#endif /* XUD_LOADER_H_ */
