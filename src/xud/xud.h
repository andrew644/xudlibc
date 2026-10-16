/***********************************************************************************************
 *
 *   xud.h - xudlibc public API
 *
 *   Owns : the public API surface and nothing else - version macros, build-time config
 *          switches (XUD_VALIDATE / XUD_GPU_TESTS), the runtime environment-variable names
 *          the library reads, the XudResult error enum, the opaque XudContext owner type,
 *          the xud_init_context()/xud_close_context() lifecycle pair, and the runtime
 *          Vulkan query xud_vulkan_supported().
 *   Layer: top of the stack (public API). Everything below it is internal: xud_core.h/.c,
 *          xud_loader.h/.c, and the Vulkan bring-up steps S3..S8 tracked in todo.md.
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

#include <stdint.h>     /* Required for: uint32_t - window extents and handle fields */

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
#define XUD_ENV_VALIDATE    "VK_INSTANCE_LAYERS"
#define XUD_ENV_GPU_TESTS   "XUD_GPU_TESTS"

/* Host-side sanity bound on window extent, used until the real gate exists: the device
   query (maxImageExtent2D, recorded into XudDeviceCaps at step S5) is authoritative. */
#define XUD_MAX_WINDOW_EXTENT 16384u

/*-----------------------------------------------------------------------------------------------
 * Error codes - the return type of every public function
 *-----------------------------------------------------------------------------------------------*/

typedef enum XudResult {
    XUD_OK               = 0,   /* the operation succeeded                                        */
    XUD_UNSUPPORTED      = 1,   /* a required feature, extension or entry point is absent at runtime */
    XUD_INVALID_ARG      = 2,   /* null out-param, non-null out-param on entry, zero extent, ... */
    XUD_OUT_OF_MEMORY    = 3,   /* a host allocation failed                                      */
    XUD_OUT_OF_RESOURCES = 4,   /* a device-side allocation failed (VkResult OUT_OF_DEVICE_MEMORY) */
    XUD_ALREADY          = 5,   /* the operation is already in effect (idempotent teardown)      */
    XUD_LOST             = 6,   /* VkResult was VK_ERROR_DEVICE_LOST: the device is unusable     */
    XUD_VULKAN_ERROR     = 7,   /* some other VkResult came back non-VK_SUCCESS                  */
    XUD_INTERNAL_ERROR   = 8    /* an internal invariant broke: close the context and start over */
} XudResult;

/* Human-readable name for an error code - for logging and for test output.
   Never returns NULL, and never allocates. */
const char *xud_result_name(XudResult result);

/*-----------------------------------------------------------------------------------------------
 * The context - the single owner of everything the library creates
 *
 * The struct itself is completed in xud_core.h; here it stays opaque so that user code
 * cannot reach a VkDevice, VkSwapchainKHR or a raw VkBuffer.
 *-----------------------------------------------------------------------------------------------*/

typedef struct XudContext XudContext;

/// True runtime query, not a build-time constant and not cached: dlopen the Vulkan loader
/// and resolve its seed entry point. XUD_OK if the loader answers, XUD_UNSUPPORTED if the
/// driver is absent or does not. Safe with no device present and no window open.
XudResult xud_vulkan_supported(void);

/// Create the library context for a width x height window: resolve the Vulkan loader,
/// create instance / physical device / device + queues / swapchain surface, and build the
/// renderer. On entry *outContext must be NULL; it is NULL on every failure path.
/// At step S2 the loader opens and its entry-point table is filled, but instance / device
/// / surface are still pending, so a valid call returns XUD_UNSUPPORTED and leaves
/// *outContext NULL, having opened and closed the loader without leaking it.
XudResult xud_init_context(XudContext **outContext, uint32_t width, uint32_t height);

/// Tear the context down (device -> surface -> instance -> loader) and free it.
/// Returns XUD_ALREADY if *context was already NULL, XUD_INVALID_ARG if context itself
/// is NULL. Always sets *context to NULL when it is not NULL.
XudResult xud_close_context(XudContext **context);

#ifdef __cplusplus
}
#endif

#endif /* XUD_XUD_H_ */
