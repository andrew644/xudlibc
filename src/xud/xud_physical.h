/***********************************************************************************************
 *
 *   xud_physical.h - the physical device layer: one VkPhysicalDevice per context, and the
 *          caps recorded from it
 *
 *   Owns : XudPhysicalDevice (the handle plus an embedded XudDeviceCaps) and the contract of
 *          the physical layer - device selection, caps recording (properties, features, memory,
 *          advertised device extensions, the chosen queue family), apiVersion read-back.
 *          This header is declarations only; xud_physical.c is in the tree with **both** halves
 *          now - the pure half landed at S5.3, the driver half (xud_physical_query_devices() and
 *          xud_physical_open()) at S5.4 - and it joined the archive with them (add_library(xud S
 *          TATIC ...) in CMakeLists.txt lists six objects, xud_physical.c.o among them), so the
 *          test binary links the archive rather than compiling that TU specially - see todo.md.
 *   Layer: above the instance, below the device. Every Vulkan call the .c makes goes through a
 *          row of the loader's struct VolkInstanceTable (xud_loader.h), filled in bulk by xud_lo
 *          ader_bind_instance at the instance layer; nothing here names a vk* function directly,
 *          and no Vulkan symbol is linked (AGENTS.md: there is no -lvulkan anywhere in the buil
 *          d). Internal like xud_loader.h: tests may include it, user code must not.
 *          The handle here is the one rung of the teardown ladder that is free-only: Vulkan has
 *          no vkDestroyPhysicalDevice (grepped in these headers - the name does not exist), a
 *          VkPhysicalDevice is a query result and not an object the driver allocated for us, so
 *          xud_physical_close() frees memory and calls no driver function at all.
 *   Loader fact restated here (2026-10-23, the loader rework onto vendored Volk): the physical
 *          layer's seven names are rows of struct VolkInstanceTable, filled in bulk by xud_loa
 *          der_bind_instance - there is no per-name slot left to grow, which is what the 2026-1
 *          0-18 note about XUD_LOADER_SLOT_COUNT going 19 -> 20 used to record. The seven are s
 *          till instance-level for lookup: probed through a live VkInstance they resolve non-NU
 *          LL and with VK_NULL_HANDLE in hand they resolve NULL. Header facts verified against V
 *          ulkan-Headers 1.4.363 here
 *          (2026-10-18, grepped, no driver involved): VkPhysicalDeviceProperties2 at vulkan_core
 *          h:5809, VkPhysicalDeviceFeatures
 *          Features2 at :5803, VkPhysicalDeviceMemoryProperties2 at :5843, and the queue-family
 *          wrapper at :5837 is named VkQueueFamilyProperties2 - no PhysicalDevice prefix, the
 *          naming trap this comment exists to record. The device-level names are PFN_vkEnumerate
 *          DeviceExtensionProperties at :4544 (it takes pLayerName, which this layer passes NULL
 *          for to get the driver's own list) and PFN_vkGetPhysicalDeviceQueueFamilyProperties2
 *          at :6182. Sizes as compiled here: VkPhysicalDeviceProperties2 840 B, VkPhysicalDevice
 *          Features2 240 B, VkPhysicalDeviceMemoryProperties2 536 B, VkQueueFamilyProperties2 40
 *          B, VkExtensionProperties 260 B - the caps embed the first four, so the struct is ~1.7
 *          KB and heap-allocated, never a stack temporary. VK_QUEUE_PRESENT_BIT is NOT in these
 *          headers (grepped across the whole vulkan include tree: zero hits) - see xud_physical
 *          _select_queue_family for what that means for the queue-family rule.
 *          The four caps queries that return **void** in these headers are PFN_vkGetPhysicalDev
 *          iceFeatures2 (:6178), PFN_vkGetPhysicalDeviceProperties2 (:6179), PFN_vkGetPhysicalDe
 *          viceMemoryProperties2 (:6183) and PFN_vkGetPhysicalDeviceQueueFamilyProperties2 (:6182
 *          ) - no VkResult for xud_result_from_vk to map, which is why xud_physical_open() zeroes
 *          every caps field before the call that fills it and treats "the struct still says noth
 *          ing" as the only failure signal those four can give (the two that do return a VkResul
 *          t are vkEnumeratePhysicalDevices at :4534 and vkEnumerateDeviceExtensionProperties at
 *          :4544). And VkPhysicalDeviceArchitectureInfo is **not** in these headers at all (grep
 *          ped: zero hits), which is why xud_physical_select() ranks on nothing today - the only
 *          core fields a discrete-preference policy could sort on are deviceType (the enum at :22
 *          33-:2238) plus vendorID/deviceName/pipelineCacheUUID, and whether to rank at all is a
 *          decision, not code.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#ifndef XUD_XUD_PHYSICAL_H_
#define XUD_XUD_PHYSICAL_H_

#include "volk.h" /* Required for: VK_NO_PROTOTYPES - must precede <vulkan/vulkan.h>, see xud_loader.h */
#include "xud.h"
#include "xud_instance.h"
#include "xud_loader.h"
#include "xud_vulkan.h"
#include <assert.h>
#include <vulkan/vulkan.h>

/*-----------------------------------------------------------------------------------------------
 * Why this pair is xud_physical and not xud_device
 *-----------------------------------------------------------------------------------------------*/

/* The name is load-bearing, and it is load-bearing before the collision can happen. S6 owns
   VkDevice, and xud_device.h/.c is the name that layer wants the moment it lands; this pair
   would collide with it. So the physical layer is xud_physical from the start and the two
   never trade names: XudPhysicalDevice holds a VkPhysicalDevice, which is a query result
   (never created, never destroyed - vkDestroyPhysicalDevice does not exist in these headers),
   and XudDevice holds the VkDevice that S6 creates from it. Renaming either pair later would
   move a file-top comment, a guard, a type and a CMake line together for no gain; this rule
   is here so nobody is tempted. */

/*-----------------------------------------------------------------------------------------------
 * Caps storage bounds
 *-----------------------------------------------------------------------------------------------*/

/* Bounds of the doubling-capacity queries this layer runs - vkEnumerateDeviceExtensionProperties
   and vkGetPhysicalDeviceQueueFamilyProperties2. Both have an input/output count and no count
   only form, so a count cannot be read without an array, which is the same shape as xud_instance
   _query_extensions in xud_instance.c and xud_diagnostics_query_layers in xud_diagnostics.c
   (verified there). The heap array this layer allocates for the advertised device extensions is
   sized on this sequence, and its capacity is recorded in the caps next to it. */
#define XUD_PHYSICAL_QUERY_MIN_CAPACITY 16u
#define XUD_PHYSICAL_QUERY_MAX_CAPACITY 512u

/* queueFamilyIndex before a family has been chosen. Not an index: the queue-family query above
   cannot return an index at or above XUD_PHYSICAL_QUERY_MAX_CAPACITY, which is what the assert
   below checks, so "no family yet" can never be mistaken for family 512. */
#define XUD_PHYSICAL_NO_QUEUE_FAMILY 0xFFFFFFFFu

static_assert(XUD_PHYSICAL_QUERY_MIN_CAPACITY >= 1u, "a query must ask for at least one row");
static_assert(XUD_PHYSICAL_QUERY_MIN_CAPACITY <= XUD_PHYSICAL_QUERY_MAX_CAPACITY,
    "the doubling must start at or below its cap");
static_assert(XUD_PHYSICAL_QUERY_MAX_CAPACITY <= (0xFFFFFFFFu / 2u),
    "doubling the cap must stay inside uint32_t");
static_assert(XUD_PHYSICAL_NO_QUEUE_FAMILY > XUD_PHYSICAL_QUERY_MAX_CAPACITY,
    "the no-family sentinel must be outside every index the query can return");

/* The advertised device-extension list is a heap array of these, so one record must be one
   fixed-size row: extensionName[VK_MAX_EXTENSION_NAME_SIZE] then one uint32_t, no padding in
   between (VK_MAX_EXTENSION_NAME_SIZE is 256 here, a multiple of 4). If a future header ever
   changed the shape, the array arithmetic in xud_physical.c would be wrong, not this layer's
   decision - so the assumption is checked, not trusted. */
static_assert(sizeof(VkExtensionProperties) == VK_MAX_EXTENSION_NAME_SIZE + sizeof(uint32_t),
    "one advertised device extension is one fixed-size record");

/* The apiVersion read-back below compares against the floor the instance layer asks for, and
   one macro owns that number (xud_instance.h) - seen through the include above, never a second
   macro that could drift. The read-back's meaning depends on that floor being at least 1.0. */
static_assert(XUD_INSTANCE_API_FLOOR >= VK_API_VERSION_1_0,
    "the apiVersion read-back assumes the instance floor is at least Vulkan 1.0");

/*-----------------------------------------------------------------------------------------------
 * The caps: what the driver said, recorded once
 *
 * AGENTS.md's Vulkan rule this obeys: every extension and feature is queried and recorded
 * before it is used, and VkDeviceCreateInfo carries only what was verified here. Everything
 * below is a record of the driver's answer, not of what xudlibc decided to ask for - the
 * intersection S6 hands vkCreateDevice is derived from caps.features by xud_physical_record
 * _features, not stored, so the caps stay exactly what the driver said.
 *
 * The three *2 structs are copied in with pNext NULL. A driver may write chained structures
 * into a *2 chain only for structures the application provided there, this layer provides
 * none, so pNext stays NULL out and freeing the caps never walks a chain. If a chained
 * struct is ever added, it must be owned by XudPhysicalDevice next to the array below,
 * never smuggled into a pNext chain that xud_physical_close() cannot see.
 *
 * The caps are zeroed as part of the one allocation that owns them, before any query writes
 * them: four of the getters this layer calls return void (see xud_physical_open), so "the
 * struct still says nothing" is the only failure signal they can give, and a zeroed struct is
 * what makes "said nothing" observable - apiVersion 0 then fails the floor check below.
 *-----------------------------------------------------------------------------------------------*/

typedef struct XudDeviceCaps {
    VkPhysicalDeviceProperties2
        properties; /* sType + pNext (NULL in, NULL out) + the core properties: apiVersion,
                       driverVersion, vendorID, deviceID, deviceType,
                       deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE], pipelineCacheUUID, limits,
                       sparseProperties */
    VkPhysicalDeviceFeatures2 features; /* the driver's answer, verbatim; the wanted ∩ this is what
                                           S6 enables, derived, not stored */
    VkPhysicalDeviceMemoryProperties2
        memoryProperties; /* the allocator's input: memoryTypeCount +
                             memoryTypes[VK_MAX_MEMORY_TYPES] + memoryHeapCount +
                             memoryHeaps[VK_MAX_MEMORY_HEAPS] */
    VkExtensionProperties*
        deviceExtensionProperties; /* heap array of the driver's own device-level extensions
                                      (pLayerName NULL), NULL until the query filled it;
                                      xud_physical_close() frees it and nothing else here */
    uint32_t deviceExtensionCount; /* names the driver wrote */
    uint32_t
        deviceExtensionCapacity; /* rows allocated for it, a step of the doubling sequence above */
    uint32_t apiVersion; /* read back from properties.properties.apiVersion by
                            xud_physical_record_api_version */
    uint32_t queueFamilyCount; /* families the query wrote, 0 until it ran */
    uint32_t queueFamilyIndex; /* the chosen family, XUD_PHYSICAL_NO_QUEUE_FAMILY until
                                  xud_physical_select_queue_family chose one */
    VkQueueFamilyProperties2
        chosenQueueFamilyProperties; /* the chosen family, copied once here: queueFlags
                                        (VK_QUEUE_GRAPHICS_BIT is what the rule checked), queueCount
                                        (S6's VkQueueCreateInfo input), timestampValidBits,
                                        minImageTransferGranularity - queried once, S6 and S7 must
                                        not re-query */
} XudDeviceCaps;

typedef struct XudPhysicalDevice {
    VkPhysicalDevice physicalDevice; /* the handle: VK_NULL_HANDLE until xud_physical_open() fills
                                        it, and never destroyed - there is nothing to destroy */
    XudDeviceCaps caps; /* embedded by value, one allocation with the struct: the caps are this
                           handle's, they never outlive it */
} XudPhysicalDevice;

/*-----------------------------------------------------------------------------------------------
 * The layer's contract
 *-----------------------------------------------------------------------------------------------*/

/// Driver: enumerate the physical devices an instance can drive, into a fresh heap array sized
/// on the doubling sequence above (vkEnumeratePhysicalDevices has no count-only form: pCount is
/// input/output, so the count cannot be read without an array - the same shape as xud_instance
/// _query_extensions in xud_instance.c and xud_diagnostics_query_layers in xud_diagnostics.c,
/// verified there). On XUD_OK *outCandidates is that array - the caller owns it and frees it
/// with free(), xud_physical_open() is the only caller and frees it on every path - and *out
/// Count how many handles the driver wrote; both are NULL on every failure path. XUD_INVALID
/// _ARG on a NULL loader/out pair or a NULL instance/VK_NULL_HANDLE; XUD_UNSUPPORTED when the
/// loader is closed or its vkEnumeratePhysicalDevices slot is nil (a driver that does not know
/// the name, reported not guessed at); XUD_VULKAN_ERROR when the driver refuses or cuts the lis
/// t at the cap (VK_INCOMPLETE is not an error in Vulkan, but a truncated device list is unusab
/// le here - the same mapping xud_result_from_vk applies).
/// The loader parameter is part of the contract, not an afterthought: this query calls the drive
/// r through a slot of the loader's table, and there is no library-wide loader to reach (AGENTS.m
/// d: no global mutable state except the single context pointer), so a query that names the drive
/// r must be handed the table it goes through. xud_physical.h declared it without one at S5.2; th
/// e doc comment above already described the slot rule, and S5.4, the step that implements this qu
/// ery, is where that lands.
XudResult xud_physical_query_devices(const XudLoader* loader, VkPhysicalDevice** outCandidates,
    uint32_t* outCount, VkInstance instance);

/// Pure: choose one of the enumerated handles, given what each one reported. The policy today
/// is "first in driver order" (the plan's note: first for now, a ranking later) - so the answer
/// is index 0 whenever there is a candidate at all. `properties` is the policy's input: the
/// fields a ranking would read are the core ones inside the wrapper (deviceType - the enum at
/// vulkan_core.h:2233-:2238, vendorID, deviceName, pipelineCacheUUID), and the array is part of
/// the signature so that a ranking can be added without touching any call site. It is delibera
/// tely not consulted by the first-for-now policy, and that is stated here rather than left to
/// be discovered in the .c: the architecture id a discrete-preference policy would rank on is
/// not available, VkPhysicalDeviceArchitectureInfo is not in these headers at all (grepped: zer
/// o hits), so all a ranking could sort on is core deviceType plus vendorID/deviceName - enough
/// to prefer a discrete GPU, not enough to identify one - and whether to do that is a decision
/// for the plan's "policy later", not something this layer invents.
/// XUD_UNSUPPORTED when candidateCount is 0 (no device at all - the same answer xud_vulkan_sup
/// ported() gives when there is no driver, and the reason xud_physical_open() fails instead of
/// recording a NULL handle); XUD_INVALID_ARG on a NULL candidates/properties array with a
/// non-zero count, or a NULL out-index. *outIndex is written only on XUD_OK - on every other
/// path it stays untouched, it is an index into the caller's own array and there is no sentinel
/// for "no device" here (the sentinel above is a queue-family index, a different thing).
XudResult xud_physical_select(uint32_t* outIndex, const VkPhysicalDevice* candidates,
    const VkPhysicalDeviceProperties2* properties, uint32_t candidateCount);

/// Pure: the VkPhysicalDeviceFeatures S6 hands to vkCreateDevice - what xudlibc wants that the
/// driver actually reported. Every field xudlibc does not want stays VkFalseString, and every
/// field the driver reported as VkFalseString stays VkFalseString: nothing is ever forced on
/// (AGENTS.md: an unverified feature stays off and the feature that needed it returns XUD_SUP
/// PORTED later). The wanted list is this layer's decision and lives in xud_physical.c, docu
/// mented there - one owner per wanted list, the same rule xud_instance.c and xud_diagnostics
/// .c follow for their extension lists. `caps` is read for caps.features, the driver's verba
/// tim answer, so the caps stay exactly what the driver said and this answer is derived from
/// them on every call.
/// XUD_INVALID_ARG on a NULL out-struct or NULL caps.
XudResult xud_physical_record_features(
    VkPhysicalDeviceFeatures* outEnabledFeatures, const XudDeviceCaps* caps);

/// Pure: choose the one queue family the rest of the library will use - AGENTS.md's one-family
/// rule, so the answer is recorded in the caps and S6/S7 never query again. The rule is a family
/// whose queueFlags carry VK_QUEUE_GRAPHICS_BIT; the first such family in driver order wins.
/// VK_QUEUE_PRESENT_BIT does not exist in these headers (grepped across the whole vulkan include
/// tree: zero hits) - presentation is not a queue flag, it is a per-window-system query (vkGet
/// PhysicalDeviceWaylandPresentationSupportKHR, Xcb, Xlib, Win32, DirectFB, UBM, Screen: the
/// system-specific set, no core form), which is what GLFW's glfwGetPhysicalDevicePresentation
/// Support does at step S7. So the family chosen here is graphics-checked only, and S7 is the
/// step that refuses a surface on a family that cannot present - that is the plan's "a graphics
/// -only family must not be chosen", carried by the graphics rule here plus GLFW's check there,
/// not by a flag bit that does not exist. transfer/compute/granularity are recorded, not
/// required: the caps keep the whole row, so the layers above can read queueFlags, queueCount
/// (S6's VkQueueCreateInfo input), timestampValidBits and minImageTransferGranularity without
/// querying again.
/// XUD_UNSUPPORTED when no family carries the graphics bit (a compute-only driver: there is
/// nothing here to draw with) and *outIndex is then set to XUD_PHYSICAL_NO_QUEUE_FAMILY; XUD
/// _INVALID_ARG on a NULL families array with a non-zero count, a NULL out-index, or a NULL
/// out-chosen row.
XudResult xud_physical_select_queue_family(uint32_t* outIndex, VkQueueFamilyProperties2* outChosen,
    const VkQueueFamilyProperties2* families, uint32_t familyCount);

/// Pure: read caps.properties.properties.apiVersion back into caps.apiVersion and compare it
/// with the floor the instance layer asked for (XUD_INSTANCE_API_FLOOR, xud_instance.h - one
/// macro owns that number, seen through the include above). XUD_OK when the driver is at or
/// above the floor: at Vulkan 1.1 and up the *2 forms this layer used are core, at the 1.0
/// floor they exist only through VK_KHR_get_physical_device_properties2, which is what xud
/// _instance.c enables - and the loader slot is filled either way (see xud_loader.h), which is
/// why the slot being present is not the test here. XUD_UNSUPPORTED below the floor: the driver
/// cannot answer the queries this layer took, and xud_physical_open() fails rather than record
/// a device it cannot drive. XUD_INVALID_ARG on a NULL caps.
XudResult xud_physical_record_api_version(XudDeviceCaps* caps);

/// Driver: bring the physical layer up - allocate the struct, bind the **six** slots this layer
/// calls through the live instance (xud_loader_bind, the same helper xud_instance.c uses: these
/// names are instance-level, they resolve through a VkInstance and are cached in the table). The
/// loader parameter is not const for exactly that reason: xud_loader_bind takes XudLoader * and a
/// dstSlot of void *, so a const XudLoader * is rejected twice over by both compilers here - the
/// argument discards const, and the address of a const member discards it again (verified with a
/// scratch TU under the project flags, gcc -Werror=discarded-qualifiers and clang
/// -Wincompatible-pointer-types-discards-qualifiers). The seventh physical name the table holds,
/// the non-2 vkGetPhysicalDeviceProperties form, is slotted for a ranking policy that has to work
/// with no extension in hand; this layer calls the *2 forms only. Then enumerate the devices, sel
/// ect one, copy VkPhysicalDeviceProperties2 / VkPhysicalDeviceFeatures2 / VkPhysicalDeviceMemor
/// yProperties2 into the caps, query the driver's own device-level extensions (pLayerName NULL) i
/// nto the caps' heap array, query the queue families and record the chosen one plus its index, a
/// nd read the apiVersion back. Every slot is checked before the call that needs it (xud_loader_i
/// s_open plus a nil-slot check -> XUD_UNSUPPORTED, the same check xud_instance.c applies to crea
/// teInstance): a name the loader does not know is reported, never called through. **The four cap
/// s getters return void in these headers** (PFN_vkGetPhysicalDeviceFeatures2 :6178, PFN_vkGetPhy
/// sicalDeviceProperties2 :6179, PFN_vkGetPhysicalDeviceMemoryProperties2 :6183, PFN_vkGetPhysic
/// alDeviceQueueFamilyProperties2 :6182 - grepped), so there is no VkResult for xud_result_from_
/// vk to map and the only failure signal those four can give is "the struct still says nothing":
/// every caps field is at a valid state **before** the call that fills it - the struct is calloc
/// ed, so the core payloads are zero and a driver that answers nothing leaves apiVersion 0, which
/// xud_physical_record_api_version then rejects, and each *2 wrapper's own sType is stamped befor
/// e the call because lavvk leaves the wrapper untouched (probed here: sType 0 in, 0 out) and an
/// unstamped caps row would claim nothing. The two that do return a VkResult are vkEnumeratePhysi
/// calDevices (the typedef at :4532) and vkEnumerateDeviceExtensionProperties (:4544). On XUD_OK
/// *outPhysicalDevice owns a live handle plus the caps recorded from it; on any error it stays NU
/// LL and everything allocated so far is freed, in reverse creation order, and nothing is destroy
/// ed - there is nothing to destroy at this layer. The three *2 caps structs' pNext is checked, n
/// ot assumed: this layer provided no chained struct, so a driver that wrote one is a driver this
/// layer cannot hand a caps struct from. XUD_INVALID_ARG on a NULL out-pointer or one that alrea
/// dy owns a device (nothing is created twice into the same slot), a NULL loader or a NULL instan
/// ce/VK_NULL_HANDLE; XUD_UNSUPPORTED when the loader is closed, a slot this layer needs is nil,
/// no device was enumerable, no graphics family was found, or the driver is below the instance fl
/// oor; XUD_OUT_OF_MEMORY/XUD_OUT_OF_RESOURCES when an allocation or a query at the cap cannot be
/// satisfied; XUD_VULKAN_ERROR when the driver refuses a query that does answer with a VkResult,
/// or chained a struct into a *2 pNext this layer never asked for.
XudResult xud_physical_open(
    XudPhysicalDevice** outPhysicalDevice, XudLoader* loader, VkInstance instance);

/// The free-only rung of the teardown ladder: Vulkan has no vkDestroyPhysicalDevice (grepped in
/// these headers, the name does not exist) because a VkPhysicalDevice is a query result, not an
/// object the driver allocated for us - so this frees the caps' one heap array (deviceExtension
/// Properties, NULL when the query never ran) and the struct, sets the pointer NULL, and calls
/// no driver function at all. That is why it takes no loader: there is nothing to resolve.
/// xud_close_context() calls it before xud_instance_destroy(), in reverse creation order, and
/// the instance must still be alive for the layers above it, not for this one.
/// XUD_ALREADY when the pointer is NULL or points at an already-closed device; XUD_INVALID_ARG
/// on a NULL pointer-to-pointer (the arguments are checked before the state, the same rule
/// xud_diagnostics_close() follows).
XudResult xud_physical_close(XudPhysicalDevice** physicalDevice);

#endif /* XUD_XUD_PHYSICAL_H_ */
