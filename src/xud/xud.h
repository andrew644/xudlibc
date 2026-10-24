/***********************************************************************************************
 *
 *   xud.h - xudlibc public API
 *
 *   Owns : the public API surface and nothing else - version macros, build-time config
 *          switches (XUD_VALIDATE / XUD_GPU_TESTS), the runtime environment-variable names
 *          the library reads, the XudResult error enum, the one XudHandle currency and its
 *          kind tags, the opaque XudContext owner type, the xud_init_context()/xud_close_context()
 *          lifecycle pair, and the runtime Vulkan query xud_vulkan_supported().
 *   Layer: top of the stack (public API). Everything below it is internal - xud_core.h/.c, xud_loa
 *          der.h/.c, xud_vulkan.h/.c, xud_diagnostics.h/.c, xud_instance.h/.c, xud_physical.h/.c,
 *          xud_device.h/.c, xud_window.h/.c, xud_swapchain.h/.c, xud_handle.h/.c - and user code
 *          includes this header only; tests may include the internal ones. Every layer below this
 *          one is in the tree now and stays internal like the rest, so nothing about a VkDevice,
 *          its queues, a GLFW window, a VkSurfaceKHR or a VkSwapchainKHR is public API yet; the
 *          context owns all of them (S8's ownership step is landed and S9's swapchain rung sits on
 *          top of it, so xud_init_context() completes to the swapchain and xud_close_context() tears
 *          it all back), and the steps after it (the allocator, the renderer) are tracked in todo.md
 *          and do not exist yet. The window layer and the swapchain layer are internal for the same
 *          reason every layer below the public API is: xud.h declares no window type, no swapchain
 *          type and calls no GLFW function, and the VkSurfaceKHR a context owns lives inside the
 *          XudWindow it owns rather than in a field of its own. The handle is the ONE exception to
 *          that rule, and is public for the reason the note below states: it is the currency a
 *          caller holds, and the slot arrays behind it are internal - xud.h declares no table type
 *          and no handle function, so no public call takes or fills a handle today.
 *
 *   xudlibc is a C11 library shaped after raylib's public API but rendered with Vulkan:
 *   no OpenGL, no EGL, no fixed-function, no GL state machine. raylib is an API-shape
 *   reference only (reference/raylib/, read-only) - it is never included or linked.
 *
 *   Rules every declaration here obeys:
 *     - every function returns XudResult, or fills an out-param; nothing is ignored
 *     - out-params must be NULL on entry, and are NULL on every failure path
 *     - one XudContext owns instance/device/swapchain/renderer and the handle tables behind
 *       the public currency; callers hold a handle and nothing else
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#ifndef XUD_XUD_H_
#define XUD_XUD_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <assert.h> /* Required for: static_assert, the layout check on XudHandle below */
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
 * The handle - the one currency every caller-addressable object is named by
 *
 * The public API's currency is this handle, not a pointer into the library and not a driver
 * name: raylib's public types embed an OpenGL name and own sub-objects through pointer arrays
 * (Texture 273-279, Mesh ending 372, Model ending 433, Font ending 324 in reference/raylib/
 * raylib.h), which is a fixed-function-era artifact this repo refuses. One handle type names
 * every kind, and the tag - not the C type - is what distinguishes them, so a cross-kind mix-
 * up is a caught runtime error rather than a layout coincidence. Validation at every public
 * entry point is the three-part test in this order - index bounds against the slot array of the
 * kind the entry point is about, then generation equality, then tag equality - and any failure
 * is XUD_INVALID_ARG. The tag is not redundant with the array's identity: a texture handle's
 * (index, generation) pair can match a live model slot by coincidence, and the tag is what
 * makes that a refusal. XUD_HANDLE_KIND_NONE is never stored in a live slot, so a zero-initia-
 * lised handle - the state a caller gets when it ignores the XUD_RESULT of a create call - is
 * refused by the tag compare before any array lookup happens. Objects the library owns and a
 * caller never names (the batch's buffers, the per-frame ring, the swapchain's images, the
 * pipelines, the descriptor sets) stay tag-less: they are never addressed by this type.
 * The Vulkan handles a slot records live INSIDE the slot records, per the existing rule that
 * callers never hold a VkDevice, a VkSwapchainKHR or a raw VkBuffer (in these headers VK_DEFI
 * NE_HANDLE is an opaque pointer, so a Vulkan handle is a 64-bit pointer and is not this).
 * The wrap case - 2^32 reuses of one index, where a bumped generation can come back to a value
 * an older handle carries - would alias, and that is documented here rather than silently
 * ignored or refused: this layer's own slot bound makes it unreachable long before it is a
 * state worth a counter for.
 *-----------------------------------------------------------------------------------------------*/

/// The one handle type every caller-addressable object kind is named by - 12 bytes, three uint32_t
/// and no padding, asserted below. Value type: copyable, storable in a caller's own struct,
/// comparable field-wise (xud_handle_equal is the all-three-field form). A handle kept across the
/// close of the object it names is REJECTED, not dangling - the generation bump is what rejects it.
/// The object steps (textures, fonts, meshes, materials, models) are what hand one out; none of
/// them is in the tree yet, which is why no public function takes or fills one today.
typedef struct XudHandle {
    uint32_t index; /* which slot of its kind's array this names                                   */
    uint32_t generation; /* bumped when the slot it named was released and taken again, so a handle kept across that stops naming it */
    uint32_t kind; /* the tag of the object kind, XUD_HANDLE_KIND_* - never XUD_HANDLE_KIND_NONE in a live slot */
} XudHandle;

#define XUD_HANDLE_KIND_NONE 0u /* the tag of a handle that names nothing, and of a zero-initialised one - never stored in a live slot */
#define XUD_HANDLE_KIND_TEXTURE 1u /* a texture: an image + its sampler variant, once the allocator and the renderer rungs land */
#define XUD_HANDLE_KIND_FONT 2u /* a font: the glyph atlas and its metrics, once the text step lands */
#define XUD_HANDLE_KIND_MESH 3u /* a mesh: the attribute streams and the index buffer, once the batch lands */
#define XUD_HANDLE_KIND_MATERIAL 4u /* a material: a pipeline variant and its set-1 bindings, once the pipelines land */
#define XUD_HANDLE_KIND_MODEL 5u /* a model: the meshes and materials a load produced, once model loading lands */
#define XUD_HANDLE_KIND_MAX                                                                                                                \
    XUD_HANDLE_KIND_MODEL /* the highest kind value in use today - a new caller-addressable kind (AUDIOSTREAM when audio lands) moves this \
                             as well as its own value */
#define XUD_HANDLE_KIND_COUNT                                                                                                              \
    (XUD_HANDLE_KIND_MAX + 1u) /* one past the highest kind value, which is the row count of the table's per-kind arrays */

static_assert(sizeof(XudHandle) == 12u,
    "xud.h: the handle is three uint32_t and no padding - index, generation and tag - so a layout change is a change of the public "
    "currency, not of one layer");
static_assert(XUD_HANDLE_KIND_NONE == 0u,
    "xud.h: NONE is the 0th kind value, which is what makes a zero-initialised handle carry the tag no live slot ever has");
static_assert(XUD_HANDLE_KIND_MAX >= XUD_HANDLE_KIND_NONE,
    "xud.h: the highest kind value must be a kind value, or the table's row count below is a row count of nothing");

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
