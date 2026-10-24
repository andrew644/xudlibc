/***********************************************************************************************
 *
 *   xud.h - xudlibc public API
 *
 *   Owns : the public API surface and nothing else - version macros, build-time config
 *          switches (XUD_VALIDATE / XUD_GPU_TESTS), the runtime environment-variable names
 *          the library reads, the XudResult error enum, the opaque XudContext owner type,
 *          the xud_init_context()/xud_close_context() lifecycle pair, and the runtime
 *          Vulkan query xud_vulkan_supported().
 *   Layer: top of the stack (public API). Everything below it is internal - xud_core.h/.c, xud_loa
 *          der.h/.c, xud_vulkan.h/.c, xud_diagnostics.h/.c, xud_instance.h/.c, xud_physical.h/.c,
 *          xud_device.h/.c, xud_window.h/.c, xud_swapchain.h/.c - and user code includes this
 *          header only; tests may include the internal ones. Every layer below this one is in the
 *          tree now and stays internal like the rest, so nothing about a VkDevice, its queues, a
 *          GLFW window, a VkSurfaceKHR or a VkSwapchainKHR is public API yet; the context owns all
 *          of them (S8's ownership step is landed and S9's swapchain rung sits on top of it, so
 *          xud_init_context() completes to the swapchain and xud_close_context() tears it all
 *          back), and the steps after it (the allocator, the renderer) are tracked in todo.md and
 *          do not exist yet. The window layer and the swapchain layer are internal for the same
 *          reason every layer below the public API is: xud.h declares no window type, no swapchain
 *          type and calls no GLFW function, and the VkSurfaceKHR a context owns lives inside the
 *          XudWindow it owns rather than in a field of its own.
 *
 *   xudlibc is a C11 library shaped after raylib's public API but rendered with Vulkan:
 *   no OpenGL, no EGL, no fixed-function, no GL state machine. raylib is an API-shape
 *   reference only (reference/raylib/, read-only) - it is never included or linked.
 *
 *   Rules every declaration here obeys:
 *     - every function returns XudResult, or fills an out-param; nothing is ignored
 *     - out-params must be NULL on entry, and are NULL on every failure path
 *     - one XudContext owns instance/device/swapchain/renderer; callers hold nothing else
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#ifndef XUD_XUD_H_
#define XUD_XUD_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h> /* Required for: uint32_t - window extents and handle fields */

/*-----------------------------------------------------------------------------------------------
 * Version
 *-----------------------------------------------------------------------------------------------*/

#define XUD_VERSION_MAJOR 0
#define XUD_VERSION_MINOR 1
#define XUD_VERSION_PATCH 0
#define XUD_VERSION "0.1"

/*-----------------------------------------------------------------------------------------------
 * Build-time configuration
 *
 * XUD_VALIDATE  - request every validation layer the loader advertises (never a hardcoded
 *                 layer name). Defaults to on for debug builds, off otherwise, and is always
 *                 overridable at runtime: the loader itself honours VK_INSTANCE_LAYERS.
 * XUD_GPU_TESTS - compile-time default for the GPU-test gate in tests/; the environment
 *                 variable of the same name overrides it at runtime.
 *-----------------------------------------------------------------------------------------------*/

#if defined(XUD_DEBUG) && !defined(XUD_VALIDATE)
#define XUD_VALIDATE 1
#endif

#ifndef XUD_VALIDATE
#define XUD_VALIDATE 0
#endif

#ifndef XUD_GPU_TESTS
#define XUD_GPU_TESTS 0
#endif

/* Environment variables the library reads at runtime. */
#define XUD_ENV_VALIDATE "VK_INSTANCE_LAYERS"
#define XUD_ENV_GPU_TESTS "XUD_GPU_TESTS"

/* Host-side sanity bound on window extent, used until the real gate exists: the device
   query (maxImageExtent2D, recorded into XudDeviceCaps at step S5) is authoritative. */
#define XUD_MAX_WINDOW_EXTENT 16384u

/*-----------------------------------------------------------------------------------------------
 * Error codes - the return type of every public function
 *-----------------------------------------------------------------------------------------------*/

typedef enum XudResult {
    XUD_OK = 0, /* the operation succeeded                                        */
    XUD_UNSUPPORTED = 1, /* a required feature, extension or entry point is absent at runtime */
    XUD_INVALID_ARG = 2, /* null out-param, non-null out-param on entry, zero extent, ... */
    XUD_OUT_OF_MEMORY = 3, /* a host allocation failed                                      */
    XUD_OUT_OF_RESOURCES = 4, /* a device-side allocation failed (VkResult OUT_OF_DEVICE_MEMORY) */
    XUD_ALREADY = 5, /* the operation is already in effect (idempotent teardown)      */
    XUD_LOST = 6, /* VkResult was VK_ERROR_DEVICE_LOST: the device is unusable     */
    XUD_VULKAN_ERROR = 7, /* some other VkResult came back non-VK_SUCCESS                  */
    XUD_INTERNAL_ERROR = 8 /* an internal invariant broke: close the context and start over */
} XudResult;

/* Human-readable name for an error code - for logging and for test output.
   Never returns NULL, and never allocates. */
const char* xud_result_name(XudResult result);

/*-----------------------------------------------------------------------------------------------
 * The context - the single owner of everything the library creates
 *
 * The struct itself is completed in xud_core.h; here it stays opaque so that user code
 * cannot reach a VkDevice, VkSwapchainKHR or a raw VkBuffer.
 *-----------------------------------------------------------------------------------------------*/

typedef struct XudContext XudContext;

/// True runtime query, not a build-time constant and not cached: volkInitialize() dlopens
/// libvulkan.so.1 by name and resolves the loader-level entry points. XUD_OK if the loader
/// answers all of them, XUD_UNSUPPORTED if the driver is absent or does not. Safe with no
/// device present and no window open.
XudResult xud_vulkan_supported(void);

/// Create the library context for a width x height window, in creation order: volkInitialize() the
/// loader, open the diagnostics layer when XUD_VALIDATE asked for it, open a GLFW window, create
/// the VkInstance carrying that window's required instance extensions, install the debug
/// messenger, open the physical layer through THAT instance (enumerate, rank, record caps), ask
/// GLFW whether that physical device can present to this window's system, create the VkDevice from
/// the recorded caps and the chosen queue family, resolve the two role queues, create the
/// VkSurfaceKHR for the window, and create the VkSwapchainKHR for that surface on that device (the
/// three surface queries' answers decide its image count, its format and its present mode -
/// nothing about it is a constant this layer hoped for). On entry *outContext must be NULL; it is
/// NULL on every failure path, and every failure path tears down what was built - a refused
/// request leaks nothing and a valid one never hands back a half-built context. XUD_UNSUPPORTED
/// means this machine cannot host a context at all (no Vulkan loader, no window system, no device
/// this layer can take, a window system this device cannot present to, a surface this layer cannot
/// get a swapchain for); the allocator is the only field still unfilled, which is why nothing can
/// be drawn yet.
XudResult xud_init_context(XudContext** outContext, uint32_t width, uint32_t height);

/// Tear the context down in the reverse order of creation - swapchain -> device -> surface ->
/// window -> physical -> instance -> diagnostics -> loader - and free it. The swapchain goes first
/// because it is presented through a queue of the device below it, its own close runs that
/// device's wait-idle guard, and it is the last rung created. The device goes next because a
/// VkDevice is created FROM the physical device below it and everything that submits through it
/// belongs to it; the surface goes next because vkDestroySurfaceKHR takes the owning VkInstance,
/// which is still alive there; the window goes after its surface because the window layer refuses
/// a window whose surface is live and may run glfwTerminate(), GLFW's own teardown, which must not
/// walk GLFW's window list while a surface still names a window. The diagnostics struct is
/// destroyed after the instance because its messenger belongs to that VkInstance and
/// xud_instance_destroy() tears the messenger down first, while the instance is still alive. The
/// physical rung is the free-only one: a VkPhysicalDevice is a query result of an instance, so
/// closing it frees memory and calls no driver function. Every rung is XUD_ALREADY on a field that
/// was never filled, which is why a context rejected part-way through bring-up is closed by this
/// same code path. Returns the first rung that refused (XUD_LOST included, XUD_ALREADY not counted
/// as a refusal), XUD_ALREADY if *context was already NULL, XUD_INVALID_ARG if context itself is
/// NULL. Always sets *context to NULL when it is not NULL.
XudResult xud_close_context(XudContext** context);

#ifdef __cplusplus
}
#endif

#endif /* XUD_XUD_H_ */
