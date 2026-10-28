/***********************************************************************************************
 *
 *   xud_window.h - the window rung: a GLFW window and the VkSurfaceKHR it hands over
 *
 *   Owns : XudWindow - the GLFWwindow*, the VkSurfaceKHR that window was handed over, the extent
 *          asked for, and the instance-level extensions GLFW's window system needs, recorded once
 *          at open time - plus the rungs that bring each half up and down. The window half is
 *          GLFW's: glfwInit, glfwCreateWindow, glfwDestroyWindow, glfwTerminate, glfwSetWindowSize
 *          and glfwGetFramebufferSize - the FRAMEBUFFER extent being what a swapchain is created
 *          at, never the extent asked for. The surface half
 *          is the library's: glfwCreateWindowSurface hands a VkSurfaceKHR to the instance it is
 *          given, and vkDestroySurfaceKHR - a row of the loader's instance table - takes it back.
 *   Layer: above the instance (a surface is created FOR an instance, and vkDestroySurfaceKHR takes
 *          that same instance while it is still alive) and below the swapchain, which is created
 *          FROM the surface filled here and recreated by the resize path through this layer. It is
 *          the only place in this tree that talks to GLFW, and GLFW is the window/input rung of
 *          the design sketch: the vendored 3.5.1 tree on the Wayland backend (AGENTS.md, settled
 *          decisions). GLFW's Vulkan half resolves its own entry points - wl_window.c resolves
 *          vkCreateWaylandSurfaceKHR and vkGetPhysicalDeviceWaylandPresentationSupportKHR through
 *          vkGetInstanceProcAddr against the instance it is handed - so the instance given here is
 *          what decides whether a surface can exist at all. That is why GLFW's required-extension
 *          list has to reach the instance layer's selection (see xud_instance.h): an instance tha
 *          t does not have GLFW's window-system extension enabled has no Wayland surface to create,
 *          and the row GLFW resolves for it answers NULL. Nothing here calls a Vulkan entry point
 *          of its own except through a row of the loader's Volk tables.
 *
 *   Include order (the GLFW trap, verified here): glfw3.h wraps its Vulkan declarations -
 *          glfwVulkanSupported, glfwGetRequiredInstanceExtensions, glfwGetPhysicalDevicePresentat
 *          ionSupport, glfwCreateWindowSurface (glfw3.h 6346-6540) - inside `#if defined(VK_VERSI
 *          ON_1_0)`, so <vulkan/vulkan.h> must have been seen BEFORE glfw3.h or those four declara
 *          tions are not in the TU and the calls in xud_window.c do not compile. The reverse order
 *          is a hard error, not a warning (verified with gcc 16.2.1 and clang 22.1.8 under the pro
 *          ject flags: implicit declaration of glfwCreateWindowSurface, and the VkResult/VkInstan
 *          ce/VkSurfaceKHR in its prototype unresolved). Every Vulkan-bearing xud header here incl
 *          udes volk.h immediately before <vulkan/vulkan.h> (the volk.h trap, xud_loader.h), so gl
 *          fw3.h comes after both, at the END of the Vulkan part of the include block - the C stdan
 *          dard headers (<assert.h>, <limits.h>, …) sit after it, which is fine: the static_asserts
 *          below read both Vulkan's stamps and GLFW's version macros, and both are already in by th
 *          en.
 *
 *   What this layer does NOT own: the VkSwapchainKHR (the swapchain layer, next step), the window
 *          's input state (GLFW's own, and no xud name reaches it yet), and the instance the surf
 *          ace belongs to (the instance layer decides which extensions are enabled - this layer
 *          only adds GLFW's required names to that decision, it never enables one itself).
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#ifndef XUD_XUD_WINDOW_H_
#define XUD_XUD_WINDOW_H_

#include "volk.h" /* Required for: VK_NO_PROTOTYPES - must precede <vulkan/vulkan.h>, see xud_loader.h */
#include "xud.h" /* Required for: XudResult - the window layer's own error codes */
#include "xud_instance.h" /* Required for: XudInstance and XUD_INSTANCE_EXTRA_MAX - the instance
                            whose enabled-extension list this layer checks GLFW's required names
                            against, and the bound of that list. Included this way round on pur-
                            pose: xud_instance.h does NOT include this one (xud_instance_create()
                            takes the window's required NAMES, not a window), so the two headers
                            cannot form an include cycle that leaves one of the two types incom-
                            plete. */
#include "xud_loader.h" /* Required for: XudLoader - the Volk rows vkDestroySurfaceKHR is called through */
#include "xud_physical.h" /* Required for: XudPhysicalDevice and its caps - the chosen queue family
                            presentation_support() is checked against lives there, not here */
#include "xud_vulkan.h" /* Required for: xud_result_from_vk (GLFW's VkResult) + xud_name_copy */
#include <vulkan/vulkan.h> /* Required for: VkSurfaceKHR, VK_NULL_HANDLE - declarations only, this
                             header creates no link dependency (AGENTS.md's Vulkan-free link rule) */

#include <GLFW/glfw3.h> /* LAST of the block: its Vulkan declarations are inside
                          #if defined(VK_VERSION_1_0), so <vulkan/vulkan.h> above must have been
                          seen first - see the include-order note in this file's header comment.
                          The vendored tree's include dir (src/external/glfw/include) is on the
                          library's include path for this (CMakeLists.txt). */
#include <assert.h> /* Required for: static_assert, the compile-time checks below */
#include <limits.h> /* Required for: INT_MAX, the extent bound below */
#include <stdbool.h>
#include <stdint.h>

/*-----------------------------------------------------------------------------------------------
 * Three facts this layer's contract rests on, all compile-time
 *-----------------------------------------------------------------------------------------------*/

static_assert(GLFW_VERSION_MAJOR == 3, "xud_window.h: the vendored GLFW is the 3.5.1 tree");
static_assert(GLFW_VERSION_MINOR == 5, "xud_window.h: the vendored GLFW is the 3.5.1 tree");
/* The Wayland backend is a build-time property of the vendored tree, not of this TU: CMakeLists
   .txt passes -D_GLFW_WAYLAND (upstream's own backend define, glfw/src/CMakeLists.txt 54-55) to
   glfw_vendor PRIVATE, so reading it here would read a macro that is only in those TUs - a stati
   c_assert on it fails in every xud TU, verified. What IS checkable here is GLFW's version, and
   the backend fact is recorded in the comment instead: GLFW_BUILD_X11 stays off, so the window
   system GLFW resolves its surface/present entry points for is Wayland's (wl_window.c), and the
   two names glfwGetRequiredInstanceExtensions answers for it are asserted about in xud_test_gpu_
   window(), not here. */

static_assert(VK_MAX_EXTENSION_NAME_SIZE == 256u, "xud_window.h: GLFW's required-extension names are copied into that-wide fixed buffers");
/* GLFW's Vulkan surface teardown. There is deliberately no glfwDestroyVulkanSurface in 3.5.1
   (grepped in the vendored glfw3.h and in src/vulkan.c: the name does not exist) - the surface
   GLFW hands over is destroyed with vkDestroySurfaceKHR, which is why this layer needs the
   loader's instance table at teardown and glfw3.h alone would not be enough. */

/* The extent bounds of a window this library will create. An extent of 0 is not a window, and
   glfwCreateWindow takes an `int` (glfw3.h 3240), so an extent above INT_MAX cannot be handed
   over without wrapping - it is a bad argument, not a truncation. */
#define XUD_WINDOW_MIN_EXTENT 1u
#define XUD_WINDOW_MAX_EXTENT 2147483647u
static_assert(XUD_WINDOW_MAX_EXTENT == (uint32_t)INT_MAX, "xud_window.h: the extent bound is glfwCreateWindow's int bound");

/* The window title every xudlibc window carries. GLFW's Wayland backend uses it as the window
   title and, through xdg-decoration, as the hint for a decorated session; a fixed title keeps
   the library's windows identifiable without adding a public knob this layer does not own. */
#define XUD_WINDOW_TITLE "xudlibc"

/* How many instance-level extensions GLFW's window system can ask of an xudlibc instance. One
   macro for one number, defined by the instance layer (XUD_INSTANCE_EXTRA_MAX, the bound of the
   extra-names array xud_instance_create() accepts): this layer's storage is that wide, so the
   list GLFW answers and the list the instance layer accepts cannot drift apart. GLFW 3.5.1's
   Wayland backend answers exactly two (VK_KHR_surface, VK_KHR_wayland_surface - probed here),
   and the bound leaves room for one more; xud_window_open() reports XUD_OUT_OF_RESOURCES rather
   than truncating a longer list. */
static_assert(XUD_INSTANCE_EXTRA_MAX >= XUD_DIAGNOSTICS_WANTED_MAX,
    "xud_window.h: the instance layer's extra-names bound must at least hold the diagnostics "
    "layer's wanted list, which xud_instance_create() passes through the same array");

typedef struct XudWindow {
    GLFWwindow* glfwWindow; /* the native window GLFW created, NULL until xud_window_open() fills
                               it (and NULL on a machine where GLFW refuses, where nothing exists
                               to destroy) */
    VkSurfaceKHR surface; /* the surface glfwCreateWindowSurface handed over for the instance
                             below, VK_NULL_HANDLE until xud_window_surface_create() fills it */
    VkInstance surfaceInstance; /* borrowed, not owned: the instance that surface was created
                                   for. vkDestroySurfaceKHR takes that same instance and the
                                   extension requires it still be alive, which is why the surface
                                   is torn down before the instance is - the order xud_close_con
                                   text() uses. */
    uint32_t width; /* extent as requested, pixels: what the compositor was asked for, not what
                       it gave back (glfwGetWindowSize is not read here) */
    uint32_t height;
    uint32_t requiredInstanceExtensionCount; /* how many rows below are live, at most XUD_INSTANCE
                                               _EXTRA_MAX */
    char requiredInstanceExtensionNameStorage[XUD_INSTANCE_EXTRA_MAX]
                                             [VK_MAX_EXTENSION_NAME_SIZE]; /* GLFW's required names, copied out of GLFW's own storage
                                                 (glfwGetRequiredInstanceExtensions returns pointers into a static GLFW keeps only
                                                 until the library is terminated) into fixed buffers this struct owns */
} XudWindow;

/// The instance-level extensions GLFW's window system needs enabled on the instance a surface
/// for this window is created for - GLFW's own answer (glfwGetRequiredInstanceExtensions), re-
/// corded at open time. The out-array points into the window's own storage, so the window must
/// outlive whatever reads it: the instance layer copies the names it is handed into its own
/// storage before it creates anything. Pure: no GLFW call, no driver call, headless-testable.
/// XUD_INVALID_ARG on a NULL window/out-array/out-count.
XudResult xud_window_required_instance_extensions(
    const XudWindow* window, const char* outNames[XUD_INSTANCE_EXTRA_MAX], uint32_t* outCount);

/// Pure: does the instance in hand have every extension GLFW's window system asked for? This is
/// the check-before-you-use rule for the surface rung, and it is what makes the merge above
/// load-bearing rather than hopeful: an instance created without GLFW's window-system extension
/// (VK_KHR_wayland_surface here) cannot have a Wayland surface at all, because GLFW resolves
/// vkCreateWaylandSurfaceKHR through vkGetInstanceProcAddr against that instance and the row
/// answers NULL (probed here: glfwCreateWindowSurface returned VK_ERROR_EXTENSION_NOT_PRESENT
/// and glfwGetPhysicalDevicePresentationSupport answered GLFW_FALSE for exactly such an instan-
/// ce). A window whose required list is empty - GLFW had no Vulkan answer at open time - asks
/// for nothing, so this answers true and the GLFW call below is the one that refuses.
/// XUD_INVALID_ARG on a NULL out-param, window or instance.
XudResult xud_window_check_instance_support(bool* outSupports, const XudWindow* window, const XudInstance* instance);

/// Driver (GLFW): bring the window half up - glfwInit (idempotent: init.c 386-389 returns GLFW
/// TRUE when GLFW is already initialized, which is what lets every context call it and is why
/// there is no init flag here), then glfwVulkanSupported (GLFW's own runtime query: it finds the
/// Vulkan loader, it creates no window and no instance), then glfwGetRequiredInstanceExtensions,
/// recorded before the window exists so a refusal needs no window teardown, then glfwCreateWin-
/// dow with the client API hinted GLFW_NO_API (glfwWindowHint, window.c 400 -> _glfw.hints.con-
/// text.client) - the only client API glfwCreateWindowSurface accepts (vulkan.c: a window whose
/// context.client is not GLFW_NO_API is refused with VK_ERROR_NATIVE_WINDOW_IN_USE_KHR). GLFW's
/// required names are copied into this struct, not kept as GLFW's pointers.
/// XUD_INVALID_ARG on a NULL out-param or one that already owns a window (nothing is created
/// twice into the same slot), on an extent of 0 or above INT_MAX. XUD_UNSUPPORTED when GLFW re-
/// fuses: no glfwInit, no Vulkan loader behind GLFW, or no window - on a machine with no Wayland
/// session this is the honest answer. XUD_OUT_OF_RESOURCES when GLFW asks for more extensions
/// than the bound above holds (truncating that list would mean a surface rung that silently can-
/// not work), XUD_INTERNAL_ERROR when GLFW lies about it or a name does not fit its buffer.
/// Every failure path frees the struct and leaves NULL.
XudResult xud_window_open(XudWindow** outWindow, uint32_t width, uint32_t height);

/// Driver (GLFW): ask the window system for a new extent - glfwSetWindowSize. This is the one
///   call in this tree that moves a window's extent at all: nothing else writes window->width /
///   height after the create, and a drag-resize is the COMPOSITOR's decision, applied by GLFW's
///   own configure handler on both backends (x11_window.c's configure_func, wl_window.c's
///   xdgSurfaceHandleConfigure), which then runs the same resizeWindow -> resizeFramebuffer
///   chain this call goes through. This call is the programmatic half of that same path, which
///   is what lets a test make a resize happen on demand instead of waiting for a drag. XUD_
///   INVALID_ARG on a NULL window, one this layer does not own, or an extent outside the
///   window layer's own bounds above; XUD_ALREADY when the window is already at that extent
///   (the rule xud_swapchain_recreate() follows one rung up: the state asked for is the state
///   present, so there is nothing to recreate); XUD_OK once GLFW has taken the new extent.
///   glfwSetWindowSize returns void, so the zero-state rule applies: the window's extent is
///   written from the request rather than read back. The FRAMEBUFFER's extent is a separate
///   question, read by xud_window_framebuffer_size() below, and the two differ by the display's
///   scale factor, which is the whole reason a resize step reads the second and not the first.
XudResult xud_window_resize(XudWindow* window, uint32_t width, uint32_t height);

/// Driver (GLFW): the extent of the window's FRAMEBUFFER as the window system gave it - glfwGet
///   FramebufferSize. This is the extent a swapchain for this window has to be created at, and
///   it is NOT the window's requested extent: GLFW's Wayland half keeps its own fbWidth / fbHeight,
///   written by resizeFramebuffer from window->wl.width / wl.height scaled by either the
///   fractional scale factor (the (w * scalingNumerator + 60) / 120 rounding wl_window.c uses)
///   or the buffer scale, and the X11 half reads XGetWindowPixmap's pixmap width/height. A
///   Hi-Wayland window is therefore 1600x900 requested and 3200x1800 of framebuffer, and a
///   swapchain built at the first is a swapchain that does not match the surface - which is
///   what the compositor answers with VK_ERROR_OUT_OF_DATE_KHR on every acquire, the two codes
///   xud_vulkan.h's swapchain-boundary funnel exists for. xud_swapchain_open() takes the
///   window's REQUESTED extent today, which is why the resize step below reads this one.
///   glfwGetFramebufferSize returns void and zeroes both out-params BEFORE it dispatches
///   (window.c 715-727), so a platform that answers nothing leaves 0/0 behind, and the bound
///   below turns that into a refusal rather than a zero-extent swapchain. XUD_INVALID_ARG on a
///   NULL out-parameter, a NULL window or one this layer does not own, or a non-positive
///   extent; XUD_OK with both extents written.
XudResult xud_window_framebuffer_size(uint32_t* outWidth, uint32_t* outHeight, const XudWindow* window);

/// Driver (GLFW): does the window system GLFW picked present to the physical device in hand,
/// through the queue family the physical layer chose (caps.queueFamilyIndex, the one family the
/// graphics rule picked)? This is glfwGetPhysicalDevicePresentationSupport, and it is the wind-
/// ow-system half of the check-before-you-use rule: there is no VK_QUEUE_PRESENT_BIT anywhere in
/// these headers (grepped, AGENTS.md) - presentation is a per-window-system query, and this is
/// that query. It is a GLFW call, not a row of our loader's tables: GLFW resolves vkGetPhysica-
/// lDeviceWaylandPresentationSupportKHR through its own vkGetInstanceProcAddr (vulkan.c 6470 ->
/// _glfw.platform.getPhysicalDevicePresentationSupport, wl_window.c), against the instance and
/// physical device it is handed - which is why those two handles, and not a loader, are the arg-
/// uments here.
/// XUD_INVALID_ARG on a NULL out-param/window/instance/physical-device, on a window that never
/// opened (GLFW's own state is what the call needs), or on a handle-less instance/physical devi-
/// ce (GLFW asserts both are live). XUD_UNSUPPORTED when the instance cannot have a surface for
/// this window at all (xud_window_check_instance_support) - the call is refused, not made. On
/// XUD_OK *outSupports is GLFW's answer: false means the window system says no, and the swap-
/// chain that needs this window must not be created.
XudResult xud_window_presentation_support(
    bool* outSupports, const XudWindow* window, const XudInstance* instance, const XudPhysicalDevice* physicalDevice);

/// Driver: create the surface for this window - glfwCreateWindowSurface, which hands the instan-
/// ce a VkSurfaceKHR for this window's wl_surface. Before calling it: the instance must have
/// everything GLFW asked for (xud_window_check_instance_support, above - the refusal is XUD_UNS-
/// UPPORTED and no call is made), and the loader's instance table must be bound to that instan-
/// ce and answer vkDestroySurfaceKHR, because a surface this layer cannot destroy is a surface
/// it must not create (the row is filled in bulk by xud_loader_bind_instance, so checking it be-
/// fore the call is free - unlike vkDestroyInstance, which xud_instance.c resolves at destroy
/// time because vkGetInstanceProcAddr answers NULL for it until an instance exists).
/// XUD_INVALID_ARG on a NULL window/loader/instance, a window that never opened, a window that
/// already has a surface (nothing is created twice into the same slot), a handle-less instance
/// or a closed loader. XUD_UNSUPPORTED when the loader's rows are not this instance's rows, the
/// destroy row is NULL, or GLFW/the driver refuses (VK_ERROR_EXTENSION_NOT_PRESENT and the like
/// map through xud_result_from_vk). On XUD_OK window->surface is live and window->surfaceInstan-
/// ce records the instance it belongs to; on any failure both stay at their zero state.
XudResult xud_window_surface_create(XudWindow* window, XudLoader* loader, const XudInstance* instance);

/// Driver: destroy the surface through the instance it belongs to, while that instance is still
/// alive - vkDestroySurfaceKHR, a row of the loader's instance table (there is no glfwDestroyVu-
/// lkanSurface in 3.5.1, see the note above). The row is checked before the call and the handle
/// cleared after it: the entry point returns void, so there is no VkResult to map and this layer
/// 's rule is the zero-state rule the other void entry points in this tree follow (vkDestroyDev-
/// ice, vkGetDeviceQueue, vkFreeMemory).
/// XUD_INVALID_ARG on a NULL window/loader or a closed loader; XUD_ALREADY when there is no
/// surface (already closed - a second destroy is reported, never a double free); XUD_UNSUPPORTED
/// when the loader's instance rows belong to another instance (a surface created for one instan-
/// ce must not be destroyed through another - the bound-handle rule of the loader layer) or the
/// destroy row is NULL. Both handles are cleared on XUD_OK.
XudResult xud_window_surface_close(XudWindow* window, XudLoader* loader);

/// Teardown of the window half: glfwDestroyWindow, then glfwTerminate once no window this layer
/// has alive is left - GLFW's own state is process-level (glfwInit is idempotent, glfwTerminate
/// tears the whole library down, and terminate() walks GLFW's window list), so the count of live
/// windows is what decides when the last rung runs. See the two comments over that count in
/// xud_window.c for why it is the one piece of state here that is not inside XudContext.
/// A window that never got a GLFW window (the handle-less state a synthetic struct is in) frees
/// without any GLFW call and without touching the count - the same handleless-teardown rule the
/// instance and device layers apply to a struct whose handle is VK_NULL_HANDLE.
/// XUD_INVALID_ARG on a NULL out-param, or on a window that still has a surface: the surface
/// belongs to this window, and destroying the window under it would leave a surface whose window
/// is gone - xud_window_surface_close() is the rung that destroys it first, and xud_close_con-
/// text() calls it in that order. XUD_ALREADY when there is no struct (already closed).
XudResult xud_window_close(XudWindow** window);

#endif /* XUD_XUD_WINDOW_H_ */
