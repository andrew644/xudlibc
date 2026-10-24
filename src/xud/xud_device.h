/***********************************************************************************************
 *
 *   xud_device.h - the device layer: one VkDevice per context, the queues it was created
 *          with, and the teardown guard
 *
 *   Owns : XudDevice (the VkDevice handle, the one queue row vkCreateDevice was handed, the
 *          queue handles resolved from that row, the features that were enabled, and the
 *          device-level extension names that were enabled) and the contract of the device
 *          layer - queue-row assembly, extension selection against the recorded caps, create-
 *          info assembly, queue selection, the vkDeviceWaitIdle teardown guard, teardown.
 *   Layer: above the physical device, below the swapchain. It reads the physical layer's
 *          recorded caps and records none of its own: the row comes from caps.queueFamilyInde
 *          x + caps.chosenQueueFamilyProperties, the features come from the intersection xud_p
 *          hysical_record_features derived from caps.features, and the extension names come from
 *          caps.deviceExtensionProperties. That is what makes AGENTS.md's "VkDeviceCreateInfo
 *          carries only what was verified" rule a property of the input type rather than a ch
 *          eck inside this layer - this layer cannot be handed a feature or a name the caps do
 *          not mention, because the only inputs it takes are the caps.
 *          Every Vulkan call the .c makes goes through a row of the loader's struct VolkDeviceTa
 *          ble (xud_loader.h), filled in bulk by xud_loader_bind_device once this layer has a Vk
 *          Device. The four names this layer calls (vkCreateDevice, vkDestroyDevice, vkGetDevice
 *          Queue, vkDeviceWaitIdle) are rows of that table, and they are still reached through t
 *          he live VkInstance handed in: xud_loader_bind_device() fills the device table by resol
 *          ving through vkGetDeviceProcAddr, and that entry point is *looked up* through the ins
 *          tance - which is why Volk keeps it in struct VolkInstanceTable - so this layer still n
 *          eeds an instance in hand and never a VkDevice to resolve through. Probed 2026-10-18 a
 *          gainst the instance xud_instance.c creates and against a bare one at apiVersion 1.0 w
 *          ith no extension and no layer: vkGetInstanceProcAddr answers non-NULL for all four an
 *          d NULL for all four with VK_NULL_HANDLE in hand, which is the reason the four are rea
 *          chable at all. vkGetDeviceProcAddr is now a row of the instance table and the one run
 *          g the device table is filled through - the loader step that landed today hands it ove
 *          r rather than leaving it for the renderer, which is still the layer that would resolv
 *          e command-record entry points.
 *          Two header facts this layer's shape depends on, grepped in Vulkan-Headers 1.4.363 here
 *          (2026-10-18, no driver involved): PFN_vkCreateDevice (:4541) takes **four** arguments -
 *          the VkPhysicalDevice this layer was handed a handle for, the create-info, the allocator
 *          and the out handle - so the legacy ppEnabledLayerNames argument the spec's own page sti
 *          ll shows is not in this signature and VkDeviceCreateInfo's two layer fields are writte
 *          n 0/NULL rather than left out; and PFN_vkDestroyDevice (:4542) and PFN_vkGetDeviceQueu
 *          e (:4547) return **void**, the same shape as the four void *2 caps getters xud_physical
 *          .h warns about, so there is no VkResult for xud_result_from_vk to map for those two an
 *          d every out handle is at a valid zero state before the call that fills it and is check
 *          ed after it rather than trusted. PFN_vkDeviceWaitIdle (:4550) is the one call here th
 *          at does answer with a VkResult.
 *          Internal like xud_loader.h: tests may include it, user code must not.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#ifndef XUD_XUD_DEVICE_H_
#define XUD_XUD_DEVICE_H_

#include "volk.h" /* Required for: VK_NO_PROTOTYPES - must precede <vulkan/vulkan.h>, see xud_loader.h */
#include "xud.h"
#include "xud_instance.h"
#include "xud_loader.h"
#include "xud_physical.h"
#include "xud_vulkan.h"
#include <assert.h>
#include <vulkan/vulkan.h>

/*-----------------------------------------------------------------------------------------------
 * Why this pair is xud_device and not the plan's xud_device_queue
 *-----------------------------------------------------------------------------------------------*/

/* xud_physical.h's naming rule, one rung up: the name is load-bearing before the collision
   can happen. The plan calls this step xud_device_queue, and that name is not used here. A
   queue cannot exist without the device that owns it - vkGetDeviceQueue takes a VkDevice and
   there is no vkDestroyQueue, a queue handle is a projection of the device's own state that
   dies with it - so the two objects have exactly one owner, one create-info and one teardown.
   XudDevice is that owner, and XudQueue below is a record of a queue, not an owning type: no
   thing frees one of these, and nothing may hold one past xud_device_close(). */

/*-----------------------------------------------------------------------------------------------
 * The queue row: one family, the two roles the frame loop needs
 *-----------------------------------------------------------------------------------------------*/

/* The two roles the frame loop submits to. These are **not** Vulkan queue flags: VK_QUEUE_PRESE
   NT_BIT does not exist in these headers (xud_physical.h's grep across the whole vulkan include
   tree: zero hits), so "which queue do I present on" is this layer's decision and not a questio
   n the driver answers. The family itself was chosen by the physical layer on the graphics bit
   plus GLFW's presentation check at S7. */
typedef enum XudQueueRole {
    XUD_QUEUE_ROLE_GRAPHICS = 0, /* the queue draws are submitted to   */
    XUD_QUEUE_ROLE_PRESENT = 1 /* the queue presents are submitted to */
} XudQueueRole;
#define XUD_DEVICE_QUEUE_ROLE_COUNT 2u

/* The ceiling on the queues this layer asks the chosen family for: the row asks for
   min(family.queueCount, this). A driver must give every queue family at least one queue, so a
   family that rowed exactly one still works and both roles name that same queue - which is what
   the driver here does (probed 2026-10-18: one family, queueCount 1, and vkGetDeviceQueue hands
   back the same handle for both roles). Asking for more than the roles need would be a request
   nothing above this layer can use. */
#define XUD_DEVICE_QUEUE_MAX_COUNT 2u

/* vkDeviceQueueCreateInfo's pQueuePriorities values must be inside [0,1], higher wins. The defau
   lt is Vulkan's own default (1.0f): neutral, so a driver is free to reorder the two queues if it
   wants to. The row's priorities are not a policy today - the plan's "one queue family, one row"
   says nothing about ordering - and the neutral default is what says nothing looks like. The range
   rule is stated here rather than asserted: a float compare is not an integer constant expression i
   n C11, and both compilers here reject folding one (gcc: "expression in static assertion is not an
   integer constant expression", clang: "folding it to a constant is a GNU extension"), so the three
   macros below are the only compile-time part of the rule, and xud_device.c sets every entry of the
   row to the default, which is inside the range by construction. */
#define XUD_DEVICE_QUEUE_PRIORITY_MIN 0.0f
#define XUD_DEVICE_QUEUE_PRIORITY_MAX 1.0f
#define XUD_DEVICE_QUEUE_PRIORITY_DEFAULT 1.0f

/* Fixed storage bound for the device-level extension names enabled in one device, the same shape
   as XUD_MAX_INSTANCE_EXTENSIONS in xud_instance.h. The wanted list itself lives in xud_device.c
   (one owner per wanted list), and xud_device.c asserts the wanted list fits this bound. */
#define XUD_DEVICE_MAX_DEVICE_EXTENSIONS 8u

/* The two sType stamps of this layer's two structs. Both are non-zero and distinct in these hea
   ders (VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO = 2 at vulkan_core.h:212, VK_STRUCTURE_TYPE_D
   EVICE_CREATE_INFO = 3 at :213), and the trap this layer exists to avoid is writing one where t
   he other belongs - the validation layer reports it (probed 2026-10-18: "pCreateInfo->sType mus
   t be VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO" and "pQueueCreateInfos[0].sType must be VK_STRUCTUR
   E_TYPE_DEVICE_QUEUE_CREATE_INFO"), but a compile-time check is cheaper than a layer. Note the
   naming trap the stamps record: the struct is VkDeviceQueueCreateInfo, and its stamp is VK_STRU
   CTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, while the queue-family wrapper xud_physical.h warns about
   is VkQueueFamilyProperties2 - three names, three different prefixes. */
static_assert(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO != VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
    "the two create-info stamps of this layer must be distinct");
static_assert(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO != 0u,
    "a stamp of zero is the unstamped state, and neither struct may ever be stamped zero");
static_assert(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO != 0u,
    "a stamp of zero is the unstamped state, and neither struct may ever be stamped zero");

static_assert(XUD_DEVICE_QUEUE_MAX_COUNT >= 1u,
    "a device with no queue row is not a device this layer can create");
static_assert(
    XUD_DEVICE_MAX_DEVICE_EXTENSIONS >= 1u, "the extension storage must hold at least one name");

/*-----------------------------------------------------------------------------------------------
 * The records this layer keeps
 *-----------------------------------------------------------------------------------------------*/

/* One queue of the chosen family, as this layer records it: the handle plus the coordinates it w
   as resolved at. The coordinates are here on purpose - the validation layer's two queue rules
   (VUID-vkGetDeviceQueue-queueFamilyIndex-00384: the family asked for must be one of the familie
   s the device was created with, and VUID-vkGetDeviceQueue-queueIndex-00385: the index must be be
   low the count asked for in that family's row) are checked against what vkCreateDevice was hand
   ed, and a caller that can see the pair can check the same two rules before the call instead of
   after the layer has printed them. Not an owning type: there is no vkDestroyQueue, a queue hand
   le lives exactly as long as the device that owns it does. */
typedef struct XudQueue {
    VkQueue queue; /* VK_NULL_HANDLE until this layer resolved it */
    uint32_t queueFamilyIndex; /* the family it was resolved from - always the device's own row's */
    uint32_t queueIndex; /* which queue of that family this handle is */
} XudQueue;

typedef struct XudDevice XudDevice;
struct XudDevice {
    VkDevice device; /* VK_NULL_HANDLE until xud_device_open() creates it, and destroyed by
                        xud_device_close() - this is the one Vulkan object the layer owns */
    VkDeviceQueueCreateInfo
        queueRow; /* the one row vkCreateDevice was handed, kept as this layer's own record of what
                     it asked for: the accessor below checks a queue request against this row, and
                     the physical layer's caps are queried once, so nothing above re-queries the
                     families */
    float queuePriorities[XUD_DEVICE_QUEUE_MAX_COUNT]; /* what queueRow.pQueuePriorities points at -
                                                          the row's pointer must stay valid as long
                                                          as the record is read, which is why the
                                                          array lives beside it in the same heap
                                                          block rather than on a frame that ends */
    VkPhysicalDeviceFeatures
        enabledFeatures; /* what was enabled: the physical layer's wanted-intersection, copied here
                            so the record says what the device was created with, not what the caps
                            say the driver can do */
    uint32_t enabledExtensionCount; /* how many rows of enabledExtensionNameStorage are live */
    char enabledExtensionNameStorage[XUD_DEVICE_MAX_DEVICE_EXTENSIONS]
                                    [VK_MAX_EXTENSION_NAME_SIZE]; /* the names, copied into fixed
                                                                     buffers, the instance layer's
                                                                     shape */
    XudQueue graphicsQueue; /* the XUD_QUEUE_ROLE_GRAPHICS queue, resolved once at open */
    XudQueue presentQueue; /* the XUD_QUEUE_ROLE_PRESENT queue, resolved once at open - the same
                              handle when the family rowed one queue */
};

/*-----------------------------------------------------------------------------------------------
 * The layer's contract
 *-----------------------------------------------------------------------------------------------*/

/// Pure: the one VkDeviceQueueCreateInfo (the plan's "one VkQueueCreateInfo" - these headers name
/// the struct VkDeviceQueueCreateInfo and stamp it VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO)
/// the device will be created with, from the physical layer's chosen family. The row is stamped
/// and its pNext cleared before it is ever handed to the driver, the same rule xud_physical.c fo
/// llows for every *2 wrapper it stamps: the validation layer refuses an unstamped row (probed 20
/// 26-10-18: "pCreateInfo->pQueueCreateInfos[0].sType must be VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREAT
/// E_INFO"), and lavvk does not stamp it for us. `priorities` is the array the row's pQueuePriorit
/// ies points at - it must have at least queueCount entries and it must outlive the call, which is
/// why xud_device_open() passes an array of its own struct rather than a frame-local. The row asks
/// for min(caps.chosenQueueFamilyProperties.queueFamilyProperties.queueCount, XUD_DEVICE_QUEUE_MAX_
/// COUNT) queues from caps.queueFamilyIndex, all at XUD_DEVICE_QUEUE_PRIORITY_DEFAULT. flags stays
/// 0: VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT would put the whole protected-memory discipline above t
/// his layer, nothing above it asks for that today, and asking for it is a decision, not a default.
/// XUD_INVALID_ARG on a NULL out-row or NULL priorities array; on caps whose queueFamilyIndex is s
/// till XUD_PHYSICAL_NO_QUEUE_FAMILY (the physical layer's own open refuses that state, so caps in
/// that shape are not a usable input); on caps whose chosen row says queueCount 0 (a family with no
/// queue in it is not a family this layer can ask for - the physical layer's rule already insists a
/// chosen row has at least one).
XudResult xud_device_fill_queue_create_info(
    VkDeviceQueueCreateInfo* outQueueRow, const float* priorities, const XudDeviceCaps* caps);

/// Pure: which of the driver's advertised device-level extensions this layer enables - the ones
/// xudlibc wants that the caps actually advertise, in wanted order (the wanted list itself lives
/// in xud_device.c, documented there, one owner per wanted list like xud_instance_select_extensio
/// ns and the diagnostics layer's list). Nothing is required: a name the caps do not advertise si
/// mply stays out of the list, and the layer that needs it answers XUD_UNSUPPORTED above it (AGEN
/// TS.md's check-before-you-use rule, and the reason S7's swapchain layer refuses rather than cal
/// ling vkCreateSwapchainKHR on a device that was not created for one). A name already in the lis
/// t is not added twice, and the selection is rewritten on every call so a reused struct cannot k
/// eep a stale list. `advertised` is the caps' own array - a query result, not something this laye
/// r allocates.
/// XUD_INVALID_ARG on a NULL out-device or a NULL advertised array with a non-zero count; XUD_OUT_
/// OF_RESOURCES when the wanted set does not fit XUD_DEVICE_MAX_DEVICE_EXTENSIONS (the assert in x
/// ud_device.c is what keeps that from happening today, and the arm is here so a future wanted lis
/// t that overflows is a refusal rather than an overflow).
XudResult xud_device_select_extensions(
    XudDevice* outDevice, const VkExtensionProperties* advertised, uint32_t advertisedCount);

/// Pure: the VkDeviceCreateInfo the device will be created with, assembled from the device's own
/// record - stamped, pNext NULL (this layer chains nothing into the device's pNext: the caps' thre
/// e *2 chains came back empty on this driver, probed 2026-10-18, so there is nothing to carry and
/// a driver that chained a struct xud_physical.c could not model was refused at the caps there), on
/// e queue row (queueCreateInfoCount 1, pQueueCreateInfos -> the device's own queueRow), the extens
/// ion names through `nameSlots` (a caller-provided array of XUD_DEVICE_MAX_DEVICE_EXTENSIONS
/// slots, filled from the device's storage and always fully NULLed past the live count so a stale
/// pointer cannot survive - the instance layer's shape), and pEnabledFeatures -> the device's own
/// enabledFea tures. enabledLayerCount/ppEnabledLayerNames are set to 0/NULL because these headers
/// say they are legacy and not used (the comment above both fields in vulkan_core.h:3747/:3749
/// reads "legacy and not used" from Vulkan-Headers 1.4.363 here) - the layers this library enables
/// are instance-level and are the diagnostics layer's answer, carried on VkInstanceCreateInfo,
/// never here. XUD_INVALID_ARG on a NULL out-create-info or NULL nameSlots; on a device whose
/// queueRow is not a t the state this layer's own fill helper produces (unstamped, or a
/// pQueuePriorities that does not point at the struct's own priorities array - a row whose pointer
/// points at a frame that ended is exactly the bug this struct layout exists to prevent).
XudResult xud_device_fill_create_info(VkDeviceCreateInfo* outCreateInfo,
    const char* nameSlots[XUD_DEVICE_MAX_DEVICE_EXTENSIONS], const XudDevice* device);

/// Pure: the queue a caller above this layer submits to, for one of the two roles - a lookup into
/// what xud_device_open() resolved, no driver call, so the headless run covers it. The answer is th
/// e handle plus the coordinates it was resolved at, so the caller can see which queue of which
/// fami ly it holds. With one queue in the row both roles answer the same handle and the same
/// coordinates (probed 2026-10-18: this driver's one family rowed one queue, and vkGetDeviceQueue
/// hands back the same handle for both roles - presenting on the graphics queue is legal,
/// presentation is not a queu e flag, see xud_physical.h). XUD_INVALID_ARG on a NULL out-record or
/// NULL device; on a device with no handle (VK_NULL_HANDLE - there is no queue to hand back for a
/// device that does not exist); on a role at or above XUD_DEVICE_QUEUE_ROLE_COUNT (the enum has two
/// values and a caller can still pass 7, and the two validation-layer VUIDs above are what a wrong
/// pair would trip, so the pair is check ed here before it is ever asked).
XudResult xud_device_queue_get(XudQueue* outQueue, const XudDevice* device, XudQueueRole role);

/// Driver: bring the device layer up - bind the four slots this layer calls through the live
/// VkInsta nce (xud_loader_bind, the same helper xud_physical_open uses: these names are
/// instance-level, they resolve through a VkInstance and are cached in the table, and the loader
/// parameter is non-const for exactly that reason, the rule xud_physical.c states and both
/// compilers enforce), assemble the row an d the create-info from the caps handed in, create the
/// device **from physical->physicalDevice** (PFN_ vkCreateDevice takes four arguments in these
/// headers - the physical handle, the create-info, the all locator, the out handle - so the device
/// is created from the handle the layer below chose, which is th e link between the two layers),
/// then resolve the two role queues with vkGetDeviceQueue through the row' s own family index and
/// index. vkGetDeviceQueue returns **void** in these headers (:4547), so there is n o VkResult for
/// xud_result_from_vk to map for it: the out handle is VK_NULL_HANDLE before the call and a handle
/// that is still VK_NULL_HANDLE after it is refused (XUD_VULKAN_ERROR - the driver claimed success
/// b y doing nothing), the same zero-state-before/check-after discipline xud_physical.h states for
/// the four v oid *2 caps getters. Every slot is checked before the call that needs it
/// (xud_loader_is_open plus a nil- slot check -> XUD_UNSUPPORTED, the same check xud_instance.c
/// applies to createInstance): a name the loade r does not know is reported, never called through.
/// On XUD_OK *outDevice owns a live VkDevice plus the queu es resolved from it; on any error it
/// stays NULL and everything created so far is torn down in reverse crea tion order - the device is
/// destroyed through the slot this layer just bound (PFN_vkDestroyDevice returns v oid too, so that
/// teardown call has no answer to check), this layer's only Vulkan object. XUD_INVALID_ARG on a
/// NULL out-pointer or one that already owns a device, a NUL L loader, a NULL
/// instance/VK_NULL_HANDLE, or caps with no chosen family; XUD_UNSUPPORTED when the loade r is
/// closed, a slot this layer needs is nil, or the driver refuses a name it does not know;
/// XUD_OUT_OF_M EMORY when the struct cannot be allocated;
/// XUD_VULKAN_ERROR/XUD_LOST/XUD_OUT_OF_RESOURCES when the drive r refuses vkCreateDevice or one of
/// the queue lookups (VK_ERROR_FEATURE_NOT_PRESENT and VK_ERROR_EXTENSIO N_NOT_PRESENT both map to
/// XUD_UNSUPPORTED through xud_result_from_vk - a driver that will not create th e device for a
/// feature or a name we asked for is unsupported, not broken, and this layer asks only for w hat
/// the caps verified, so reaching that arm means the caps and the driver disagree).
XudResult xud_device_open(XudDevice** outDevice, XudLoader* loader,
    const XudPhysicalDevice* physical, VkInstance instance);

/// Driver: the teardown guard the plan names as xud_wait_idle (named with this layer's prefix here,
/// the repo's rule that every function is xudSnakeCase and layer-prefixed - xud_wait_idle would be
/// the one unprefixed name in the tree). vkDeviceWaitIdle through the cached slot, so a teardown
/// can not be racing an in-flight submission: after XUD_OK nothing is in flight on this device,
/// which is what makes it safe to call vkDestroyDevice next. XUD_ALREADY when the struct has no
/// handle (there is nothing in flight on a device that does not exist, and the state is already the
/// one asked for); XUD_UNSUPPORTED when the loader is closed or its vkDeviceWaitIdle slot is nil
/// (reported, never cal led through); XUD_LOST when the driver says the device is lost
/// (xud_result_from_vk's mapping of VK_ ERROR_DEVICE_LOST), which is the one answer that means
/// everything above this layer has to stop usi ng it. XUD_INVALID_ARG on a NULL loader or NULL
/// device.
XudResult xud_device_wait_idle(const XudDevice* device, const XudLoader* loader);

/// Driver: the teardown rung - the guard above first, then vkDestroyDevice through the cached slot,
/// then free. The order is the spec's: nothing may be in flight when a device is destroyed, and thi
/// s layer's own guard is what guarantees it rather than trusting the caller above to have drained
/// its submissions. A lost device is still destroyed - vkDestroyDevice is this layer's teardown cal
/// l and skipping it would leak an object the driver may still hold - and the XUD_LOST answer is st
/// ill propagated so the caller knows the device is gone. The struct's own memory is freed on ever
/// y path, so a teardown leaks nothing even when the driver refuses. **PFN_vkDestroyDevice returns
/// v oid in these headers (:4542)**, so this rung has no VkResult to map for the destroy itself:
/// the answer it returns is the guard's (XUD_LOST travels) or XUD_OK, and the destroy's own failure
/// is not reportable through this signature - the same shape as the four void *2 caps getters
/// xud_phys ical.h warns about. XUD_ALREADY when the pointer-to-pointer is NULL-in-handling (there
/// is no struct to tear down); a struct whose handle is already VK_NULL_HANDLE is freed and answers
/// XUD_OK (nothing to guard, not hing to destroy, and freeing is what this rung is for);
/// XUD_INVALID_ARG on a NULL pointer-to-poin ter or a NULL/closed loader (the arguments are checked
/// before the state, the same rule xud_physic al_close() follows); XUD_UNSUPPORTED when the guard's
/// slot is nil (the rung refuses: the device is not destroyed and the struct is not freed) or when
/// the destroy slot is nil while a handle is in h and (the handle would leak silently otherwise).
XudResult xud_device_close(XudDevice** device, const XudLoader* loader);

#endif /* XUD_XUD_DEVICE_H_ */
