/***********************************************************************************************
 *
 *   xud.h - xudlibc public API
 *
 *   Owns : the public API surface and nothing else - version macros, build-time config
 *          switches (XUD_VALIDATE / XUD_GPU_TESTS), the runtime environment-variable names
 *          the library reads, the XudResult error enum, the two value types a caller passes
 *          down (XudVector3 / XudColor), the one XudHandle currency and its kind tags, the
 *          opaque XudContext owner type, the xud_init_context()/xud_close_context() life-
 *          cycle pair, the drawing API above it (xud_begin_drawing() / xud_draw_triangle_3d
 *          () / xud_draw_cube() / xud_end_drawing()), and the runtime Vulkan query xud_vulkan
 *          _supported().
 *   Layer: top of the stack (public API). Everything below it is internal - xud_core.h/.c, xud_loa
 *          der.h/.c, xud_vulkan.h/.c, xud_diagnostics.h/.c, xud_instance.h/.c, xud_physical.h/.c,
 *          xud_device.h/.c, xud_window.h/.c, xud_swapchain.h/.c, xud_handle.h/.c, xud_shader.h/.c,
 *          xud_pipeline.h/.c, xud_renderpass.h/.c, xud_frame.h/.c, xud_renderer.h/.c, xud_alloca
 *          tor.h/.c, xud_descriptor.h/.c, xud_batch.h/.c and xud_shapes.h/.c - and user code inclu
 *          des this header only; tests may include the internal ones.
 *          Every layer below this one is in the tree now, and every one of them stays internal be
 *          hind the two exceptions this note names: nothing about a VkDevice, its queues, a
 *          GLFW window, a VkSurfaceKHR, a VkSwapchainKHR, a VkPipeline, a VkRenderPass, a
 *          VkFramebuffer, a VkCommandBuffer or a VkDeviceMemory is reachable from here; the conte
 *          xt owns all of them (S8's ownership step is landed, S9's swapchain rung sits on top of
 *          it, S10's handle layer and S11's five render rungs on top of that, S12's allocator rung
 *          sits above the swapchain and below the descriptor and the renderer, and S16's batch and
 *          S18's descriptor rungs sit below the renderer above it, so xud_init_context() completes
 *          to the renderer and xud_close_context() tears all of it back and NO field of XudConte
 *          xt is a sentinel any more). The window layer, the swapchain layer, the render rungs,
 *          the allocator, the descriptor and the batch are internal for the same reason every laye
 *          r below the public API is: xud.h declares no window type, no swapchain type, no rende
 *          r type, no allocator type, no batch type and calls no GLFW function, and the VkSurfac
 *          eKHR a context owns lives inside the XudWindow it owns rather than in a field of its
 *          own. The TWO exceptions to that rule are the handle and the shapes rung's public call
 *          s, and both are public for the reason their own notes state: the handle is the curren
 *          cy a caller holds and the slot arrays behind it are internal (xud.h still declares no
 *          table type and no handle function, so no public call takes or fills one today and no
 *          handle names an object yet); the shapes rung's calls are the geometry a caller draws,
 *          which is what the drawing API above the renderer reaches - xud.h declares the frame pa
 *          ir and three shapes, and what is still missing above the renderer is the rest of rayli
 *          b's 3D shape family (the line/point forms and the wire forms, which need a topology the
 *          pipeline rung does not have, and DrawMesh, which is the mesh rung), the camera + trans
 *          form math, the handle layer's material record and the object kinds a public handle wou
 *          ld name, which is why a caller can draw a triangle, a box and a sphere today
 *          and nothing else, and why no handle names an object yet.
 *
 *   xudlibc is a C11 library shaped after raylib's public API but rendered with Vulkan:
 *   no OpenGL, no EGL, no fixed-function, no GL state machine. raylib is an API-shape
 *   reference only (reference/raylib/, read-only) - it is never included or linked.
 *
 *   Rules every declaration here obeys:
 *     - every function returns XudResult, or fills an out-param; nothing is ignored
 *     - out-params must be NULL on entry, and are NULL on every failure path
 *     - one XudContext owns instance/device/swapchain/allocator/renderer and the handle tables
 *       behind the public currency; callers hold a handle, a context pointer and nothing else
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
 * The value types a caller passes down
 *
 * These are raylib's own two - Vector3 (reference/raylib/raylib.h 222) and Color (253) - and
 * they are value types in the opposite bucket from the handle below: a handle ADDRESSES an
 * object the library owns and a caller never reaches into, and these two carry the bytes a
 * caller hands DOWN for the library to write into the batch's streams. Nothing here owns a
 * driver name, so neither type has a lifetime, a close or an error code of its own.
 *
 * The colour is four uint8_t in R,G,B,A order, which is the byte order the batch's COLOR strea
 * m's VkFormat is in (xud_batch.h's note: VK_FORMAT_R8G8B8A8_UNORM, and the reason that forma
 * t and not an invented one). A caller's colour is therefore the stream's bytes, read field-
 * wise rather than converted.
 *-----------------------------------------------------------------------------------------------*/

/// The one 3-component vector a shape's point is named by - raylib's Vector3, same three float
/// fields, same by-value passing.
typedef struct XudVector3 {
    float x; /* vector x component */
    float y; /* vector y component */
    float z; /* vector z component */
} XudVector3;

/// The one colour a shape is filled with - raylib's Color, R8G8B8A8, four bytes and no more.
typedef struct XudColor {
    uint8_t r; /* colour red value   */
    uint8_t g; /* colour green value */
    uint8_t b; /* colour blue value  */
    uint8_t a; /* colour alpha value */
} XudColor;

static_assert(sizeof(XudColor) == 4u,
    "xud.h: a colour is the four bytes the batch's COLOR stream holds, so the public type and the "
    "stream's element are one number and not a conversion");

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
/// nothing about it is a constant this layer hoped for), then the allocator rung (one host
/// allocation, calls no driver function and creates no block, so a context that comes back XUD_OK
/// owns an allocator with no VkDeviceMemory in it at all), then the renderer rung above it (one
/// pass, one pipeline, one framebuffer per swapchain image, one frame loop, and its own open runs
/// the pass/pipeline/framebuffer/frame ladder), then the handle table as the top rung. On entry
/// *outContext must be NULL; it is NULL on every failure path, and every failure path tears down
/// what was built - a refused request leaks nothing and a valid one never hands back a half-built
/// context. XUD_UNSUPPORTED means this machine cannot host a context at all (no Vulkan loader, no
/// window system, no device this layer can take, a window system this device cannot present to,
/// a surface this layer cannot get a swapchain for, a renderer or an allocator this layer cannot
/// open); no field of XudContext is a sentinel any more, and what is missing is not a field but a
/// rung above the renderer - the rest of raylib's 3D shape family, the camera + transform math an
/// d the object kinds a public handle would name - which is why a caller can draw a triangle, a box
/// and a sphere today and nothing else, and why no handle names an object yet.
XudResult xud_init_context(XudContext** outContext, uint32_t width, uint32_t height);

/// Tear the context down in the reverse order of creation - handle table -> renderer -> allocator
/// -> swapchain -> device -> surface -> window -> physical -> instance -> diagnostics -> loader -
/// and free it. The handle table goes first because it is the TOP of the tree: a live table can
/// name objects the rungs below it are about to retire, so it must die before them. The renderer
/// goes next because it is the owner of the four render rungs and every submission that reads a
/// pipeline, a pass, a framebuffer or a command buffer is its submission. The allocator goes next
/// because a VkDeviceMemory is created FROM the VkDevice below it and a submission or a presentation
/// may still be reading the very blocks its own close frees (that close runs the device's wait-idle
/// guard first, for exactly that reason). The swapchain goes next because it is presented through a
/// queue of the device below it, its own close runs that device's wait-idle guard, and it is the
/// last rung the bring-up ladder built below the allocator. The device goes next because a
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

/*-----------------------------------------------------------------------------------------------
 * The drawing API - the frame pair and the shapes a frame is made of
 *
 * This is the raylib-shaped half of the public surface above the context: BeginDrawing/EndDrawi
 * ng (raylib.h 1048/1049) and the 3D shape calls (DrawTriangle3D, raylib.h 1588, DrawCube, 1590,
 * DrawSphere, 1594).
 * The pair is the public frame boundary and the shape calls are what sits between them, which is the order
 * raylib's own comment states for the pair (raylib.h 1100: "By default EndDrawing() does this
 * job: draws everything + SwapScreenBuffer() + manage frame timing + PollInputEvents()") - the
 * draws append, the end of the frame is what submits and presents them. Nothing here is a stat
 * e machine and nothing here is a GL call: the pair maps onto the renderer rung's one frame ca
 * ll and the shape calls map onto the batch's appends.
 *-----------------------------------------------------------------------------------------------*/

/// Begin canvas drawing: the public frame boundary, opened. It resets the batch, so the frame
/// about to be drawn starts from an empty one rather than from whatever the frame below left be
/// hind - the frame loop's own reset at the end of a completed frame is the same property seen
/// from the other side, and this call is what makes it a public boundary instead of a side effe
/// ct of a rung a caller cannot name. XUD_INVALID_ARG on a NULL context or on a renderer whose
/// batch rung is gone; XUD_UNSUPPORTED on a context that never completed bring-up.
XudResult xud_begin_drawing(XudContext* context);

/// Draw one filled triangle, counter-clockwise vertex order in raylib's own wording (raylib.h
/// 1588): the three points are appended in the order the caller named them, with the flat +Z no
/// rmal and the zero uv xud_shapes.h states, and the colour is written to the batch's COLOR str
/// eam in the caller's R,G,B,A order. The append is atomic: a triangle that does not fit the bat
/// ch's capacities is refused before a single byte of it is written. The coordinates are the
/// shape's own - the frame's viewProj and this draw's model matrix are both identity today, so
/// the space they land in is the clip space the vertex shader answers in, and a non-identity ma
/// trix is the camera + transform step's to choose (see todo.md). XUD_INVALID_ARG on a NULL con
/// text or a renderer with no batch; XUD_UNSUPPORTED on a context that never completed bring-up
/// ; XUD_OUT_OF_MEMORY when the three vertices or the three indices do not fit the batch.
XudResult xud_draw_triangle_3d(XudContext* context, XudVector3 v0, XudVector3 v1, XudVector3 v2, XudColor color);

/// Draw one filled box, raylib's DrawCube (raylib.h 1590, "Draw cube", whose arguments are (positi
/// on, width, height, length, color)): the position is the box's CENTRE and the three floats are
/// its full extents along x/y/z, which is how raylib's own DrawCube reads them (start = position -
/// size*0.5, end = position + size*0.5). The box is axis-aligned, because a box that could be rota
/// ted would be rotated by a model matrix and the model matrix is the camera + transform step's.
/// It appends 24 vertices - four per face, so a face's outward normal belongs to that face alone -
/// and the 36 indices that name them, two triangles per face, wound counter-clockwise seen from o
/// utside; the uv is the zero xud_shapes.h states and the colour is the caller's at every vertex.
/// The append is atomic in the same shape as the triangle's: a box that does not fit the batch's
/// capacities is refused before a byte of it is written, and one box fits the batch's initial capa
/// city with no grow asked for. The coordinates are the shape's own, in the same clip space the
/// triangle's land in today. XUD_INVALID_ARG on a NULL context or a renderer with no batch; XUD_
/// UNSUPPORTED on a context that never completed bring-up; XUD_OUT_OF_MEMORY when the 24 vertices
/// or the 36 indices do not fit the batch (both bounds are checked because an index names a live
/// vertex rather than a new one, so a batch whose existing content reuses vertices can be full on
/// its indices while it still has twenty-four vertices' worth of room).
XudResult xud_draw_cube(XudContext* context, XudVector3 position, float width, float height, float length, XudColor color);

/// Draw one filled sphere, raylib's DrawSphere (raylib.h 1594, "Draw sphere", whose arguments are
/// (centerPos, radius, color)): the position is the sphere's CENTRE and the float is its RADIUS,
/// which is the same centre-not-corner reading DrawCube above already makes. It appends 17 vertic
/// es and the 90 indices that close them - two single-vertex poles, three rows of five vertices be
/// tween them, two fans of five triangles and two bands of five quads - and the NORMAL of every ve
/// rtex is the unit direction that placed it, which is what makes a sphere share vertices where a
/// box cannot: a sphere's normal is radial by definition, so the vertex's own direction is the one
/// that shades it and no corner needs three of them. That sharing is also why the sphere spends the
/// 6V-12 indices a closed MANIFOLD surface needs (90 over its 17 vertices, four times the 3n/2 per
/// vertex headroom the batch sizes at - the box's 1.5 per vertex is its SPLIT-per-face spelling rat
/// her than that bound), and why the INDEX bound is the one that refuses a sphere first (xud_shape
/// s.h
/// states the same fact as an assert, both ways round: one sphere fits the batch's initial index hea
/// droom
/// and two do not). The tessellation is the header's, not the ca
/// ller's: a tessellation the caller chose is a claim about the batch's capacity rather than about
/// the sphere, and this rung is pure and cannot ask the batch to grow. A radius that is not strictl
/// y positive is refused rather than drawn - a sphere with no radius has no surface and no radial
/// direction, and the normal is that direction divided by it, so a zero radius would put a NaN in
/// the NORMAL stream rather than a value the pipeline can read (a stronger reason than the box's
/// flat face has for the same refusal); a NaN radius is refused by the same test. The append is ato
/// mic in the same shape as the triangle's and the box's, and BOTH of the batch's bounds are read
/// first. XUD_INVALID_ARG on a NULL context or a renderer with no batch; XUD_UNSUPPORTED on a con
/// text that never completed bring-up; XUD_OUT_OF_MEMORY when the 17 vertices or the 90 indices do
/// not fit the batch.
XudResult xud_draw_sphere(XudContext* context, XudVector3 centerPos, float radius, XudColor color);

/// End canvas drawing and swap buffers: the public frame boundary, closed. It runs one frame -
/// acquire, record, upload, bind, draw the batch's recorded rows, submit, present - through the
/// renderer rung this context owns, and then acts on the recreate ask that frame left behind by
/// recreating the swapchain at the window's FRAMEBUFFER extent, which is the one place above the
/// frame loop a public caller can act on it: the swapchain is the context's, so a recreate repla
/// ces every image, every framebuffer row and every fence index the renderer holds, and it has
/// to happen between two draws rather than inside one. XUD_INVALID_ARG on a NULL context or a
/// context missing a rung this frame needs; XUD_UNSUPPORTED when the loader is closed or its ro
/// ws are not this device's; the frame's own codes otherwise, and a frame that refused leaves th
/// e batch alone.
XudResult xud_end_drawing(XudContext* context);

#ifdef __cplusplus
}
#endif

#endif /* XUD_XUD_H_ */
