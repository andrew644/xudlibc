/***********************************************************************************************
 *
 *   xud_instance.h - the Vulkan instance layer: one VkInstance per context
 *
 *   Owns : the XudInstance type (the handle plus the create-info that produced it), the pure
 *          parts of instance bring-up (application info, extension selection, create-info
 *          assembly), and the two driver calls that create/destroy the instance. It borrows the
 *          diagnostics layer's answer (xud_diagnostics.h) for the validation layers, the
 *          validation-feature request and the pNext chain - it does not own those.
 *   Layer: above the loader - every Vulkan call here goes through a row of the loader's Volk
 *          tables (struct VolkInstanceTable, filled in bulk by xud_loader_bind_instance once thi
 *          s layer has a VkInstance) - and below the physical device (which resolves through th
 *          e instance this layer fills) and the swapchain. Sees volk.h (VK_NO_PROTOTYPES, whic
 *          h must come before the Vulkan header) and <vulkan/vulkan.h> for declarations only; n
 *          o Vulkan symbol is linked (AGENTS.md: there is no -lvulkan anywhere in the build, an
 *          d under VK_NO_PROTOTYPES volk.h turns that rule into a link-time fact). Internal lik
 *          e xud_loader.h: tests may include it, user code must not.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#ifndef XUD_XUD_INSTANCE_H_
#define XUD_XUD_INSTANCE_H_

#include "volk.h" /* Required for: VK_NO_PROTOTYPES - must precede <vulkan/vulkan.h>, see xud_loader.h */
#include "xud.h"
#include "xud_diagnostics.h"
#include "xud_loader.h"
#include "xud_vulkan.h"
#include <assert.h>
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

/* How many instance-level extensions a layer above this one can ask to have enabled, on top of
   the diagnostics layer's wanted list: GLFW's window system answers its required list through
   glfwGetRequiredInstanceExtensions (VK_KHR_surface plus the window system's own name - VK_KHR_wa
   yland_surface for the vendored Wayland backend, probed here), and xud_window.h sizes its storage
   by this macro, so the list GLFW answers and the list this layer accepts cannot drift apart. One
   macro for one number, and the bound of the extra-names array below. */
#define XUD_INSTANCE_EXTRA_MAX 3u

/* The diagnostics layer's wanted instance-level extensions have exactly one mechanism here:
   xud_diagnostics_wanted_extensions() (xud_diagnostics.h), which xud_instance_create() asks
   before selecting - the accessor, not an array declared here. An `extern const char *const
   XUD_INSTANCE_DIAGNOSTICS_WANTED_EXTENSIONS[]` for the same list was deleted rather than left
   unused: a declaration nothing defines is not a compile error, it is an undefined symbol at
   link time, and reading it here would have been that trap. One owner per wanted list (this
   layer's own list is in xud_instance.c, the diagnostics layer's in xud_diagnostics.c), one
   place that decides what goes into VkInstanceCreateInfo.
   There is deliberately no second macro for the size of that list: the bound of the accessor's
   out-array is the diagnostics layer's own cap, XUD_DIAGNOSTICS_WANTED_MAX, which this header
   sees through the include above. Two macros for one number could drift apart silently and
   leave the arrays below one slot too small or too large; one macro cannot, and xud_instance.c
   asserts at compile time that this layer's wanted list plus the diagnostics one still fits
   XUD_MAX_INSTANCE_EXTENSIONS. */

typedef struct XudInstance {
    VkInstance instance; /* VK_NULL_HANDLE until xud_instance_create() fills it */
    VkApplicationInfo applicationInfo; /* what was asked for: names, versions, apiVersion floor */
    uint32_t enabledExtensionCount; /* how many rows of enabledExtensionNameStorage are live */
    char enabledExtensionNameStorage[XUD_MAX_INSTANCE_EXTENSIONS][VK_MAX_EXTENSION_NAME_SIZE]; /* the names, copied into fixed
                                                                                                  buffers */
} XudInstance;

/// The VkApplicationInfo every xudlibc instance carries: XUD_INSTANCE_NAME in both name
/// slots, the library version in both version slots, XUD_INSTANCE_API_FLOOR as the floor.
/// Pure - no driver call - so tests assert the floor and the names with no device present.
/// XUD_INVALID_ARG on a NULL out-param.
XudResult xud_instance_fill_application_info(VkApplicationInfo* outApplicationInfo);

/// Choose which of the advertised instance-level extensions to enable: the ones xudlibc
/// wants that the driver actually advertised, in wanted order (the wanted list itself lives
/// in xud_instance.c, documented there), then the ones `extraNames`/`extraCount` ask for
/// on top - today the diagnostics layer's list (VK_EXT_debug_utils, VK_EXT_validation_features),
/// documented in xud_diagnostics.c, followed by the window layer's GLFW-required list (VK_KHR
/// _surface plus the window system's own name, from xud_window.h). Nothing is required - an exten
/// sion that was not advertised simply stays out of the list, and the feature that needed it fails
/// later with XUD_UNSUPPORTED. A name already in the first list is not added twice.
/// Pure: `advertised` is just an array and `extraNames` just an array of names, so tests feed
/// synthetic ones and the selection is covered with no device present. The array is XUD_DIAGNOSTI
/// CS_WANTED_MAX + XUD_INSTANCE_EXTRA_MAX pointers wide - the widest list this header's own
/// `xud_instance_create()` can build - and the function reads `extraCount` entries, never past it.
/// Rewrites enabledExtensionCount/Storage on every call, including when nothing matches. XUD_INVA
/// LID_ARG when `advertised` is NULL while advertisedCount is not 0, or `extraNames` is NULL while
/// extraCount is not 0.
XudResult xud_instance_select_extensions(XudInstance* outInstance, const VkExtensionProperties* advertised, uint32_t advertisedCount,
    const char* const extraNames[XUD_DIAGNOSTICS_WANTED_MAX + XUD_INSTANCE_EXTRA_MAX], uint32_t extraCount);

/// Assemble the VkInstanceCreateInfo for an XudInstance: sType, pNext NULL when `diagnostics` is
/// NULL, -> &diagnostics->validationFeatures when it is not **and every extension the
/// diagnostics layer asked for is in instance->enabledExtensionNameStorage** (the VkValidation
/// FeaturesEXT piece of the chain - the validation layer reads it, and a pNext chain may carry a
/// struct for an enabled extension and for nothing else, which is why the gate is there: this
/// driver does not advertise VK_EXT_validation_features, so the chain stays empty and the layer
/// runs on its own defaults, see xud_instance_all_names_enabled in xud_instance.c), flags 0
/// (VK_INSTANCE_CREATE_FLAG_PORTABILITY_BIT is not set here - the extension is requested
/// explicitly and the flag only changes what a driver is allowed to *hide*, see the comment
/// in xud_instance.c), pApplicationInfo -> &instance->applicationInfo, the diagnostics layer's
/// selected layers in enabledLayerCount/ppEnabledLayerNames (0/NULL when there is no diagnostics
/// struct: a release build asks for no layer at all), and the selected extension names.
/// `nameSlots` is a caller-provided array of XUD_MAX_INSTANCE_EXTENSIONS slots: it is filled
/// with pointers into instance->enabledExtensionNameStorage and outCreateInfo->ppEnabledExtension
/// Names points at it, so both must outlive the vkCreateInstance call.
/// Pure. XUD_INVALID_ARG on a NULL instance, NULL nameSlots array, NULL out-param, or a
/// diagnostics struct whose layer count does not fit its own storage; the accessor's own
/// XUD_INVALID_ARG (NULL out-params) can only come from a bug in this file, and is passed through.
XudResult xud_instance_fill_create_info(const XudInstance* instance, const char* nameSlots[XUD_MAX_INSTANCE_EXTENSIONS],
    const XudDiagnostics* diagnostics, VkInstanceCreateInfo* outCreateInfo);

/// Create the instance. `loader` must be open (xud_loader_open filled the global-level slots
/// this needs). `diagnostics` is the diagnostics layer's answer (see xud_diagnostics.h) or NULL:
/// when present its selected layers go into enabledLayerCount/ppEnabledLayerNames, its wanted
/// extensions (VK_EXT_debug_utils, VK_EXT_validation_features - asked for through
/// xud_diagnostics_wanted_extensions(), the accessor, not an array declared here) are added to
/// the extension list, and its VkValidationFeaturesEXT goes on the pNext chain **only when that
/// list was fully enabled** - all of it still checked against what the driver advertised, nothing
/// forced on. On XUD_OK `outInstance` owns a live VkInstance plus the create-info that produced it;
/// on any error it stays NULL and everything created so far is destroyed, in reverse creation
/// order. XUD_INVALID_ARG on a NULL out-pointer, an out-pointer that already owns an instance
/// (nothing is created twice into the same slot), or a missing/unopen loader. extraNames/extraCou
/// nt are a layer above this one's required names - GLFW's window system answers them, and a surf
/// ace cannot be created for an instance that does not have them enabled, because glfwCreateWind
/// owSurface resolves vkCreateWaylandSurfaceKHR through vkGetInstanceProcAddr against the instance
/// it is handed and that row answers NULL otherwise (probed here: the call answered VK_ERROR_EXTEN
/// SION_NOT_PRESENT for an instance created without it) - so they are appended after the diagnos
/// tics layer's list and still enabled only if the driver advertised them. NULL/0 when there is no
/// window above this one. Driver calls, in order: vkEnumerateInstanceExtensionProperties (twice at
/// least - pCount is input/output, see xud_instance_query_extensions), vkCreateInstance through a
/// row of the loader's instance table.
/// vkDestroyInstance is *not* resolved here: it is an instance-level command, vkGetInstanceProcAddr
/// answers NULL for it without a live handle (verified here), and a failure to resolve it after a
/// successful create would leave an instance nothing could free - so it is resolved by
/// xud_instance_destroy(), through the instance it frees.
XudResult xud_instance_create(XudInstance** outInstance, XudLoader* loader, XudDiagnostics* diagnostics,
    const char* extraNames[XUD_INSTANCE_EXTRA_MAX], uint32_t extraCount);

/// Destroy the instance and clear the out pointer. The arguments are checked before the state:
/// XUD_INVALID_ARG when the pointer itself is NULL or the loader is missing/not open, and
/// XUD_ALREADY only when those are sound and the instance they point at is already NULL - a
/// second destroy is reported, never a double free. `diagnostics` is the diagnostics layer's
/// struct (or NULL): when it holds a messenger it is destroyed first, through the instance that
/// is about to go away, because vkDestroyDebugUtilsMessengerEXT takes the owning VkInstance and
/// the extension says that instance must still be alive; the struct's own messenger/instance hand
/// les are then cleared, so xud_close_context() frees the struct itself through xud_diagnostics_c
/// lose() after this returns - this layer only holds a copy of that pointer, so freeing it here
/// would leave the caller holding a dangling one. A messenger that belongs to a different VkInst
/// ance than the one being destroyed is XUD_INVALID_ARG, checked before anything is torn down. If
/// the loader cannot resolve vkDestroyDebugUtilsMessengerEXT or vkDestroyInstance, what it owns
/// leaks and the failure is returned - that is a broken loader, not something this layer papers
/// over.
XudResult xud_instance_destroy(XudInstance** instance, XudLoader* loader, XudDiagnostics* diagnostics);

#endif /* XUD_XUD_INSTANCE_H_ */
