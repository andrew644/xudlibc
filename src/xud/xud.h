/***********************************************************************************************
 *
 *   xud.h - xudlibc public API
 *
 *   Owns : the public API surface and nothing else - version macros, build-time config
 *          switches (XUD_VALIDATE / XUD_GPU_TESTS), the runtime environment-variable names
 *          the library reads, the XudResult error enum, the value types a caller passes
 *          down (XudVector2 / XudVector3 / XudColor), the one XudHandle currency and its kind
 *          tags, the opaque XudContext owner type, the xud_init_context()/xud_close_context()
 *          life-cycle pair, the drawing API above it (xud_begin_drawing() / xud_draw_triangle_3d()
 *          / xud_draw_cube() / xud_draw_sphere() / xud_draw_line_3d() / xud_draw_point_3d() /
 *          xud_draw_cube_wires() / xud_draw_sphere_wires() / xud_begin_camera_3d() / xud_end_
 *          camera_3d() / xud_clear_background() / xud_end_drawing()), the object API above the
 *          handle layer (the three crea
 *          te/close pairs xud_create_material() / xud_close_material() / xud_create_texture() / xud_
 *          close_texture() / xud_upload_mesh() / xud_close_mesh() and the scoped-state pair xud_be
 *          gin_material() / xud_end_material()), the value types a caller passes down (XudVector2 /
 *          XudVector3 / XudColor / XudMaterial / XudTextureData / XudMeshData), the input API above the input rung (raylib's keyboard and
 * mouse half - xud_ is_key_pressed() / xud_is_key_pressed_repeat() / xud_is_key_down() / xud_is_key_relea sed() / xud_is_key_up() /
 * xud_get_key_pressed() / xud_get_char_pressed() / xud_get_key _name() and the mouse block below
 * it), the clock API above the clock rung (raylib's GetTime/GetFrameTime half - xud_get_time() / xud_get_frame_time() /
 * xud_get_timer_value() / xud_get_timer_frequency()), the font API above the font rung (raylib's LoadFontEx/MeasureTextEx/GetGlyphInfo half
 * - xud_load_font() / xud_close_font() / xud_measure_text() / xud_get_glyph_index() / xud_get_glyph_info() / xud_get_glyph_atlas_rec() / xud_get_font_atlas_dim()
 *   / xud_draw_text() / xud_draw_text_ex()),
 * the value types a caller passes down (XudVector2 / XudVector3 / XudColor / XudMaterial / XudTextureData / XudMeshData / XudFontData /
 * XudGlyphInfo / XudGlyphAtlasRec), the key and mouse-button tokens that API's queries take, and the runtime Vulkan query xud_vul
 * kan_supported(). Layer: top of the stack (public API). Everything below it is internal - xud_core.h/.c, xud_loa der.h/.c,
 * xud_vulkan.h/.c, xud_diagnostics.h/.c, xud_instance.h/.c, xud_physical.h/.c, xud_device.h/.c, xud_window.h/.c, xud_swapchain.h/.c,
 * xud_handle.h/.c, xud_shader.h/.c, xud_pipeline.h/.c, xud_renderpass.h/.c, xud_frame.h/.c, xud_renderer.h/.c, xud_alloca tor.h/.c,
 * xud_descriptor.h/.c, xud_batch.h/.c, xud_camera.h/.c, xud_input.h/.c, xud_mat erial.h/.c, xud_mesh.h/.c, xud_texture.h/.c, xud_time.h/.c
 * and xud_font.h/.c - and user code includes this header only; tests may include the internal ones. Every layer below this one is in the
 * tree now, and every one of them stays internal be hind the two exceptions this note names: nothing about a VkDevice, its queues, a GLFW
 * window, a VkSurfaceKHR, a VkSwapchainKHR, a VkPipeline, a VkRenderPass, a VkFramebuffer, a VkCommandBuffer or a VkDeviceMemory is
 * reachable from here; the conte xt owns all of them (S8's ownership step is landed, S9's swapchain rung sits on top of it, S10's handle
 * layer and S11's five render rungs on top of that, S12's allocator rung sits above the swapchain and below the descriptor and the
 * renderer, and S16's batch and S18's descriptor rungs sit below the renderer above it, so xud_init_context() completes to the renderer and
 * xud_close_context() tears all of it back and NO field of XudConte xt is a sentinel any more). The window layer, the swapchain layer, the
 * render rungs, the allocator, the descriptor and the batch are internal for the same reason every laye r below the public API is: xud.h
 * declares no window type, no swapchain type, no rende r type, no allocator type, no batch type and calls no GLFW function, and the
 * VkSurfac eKHR a context owns lives inside the XudWindow it owns rather than in a field of its own. The TWO exceptions to that rule are
 * the handle and the shapes rung's public call s, and both are public for the reason their own notes state: the handle is the curren cy a
 * caller holds and the slot arrays behind it are internal (xud.h declares no table type and calls no handle function, so no public call
 * takes or fills a table today and the object pairs above it are what hand a handle out and what a handle names); the sha pes rung's calls
 * are the geometry a caller draws, which is what the drawing API above the renderer reaches. What is here today above the renderer is the
 * frame pair, the shapes family (a triangle, a box, a sphere, a line, a point and the two wire forms), the camera pair, the material pair,
 * the mesh pair, the model pair, the text pair, the screen-space pair (xud_draw_texture() / xud_draw_text_screen()) and the draws that
 * name a handle (xud_draw_mesh() / xud_draw_mesh_ex() / xud_draw_model() / xud_draw_model_ex()), the _ex forms carrying the caller's
 * model matrix and colour the row's own push channel spends; no object kind is left without a rung above the table today (the FONT kind's
 * public half landed with the font pair, which was the last one missing), and the textured 2D surface is the pipeline list's UI column
 * with its screen-fixed view and depth off, the same row the font's screen walk and the UI draw record their quads into.
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
#include <stdbool.h> /* Required for: bool - the input queries' answers, which are states rather than codes */
#include <stdint.h> /* Required for: uint32_t - window extents and handle fields */

#include "xud_nuklear.h" /* Required for: the nuklear API the ui rung exposes - struct nk_context (what xud_ui_get_context() hands back and what the caller's nk_begin / nk_button_label / nk_end frame is written against). The one deliberate non-xud include in the public surface: the gui is a first-class rung, not an opaque handle. */

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

/* Public host-side bound on window extent, per axis: above it the public entry points refuse as a
   bad argument, and the device's own 2D image limit (maxImageDimension2D, recorded into XudDeviceCaps
   at step S5) tightens it further - xud_init_context() and xud_set_window_size() refuse an extent
   above the smaller of the two as XUD_UNSUPPORTED. */
#define XUD_MAX_WINDOW_EXTENT 16384u

/* The ceiling of live textures one context can hold: the sampler array is 512 entries and six
   of them are permanent residents - the white default, the HDR scratch and the four bloom-chain
   levels, all claimed at bring-up - so 506 of them are textures'. The array's width is frozen
   into the pipeline layout, so the number is fixed per context: a scene that loads a 507th dis
   tinct texture gets XUD_OUT_OF_RESOURCES out of xud_create_texture, and font atlases, UI atla
   ses and model maps all count against the same ceiling. */
#define XUD_MAX_TEXTURES 506u

/* The ceiling of lights one context can shade by: the frame's uniform block carries a fixed table
   of XUD_MAX_LIGHTS rows (xud_descriptor.h's XudLightRow) and the pipeline layout freezes that
   width into every pipeline, so the number is fixed per context. Dozens of dynamic lights want the
   clustered path todo.md's S4.2 states, not a wider table on this rung. */
#define XUD_MAX_LIGHTS 8u

/* The three kinds the fragment stage branches on. The numbers are the light table's params.w
   values, so they are the wire format between the C record and the shader and the tokens are their
   names - the same convention XUD_CAMERA_MODE_* gives the camera's axis. */
#define XUD_LIGHT_KIND_SUN 0u /* light from a direction at infinity: a direction, a colour, no position, no range */
#define XUD_LIGHT_KIND_POINT 1u /* light from one world point: fades with range, no cone */
#define XUD_LIGHT_KIND_SPOT 2u /* light from one world point inside one cone: fades with range AND cone angle */
#define XUD_LIGHT_KIND_COUNT 3u /* the length of this axis */

static_assert(XUD_LIGHT_KIND_COUNT == (XUD_LIGHT_KIND_SPOT + 1u),
    "xud.h: the kind count is one past the last token, so the setter's range check is one "
    "comparison rather than a list of three cases - the same shape XUD_CAMERA_MODE_COUNT gives the "
    "camera's modes");

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
    XUD_INTERNAL_ERROR = 8, /* an internal invariant broke: close the context and start over */
    XUD_NOT_FOUND = 9, /* a path the caller handed names no file the library can open        */
    XUD_MALFORMED = 10 /* a file that exists is not a container or document this library can read */
} XudResult;

/* Human-readable name for an error code - for logging and for test output.
   Never returns NULL, and never allocates. */
const char* xud_result_name(XudResult result);

/*-----------------------------------------------------------------------------------------------
 * The value types a caller passes down
 *
 * These are raylib's own - Vector2 (reference/raylib/raylib.h 221), Vector3 (222) and Color (253)
 * - and they are value types in the opposite bucket from the handle below: a handle ADDRESSES an
 * object the library owns and a caller never reaches into, and these carry the bytes a
 * caller hands DOWN - a shape's point into the batch's streams, a mouse position out of the
 * input rung's records. Nothing here owns a driver name, so no one of them has a lifetime, a
 * close or an error code of its own.
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

/// The one 2-component vector a mouse position and a mouse delta are named by - raylib's Vector2
/// (reference/raylib/raylib.h 221), the same two float fields, and the one public type the input
/// API's position and delta queries fill rather than a pair of loose coordinates: raylib's GetMou
/// sePosition, GetMouseDelta and GetMouseWheelMoveV all return this.
typedef struct XudVector2 {
    float x; /* vector x component */
    float y; /* vector y component */
} XudVector2;

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

/// The one 3D camera a frame is drawn with - raylib's Camera3D (reference/raylib/rcamera.h 102:
/// position, target, up, fovy) plus the two frustum planes, which raylib's Camera aggregate carries
/// (rcore.h, not in reference/) and this library has no aggregate to carry them, so they sit on the
/// one record. A value type in the same bucket as the two above: a caller hands it DOWN, it names
/// nothing the library owns, and it has no lifetime, no close and no handle of its own. The camera
/// is the one perspective camera the whole scope is drawn with (AGENTS.md: 3D only, one camera, one
/// depth buffer, one render pass) and the two planes are the caller's because the scene's depth
/// budget is the caller's - the library's depth buffer is D32_SFLOAT and maps [near,far] to the
/// reversed 1/z range (near at 1, far at near/far), which keeps precision at both ends of a wide
/// frustum, so a scene that states its own planes gets the whole depth range across its own geometry.
typedef struct XudCamera3D {
    XudVector3 position; /* the eye, in world units                                       */
    XudVector3 target; /* what it looks at - the forward vector is target - position    */
    XudVector3 up; /* the rotation axis, raylib's "rotation over its axis"         */
    float fovy; /* the vertical aperture in degrees, raylib's fovy              */
    float near; /* the near plane's distance from the eye, in world units       */
    float far; /* the far plane's distance from the eye, in world units        */
} XudCamera3D;

static_assert(sizeof(XudCamera3D) == 48u,
    "xud.h: the camera is twelve floats - nine of vectors plus three of the aperture and the two "
    "planes - align 4 and no padding past a float, so the record a caller declares is the record "
    "the camera rung reads");

/// The one ray a screen point is traced along - raylib's Ray (reference/raylib/raylib.h 445:
/// position, direction), the direction already normalised. A value type in the same bucket as the
/// three above: xud_get_screen_to_world_ray() fills one, the collision calls below read one, and it
/// owns nothing, so it has no lifetime and no close.
typedef struct XudRay {
    XudVector3 position; /* the ray's origin, in world units        */
    XudVector3 direction; /* the ray's direction, normalised          */
} XudRay;

static_assert(sizeof(XudRay) == 24u,
    "xud.h: a ray is six floats - an origin and a direction - so the record the camera's fold fills "
    "is the record the collision rung reads with no conversion between them");

/// The axis-aligned box a collision test is about - raylib's BoundingBox (reference/raylib/raylib.h
/// 459: min, max). The two corners rather than a centre and a size: the corners are what the slab casts
/// divide by, and a caller holding a centre and a size - every DrawCube-shaped call in the picking and
/// the player examples - writes the two halves out once at the call.
typedef struct XudBoundingBox {
    XudVector3 min; /* the corner with the least of every axis */
    XudVector3 max; /* the corner with the most of every axis  */
} XudBoundingBox;

static_assert(sizeof(XudBoundingBox) == 24u,
    "xud.h: a box is six floats - two corners - so the record a caller builds from a centre and a size "
    "is the record the collision rung reads with no conversion between them");

/// What one cast hit - raylib's RayCollision (reference/raylib/raylib.h 451: hit, distance, point,
/// normal). A value type, filled whether or not anything was hit: the two casts write all-zero for a
/// miss, so a caller that declares one outside its loop and reads it every frame needs no clearing.
typedef struct XudRayCollision {
    bool hit; /* did the ray meet the solid?            */
    float distance; /* how far along the direction it met it */
    XudVector3 point; /* the hit, in world units                */
    XudVector3 normal; /* the surface's outward normal, unit length */
} XudRayCollision;

static_assert(sizeof(XudRayCollision) == 32u,
    "xud.h: a hit is a flag, a distance and two vectors - the flag and the distance share one eight-byte "
    "row and nothing pads past that - so the record the cast fills is the record a caller declares");

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
/// close of the object it names is REJECTED, not dangling - the generation bump is what rejects it
/// . The object steps are what hand one out, and one of them is in the tree: the MATERIAL kind's
/// create/close pair (xud_create_material()/xud_close_material(), below the drawing API) is the on
/// e public pair that fills and releases one today, and three more since (the MESH, TEXTURE and
/// MODEL kinds' pairs do too), which is why the one kind left without a step is the FONT kind.
typedef struct XudHandle {
    uint32_t index; /* which slot of its kind's array this names                                   */
    uint32_t generation; /* bumped when the slot it named was released and taken again, so a handle kept across that stops naming it */
    uint32_t kind; /* the tag of the object kind, XUD_HANDLE_KIND_* - never XUD_HANDLE_KIND_NONE in a live slot */
} XudHandle;

#define XUD_HANDLE_KIND_NONE 0u /* the tag of a handle that names nothing, and of a zero-initialised one - never stored in a live slot */
#define XUD_HANDLE_KIND_TEXTURE 1u /* a texture: an image + its sampler variant, once the allocator and the renderer rungs land */
#define XUD_HANDLE_KIND_FONT 2u /* a font: the glyph atlas and its metrics, once the text step lands */
#define XUD_HANDLE_KIND_MESH 3u /* a mesh: the attribute streams and the index buffer, once the batch lands */
#define XUD_HANDLE_KIND_MATERIAL 4u /* a material: a pipeline variant and its sampler-array entry, once the pipelines land */
#define XUD_HANDLE_KIND_MODEL 5u /* a model: the meshes and materials a load produced, once model loading lands */
#define XUD_HANDLE_KIND_AUDIOSTREAM 6u /* an audio stream: the decoded frames and their format, once the audio rung lands */
#define XUD_HANDLE_KIND_RENDERTEXTURE                                                                                                      \
    7u /* an off-screen render target: the image, its sampler entry and the pass that draws it, once the target rung lands */
#define XUD_HANDLE_KIND_MAX                                                                                                                \
    XUD_HANDLE_KIND_RENDERTEXTURE /* the highest kind value in use today - a new caller-addressable kind moves this and its own value,      \
                                      which is why the table's row count below is derived rather than written */
#define XUD_HANDLE_KIND_COUNT                                                                                                              \
    (XUD_HANDLE_KIND_MAX + 1u) /* one past the highest kind value, which is the row count of the table's per-kind arrays */

/// The handle that names nothing, spelled out: a zero-initialised XudHandle carries the kind no liv
/// e slot ever has, so this is the value a caller writes where a public description may name an obj
/// ect but does not have to - the diffuse map of an unmapped XudMaterial today.
#define XUD_HANDLE_NONE ((XudHandle) { .index = 0u, .generation = 0u, .kind = XUD_HANDLE_KIND_NONE })

static_assert(sizeof(XudHandle) == 12u,
    "xud.h: the handle is three uint32_t and no padding - index, generation and tag - so a layout change is a change of the public "
    "currency, not of one layer");
static_assert(XUD_HANDLE_KIND_NONE == 0u,
    "xud.h: NONE is the 0th kind value, which is what makes a zero-initialised handle carry the tag no live slot ever has");
static_assert(XUD_HANDLE_KIND_MAX >= XUD_HANDLE_KIND_NONE,
    "xud.h: the highest kind value must be a kind value, or the table's row count below is a row count of nothing");

/*-----------------------------------------------------------------------------------------------
 * The texture - the object kind a handle names that has a rung above the handle table
 *
 * A texture is the object a XUD_HANDLE_KIND_TEXTURE slot records, and the value type below is what
 * a caller describes when it asks for one. raylib's Texture2D (reference/raylib/raylib.h 273-279) is
 * { int id; int mipmaps; int width; int height; } - one GL texture name carrying texels, filtering and
 * wrap mode inside it, because GL's texture object IS the sampling state. Vulkan splits the three: a
 * VkImage is bytes with a format and no sampling state, a VkSampler is sampling state with no bytes,
 * and a VkImageView is the one thing a descriptor may name (an image alone cannot go in a descriptor -
 * VkDescriptorImageInfo names .imageView, not .image). All three are therefore the library's to own
 * behind one handle, and the pair below hands out one handle for all three.
 *
 * The description is one array of texels plus three tokens, and the texels are written at the create
 * call rather than at a later one: a texture's bytes are written once, by the CPU, at the upload, and
 * read by a shader for the texture's whole life - the same reason the mesh's arena is mapped once at
 * open and unmapped once at close. The array is therefore read once and the caller may free it as soon
 * as the call returns.
 *
 * The mip chain is the library's work rather than the caller's data: raylib generates one too (its
 * Texture2D has an `int mipmaps` member and GenTextureMipmaps is what fills it), and the chain's length
 * is the spec's maximum for the extent rather than a quality level chosen behind the caller's back, so
 * every LOD a fragment stage's derivative can select has a level to select. The format is this rung's
 * one constant (R8G8B8A8_UNORM, four bytes per texel, the one format that needs no decoder here) rather
 * than an axis, and the format axis is an open ask in todo.md.
 *
 * The tokens are this library's own rather than raylib's names: raylib's SetTextureFilter takes a one-
 * token filter and its SetTextureWrap takes three wrap tokens, and xud_texture.h asserts the three
 * public tokens equal Vulkan's own numbers, so a caller's token is a name in this library's vocabulary
 * and a fact about the driver at the same time, with no translation table in between.
 *-----------------------------------------------------------------------------------------------*/

#define XUD_TEXTURE_FILTER_NEAREST 0u /* nearest texel, no interpolation */
#define XUD_TEXTURE_FILTER_LINEAR 1u /* interpolated between texels */
#define XUD_TEXTURE_FILTER_COUNT 2u /* the length of this axis */

#define XUD_TEXTURE_ADDRESS_REPEAT 0u /* wrap */
#define XUD_TEXTURE_ADDRESS_MIRRORED_REPEAT 1u /* wrap with a mirrored neighbour */
#define XUD_TEXTURE_ADDRESS_CLAMP_TO_EDGE 2u /* the edge texel, raylib's CLAMP */
#define XUD_TEXTURE_ADDRESS_COUNT 3u /* the length of this axis */

static_assert(XUD_TEXTURE_FILTER_COUNT == (XUD_TEXTURE_FILTER_LINEAR + 1u),
    "xud.h: the two filter tokens are contiguous from nearest up, so the count is one past the highest");
static_assert(XUD_TEXTURE_ADDRESS_COUNT == (XUD_TEXTURE_ADDRESS_CLAMP_TO_EDGE + 1u),
    "xud.h: the three wrap tokens are contiguous from repeat up, so the count is one past the highest");

/// The one texture description a caller hands down: the extent, the texels, and the two tokens that
/// name the sampling state. Every member is caller-owned and read once at the create call; the texel
/// array is the one array a texture has, and unlike a mesh's optional attribute streams it has no
/// neutral value that means "draw this" - a texture with no texels is not a texture with a default.
/// width*height must be at least one texel and at most the bound the texture rung states beside its
/// own note, and every texel is four bytes in the order the rung's one format names.
typedef struct XudTextureData {
    uint32_t width; /* mip 0's width, in texels */
    uint32_t height; /* mip 0's height, in texels */
    const uint8_t* rgba; /* mip 0's texels, width*height*4 bytes, read once at the create call */
    uint32_t filter; /* XUD_TEXTURE_FILTER_*: what both the minifying and the magnifying filter do */
    uint32_t addressMode; /* XUD_TEXTURE_ADDRESS_*: the wrap mode of the texture's two live axes */
} XudTextureData;

/*-----------------------------------------------------------------------------------------------
 * The material - the one object kind a handle names that has a rung above the handle table
 *
 * A material is the object a XUD_HANDLE_KIND_MATERIAL slot records, and the value type below is
 * what a caller describes when it asks for one. raylib's Material (reference/raylib/raylib.h 388
 * -392) is { shader, maps[], params[] } and none of those three is a blend mode - raylib's blend
 * mode is a global its BeginBlendMode()/EndBlendMode() pair scopes (raylib.h 1058-1059). In Vulka
 * n the blend attachment state is PIPELINE state rather than draw state, so the row of the pipeli
 * ne list a draw binds is the material's own, and the first member this type carries is that row.
 * The shader half is not a member at all: this tree has one shader pair and no second pipeline to
 * choose between, so a per-material shader is an open ask rather than a field.
 *
 * The maps half is ONE member here rather than raylib's ten-row array, and the one map is the dif
 * fuse map (raylib's MATERIAL_MAP_DIFFUSE, the alias of MATERIAL_MAP_ALBEDO at raylib.h 771/784, w
 * hich SetMaterialTexture names - raylib.h 1657) because it is the only map this tree has a pipeli
 * ne row that reads: the textured variant's frag module reads the sampler array and no other row samples any
 * thing. So a material that names a map draws through XUD_BLEND_TEXTURED whatever its blendMode sa
 * ys - the mapped surface is a different pipeline row, not a flag beside the row - and the other n
 * ine of raylib's maps (metalness, normal, roughness, occlusion, emission, height, the three cube
 * s, BRDF) are open asks rather than members, because a member nothing reads is a member a caller
 * would set for nothing.
 *
 * The map is a handle and the texture rung's slot stays the owner of the image: a material NAMES i
 * ts map and does not own it, so two materials may sample one texture, closing the material closes
 * nothing of the texture's, and a texture closed while a material still names it is caught at the
 * next xud_begin_material() as a refusal rather than drawn through as a sample of a destroyed imag
 * e.
 *
 * The tokens are this library's own two variants rather than raylib's BLEND_ALPHA/ADDITIVE/... enum
 * : raylib's eight names are blend FACTOR pairs and this tree implements exactly one of them (SRC_
 * ALPHA over ONE_MINUS_SRC_ALPHA plus VK_BLEND_OP_ADD, which is raylib's own default glBlendFunc),
 * so a token naming an unimplemented factor pair would be a token that refuses at the create call
 * rather than a token that means something. xud_material.h asserts the tokens equal the batch's va
 * riant axis, so the mirror is a compile-time fact rather than a table someone trusts.
 *-----------------------------------------------------------------------------------------------*/

#define XUD_BLEND_OPAQUE 0u /* blend disabled: every draw of this material overwrites what is behind it */
#define XUD_BLEND_TRANSPARENT                                                                                                              \
    1u /* blend enabled, SRC_ALPHA over ONE_MINUS_SRC_ALPHA plus VK_BLEND_OP_ADD: caller-ordered, the library never sorts */
#define XUD_BLEND_TEXTURED                                                                                                                 \
    2u /* blend enabled against the sampled texel: the textured pipeline row, the one a glyph atlas needs, because a glyph is alpha and an \
          unblended glyph over a cleared background is a rectangle of the atlas rather than a letter */
#define XUD_BLEND_COUNT 3u /* the length of this axis, which is the length of the pipeline list's variant axis */

static_assert(XUD_BLEND_COUNT == (XUD_BLEND_TEXTURED + 1u),
    "xud.h: the three blend tokens are contiguous from opaque up, so the count is one past the highest");

/// The one material description a caller hands down: the blend behaviour every draw of this materi
/// al uses, and the one texture those draws sample. A value type in the handle's opposite bucket -
/// it names nothing the library owns, has no lifetime and no close of its own, and the object it d
/// escribes is the handle's. The blendMode is the row of the pipeline list the draws bind UNLESS di
/// ffuseMap names a texture: the row that samples a map is a different pipeline row, so a mapped ma
/// terial draws through XUD_BLEND_TEXTURED, whose blend attachment state is the transparent row's -
/// so the override loses the name of the blend mode and nothing else. A diffuseMap of kind XUD_HAN
/// DLE_KIND_NONE (XUD_HANDLE_NONE, which is what a zero-initialised description already carries) is
/// the no-map state and moves no row.
typedef struct XudMaterial {
    uint32_t blendMode; /* XUD_BLEND_* - the row of the pipeline list this material's draws bind, unless the map below names a texture */
    XudHandle diffuseMap; /* a live TEXTURE slot of this same context, or XUD_HANDLE_NONE - the texel every draw of this material samples */
} XudMaterial;

static_assert(sizeof(XudMaterial) == 16u,
    "xud.h: a material description is the axis's uint32_t plus the three uint32_t of one handle, s"
    "o a caller's record is a variant and a named map and not a conversion");

/// The one mesh description a caller hands down: the geometry of one object, as the batch's four at
/// tribute streams plus the one index buffer the settled decision names. Every array is the caller's
/// own and is read once, at the upload, never kept - the library copies it into the mesh's block and
/// lets go of the caller's arrays after that. A NULL attribute array is not a refusal: the mesh's st
/// reams all have to exist, so a missing one is filled with that stream's documented default (zero p
/// osition, zero uv, opaque white, +Z normal) rather than leaving a stream of nothing. The index arr
/// ay has no default - a mesh without indices is a mesh whose topology is undefined - and every ind
/// ex must be below vertexCount, because an out-of-range index is not something a driver would refus
/// e later, it is undefined behaviour at the draw.
typedef struct XudMeshData {
    uint32_t vertexCount; /* vertices this mesh holds, the one count every stream is sized from */
    uint32_t indexCount; /* indices it holds - at least three and a whole number of triangles */
    const float* pos; /* vertexCount*3, or NULL for a zero stream                        */
    const float* texcoords; /* vertexCount*2, or NULL for a zero stream                      */
    const uint8_t* color; /* vertexCount*4, or NULL for opaque white                         */
    const float* normal; /* vertexCount*3, or NULL for +Z                                   */
    const uint32_t* index; /* indexCount, every one below vertexCount - the one array with no default */
} XudMeshData;

/*-----------------------------------------------------------------------------------------------
 * The font - the one object kind a handle names whose atlas is another rung's texture
 *
 * A font is the object a XUD_HANDLE_KIND_FONT slot records, and the value type below is what a
 *   caller describes when it asks for one: a font FILE rather than a baked atlas, which is raylib's
 *   own shape (its LoadFontEx takes a fileName and a fontSize and does the baking inside, raylib.h
 *   410) and the reason this type names bytes rather than a path - a path would make this call a fil
 *   e-system read, and the loader rung that reads a model's files is a different rung's decision.
 *
 * What the description does NOT carry, and why: the atlas's extent and the two sampling tokens. The
 *   extent is this rung's to find (stb's own packer note says its packing is "very crappy", so no numb
 *   er a caller could write predicts whether a set fits), and the tokens are a font's rather than a caller
 *   's: a glyph's coverage is a gradient, so a NEAREST filter turns every edge into a stair, and an atlas i
 *   s not a tileable surface, so a REPEAT would bleed the glyph at the far edge into the glyph at this edge. A
 *   caller that wants a different answer wants a different texture, and the texture rung below already owns tha
 *   t one.
 *
 * The codepoint array's ordering is a precondition rather than a convenience: the lookup every draw and
 *   every measure owes is a binary search over the rows, and raylib's own LoadFontEx sorts the array it was
 *   handed before it packs the atlas (text.c), which is the same requirement stated here as a precondition ins
 *   tead of a sort this library performs behind a caller's back - a sort would move a caller's rows relative t
 *   o the array it kept, and a caller that indexed its own array would then be reading the wrong glyph.
 *-----------------------------------------------------------------------------------------------*/

/// The one font description a caller hands down: the font file's bytes, the em height to bake them at,
///  and the set of codepoints to bake. A NULL codepoints array paired with a zero count is the one defau
/// lt set this rung bakes (the printable ASCII run, raylib's default charset); any other pairing is a descript
/// ion this rung cannot lay out. The array is the caller's, is read once at the load and is never kept - the rows
///  this rung bakes from it are its own.
typedef struct XudFontData {
    const uint8_t* fileData; /* the font file's bytes, read once at the load and never kept */
    uint32_t fileBytes; /* their length - at least sfnt's own shortest header */
    uint32_t pixelSize; /* the bake's em height, in pixels - raylib's Font.baseSize */
    const uint32_t* codepoints; /* codepointCount, sorted ascending and unique, or NULL for the default set */
    uint32_t codepointCount; /* codepoints this font bakes - 0 only beside a NULL array */
} XudFontData;

/// One glyph's pen metrics: raylib's GlyphInfo (raylib.h 404-408), the same four numbers and the same
///  names. The three floats are in the bake's own pixel space, which is why the bake's height is part of
///  every query that turns them into a position.
typedef struct XudGlyphInfo {
    uint32_t value; /* the codepoint this glyph was baked for */
    float offsetX; /* the pen's step to the glyph box's left edge at the baseline */
    float offsetY; /* the pen's step to the glyph box's top edge */
    float advanceX; /* how far the pen moves after this glyph */
} XudGlyphInfo;

/// One glyph's box in the atlas, in texels rather than in texture coordinates: raylib's GetGlyphAtlasRe
/// c answer is a Rectangle in texels, and the divide that turns it into the [0,1] a sampler wants is a divi
/// de by an extent both the caller and this rung can read, so the rung that states the box states it in texels a
/// nd the rung that draws converts.
typedef struct XudGlyphAtlasRec {
    float x; /* the box's left texel */
    float y; /* the box's top texel */
    float width; /* the box's width in texels - 0 for a codepoint with no outline, a space */
    float height; /* the box's height in texels */
} XudGlyphAtlasRec;

/// One audio stream's bytes, as a caller hands them to xud_load_audio(): the container's own bytes
/// rather than a path, so this header calls no file function and a caller decides what read means.
/// There is no format token here, and that is a decision rather than an omission: miniaudio sniffs
/// the container, so a caller's token could disagree with the file it names and become a claim this
/// rung would then be making about a buffer it never read.
typedef struct XudAudioData {
    const uint8_t* fileData; /* the container's bytes: wav, flac, mp3 or ogg, whatever the decoder reads */
    uint32_t fileBytes; /* their length, which is the one bound this rung can apply before it spends */
} XudAudioData;

/// What a stream's frames are, as xud_get_audio_info() answers it: raylib's GetAudioLength half needs
/// the rate to turn a frame count into seconds, so the three numbers are the answer rather than a
/// length stored at the load that could disagree with the count it came from.
typedef struct XudAudioInfo {
    uint32_t channels; /* what the decoder answered, not what this rung asked for */
    uint32_t sampleRate; /* what the decoder answered, in Hz */
    uint64_t frameCount; /* one frame is one sample per channel, so the block is frameCount*channels samples */
} XudAudioInfo;

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

/// Create the library context for a window of `width` x `height` named `title` - raylib's
/// InitWindow(width, height, title) with the out-param first. The title is the name the window
/// system shows and decorates with: GLFW copies it when the window is created, so the caller's
/// string is read during this call and may be freed the moment it returns, this library keeps no
/// copy of it, and an empty one is a window with no name rather than a bad argument. Creation order:
/// volkInitialize() the loader, open the diagnostics layer when XUD_VALIDATE asked for it, open the
/// named GLFW window, create the VkInstance carrying that window's required instance extensions,
/// install the debug messenger, open the physical layer through THAT instance (enumerate, rank,
/// record caps), ask
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
/// context. A NULL `title` is refused beside the out-param and the extent bounds. XUD_UNSUPPORTED means
/// this machine cannot host a context at all (no Vulkan loader, no
/// window system, no device this layer can take, a window system this device cannot present to,
/// a surface this layer cannot get a swapchain for, a renderer or an allocator this layer cannot
/// open); no field of XudContext is a sentinel any more, and what is missing is not a field but a
/// rung above the renderer - the rest of raylib's 3D shape family, the camera + transform math an
/// d the object kinds a public handle would name - which is why a caller can draw a triangle, a box
/// and a sphere today and nothing else, and why no handle names an object yet.
XudResult xud_init_context(XudContext** outContext, uint32_t width, uint32_t height, const char* title);

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

/// Whether the window system has asked for this window to be closed - raylib's WindowShouldClose
/// (raylib.h 990) without its key half: raylib's query also ends the app on the key SetExitKey
/// records, and this library records no such key, so the query answers the other half only - the
/// window manager's close button, delivered by the window system to the window's event loop, which
/// the poll at the end of the frame is what runs. The key that ends a run is the caller's own
/// decision: the caller checks its key with xud_is_key_down() and quits with xud_close_context()
/// itself, which stands for raylib's CloseWindow in this tree - the library never exits a process
/// of its own. XUD_INVALID_ARG on a NULL out-should-close or a NULL context; XUD_UNSUPPORTED on a
/// context that never completed bring-up; XUD_OK with the window system's answer, false on a window
/// that was never asked to close.
XudResult xud_window_should_close(bool* outShouldClose, XudContext* context);

/// Move the window to the requested size - raylib's SetWindowSize (raylib.h 1015). The request
/// goes to GLFW and the compositor is free to answer a different size than the one asked (a tiling
/// layout re-tiles the window): the swapchain follows the window's framebuffer size at the next
/// xud_end_drawing() boundary, the same boundary that already rebuilds a resized-from swapchain
/// after a drag resize, so this call moves the window and the drawing catches up on its own. The
/// context's recorded extent follows the swapchain when that rebuild runs, which is what the ui
/// fold and the camera's aspect divide by. XUD_OK when GLFW took the new extent, XUD_ALREADY when
/// the window already has it, XUD_INVALID_ARG on a NULL context or a size outside [1,
/// XUD_MAX_WINDOW_EXTENT] per axis, XUD_UNSUPPORTED on a context that never completed bring-up.
XudResult xud_set_window_size(XudContext* context, uint32_t width, uint32_t height);

/// Set the frame's target rate - raylib's SetTargetFPS, the window's present interval through it:
/// a target of zero asks for the unsynchronised interval (the present goes as soon as the frame is
/// ready, which is the frame rate the machine and the caller's loop agree on), and any positive
/// target asks for the synchronised one (the present waits for the compositor's refresh, which is
/// the ceiling this call can set - the rate the display refreshes at - because this tree has no
/// software frame limiter of its own: the frame boundary is a synchronous record-submit-wait, and
/// a positive target's cap is the refresh rather than a clock). The change takes effect from the
/// next present, and it is the whole of the control the window system gives: a positive target of
/// 60 on a 144 Hz display runs at 144, the way raylib's own note states its target is a maximum
/// rather than a promise. The context's window records the fps asked (the default, before any
/// call, is zero - the state GLFW opens with, vsync on, and the state xud_get_target_fps() below
/// answers back). XUD_INVALID_ARG on a NULL context; XUD_UNSUPPORTED on a context that never
/// completed bring-up.
XudResult xud_set_target_fps(XudContext* context, uint32_t fps);

/// The fps the target setter last asked - the window's recorded value rather than a read from the
/// window system, which GLFW cannot give back (glfwSwapInterval returns void): zero before any set
/// call, which is the state the window opened in (vsync on) and the default raylib's GetTargetFPS
/// answers the same way. XUD_INVALID_ARG on a NULL out-fps or context; XUD_OK with the window's
/// answer on a completed context.
XudResult xud_get_target_fps(uint32_t* outFps, XudContext* context);

/// The width a frame renders at, in pixels - raylib's GetScreenWidth (raylib.h 1017). This is the
/// swapchain's own extent rather than the size GLFW was asked for: a compositor answers a different
/// size than a request, and this is the extent the frame's pixels, the depth buffer and the camera's
/// aspect all are. A drag resize or a xud_set_window_size() moves it at the xud_end_drawing()
/// boundary, so a caller that reads it every frame never divides by an extent that is gone. XUD_
/// INVALID_ARG on a NULL out-width or context; XUD_UNSUPPORTED on a context that never completed
/// bring-up.
XudResult xud_get_screen_width(uint32_t* outWidth, XudContext* context);

/// The height a frame renders at, in pixels - raylib's GetScreenHeight (raylib.h 1018), the same
/// extent's other axis and the same two refusals.
XudResult xud_get_screen_height(uint32_t* outHeight, XudContext* context);

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
/// ct of a rung a caller cannot name. It GROWS the batch first when the frame before this one
/// asked for more than it had room for, which is what lets the public surface draw a scene bigge
/// r than the batch's initial capacity: a shape's append is atomic, so an over-capacity frame sto
/// ps one shape short of full and its counts say nothing about it, and the ask the refusal records
/// is what says so (see xud_batch.h's capacity-ask note). XUD_INVALID_ARG on a NULL context or on
/// a renderer whose batch rung is gone; XUD_UNSUPPORTED on a context that never completed bring-u
/// p, or on a grow that cannot happen; the grow's own codes when a grow is asked for.
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

/// Draw one line in 3D space, raylib's DrawLine3D (raylib.h 1585, "Draw a 3D line", whose
/// arguments are (startPos, endPos, color) and whose own comment says it is "drawn as a line using
/// immediate mode (rlBegin(RL_LINES))"): it appends two vertices and the two indices that name
/// them, and it is drawn through the pipeline list's LINE_LIST row rather than through the tria
/// ngles' one. That row exists because topology is PIPELINE state (it lives in
/// VkPipelineInputAssemblyStateCreateInfo, not in a draw call), which is also why a line cannot be
/// drawn by the shapes rung alone on a list of one topology - a line through a TRIANGLE_LIST pipel
/// ine is a line made of triangles, and that is a shape the caller did not ask for. The append mov
/// es the batch's topology state and closes the run that was pending under the old one into a row
/// of the same batch, so a frame that draws a sphere and a line records TWO rows and replays both;
/// the shape owns that state change rather than the caller, because in raylib it is the shape func
/// tion that calls rlBegin and a caller has no topology to ask for. The normal is the flat +Z the
/// other no-surface shapes carry. The append is atomic in the same shape as the other shapes'. XUD
/// _INVALID_ARG on a NULL context or a renderer with no batch; XUD_UNSUPPORTED on a context that
/// never completed bring-up; XUD_OUT_OF_MEMORY when the two vertices or the two indices do not fit
/// the batch.
XudResult xud_draw_line_3d(XudContext* context, XudVector3 startPos, XudVector3 endPos, XudColor color);

/// Draw a ray as the line it is: raylib's DrawRay (raylib.h 1604, whose arguments are (ray, color)).
/// The line is the ray's own origin to its origin plus one over ten thousand units along its
/// direction, which is raylib's length (rmodels.c 1073-1084) and the reason a picked ray reaches past
/// every wall a scene has: the visible half of a ray is the part in front of the camera, and the
/// shape has to be long enough that the near-plane crossing is inside the drawn stretch. The append is
/// the line above's, two vertices and two indices, atomic in the same shape; a ray whose direction is
/// not normalised draws the same LINE row, just a shorter or longer one. XUD_INVALID_ARG on a NULL
/// context or a renderer with no batch; XUD_UNSUPPORTED on a context that never completed bring-up;
/// XUD_OUT_OF_MEMORY when the two vertices or the two indices do not fit the batch.
XudResult xud_draw_ray(XudContext* context, XudRay ray, XudColor color);

/// Draw a point in 3D space, raylib's DrawPoint3D (raylib.h 1586, "Draw a point in 3D space",
/// "actually a small line", whose arguments are (position, color)): a point here is a short line
/// either side of the caller's position along z, which is what makes it need no third topology row
/// of the pipeline list and no POINT_LIST row at all - it is the call above with its endpoints pla
/// ced either side rather than a shape of its own. The half-length is xud_shapes.h's named consta
/// nt, in the same space every other shape here is in (the clip space the camera step has not repl
/// aced), so a point spans 0.1 of that space today. XUD_INVALID_ARG on a NULL context or a rendere
/// r with no batch; XUD_UNSUPPORTED on a context that never completed bring-up; XUD_OUT_OF_MEMORY
/// when the two vertices or the two indices do not fit the batch.
XudResult xud_draw_point_3d(XudContext* context, XudVector3 position, XudColor color);

/// Draw a cube's wires in 3D space, raylib's DrawCubeWires (raylib.h 1592, "Draw cube wires", whose
/// arguments are (position, width, height, length, color)): the same box the filled xud_draw_cube()
/// above is given the same way - centre, three full extents, axis-aligned - drawn as the 12 edges of
/// the cube graph over the 8 corners of one box rather than as the filled form's 24 corners-of-six
/// faces. The two forms are the same box in space at the same arguments, and the wire is its outline:
/// the corners are shared here because a wire has no face whose normal a shared corner would have to
/// contradict, so every corner carries the flat +Z the line and the flat shapes carry. The indices are
/// assembled under the batch's LINE_LIST topology, which is why a frame that draws a filled cube and
/// a wire cube is TWO rows through two pipeline rows rather than one row at whichever topology came
/// last. XUD_INVALID_ARG on a NULL context or a renderer with no batch; XUD_UNSUPPORTED on a context
/// that never completed bring-up; XUD_OUT_OF_MEMORY when the 8 vertices or the 24 indices do not fit
/// the batch.
XudResult xud_draw_cube_wires(XudContext* context, XudVector3 position, float width, float height, float length, XudColor color);

/// Draw a bounding box's wires: raylib's DrawBoundingBox (raylib.h 1623, rmodels.c 4143), which is
/// itself nothing but the wire cube above over the box's absolute size and midpoint - so this is that
/// one append, and the box drawn is the box the two corners enclose even when the corners came in the
/// wrong order. The bbox a picking example draws before it walks a mesh is this call; the box the same
/// example tests against is xud_get_ray_collision_box(), and the bounds to hand to either come from
/// xud_get_mesh_bounding_box() or xud_get_model_bounding_box(). The refusals are xud_draw_cube_wires()'
/// exactly, and a box with no extent on one axis draws the flat rectangle that is all it encloses.
XudResult xud_draw_bounding_box(XudContext* context, XudBoundingBox box, XudColor color);

/// Draw a sphere's wires in 3D space, raylib's DrawSphereWires (raylib.h 1596, "Draw sphere", whose
/// arguments are (centerPos, radius, rings, slices, color) - the tessellation is the caller's there,
/// and here it is the header's, for the reason xud_draw_sphere() above states: a tessellation is a
/// claim about the batch's capacity rather than about the sphere). The grid is the globe's grid rather
/// than the mesh's wireframe: the latitude circles and the meridian polylines, each edge named once,
/// over the SAME 17 vertices the filled sphere places in the same order - so the wire form spends 70
/// indices, BELOW the filled form's 90, because a band's diagonal is an artifact of triangulating a
/// quad rather than an edge of a sphere, and a wire that drew it would be drawing the mesh's
/// construction lines. The tessellation is the header's default here, so a wire sphere and a filled
/// sphere in one frame are the same 17 vertices placed twice rather than one layout read twice -
/// sharing them across two calls would be a mesh rung, which is the item below this one. XUD_INVALID_ARG
/// on a NULL context or a renderer with no batch; XUD_UNSUPPORTED on a context that never completed
/// bring-up; XUD_OUT_OF_MEMORY when the 17 vertices or the 70 indices do not fit the batch.
XudResult xud_draw_sphere_wires(XudContext* context, XudVector3 centerPos, float radius, XudColor color);

/// Draw a ground plane, raylib's DrawPlane (raylib.h 1600, "Draw a plane (oriented xz-plane)", whose
/// arguments are (centerPos, size, color)): the centre is the plane's CENTRE, size.x runs along world
/// x and size.y along world z, and the plane is always horizontal - the same XZ ground raylib says it
/// builds (rmodels.c: "NOTE: Plane is always created on XZ ground"). It appends the four corners and
/// the six indices that close them as two triangles, with the +Y normal raylib hands its own plane
/// (rmodels.c: rlNormal3f(0, 1, 0)) rather than the flat +Z the shapes with no surface carry, because
/// this shape does have one and the Lambert term in solid.frag reads it. XUD_INVALID_ARG on a NULL
/// context or a renderer with no batch; XUD_UNSUPPORTED on a context that never completed bring-up;
/// XUD_OUT_OF_MEMORY when the 4 vertices or the 6 indices do not fit the batch.
XudResult xud_draw_plane(XudContext* context, XudVector3 centerPos, XudVector2 size, XudColor color);

/// Draw a cylinder, cone or frustum standing on its base, raylib's DrawCylinder (raylib.h 1598, whose
/// arguments are (position, radiusTop, radiusBottom, height, color)): position is the BASE, the axis
/// runs up +Y, and a radiusTop of zero is the cone raylib's own note names. Every vertex carries the
/// slanted normal rather than a flat one, so the cone shades as one surface. The tessellation is the
/// header's (XUD_SHAPES_CYLINDER_SIDE_COUNT), for the reason xud_draw_sphere() states. XUD_INVALID_ARG
/// on a NULL context, a negative or NaN radius, or a height of zero, which names no axis to be round
/// about; XUD_OUT_OF_MEMORY when the shape does not fit the batch.
XudResult xud_draw_cylinder(XudContext* context, XudVector3 position, float radiusTop, float radiusBottom, float height, XudColor color);

/// Draw a cylinder's wires, raylib's DrawCylinderWires: the two rims and one vertical per side, over
/// the two rings only. A second row through the pipeline's LINES column, so a filled cylinder and its
/// wires in one frame are two draws. Same refusals as xud_draw_cylinder().
XudResult xud_draw_cylinder_wires(
    XudContext* context, XudVector3 position, float radiusTop, float radiusBottom, float height, XudColor color);

/// Draw a capsule, raylib's DrawCapsule (raylib.h 1596, (startPos, endPos, radius, color)): two hemi-
/// spherical caps about the two centres and the band between them, the caps' wide rings shared with the
/// band. startPos == endPos is a sphere rather than a refusal, as it is in raylib. XUD_INVALID_ARG on a
/// NULL context or a radius that is not greater than zero; XUD_OUT_OF_MEMORY when the shape does not fit.
XudResult xud_draw_capsule(XudContext* context, XudVector3 startPos, XudVector3 endPos, float radius, XudColor color);

/// Draw a capsule's wires, raylib's DrawCapsuleWires: the mesh's own edges - a ring around each row of
/// each cap, the longitudinal segments between the rows and into the poles, and the band's longitudinal.
XudResult xud_draw_capsule_wires(XudContext* context, XudVector3 startPos, XudVector3 endPos, float radius, XudColor color);

/// Draw a ground grid, raylib's DrawGrid (raylib.h 1602, (slices, spacing)): divisions+1 lines each way
/// across the XZ plane `spacing` apart, centred on centerPos, the two lines through the centre in
/// centerColor and the rest in gridColor - raylib's two-tone grid with both colours the caller's, as
/// every colour here is. XUD_INVALID_ARG on a NULL context, a spacing that is not greater than zero, or
/// divisions of zero; XUD_OUT_OF_MEMORY when the 4*(divisions+1) vertices do not fit the batch.
XudResult xud_draw_grid(
    XudContext* context, XudVector3 centerPos, uint32_t divisions, float spacing, XudColor centerColor, XudColor gridColor);

/// Put a 3D camera in the scene: the public BeginCamera3D() of raylib's camera pair (reference/ray
/// lib/rcamera.h 369, raylib.h's BeginCamera3D()), whose camera is a global the library holds and
/// whose state therefore cannot be validated at the call. Here the record is the caller's and this
/// call copies it into the renderer that owns the frame loop, and it is VALIDATED here: a camera
/// that is degenerate (an eye sitting on its target, a zero up vector, an up parallel to the eye's
/// forward, a fovy at or past 0 or 180 degrees, a near plane at or behind the eye, a far plane at
/// or behind the near, any of them NaN) answers XUD_INVALID_ARG and leaves the camera that was
/// already live alone, so a refused begin is a frame drawn with the previous camera rather than a
/// frame drawn with a NaN matrix. A begin while a camera is already live REPLACES it rather than
/// stacking it - the scope is one camera per frame (AGENTS.md), so there is no camera stack here.
/// XUD_INVALID_ARG on a NULL context or a degenerate camera; XUD_UNSUPPORTED on a context that ne
/// ver completed bring-up.
XudResult xud_begin_camera_3d(XudContext* context, const XudCamera3D camera);

/// Take the 3D camera away: the public EndCamera3D() of the same pair. The flush that carries this
/// frame's draws still projects with this camera - the close sits before that flush (the raylib pair
/// lives INSIDE BeginDrawing/EndDrawing, and this library's draws are recorded, not executed, until
/// the boundary), so a camera begun and ended inside one frame draws THAT frame and no later one:
/// the frames drawn after the next boundary are drawn with no camera at all, the identity matrix
/// the descriptor rung opens set 0's block with. XUD_ALREADY when no camera was live (the state is
/// already the one this call asks for); XUD_INVALID_ARG on a NULL context; XUD_UNSUPPORTED on a
/// context that never completed bring-up.
XudResult xud_end_camera_3d(XudContext* context);

/*-----------------------------------------------------------------------------------------------
 * The five modes xud_update_camera() drives a camera with (raylib's CameraMode, rcamera.h 119-125)
 *
 * The numbers are raylib's own in raylib's own order, so a caller porting an rcamera call names the
 * same token for the same behaviour. raylib's CameraProjection (rcamera.h 113: PERSPECTIVE / ORTHO
 * GRAPHIC) is deliberately NOT mirrored: this library's one camera is perspective, so there is no
 * projection to toggle and no aperture reading whose meaning would change under the toggle. The four
 * speeds and the sensitivity the modes are scaled by are the rung's constants (xud_camera.h), because
 * they are arithmetic the library performs rather than a token a caller chooses.
 *-----------------------------------------------------------------------------------------------*/

#define XUD_CAMERA_MODE_CUSTOM 0u /* the caller owns the record: xud_update_camera() touches nothing */
#define XUD_CAMERA_MODE_FREE 1u /* free flight: every move and every rotation, up and down included */
#define XUD_CAMERA_MODE_ORBITAL 2u /* the eye circles the target at a fixed distance; wheel zooms */
#define XUD_CAMERA_MODE_FIRST_PERSON 3u /* walks the world plane, looks with the mouse and the arrows */
#define XUD_CAMERA_MODE_THIRD_PERSON 4u /* the same hand as the first person's, around the target */
#define XUD_CAMERA_MODE_COUNT 5u /* the length of this axis */

static_assert(XUD_CAMERA_MODE_COUNT == (XUD_CAMERA_MODE_THIRD_PERSON + 1u),
    "xud.h: the mode count is one past the last token, so xud_update_camera()'s range check is one "
    "comparison rather than a list of five cases - the same shape XUD_TEXTURE_FILTER_COUNT gives the "
    "filter and wrap setters");

/// The camera's own forward - raylib's GetCameraForward (rcamera.h 141): normalize(target - position).
/// XUD_INVALID_ARG on a NULL out-vector or a camera with no basis (an eye on its target, a zero up,
/// an up parallel to the forward).
XudResult xud_get_camera_forward(XudVector3* outForward, const XudCamera3D* camera);

/// The camera's own up, normalised - raylib's GetCameraUp (rcamera.h 143). Note that the record's up
/// is not necessarily perpendicular to the forward (rcamera.h 135 says so of raylib's too); this is
/// the axis roll and yaw turn over, not the frame's vertical. Same refusals as the forward.
XudResult xud_get_camera_up(XudVector3* outUp, const XudCamera3D* camera);

/// The camera's own right - raylib's GetCameraRight (rcamera.h 147): normalize(cross(forward, up)),
/// the same cross the view matrix is built from. Same refusals as the forward.
XudResult xud_get_camera_right(XudVector3* outRight, const XudCamera3D* camera);

/// Walk the camera along its forward, eye and target together - raylib's CameraMoveForward (rcamera.h
/// 146). moveInWorldPlane drops the step's component along the axis the camera's up points along
/// first, so a first-person walk stays on the ground while the eye is looking at the sky. The step
/// goes into a copy and the copy's basis is asked before anything is stored: a distance that is NaN,
/// or a step that would leave the eye on its target, answers XUD_INVALID_ARG and leaves the camera
/// alone. XUD_INVALID_ARG on a NULL camera or a camera with no basis to walk from.
XudResult xud_camera_move_forward(XudCamera3D* camera, float distance, bool moveInWorldPlane);

/// Slide the camera along its up, eye and target together - raylib's CameraMoveUp (rcamera.h 147).
/// The same refusal-before-store as xud_camera_move_forward().
XudResult xud_camera_move_up(XudCamera3D* camera, float distance);

/// Slide the camera along its right, eye and target together - raylib's CameraMoveRight (rcamera.h
/// 148), the same world-plane option and the same refusals as the forward's.
XudResult xud_camera_move_right(XudCamera3D* camera, float distance, bool moveInWorldPlane);

/// Move the EYE along the forward until it holds the stated delta more (or less) distance from the
/// target - raylib's CameraMoveToTarget (rcamera.h 149), which is what the wheel and the keypad's
/// zoom keys drive. The target does not move, so this is a zoom and not a step. A delta that would
/// land the eye on or past its target stops at XUD_CAMERA_MIN_TARGET_DISTANCE instead, the same
/// floor rcamera.h 318 clamps to. Same refusals as the forward's.
XudResult xud_camera_move_to_target(XudCamera3D* camera, float delta);

/// Turn the camera about its own up - raylib's CameraYaw (rcamera.h 152), angle in RADIANS. With
/// rotateAroundTarget the eye orbits the target (third person, orbital); without it the target
/// swings under the eye (first person). The turned copy's basis is asked before anything is stored.
/// XUD_INVALID_ARG on a NULL camera or a camera with no basis.
XudResult xud_camera_yaw(XudCamera3D* camera, float angle, bool rotateAroundTarget);

/// Tip the camera about its own right - raylib's CameraPitch (rcamera.h 153), angle in RADIANS.
/// lockView is the somersault guard: the angle is cut so the view vector stays inside the cone
/// between the camera's up and its negation, minus this rung's margin. rotateUp tips the record's up
/// with it, which is what a free-flying camera wants and a walking one does not. Same refusals.
XudResult xud_camera_pitch(XudCamera3D* camera, float angle, bool lockView, bool rotateAroundTarget, bool rotateUp);

/// Roll the camera about its own forward - raylib's CameraRoll (rcamera.h 154), angle in RADIANS:
/// the record's up turns, the eye and the target do not, and the up's own length is kept. Refuses
/// XUD_INVALID_ARG on a NULL camera, a camera with no basis, or an angle that leaves none.
XudResult xud_camera_roll(XudCamera3D* camera, float angle);

/// Drive a camera from this context's own input and clock for one frame: raylib's UpdateCamera
/// (rcamera.h 445), including the mouse look, the W/A/S/D walk, the arrow-key turn, the Q/E roll, the
/// free mode's Space and Left-Ctrl, the middle-button pan and the wheel zoom. The three keys, the
/// cursor delta and the wheel are read from the input rung and the frame's speed from the clock rung,
/// so the same walk is the same speed at any frame rate; the first frame's span is zero, so the first
/// update does nothing. XUD_CAMERA_MODE_CUSTOM touches nothing and answers XUD_OK. raylib's gamepad
/// half is NOT here - this library owns no gamepad rung - and xud_update_camera_pro() is where a
/// caller's own axes go. Every step lands on a copy, so a refusal leaves the caller's record as it
/// was. XUD_INVALID_ARG on a NULL context or camera, a mode outside the five, or a camera the
/// frustum would refuse; XUD_UNSUPPORTED on a context with no input or clock rung.
XudResult xud_update_camera(XudContext* context, XudCamera3D* camera, uint32_t mode);

/// The same update with the caller's own movement, rotation and zoom instead of the context's input:
/// raylib's UpdateCameraPro (rcamera.h 532). movement is forward-back, right-left, up-down in world
/// units; rotation is yaw, pitch, roll in DEGREES, as raylib takes them; zoom is the change in the
/// eye-to-target distance. Nothing is read from the context, so this is the half a caller with its
/// own input - a gamepad, a replay, a recorded path - drives. The same refusals as xud_update_
/// camera()'s, minus the ones that need a context.
XudResult xud_update_camera_pro(XudCamera3D* camera, XudVector3 movement, XudVector3 rotation, float zoom);

/*-----------------------------------------------------------------------------------------------
 * The lights a frame is shaded by
 *
 * raylib has no lighting API to mirror: its examples hand a custom shader three uniform arrays and
 * run the attenuation loop themselves. What is mirrored here is the SHAPE those examples keep
 * landing on - a table of lights as state, replaced whole, plus one ambient floor - stated as
 * context state the way the camera and the audio listener are: the frame reads whatever the last
 * call left, and no frame has to be open to change it.
 *
 * The table defaults to ONE white SUN along normalize(1, 2, 3) and an ambient floor of white at a
 * quarter: that is the fixed light and floor the fragment stage used to compile in, so a context
 * that never calls either setter shades its frames exactly as it shaded them before this rung
 * existed. The first xud_set_lights() call replaces that default whole - lights never accumulate
 * across calls, and an empty table is a legal one: it leaves the ambient floor alone.
 *-----------------------------------------------------------------------------------------------*/

/// One light: sun, point or spot, named by the fields its kind reads. Which fields a kind reads is
/// stated per member below and checked by xud_set_lights(); fields a kind does not read are free to
/// hold anything, which is what lets one record serve the three kinds the way raylib's example's
/// single Light struct does.
typedef struct XudLight {
    XudVector3 position; /* POINT and SPOT: the world point the light shines from; SUN: ignored */
    XudVector3
        direction; /* SUN: the direction TOWARD the light (a vector at infinity); SPOT: the cone axis, pointing AWAY from the light; POINT: ignored */
    XudColor color; /* the light's rgb; its alpha byte is not read - intensity is its own float */
    float intensity; /* >= 0: the rgb is scaled by this before the shade is summed, so > 1 is a light brighter than white */
    float range; /* POINT and SPOT: >= 0; the light fades to nothing AT this distance, and 0 means it never fades; SUN: ignored */
    float innerConeDegrees; /* SPOT: 0..180, fully lit out to this angle off the axis; SUN and POINT: ignored */
    float outerConeDegrees; /* SPOT: innerConeDegrees..180, nothing past this angle; SUN and POINT: ignored */
    uint32_t kind; /* one of the XUD_LIGHT_KIND_* tokens */
} XudLight;

/// Replace the context's whole light table: raylib's examples' `lights` array and its count, as
/// one call. The table becomes `lights[0..count)` for every frame from now until the next call -
/// the fragment stage sums exactly these, in this order, over its Lambert terms. count 0 (and a
/// NULL lights) is legal and asks for a table with no lights in it: faces then keep only the
/// ambient floor. XUD_INVALID_ARG on a NULL context, a NULL lights beside a non-zero count, a count
/// past XUD_MAX_LIGHTS, a kind outside the tokens, a NaN or infinite float anywhere the kind
/// reads, a negative intensity or range, a SUN or SPOT whose direction is the zero vector (a light
/// that points nowhere has no Lambert term to take), or a SPOT whose outer cone is narrower than
/// its inner; XUD_UNSUPPORTED on a context that never completed bring-up.
XudResult xud_set_lights(XudContext* context, const XudLight* lights, uint32_t count);

/// The floor every face keeps where no light reaches: the ambient row of the frame's block.
/// The default is white at intensity 0.25, the constant the fragment stage used to compile in.
/// XUD_INVALID_ARG on a NULL context; XUD_UNSUPPORTED on a context that never completed bring-up.
XudResult xud_set_ambient_light(XudContext* context, XudColor color, float intensity);

/*-----------------------------------------------------------------------------------------------
 * The two ways between the frame's pixels and the world the camera frames
 *
 * raylib's GetScreenToWorldRay/Ex and GetWorldToScreen/Ex (raylib.h 1083-1086). The four are the
 * picking and the on-screen-label half of raylib's API: one turns a mouse position into the ray to
 * trace, the other turns a world point into the pixel a label goes at. Both read the camera by
 * pointer rather than from the library's state, so a caller can ask about a camera it never began,
 * and both fold through the same basis and focal length the view matrix is built from, so a label
 * drawn at the pixel one of these answers sits on the geometry it labels rather than beside it.
 *-----------------------------------------------------------------------------------------------*/

/// Trace the ray from the eye through one of the frame's pixels: raylib's GetScreenToWorldRay
/// (raylib.h 1083, the function its GetMouseRay became). The pixel is taken in the same top-left-
/// anchored floats the input rung's position and delta are, the origin is the eye and the direction
/// is normalised, both in world units. raylib takes the ray's two bounding planes from a global cull
/// distance because its camera record carries neither; this record carries both, and the ray is built
/// from the camera the caller holds. The pixel need not be inside the frame - a point outside the
/// extent traces the ray the frame's edges would extrapolate to, which is what a pointer dragged
/// past an edge asks for. XUD_INVALID_ARG on a NULL out-ray, context or camera, or a camera the view
/// matrix would refuse; XUD_UNSUPPORTED on a context that never completed bring-up.
XudResult xud_get_screen_to_world_ray(XudRay* outRay, XudContext* context, XudVector2 screen, const XudCamera3D* camera);

/// The same trace through a viewport of the caller's own extent rather than the context's - raylib's
/// GetScreenToWorldRayEx (raylib.h 1084), the form a half of a split screen un-projects inside. Pure:
/// no context, and no extent read from one. XUD_INVALID_ARG on a NULL out-ray or camera, a camera the
/// view matrix would refuse, or an extent that is not greater than zero on either axis.
XudResult xud_get_screen_to_world_ray_ex(XudRay* outRay, XudVector2 screen, const XudCamera3D* camera, float width, float height);

/// Where a world point lands on the frame's pixels: raylib's GetWorldToScreen (raylib.h 1085). The
/// answer is in the top-left-anchored pixels every screen-space call takes, including the y fold
/// raylib applies on the way out of NDC. A point at or behind the eye is refused rather than answered
/// with the mirrored position a blind divide gives - a label for a point the camera cannot see has no
/// pixel to sit at. XUD_INVALID_ARG on a NULL out-screen, context or camera, a camera the view matrix
/// would refuse, or such a point; XUD_UNSUPPORTED on a context that never completed bring-up.
XudResult xud_get_world_to_screen(XudVector2* outScreen, XudContext* context, XudVector3 position, const XudCamera3D* camera);

/// The same fold into a viewport of the caller's own extent - raylib's GetWorldToScreenEx (raylib.h
/// 1086), which is also the form a caller uses to place a label in one half of a split screen. Same
/// refusals as the trace above's, minus the ones that need a context.
XudResult xud_get_world_to_screen_ex(XudVector2* outScreen, XudVector3 position, const XudCamera3D* camera, float width, float height);

/*-----------------------------------------------------------------------------------------------
 * The five collision tests over the world the camera frames
 *
 * raylib's CheckCollisionSpheres/Boxes/BoxSphere and GetRayCollisionSphere/Box (raylib.h 1668-1672),
 * the picking half and the player-versus-enemy half of its collision API. None of the five reads the
 * context, the camera or the batch: they are host arithmetic on the caller's own floats, which is why
 * an answer arrives through an out-parameter beside an XudResult rather than as the bare bool raylib
 * can afford - a NaN centre, a swapped box or a negative radius is a refusal here and a silent false
 * there.
 *-----------------------------------------------------------------------------------------------*/

/// Do two spheres overlap: raylib's CheckCollisionSpheres (raylib.h 1668) - the squared distance between
/// the centres against the squared sum of the radii, no square root taken. Touching surfaces count as
/// overlap. XUD_INVALID_ARG on a NULL out-hit, a non-finite component of either centre, or a radius not
/// greater than zero.
XudResult xud_check_collision_spheres(bool* outHit, XudVector3 center1, float radius1, XudVector3 center2, float radius2);

/// Do two boxes overlap: raylib's CheckCollisionBoxes (raylib.h 1669) - overlap on all three axes, faces
/// sharing a plane counting as overlap on every axis. raylib is inconsistent here: its x test is inclusive
/// while its y and z tests are strict, so two crates resting on one another answer false there and true
/// here. XUD_INVALID_ARG on a NULL out-hit, corners that are not finite, or a box whose minimum is past its
/// maximum on any axis.
XudResult xud_check_collision_boxes(bool* outHit, XudBoundingBox box1, XudBoundingBox box2);

/// Does a box meet or contain a sphere: raylib's CheckCollisionBoxSphere (raylib.h 1670) - the box's point
/// nearest the centre, against the radius. The same refusals as the two tests above, for one box and one
/// radius.
XudResult xud_check_collision_box_sphere(bool* outHit, XudBoundingBox box, XudVector3 center, float radius);

/// Where a ray meets a sphere: raylib's GetRayCollisionSphere (raylib.h 1671). The record is written
/// whether or not anything was hit - a miss answers all-zero - so the picking example's click-to-select-
/// again is one assignment rather than a clearing. A ray that begins inside answers the far side of the
/// sphere with the normal pointing back at the origin, which is the surface the ray leaves through. The
/// distance is a multiple of the direction the ray carries, so the normalised direction the fold answers
/// makes it world units. XUD_INVALID_ARG on a NULL out-collision, a ray with no direction long enough to
/// trace, a non-finite component of either, or a radius not greater than zero.
XudResult xud_get_ray_collision_sphere(XudRayCollision* outCollision, XudRay ray, XudVector3 center, float radius);

/// Where a ray meets a box: raylib's GetRayCollisionBox (raylib.h 1672), by the slab method. A ray that
/// begins inside answers the face it leaves through at a positive distance - what raylib arrives at by
/// tracing a negated ray and negating the distance and the normal back - and the normal is the exact axis
/// unit of the face hit, where raylib's integer projection of the centre-to-hit vector gets a face right
/// and an edge approximate. The same refusals as the sphere cast, with a box's corners in place of the
/// sphere.
XudResult xud_get_ray_collision_box(XudRayCollision* outCollision, XudRay ray, XudBoundingBox box);

/// Where a ray meets a triangle: raylib's GetRayCollisionTriangle (raylib.h 1674), Moller-Trumbore.
/// Both windings hit - a triangle has no front, and a cast that culled the back would pick the near
/// side of a floor and miss the ceiling above it - and the normal carries the SIGN of the caller's
/// winding: counter-clockwise vertices answer it on the side the vertices face, which is raylib's
/// answer as well, so this is the one cast here that does not turn its normal to face the ray. A hit
/// at the origin is not a hit: the distance must clear XUD_COLLISION_PARALLEL_COMPONENT, which is what
/// keeps a cast launched from a surface from answering the surface it started on. XUD_INVALID_ARG on
/// the sphere cast's refusals, with three vertices in place of the sphere.
XudResult xud_get_ray_collision_triangle(
    XudRayCollision* outCollision, XudRay ray, XudVector3 vertex0, XudVector3 vertex1, XudVector3 vertex2);

/// Where a ray meets a quad: raylib's GetRayCollisionQuad (raylib.h 1675) - the two triangles of the
/// (0, 1, 3) diagonal, with the NEARER of the two hits answered. raylib returns whichever triangle it
/// tried first, which is the same answer on a flat quad and can be the far one on a quad whose corners
/// are not coplanar. The same refusals again, for four vertices.
XudResult xud_get_ray_collision_quad(
    XudRayCollision* outCollision, XudRay ray, XudVector3 vertex0, XudVector3 vertex1, XudVector3 vertex2, XudVector3 vertex3);

/// Where a ray meets a mesh: raylib's GetRayCollisionMesh (raylib.h 1673, rmodels.c 4324), over the
/// caller's OWN description rather than a handle - a mesh this library uploaded keeps no CPU copy of
/// what it uploaded, so the triangles a cast can read are the ones the caller still holds, which is
/// also what makes the same description drawable by xud_upload_mesh() and testable by this call with
/// one set of arrays. Every triangle is walked and the nearest hit kept; the transform, NULL for the
/// identity, is column-major float[16] as every other matrix here is, and goes onto the vertices
/// rather than running the ray back through an inverse, so a flattened mesh is a mesh with no hit
/// rather than a refusal. A miss is the all-zero record. XUD_INVALID_ARG on the ray refusals, on a
/// NULL out or mesh or index array, a vertexCount of zero, an indexCount that is not a whole number of
/// triangles, any index at or past vertexCount (that index is a read of the memory after the caller's
/// positions, so it is refused before the walk rather than dereferenced during it), or a transform
/// with a non-finite entry.
XudResult xud_get_ray_collision_mesh(XudRayCollision* outCollision, XudRay ray, const XudMeshData* mesh, const float* transform);

/// A description's bounds: raylib's GetMeshBoundingBox (raylib.h 1634, rmodels.c 3782), the position
/// stream read and no other - the broad phase the mesh cast above wants, since a box test that misses
/// costs six comparisons where the walk costs every triangle in the mesh. It lives beside that cast
/// rather than in the mesh rung because it is the same pure arithmetic over the caller's arrays:
/// xud_get_model_bounding_box reads the record a loaded model kept, and this reads what the caller is
/// still holding. XUD_INVALID_ARG on a NULL out or mesh, a vertexCount of zero, or a position with a
/// non-finite component in it - a NaN compares false against both ends of a bound, which would answer
/// a box that quietly leaves out the vertex it should have been widest for.
XudResult xud_get_mesh_bounding_box(XudVector3* outMin, XudVector3* outMax, const XudMeshData* mesh);

/*-----------------------------------------------------------------------------------------------
 * The matrix rung - the transforms the _ex draws place their object with
 *
 * raymath's Matrix family (raymath.h: MatrixIdentity 988, MatrixTranslate 1941, MatrixScale 1963,
 * MatrixRotate 1792, MatrixRotateXYZ 1897, MatrixMultiply 968, Vector3Transform 891). raymath keeps
 * them in a struct of fifteen named floats and multiplies the row vector from the left; this library
 * hands out the float[16] its vertex stage reads - column-major, the vector a column, the translation
 * at 12, 13, 14 - so a matrix read out of a glTF file goes in as it came out. Multiplication is
 * left*right, which spends the RIGHT factor first, so a placement is translate*rotate*scale.
 *-----------------------------------------------------------------------------------------------*/

/// The transform that moves nothing: raymath's MatrixIdentity (raymath.h 988). XUD_INVALID_ARG on a NULL out.
XudResult xud_matrix_identity(float* outMatrix);

/// A shift, raymath's MatrixTranslate (raymath.h 1941) - the identity with the caller's point in its fourth column.
XudResult xud_matrix_translate(float* outMatrix, XudVector3 translation);

/// A stretch about the world's origin: raymath's MatrixScale (raymath.h 1963), three floats on the diagonal.
XudResult xud_matrix_scale(float* outMatrix, XudVector3 scale);

/// A rotation about an axis through the origin: raymath's MatrixRotate (raymath.h 1792). The axis needs no
/// normalising; a non-finite angle or an axis with no direction in it is refused.
XudResult xud_matrix_rotate(float* outMatrix, XudVector3 axis, float radians);

/// Three rotations at once: raymath's MatrixRotateXYZ (raymath.h 1897), the angles in radians, the third
/// applied first and the first last (roll, then yaw, then pitch).
XudResult xud_matrix_rotate_xyz(float* outMatrix, XudVector3 radians);

/// left*right, raymath's MatrixMultiply (raymath.h 968) with the factor order the column convention asks
/// for. The product is built in full before it is written, so outMatrix may be left or right.
XudResult xud_matrix_multiply(float* outMatrix, const float* left, const float* right);

/// The placement a draw spends: translate*rotate*scale, the three the calls above would take two
/// multiplies to combine. Radians are the same three xud_matrix_rotate_xyz() takes.
XudResult xud_matrix_compose(float* outMatrix, XudVector3 translation, XudVector3 radians, XudVector3 scale);

/// A point through a transform: raymath's Vector3Transform (raymath.h 891), affine half only - the
/// fourth row is not read, so this places and never projects.
XudResult xud_matrix_transform_point(XudVector3* outPoint, const float* matrix, XudVector3 point);

/*-----------------------------------------------------------------------------------------------
 * The generation rung - the solids raylib's GenMesh* family cuts, as arrays the caller owns
 *-----------------------------------------------------------------------------------------------*/

#define XUD_MESH_KIND_PLANE 0u /* a flat grid on the world's floor: width by length, sliced and stacked */
#define XUD_MESH_KIND_CUBE 1u /* six quads, 24 vertices, the four unused numbers of the shape ignored */
#define XUD_MESH_KIND_SPHERE 2u /* rings of latitude and slices of longitude about a radius */
#define XUD_MESH_KIND_HEMISPHERE 3u /* the same, from a pole to the equator's ring */
#define XUD_MESH_KIND_CYLINDER 4u /* a tube of slices around a height, capped at both ends */
#define XUD_MESH_KIND_TORUS 5u /* a tube of tubeRadius swept around a circle of radius */
#define XUD_MESH_KIND_KNOT 6u /* a trefoil of tubeRadius swept along a curve of radius */
#define XUD_MESH_KIND_POLY 7u /* a regular polygon of slices sides, fanned from its centre */
#define XUD_MESH_KIND_HEIGHTMAP 8u /* a grid of the CALLER's own samples: width by length, one vertex per sample */
#define XUD_MESH_KIND_COUNT 9u /* the length of this axis */

static_assert(XUD_MESH_KIND_COUNT == (XUD_MESH_KIND_HEIGHTMAP + 1u),
    "xud.h: the shape axis is contiguous, because xud_gen_mesh() reads a kind as an index into its own table");

/// What to cut: one description for every kind, the way XudMeshData and XudFontData are one
/// description each. Which numbers a kind reads is stated at the axis above - a kind ignores the
/// fields it has no use for, so a description is never half-filled by accident. Extents and radii
/// are world units, and the floors they are refused under are the rung's (XUD_GENERATE_MIN_EXTENT,
/// XUD_GENERATE_MIN_RADIUS): a solid with a zero extent has no inside, and raylib answers that with
/// an all-zero Mesh and a log line, which this library answers with a code.
typedef struct XudMeshShape {
    uint32_t kind; /* one of XUD_MESH_KIND_*, and the only field every kind reads */
    float radius; /* sphere, hemisphere, cylinder, torus ring, poly, knot curve */
    float tubeRadius; /* the torus's tube and the knot's thickness */
    float width; /* plane, cube and heightmap, along x */
    float height; /* cube and cylinder, along y */
    float length; /* plane, cube and heightmap, along z */
    uint32_t slices; /* longitudes, tube rings around a solid, plane columns, poly sides */
    uint32_t stacks; /* latitudes, plane rows, the knot's segments along its curve */
    const float* heights; /* heightmap only: heightColumns*heightRows samples, row-major, in WORLD units */
    uint32_t heightColumns; /* heightmap only: samples along x, and so the mesh's vertex columns; at least 2 */
    uint32_t heightRows; /* heightmap only: samples along z, and so the mesh's vertex rows; at least 2 */
} XudMeshShape;

/// What a shape needs: the two counts xud_gen_mesh_count() answers and xud_gen_mesh() writes, in
/// the one pair the caller sizes its buffers from.
typedef struct XudMeshCounts {
    uint32_t vertexCount; /* vertices the shape has, the count every stream is sized from */
    uint32_t indexCount; /* indices it has, a whole number of triangles */
} XudMeshCounts;

/// Where the shape lands: the arrays the caller owns and sized from the counts. pos and index are
/// the two streams a mesh cannot be described without (xud_mesh_count() refuses a shape whose
/// position or index array is missing, and xud_upload_mesh() refuses the description it makes);
/// texcoords and normal MAY be NULL, which is the stream left off rather than a stream of zeros -
/// a mesh with no normals is a mesh the shaders light from +Z, which is the default the mesh rung's
/// own note gives. A NULL stream is written nothing, so the caller's own content in it (there is
/// none) is untouched.
typedef struct XudMeshBuffers {
    float* pos; /* vertexCount*3, the one stream with no default */
    float* texcoords; /* vertexCount*2, or NULL */
    float* normal; /* vertexCount*3, or NULL */
    uint32_t* index; /* indexCount, the other stream with no default - see the note above */
} XudMeshBuffers;

/// How big a shape is: the count half of the pair, and the only way to size the buffers the fill
/// half wants. raylib's GenMesh* family has no half like this because it allocates inside the call
/// and hands back arrays only UnloadMesh() may free; this library keeps nothing a caller did not
/// hand it, so the counts are asked for first. The two calls run the same validation and the same
/// arithmetic, which is what makes the fill's writes provably in bounds - the count it writes to is
/// this function's answer, not a number the caller guessed. XUD_INVALID_ARG on a NULL out or shape,
/// a kind outside the axis, an extent or radius under its floor or not finite, a slice, stack or
/// side count under the rung's floor for that kind, a heightmap with no samples, with fewer than two
/// samples on an axis, or with a sample that is not finite (one NaN height is a vertex at NaN and two
/// NaN normals), or a shape whose vertex count is past the mesh rung's ceiling (XUD_MESH_MAX_VERTICES)
/// - which is refused here because a solid the mesh rung could never upload is not a solid worth cutting.
XudResult xud_gen_mesh_count(XudMeshCounts* outCounts, const XudMeshShape* shape);

/// Cut a shape into the caller's buffers: the fill half, raylib's GenMeshPlane (rmodels.c 1027),
/// GenMeshCube (1100), GenMeshSphere (1136), GenMeshHemiSphere (1215), GenMeshCylinder (1287),
/// GenMeshTorus (1374), GenMeshKnot (1460) and GenMeshPoly (1546) as ONE call over a described
/// shape. Every triangle is wound counter-clockwise seen from outside, which is the front face the
/// pipelines name (XUD_PIPELINE_FRONT_FACE) even though nothing is culled today, and every normal it
/// writes points out of the solid by that same winding. The position stream is the solid at the
/// origin, untransformed: a placed solid is placed by the matrix beside xud_draw_mesh_ex(), and a
/// ray is thrown at it through the same matrix (xud_get_ray_collision_mesh's transform). The same
/// refusals as the count half, and two more: a NULL buffers, or a missing pos or index array in it.
/// On success *outMesh is the description of what was written - counts, and the stream pointers the
/// caller handed in, ready for xud_upload_mesh() and for the casts without a second copy.
XudResult xud_gen_mesh(XudMeshData* outMesh, const XudMeshShape* shape, const XudMeshBuffers* buffers);

/// Put a material in the drawing state, for this frame and the ones that follow: the Begin*/End*
/// shape of raylib's scoped state calls (BeginBlendMode()/EndBlendMode(), raylib.h 1058-1059), a
/// bove the material rung. The handle is VALIDATED here - index bounds against the slot array of t
/// he MATERIAL kind of THIS context's table, then generation equality, then tag equality - so a han
/// dle from another context, a handle kept across the close of the material it names, and a handle
/// of another kind are all XUD_INVALID_ARG rather than a read of the wrong array. What is stored in
/// the renderer is a COPY of the two numbers the batch's state move needs (the handle by value, the
/// variant off the record) rather than a pointer into the table: the renderer owns the frame loop a
/// nd re-applies the pair at every frame opening, which is what makes a material begun in one frame
/// drawn with in the next rather than leaked into it. The state also moves at the call, because a
/// scoped state is what the draws that follow in the SAME frame read: a material change is the batc
/// h's row split, so a begin mid-frame closes the run that was pending under the previous state in
/// to a row of its own and starts a new one, and a frame that mixes two materials records one row
/// per material. XUD_INVALID_ARG on a NULL context or on a handle that does not name a live MATERI
/// AL slot of this context - or on one whose DIFFUSE MAP no longer names a live TEXTURE slot of it,
/// which is the texture closed behind the material that names it and is refused here rather than dr
/// awn through as a sample of a destroyed image; XUD_UNSUPPORTED on a context that never completed b
/// ring-up.
XudResult xud_begin_material(XudContext* context, XudHandle handle);

/// Take the material away: raylib's EndBlendMode() read literally ("reset to default: alpha blen
/// ding", raylib.h 1059). The frames drawn after here are drawn with no material at all, and the b
/// atch moves back to that default HERE rather than waiting for the next frame's reset, so the draw
/// s that follow in this frame are drawn with no material too - which is what makes a material tha
/// t began and ended inside one frame a no-op rather than a leak of state into the next frame. XUD_
/// ALREADY when no material was live (the state is already the one this call asks for); XUD_INVALI
/// D_ARG on a NULL context; XUD_UNSUPPORTED on a context that never completed bring-up.
XudResult xud_end_material(XudContext* context);

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

/// The frame's batch as the caller sees it: the live counts, the two capacities the appends are
/// checked against, the recorded draw rows, the arena's total, and the capacity ask the frame
/// boundary reads before it grows - the largest total a refused append was asked to hold since the
/// last grow, 0/0 when nothing has been refused. The two flags are the diagnosis a refused frame
/// needs: growthAsked says the ask exceeds the current capacity, so the next xud_begin_drawing()
/// will try to grow, and atBound says the capacity is at the batch's bound, so the grow cannot
/// happen and an ask above the capacity is refused until the scene fits the capacity it has. The
/// counters are the batch's own rows read through its accessors, live in the batch a completed
/// bring-up owns.
typedef struct XudBatchStats {
    uint32_t vertexCount; /* the live vertices, the count every stream holds */
    uint32_t indexCount; /* the live indices */
    uint32_t vertexCapacity; /* the capacity an append's vertex bound is checked against */
    uint32_t indexCapacity; /* the capacity an append's index bound is checked against */
    uint32_t drawCount; /* the draw rows recorded for the frame so far */
    uint64_t stagingBytes; /* the arena's total, in bytes */
    uint32_t wantedVertices; /* the largest refused append's vertex ask since the last grow - 0 when nothing was refused */
    uint32_t wantedIndices; /* the index half of the same ask */
    uint32_t growthAsked; /* 1 when the ask above exceeds the current capacity: the next xud_begin_drawing() grows */
    uint32_t
        atBound; /* 1 when the capacity is at the batch's bound: a grow cannot happen, so an ask above the capacity is refused until the scene fits */
} XudBatchStats;

/// The batch stats above, for the context's frame batch. XUD_INVALID_ARG on a NULL out-stats or
/// context; XUD_UNSUPPORTED on a context that never completed bring-up.
XudResult xud_get_batch_stats(XudBatchStats* outStats, XudContext* context);

/// Set the background the frames this context draws are cleared to: raylib's ClearBackground
/// (raylib.h 1047, "Clear background (framebuffer) to color") at this library's currency - a call
/// with the caller's colour and a frame whose clear is the answer, rather than a clear that
/// happens at the call. The colour is stored in the renderer that owns the frame loop, AS GIVEN
/// (the same value-not-pointer rule the camera's record follows one section over), and the
/// renderer's draw entry writes it into the render pass's clear value before the frame loop begins
/// the pass it records - which is what makes a call in frame N change frame N's own clear rather
/// than the frame after: the recording runs at xud_end_drawing(), after every draw call the frame
/// made. A frame drawn before any call is cleared with the default the render pass's note gives,
/// so a context that never calls here draws the background it always has. XUD_INVALID_ARG on a
/// NULL context; XUD_UNSUPPORTED on a context that never completed bring-up.
XudResult xud_clear_background(XudContext* context, XudColor color);

/*-----------------------------------------------------------------------------------------------
 * The object API - the materials a context owns, and the handles that name them
 *
 * This is the first place in the public surface a XudHandle is handed out and released, and it is
 * the object half today: four kinds (MATERIAL, MESH, TEXTURE and MODEL) have a rung above the
 * handle table, and the one kind left without a step is the FONT kind. The pairs are raylib's Load
 * /Unload shape read at this library's currency - raylib hands a struct with an embedded GL name in
 * side (raylib.h's Texture2D/Mesh/Model), which this repo refuses, so what a caller holds is the h
 * andle and what the library owns is the slot and its record.
 *-----------------------------------------------------------------------------------------------*/

/// Create a material this context owns: the object half of the material rung. The description is v
/// alidated before anything is spent, so a blend mode outside the axis is a bad argument rather th
/// an a material that draws through a row that does not exist, and a diffuseMap that is neither XUD
/// _HANDLE_NONE nor a live TEXTURE slot of THIS context's table is refused the same way - the map i
/// s named by handle and owned by the texture rung, so the create takes no ownership of it and no i
/// mage is read here. The variant the record carries is the resolved one: the textured row when the
/// material has a map, the blend mode's row when it has none. The slot and the record are then allo
/// cated in the one order that leaves nothing reachable and unfilled, so a refusal on either alloca
/// tion leaves no half-material behind. On XUD_OK *outHandle names a live MATERIAL slot of THIS con
/// text's table and the caller holds it for as long as the material does - a handle kept past the
/// close that released it is refused, not dangling. XUD_INVALID_ARG on a NULL out-handle or context
/// , on a blend mode outside the axis, or on a diffuse map that names no live TEXTURE slot of this c
/// ontext; XUD_OUT_OF_RESOURCES at the handle layer's own slot bound; XUD_OUT_OF_MEMORY when the rec
/// ord cannot be allocated; XUD_UNSUPPORTED on a context that never completed bring-up.
XudResult xud_create_material(XudHandle* outHandle, XudContext* context, const XudMaterial material);

/// Release one material: the object half closed. The handle is validated against the MATERIAL kind
/// first, so a stale handle, a handle of another kind and a fabricated one refuse without freeing a
/// nything; the record is cleared from the slot BEFORE it is freed, so a close run twice frees once
/// and the table's own teardown can never find a record it would have to free twice. A material the
/// renderer is currently drawing with is taken out of that state as well, so the frames after here
/// are drawn with no material rather than with rows naming a slot whose record no longer exists - t
/// he same rule the camera pair states for a camera that began and ended inside one frame. The dif
/// fuse map is NOT closed with the material: the texture rung's slot owns the image and outlives ev
/// ery material that names it, so a caller that wants the map gone closes it itself and the materia
/// ls that named it start refusing their begin. XUD_INVALID_ARG on a NULL context or on a handle th
/// at does not name a live MATERIAL slot of this context
/// ; XUD_UNSUPPORTED on a context that never completed bring-up.
XudResult xud_close_material(XudContext* context, XudHandle handle);

/// Upload a mesh this context owns: the object half of the mesh rung, and the first public create pa
/// ir whose record owns driver objects. The description is validated and gated on the caps BEFORE an
/// ything is spent, so a refusal leaves no slot and no half-mesh; the slot, the mesh's five buffers
/// and the ONE host-visible block their bytes live in are then brought up in the one order that lea
/// ves nothing reachable and unfilled, and the mesh's bytes are written into that block once, here,
/// and never again - which is why this call is an upload rather than a draw. On XUD_OK *outHandle n
/// ames a live MESH slot of THIS context's table and the caller holds it for as long as the mesh do
/// es; a handle kept past the close that released it is refused, not dangling. XUD_INVALID_ARG on a
/// NULL out-handle, context or description, or on a mesh the rung's own check refuses (no vertices,
/// no indices, an index count that is not a whole number of triangles, an index at or past the vert
/// ex count, or a mesh past this rung's size bound); XUD_UNSUPPORTED on a context that never comple
/// ted bring-up, on a loader whose rows this rung needs are not there, on a device that is not this
/// loader's, or on a mesh the caps gate refuses; XUD_OUT_OF_RESOURCES at the handle layer's own slot
/// bound; XUD_OUT_OF_MEMORY or XUD_OUT_OF_RESOURCES when the block cannot be allocated; XUD_VULKAN_E
/// RROR on a driver that refuses a create or reports a buffer that asks for no memory.
XudResult xud_upload_mesh(XudHandle* outHandle, XudContext* context, const XudMeshData* mesh);

/// Release one mesh: the object half closed, and the first public close pair that calls a driver. Th
/// e handle is validated against the MESH kind first, so a stale handle, a handle of another kind an
/// d a fabricated one refuse without destroying anything; the wait-idle guard runs before the destro
/// ys, because a mesh's buffers are read by a submission and VUID-vkDestroyBuffer-buffer-00007 is t
/// he same rule the swapchain's recreate and the allocator's close state for their own objects. The
/// arena is unmapped, the five buffers destroyed and the block released, and the slot's record field
/// is cleared BEFORE the record is freed, so a close run twice frees once and the table's own teard
/// own can never find a record it would have to free twice. XUD_INVALID_ARG on a NULL context or on
/// a handle that does not name a live MESH slot of this context; XUD_UNSUPPORTED on a context that n
/// ever completed bring-up or on a loader whose rows are not this device's; the device's own code wh
/// en the guard cannot run.
XudResult xud_close_mesh(XudContext* context, XudHandle handle);

/// Create a texture this context owns: the object half of the texture rung, and the second public
/// create pair whose record owns driver objects (after the mesh's). The description is validated and
/// gated on the caps BEFORE anything is spent, so a refusal leaves no slot and no half-texture; the
/// slot, the image, the view, the sampler and the ONE host-visible block the texels live in are then
/// brought up in the one order that leaves nothing reachable and unfilled, and the texels and the wh
/// ole mip chain are written into that block once, here, and never again - which is why this call nam
/// es a create rather than a draw. On XUD_OK *outHandle names a live TEXTURE slot of THIS context's
/// table and the caller holds it for as long as anything samples it; a handle kept past the close tha
/// t released it is refused, not dangling. XUD_INVALID_ARG on a NULL out-handle, context or descripti
/// on, or on a texture the rung's own check refuses (no texels, an extent past this rung's bound, a t
/// exel count past it, a filter or wrap mode outside its axis, or a NULL texel array); XUD_UNSUPPORTED
/// on a context that never completed bring-up, on a loader whose rows this rung needs are not there,
/// on a device that is not this loader's, on a device whose maxImageDimension2D is below either axis,
/// or on a driver whose reported subresource layout does not match this rung's; XUD_OUT_OF_RESOURCES a
/// t the handle layer's own slot bound, at the context's XUD_MAX_TEXTURES ceiling of live textures, or
/// when the block cannot be carved; XUD_OUT_OF_MEMORY when the
/// record cannot be allocated; XUD_VULKAN_ERROR or XUD_LOST when the driver refuses for its own reas
/// on.
XudResult xud_create_texture(XudHandle* outHandle, XudContext* context, const XudTextureData* texture);

/// Release one texture: the object half closed, and the second public close pair that calls a driver.
/// The handle is validated against the TEXTURE kind first, so a stale handle, a handle of another kin
/// d and a fabricated one refuse without freeing anything; the wait-idle guard runs before the destroy
/// s, because a texture's image is read by a submission and VUID-vkDestroyImage-image-00009 is the sam
/// e rule the mesh's buffer destroy and the swapchain's recreate state state for their own objects. The
/// mapping is unmapped, the sampler, the view and the image destroyed and the block released, and the
/// slot's record field is cleared BEFORE the record is freed, so a close run twice frees once and the
/// table's own teardown can never find a record it would have to free twice. XUD_INVALID_ARG on a NULL
/// context or on a handle that does not name a live TEXTURE slot of this context; XUD_UNSUPPORTED on a
/// context that never completed bring-up or on a loader whose rows are not this device's; the device's
/// own code when the guard cannot run.
XudResult xud_close_texture(XudContext* context, XudHandle handle);

/// Change how a live texture is filtered: raylib's SetTextureFilter, the sampler's two filter rows
/// (magnify and minify) at once, the one token raylib names for both. The token is the sampler's own
/// - XUD_TEXTURE_FILTER_NEAREST or XUD_TEXTURE_FILTER_LINEAR - and the change is a recreate: the
/// texture's own sampler is destroyed and a new one created with the new filter, then the entry of
/// set 0's sampler array that names the texture is rewritten to hold the new sampler, which is what
/// makes the change live for every draw that samples the texture from this call on, the 3D ones and
/// the screen-space one alike. The change takes no upload: the texels, the image, the view and the
/// mip chain are untouched, and a draw in the frame this call runs in samples the new filter,
/// because the frame loop records its commands when the frame ends, after the call has already
/// rewritten the set. The frame loop's single-flight order is what makes the swap need no wait of
/// its own: the device is idle while the caller runs, so no in-flight command names the sampler
/// that is destroyed. A token the sampler already names is a no-op that answers XUD_OK without
/// touching the driver. XUD_INVALID_ARG on a NULL context, on a token outside the filter axis, or
/// on a handle that does not name a live TEXTURE slot of this context; XUD_UNSUPPORTED on a context
/// that never completed bring-up or on a loader whose rows this rung needs are not there; the
/// driver's own code when the recreate's create or the set's rewrite refuses.
XudResult xud_set_texture_filter(XudContext* context, XudHandle handle, uint32_t filter);

/// Change how a live texture wraps: raylib's SetTextureWrap, the sampler's two live address rows
/// (U and V) at once, the one token raylib names for both. The token is the sampler's own -
/// XUD_TEXTURE_ADDRESS_REPEAT, XUD_TEXTURE_ADDRESS_MIRRORED_REPEAT or
/// XUD_TEXTURE_ADDRESS_CLAMP_TO_EDGE - and the change is the same recreate the filter setter makes:
/// the texture's own sampler is destroyed and a new one created with the new wrap, then the entry of
/// set 0's sampler array that names the texture is rewritten to hold the new sampler, which is what
/// makes the change live for every draw that samples the texture from this call on. The W axis of
/// the sampler stays the rung's own mip clamp, because it is the axis of a 2D texture's chain rather
/// than a wrap the caller can choose. A token the sampler already names is a no-op that answers
/// XUD_OK without touching the driver. XUD_INVALID_ARG on a NULL context, on a token outside the
/// address axis, or on a handle that does not name a live TEXTURE slot of this context;
/// XUD_UNSUPPORTED on a context that never completed bring-up or on a loader whose rows this rung
/// needs are not there; the driver's own code when the recreate's create or the set's rewrite
/// refuses.
XudResult xud_set_texture_wrap(XudContext* context, XudHandle handle, uint32_t addressMode);

/// Create an off-screen draw target: raylib's LoadRenderTexture, whose image is the caller's extent.
/// The target owns a colour image (drawable AND sampleable), a depth image of the same extent, the
/// render pass and framebuffer that draw into them, a batch of its own for the geometry appended
/// while its scope is open, and one entry of set 0's sampler array so xud_draw_texture() can sample
/// the result by this same handle. The colour format is the swapchain's; depth always exists. On
/// XUD_OK *outTarget names a live RENDERTEXTURE slot of THIS context's table. XUD_INVALID_ARG on a
/// NULL out-handle or context, or a zero extent; XUD_UNSUPPORTED on a context that never completed
/// bring-up; XUD_OUT_OF_RESOURCES at the handle layer's slot bound; the target rung's and the
/// allocator's own codes on the objects they bring up.
XudResult xud_create_render_texture(XudHandle* outTarget, XudContext* context, uint32_t width, uint32_t height);

/// Release one render target: its image, sampler entry, pass, framebuffer and batch, in the order
/// that leaves no descriptor entry naming a destroyed object. A target whose scope is open right
/// now cannot be closed - XUD_UNSUPPORTED - because the renderer still points its draw paths at
/// that target's batch; xud_end_texture_mode() first. XUD_INVALID_ARG on a NULL context or a
/// handle that does not name a live RENDERTEXTURE slot of this context; the rung's own codes on
/// the objects it retires.
XudResult xud_close_render_texture(XudContext* context, XudHandle target);

/// Open one target's scope for this frame: raylib's BeginTextureMode. Every draw appended from here
/// to xud_end_texture_mode() lands in the target's own batch, and the frame records the target's
/// pass BEFORE the screen's, so the image is sampleable by the screen pass of the same frame. The
/// target clears to the current background colour when its pass opens. Re-opening a target already
/// open this frame is the same scope, not a second one. One scope may be open at a time. XUD_
/// INVALID_ARG on a NULL context or a handle that names no live RENDERTEXTURE slot; XUD_ALREADY
/// when a DIFFERENT target's scope is already open; XUD_UNSUPPORTED when this frame's scope rows
/// are all live.
XudResult xud_begin_texture_mode(XudContext* context, XudHandle target);

/// Close the open scope: raylib's EndTextureMode. Draws return to the screen. XUD_INVALID_ARG on a
/// NULL context; XUD_ALREADY when no scope is open.
XudResult xud_end_texture_mode(XudContext* context);

/// Load a model this context owns: the object half of the model rung, and the fourth public create
/// pair whose record owns driver objects - and the first whose record owns OTHER handles. A model is
/// not an object of the driver's: it is one MESH slot per glTF primitive and one MATERIAL slot per
/// glTF material plus the default every untextured primitive draws with, which is why this pair is
/// named load/close rather than create/close and why raylib's LoadModel - a struct with a mesh list
/// and an embedded GL name inside it - is the shape this repo refuses to mirror. cgltf is read here,
/// above the rung, in the order parse, buffer load, validate: cgltf_validate() checks the accessor
/// ranges against the bytes a document actually has, so a document whose buffers were never loaded
/// has nothing to check. The document is then freed here rather than kept, because the mesh rung's
/// own create copied the bytes the accessors pointed at into the mesh's mapped arena - so a model is
/// a list of the objects the rungs below own and never a live document. On XUD_OK *outHandle names a
/// live MODEL slot of THIS context's table. The draw half is xud_draw_model() above this pair rather
/// than a member of it: a model is a list of MESH slots, so the draw is the loop over that list, and
/// the batch's row that replays through a mesh's own buffers beside the arena is the batch rung's. XUD
/// INVALID_ARG on a NULL out-handle, context or path, on an empty path, on a path whose directory
/// prefix does not fit this rung's bound, or on a document that is broken (no mesh, no POSITION, accessors
/// that disagree about the vertex count, an index at or past the vertex count, an accessor that reads
/// past its buffer view, a buffer whose bytes were never loaded, or counts past this rung's bounds);
/// XUD_NOT_FOUND when the path names no file; XUD_MALFORMED when the container or the JSON does not
/// parse; XUD_UNSUPPORTED on a context that never completed bring-up, on a loader whose rows this
/// rung needs are not there, on a device that is not this loader's, or on a well-formed document this
/// rung does not read (a glTF 1.0 asset, a non-triangle primitive, a Draco block, a morph target, a
/// sparse accessor, a tangent, a second UV); XUD_OUT_OF_RESOURCES at the handle layer's own slot bound
/// ; XUD_OUT_OF_MEMORY when the record or a stage block cannot be allocated; the mesh rung's and the
/// material rung's own codes on the objects they bring up.
XudResult xud_load_model(XudHandle* outHandle, XudContext* context, const char* path);

/// Release one model: the object half closed, and the first public close pair that closes OTHER cal
/// ler handles. The handle is validated against the MODEL kind first, so a stale handle, a handle of
/// another kind and a fabricated one refuse without freeing anything; every MESH slot this load crea
/// ted is closed through the mesh rung below and every MATERIAL slot through the material rung, in
/// that order, because a mesh's record names the material it draws with and a material still named by
/// a live mesh is a state a caller could draw through. The slot's record field is cleared BEFORE the
/// record is freed, the rule every close in this tree states, so a close run twice frees once and the
/// table's own teardown cannot find a record it would have to free twice. XUD_INVALID_ARG on a NULL
/// context or on a handle that does not name a live MODEL slot of this context; XUD_UNSUPPORTED on a
/// context that never completed bring-up or on a loader whose rows are not this device's; the mesh
/// rung's own code when one of its closes refuses.
XudResult xud_close_model(XudContext* context, XudHandle handle);

/// How many meshes a model owns: raylib's GetModelCount read at this library's currency. A model is a
/// list of slots rather than a struct with a mesh array inside it, so the count is what a caller needs
/// in order to ask for the handles one at a time - and the handles themselves are not exposed here, bec
/// ause a mesh handle a caller copied out of a model could be closed on its own and leave the model's
/// record naming a released slot. XUD_INVALID_ARG on a NULL out-count or context, or on a handle that
/// does not name a live MODEL slot of this context; XUD_UNSUPPORTED on a context that never completed
/// bring-up.
XudResult xud_get_model_mesh_count(uint32_t* outCount, XudContext* context, XudHandle handle);

/// The model's bounding box: raylib's GetModelBoundingBox, folded at the load rather than walked from
/// the buffers at the query. The stage a primitive is read through is gone by the time a caller could
/// ask for it, so the box is the record's own two corners and the query answers in constant time. XUD_
/// INVALID_ARG on a NULL out-corner or context, or on a handle that does not name a live MODEL slot of
/// this context; XUD_UNSUPPORTED on a context that never completed bring-up.
XudResult xud_get_model_bounding_box(XudVector3* outMin, XudVector3* outMax, XudContext* context, XudHandle handle);

/// Draw one mesh this context owns: the first public draw that names a handle, and the mesh's row of
/// the drawing API. Nothing is appended here - the mesh's bytes are already in its own five buffers,
/// uploaded once at xud_upload_mesh() - so the call records one draw row that replays through them:
/// the batch's pending run of its own arena is closed into a row of its own first if it has content
/// in it, and the mesh's row names the whole of that mesh's index range, the topology the mesh rung's
/// one topology states (TRIANGLES, whatever the pending run was assembled as), and the variant and
/// the material the batch's state is currently under - a mesh draws with the material that was begun
/// around it, which is why this call takes no material of its own. XUD_INVALID_ARG on a NULL context
/// or on a handle that does not name a live MESH slot of this context; XUD_UNSUPPORTED on a context
/// that never completed bring-up; XUD_INTERNAL_ERROR on a live MESH slot whose buffers are gone, which
/// is a state the mesh rung's own open made rather than an argument a caller could fix; the batch's
/// codes otherwise, XUD_OUT_OF_MEMORY being the only one a caller can act on.
XudResult xud_draw_mesh(XudContext* context, XudHandle handle);

/// Draw a mesh with the caller's transform and colour: raylib's DrawMeshEx. The matrix is the 16
/// floats in column-major order the row's own push channel carries - the vertex stage applies it to
/// the position and to the normal, so a posed mesh is placed by the matrix beside the camera's
/// view-projection rather than by the camera alone, and the fragment stage multiplies the caller's
/// colour into the row. The material around the call is still the material the row draws under: the
/// pose and the colour are per-draw channels, the material is the state above, and the two do not
/// mix. XUD_INVALID_ARG on a NULL context, a NULL matrix, a non-finite element of the matrix, or on
/// a handle that does not name a live MESH slot of this context; XUD_UNSUPPORTED on a context that
/// never completed bring-up; XUD_OUT_OF_MEMORY being the only batch code a caller can act on.
XudResult xud_draw_mesh_ex(XudContext* context, XudHandle handle, const float* modelMatrix, XudColor tint);

/// Draw every mesh a model owns, each with the material the load read for it: the model's row of the
/// drawing API. A model is a list of MESH slots and one MATERIAL slot per primitive, so this call
/// is the loop over that list rather than one row: each mesh is recorded with the variant and the
/// material slot the load read for it, whatever material was begun around the call, and the batch's
/// state moves to the LAST mesh's pair on success, which is raylib's DrawModel leaving the last mesh's
/// texture state behind it. XUD_INVALID_ARG on a NULL context or on a handle that does not name a live
/// MODEL slot of this context; XUD_UNSUPPORTED on a context that never completed bring-up; XUD_INTERNAL_ERROR if
/// any MESH or MATERIAL slot the load created has lost its buffers or its record, which is a state the
/// model rung's own open made; the batch's codes otherwise, and a row that refused leaves the rows
/// recorded so far recorded - the batch's rows are a list, and a partial model is a frame that drew
/// part of it rather than a frame that drew none of it.
XudResult xud_draw_model(XudContext* context, XudHandle handle);

/// Draw a model with the caller's transform and colour: raylib's DrawModelEx. The matrix is the 16
/// floats in column-major order the row's own push channel carries - the vertex stage applies it to
/// the position and to the normal, so a posed model is placed by the matrix beside the camera's
/// view-projection rather than by the camera alone, and the fragment stage multiplies the caller's
/// colour into the row. The pose is beside every row the model's primitives record, because the pose
/// is the model's transform and every primitive of the model lives in the model's space; the
/// material is still the one the load read for each primitive, a model's own rather than the state
/// around the call. XUD_INVALID_ARG on a NULL context, a NULL matrix, a non-finite element of the
/// matrix, or a model handle the table does not name; XUD_UNSUPPORTED on a context with no batch to
/// record into; XUD_OUT_OF_MEMORY when a row does not fit the batch, with the rows before it already
/// recorded.
XudResult xud_draw_model_ex(XudContext* context, XudHandle handle, const float* modelMatrix, XudColor tint);

/// Draw one texture at the screen: the textured half of the 2D surface, raylib's DrawTexture. The
/// texture's whole extent is one screen-space quad at the caller's pixel position, through the
/// pipeline list's UI column with depth test and write both off and the view fixed to the screen -
/// a 3D camera in effect moves the scene around the quad rather than the quad, and the quad does not
/// write the depth a later 3D row tests against. The position is the top-left corner in PIXELS with y
/// DOWN from the framebuffer's top-left, the quad is the texture's own extent at its own size (the
/// scaled form is a later step's), and the caller's colour is the quad's own vertex colour: the row
/// it records into carries the white push tint the plain record stamps, and the fragment stage's
/// product is the sampled texel times the vertex colour times that white, the tinted texture. The
/// texture draws through its own sampler-array entry, a record that
/// holds none drawing through the sentinel the frame loop maps to slot 0's default. XUD_INVALID_ARG
/// on a NULL context, a texture handle the table does not name, a zero framebuffer extent, or a quad
/// the batch does not fit; XUD_UNSUPPORTED on a context with no batch to append into; XUD_OUT_OF_MEMORY
/// when the quad does not fit the batch.
XudResult xud_draw_texture(XudContext* context, XudHandle handle, int32_t posX, int32_t posY, XudColor tint);

/// Draw a filled rectangle at the screen: the plain half of the 2D surface, raylib's DrawRectangle,
/// and the quad no texture walks for. It is one row of the pipeline list's UI column, the same
/// column the texture pair above and the screen-space text draw through: depth test and write are
/// both off and the view is fixed to the screen, so a 3D camera in effect moves the scene around
/// the rectangle rather than the rectangle, and the rectangle does not write the depth a 3D row
/// later in the frame tests against. The position is the top-left corner in PIXELS with y DOWN from
/// the framebuffer's top-left, the extent is the rectangle's own in pixels, and a zero or negative
/// extent is refused rather than drawn as a degenerate quad - the same refusal the textured half
/// makes of a zero framebuffer extent, applied to the caller's own numbers. The colour is the quad's
/// own vertex colour for the same reason the textured half above states it: the row it records into
/// carries the white push tint the plain record stamps, and the fragment stage's product is the
/// vertex colour times that white, the colour as given. XUD_INVALID_ARG on a NULL context, a zero or
/// negative extent, or a zero framebuffer extent; XUD_UNSUPPORTED on a context with no batch to
/// append into; XUD_OUT_OF_MEMORY when the quad does not fit the batch.
XudResult xud_draw_rectangle(XudContext* context, int32_t posX, int32_t posY, int32_t width, int32_t height, XudColor color);

/// Draw a line at the screen: raylib's DrawLine, the 2D half of the line pair, through the same
/// UI column as the texture and rectangle above: the two endpoints at the caller's own pixels, y
/// DOWN from the framebuffer's top-left, mapped to clip at the framebuffer's extent and appended
/// as the batch's line shape with depth test and write both off, so the line is screen-fixed and
/// a 3D camera in effect moves the scene around it rather than it, and it does not write the
/// depth a 3D row later in the frame tests against. The endpoints carry no half-pixel offset of
/// the outline kind below - raylib's own DrawLine passes the coordinates through unshifted - and
/// a line whose endpoints coincide is the zero-length line raylib draws, appended rather than
/// refused. The colour is the line's own vertex colour, the row it records into carrying the
/// white push tint the plain record stamps, the same product the rectangle above states.
/// XUD_INVALID_ARG on a NULL context or a zero framebuffer extent; XUD_UNSUPPORTED on a context
/// with no batch to append into; XUD_OUT_OF_MEMORY when the line does not fit the batch.
XudResult xud_draw_line(XudContext* context, int32_t startPosX, int32_t startPosY, int32_t endPosX, int32_t endPosY, XudColor color);

/// Draw a pixel at the screen: raylib's DrawPixel, one pixel of geometry - the filled rectangle
/// above at a one-pixel extent, so the call is that rectangle at the caller's pixel position
/// rather than a second append of the same quad, through the same UI column with the same depth
/// and state agreement. XUD_INVALID_ARG on a NULL context or a zero framebuffer extent;
/// XUD_UNSUPPORTED on a context with no batch to append into; XUD_OUT_OF_MEMORY when the quad
/// does not fit the batch.
XudResult xud_draw_pixel(XudContext* context, int32_t posX, int32_t posY, XudColor color);

/// Draw a rectangle outline at the screen: raylib's DrawRectangleLines - the four edges of the
/// caller's rectangle as the batch's four lines, through the same UI column as the line above,
/// the four appends sharing one state swap so the outline flushes as one row of its own rather
/// than four. The endpoints carry the same half-pixel inset raylib's outline applies - its 0.5
/// over the transform's scale, which at the screen's own transform is half a pixel - because an
/// edge that sat on a whole pixel row would straddle the boundary of two rows, and the inset puts
/// the line's centre on the row it names. The position is the top-left corner in PIXELS with y
/// DOWN, the extent the outline's own in pixels, and a zero or negative extent is refused rather
/// than drawn as a degenerate outline, the same refusal the filled rectangle above makes. The
/// colour is the edges' own vertex colour, the same reason the rectangle above states it.
/// XUD_INVALID_ARG on a NULL context, a zero or negative extent, or a zero framebuffer extent;
/// XUD_UNSUPPORTED on a context with no batch to append into; XUD_OUT_OF_MEMORY when an edge does
/// not fit the batch, with the edges before it already appended and the state the call found
/// restored.
XudResult xud_draw_rectangle_lines(XudContext* context, int32_t posX, int32_t posY, int32_t width, int32_t height, XudColor color);

/// Draw a color-filled triangle at the screen: raylib's DrawTriangle (raylib.h 1285, "Draw a
/// color-filled triangle, counter-clockwise vertex order"): the caller's three own screen-space
/// points, in the order the caller named them, y DOWN from the framebuffer's top-left in the same
/// space the rectangle and the line above take, mapped to clip at the framebuffer's extent and
/// appended as the batch's triangle shape through the pipeline list's UI column, the same way the
/// rectangle above draws its quad: the state swap moves the batch to the UI row for the triangle
/// and back when it is done, so the triangle flushes as one row of its own and the shapes
/// appended beside it draw with the state the call found. The three vertices the caller named are
/// the row's three, in the order the caller named them: the winding raylib states is advisory
/// here, because the UI row's rasterizer culls nothing, so a triangle named clockwise draws as
/// well as one named counter-clockwise, and a triangle whose points coincide is a degenerate
/// triangle appended rather than refused, the same way the zero-length line above is. The points
/// are the caller's floats passed through, the way the 3D triangle above takes the caller's world
/// coordinates: the framebuffer's extent is checked before it is divided by, and nothing below
/// is. XUD_INVALID_ARG on a NULL context or a zero framebuffer extent; XUD_UNSUPPORTED on a
/// context with no batch to append into; XUD_OUT_OF_MEMORY when the triangle's three vertices or
/// its three indices do not fit the batch.
XudResult xud_draw_triangle(XudContext* context, XudVector2 v0, XudVector2 v1, XudVector2 v2, XudColor color);

/// Draw a triangle outline at the screen: raylib's DrawTriangleLines (raylib.h 1287, "Draw triangle
/// outline, counter-clockwise vertex order"): the caller's three own screen-space points, in the
/// order the caller named them, y DOWN from the framebuffer's top-left in the same space the
/// triangle above takes, the three edges between them in the order raylib's own vertex sequence
/// walks them - first to second, second to third, and third back to first - appended as the
/// batch's three lines through the pipeline list's UI column the line above draws its one with:
/// the state swap moves the batch to the UI row for the outline and back when it is done, so the
/// outline flushes as one row of its own instead of three, the same one-run agreement the
/// rectangle outline above states for its four edges. The points are the caller's floats passed
/// through at the same extent the triangle above divides by, and unlike the rectangle outline
/// above, raylib's triangle outline carries no half-pixel inset, so the edges sit on the caller's
/// points rather than inside the triangle they bound: a triangle whose points coincide is
/// degenerate edges appended rather than refused, the same way the zero-length line above is.
/// XUD_INVALID_ARG on a NULL context or a zero framebuffer extent; XUD_UNSUPPORTED on a context
/// with no batch to append into; XUD_OUT_OF_MEMORY when an edge's two vertices or its two indices
/// do not fit the batch.
XudResult xud_draw_triangle_lines(XudContext* context, XudVector2 v0, XudVector2 v1, XudVector2 v2, XudColor color);

/// Draw a filled circle at the screen: raylib's DrawCircle (raylib.h 1306, "Draw a color-filled
/// circle"), the 36-segment fan raylib's own DrawCircleSector walks for a full circle: the centre
/// plus the 36 rim points at 10 degrees apart and the 36 triangles the 108 indices name, the
/// centre and a pair of neighbouring rim points each. The centre and the radius are in the pixel
/// space the rectangle and the line above take, y DOWN from the framebuffer's top-left, and the
/// radius is refused when it is not positive - raylib draws nothing for a radius of zero or less,
/// and the refusal is stated rather than appended as a fan of 37 coincident points. The rim is
/// computed in pixel space at the caller's centre and radius and every one of the 37 points is
/// mapped to clip at the framebuffer's extent the way the 2D shapes above map their points: the
/// extent is checked before it is divided by, and nothing below is. XUD_INVALID_ARG on a NULL
/// context, a radius that is not positive, or a zero framebuffer extent; XUD_UNSUPPORTED on a
/// context with no batch to append into; XUD_OUT_OF_MEMORY when the circle's 37 vertices or its
/// 108 indices do not fit the batch.
XudResult xud_draw_circle(XudContext* context, int32_t centerX, int32_t centerY, float radius, XudColor color);
/// Draw a filled ellipse at the screen: raylib's DrawEllipse (raylib.h 1315, "Draw ellipse"),
/// the 36-segment fan the circle above walks with the two radii raylib's takes: the centre plus
/// the 36 rim points at 10 degrees apart and the 36 triangles the 108 indices name, the centre
/// and a pair of neighbouring rim points each, with the rim's X offset the caller's radiusX and
/// its Y offset the caller's radiusY - the circle above is this fan with the two radii equal.
/// The centre and the two radii are in the pixel space the rectangle and the line above take, y
/// DOWN from the framebuffer's top-left, and a radius is refused when it is not positive -
/// raylib draws nothing for a radius of zero or less, and the refusal is stated rather than
/// appended as a fan of 37 coincident points, the same refusal the circle above makes of its
/// one. The rim is computed in pixel space at the caller's centre and radii and every one of the
/// 37 points is mapped to clip at the framebuffer's extent the way the 2D shapes above map their
/// points: the extent is checked before it is divided by, and nothing below is. XUD_INVALID_ARG
/// on a NULL context, a radius that is not positive, or a zero framebuffer extent;
/// XUD_UNSUPPORTED on a context with no batch to append into; XUD_OUT_OF_MEMORY when the
/// ellipse's 37 vertices or its 108 indices do not fit the batch.
XudResult xud_draw_ellipse(XudContext* context, int32_t centerX, int32_t centerY, float radiusX, float radiusY, XudColor color);

/*-----------------------------------------------------------------------------------------------
 * The font API - raylib's font half, above the font rung
 *
 * This is raylib's font surface (reference/raylib/raylib.h 400-418: Font, GlyphInfo, GetGlyphIndex,
 *   GetGlyphInfo, GetGlyphAtlasRec, LoadFont, LoadFontEx, LoadFontFromImage, MeasureText, MeasureText
 *   Ex, DrawText, DrawTextEx, DrawText3D and the two font-format loaders beside them) above the font
 *   rung: the bake is the font rung's and the atlas it bakes is uploaded by the texture rung below it,
 *   so a caller names a context and a font FILE and never an atlas, a sampler or a VkImage.
 *
 * What the text pair draws through, and what is still NOT here: the glyphs draw through the textured
 *   variant of the pipeline list, and the sampler-array entry they sample with is the atlas's own entry of the
 *   sampler array, resolved from the texture record the atlas is - a record whose atlas occupies no
 *   entry of the array draws through slot 0's white default rather than through a refusal, the same
 *   answer the material rung gives a map it cannot sample. Absent for the reason the font rung's own
 *   note gives: raylib's FONT_SDF (a second atlas format and a second variant reading it), its
 *   LoadFontFromImage and its bitmap/sprite font loaders (file formats stb_truetype does not read at
 *   all), its DrawText3D (a text run whose pen walks a plane other than the screen's, which is a
 *   transform this library's camera does not hand a caller per draw) and its default bitmap font -
 *   which is why this library has no xud_load_font_default() and no default font a context falls back
 *   to.
 *-----------------------------------------------------------------------------------------------*/

/// Load a font this context owns: the object half of the font rung, and the fifth public create pair wh
/// ose record owns driver objects - and the second whose record owns OTHER handles (after the model's, which owns
/// MESH and MATERIAL slots). The description is checked before a slot is spent, so a refusal leaves no half-font; the
/// slot, the record and the rows are then allocated, the atlas is baked at the rung's first attempt and doubled until stb'
/// s own packer accepts it, and the accepted atlas is handed down as one texture description rather than uploaded here - the
/// texture rung below owns the staging arena, the copy, the layout transition, the sampler and the view, and a second upload p
/// ath in this rung would be a second copy of that path. On XUD_OK *outHandle names a live FONT slot of THIS context's table and
/// the caller holds it for as long as anything measures or will draw with it; the atlas it owns is a TEXTURE slot no caller can reac
/// h, and closing it behind the font's back is a mistake the handle layer's kind check turns into a refusal. XUD_INVALID_ARG on a NUL
/// L out-handle, context or description, or on a font the rung's own check refuses (no bytes, bytes shorter than sfnt's own shortest head
/// er, an em height outside this rung's range, a codepoint array named without its bytes, a codepoint outside the admitted range, an array
/// that is not sorted ascending and unique, or a set whose boxes this rung's atlas cannot hold); XUD_UNSUPPORTED on a context that never
/// compl eted bring-up, on a loader whose rows this rung needs are not there, on a device that is not this loader's, on a buffer this rung
/// cannot read as a font (one answer for a buffer that is not a font at all and for one it is but cannot walk), or on a set that fits no
/// atlas this rung will try; XUD_OUT_O F_RESOURCES at the handle layer's own slot bound or when the atlas's block cannot be carved;
/// XUD_OUT_OF_MEMORY when the record, the rows or the atlas cannot be allocated; the texture rung's own code on the atlas it brings up.
XudResult xud_load_font(XudHandle* outHandle, XudContext* context, const XudFontData* font);

/// Release one font: the object half closed, and the second public close pair that closes OTHER caller handles (after the model'
/// s). The handle is validated against the FONT kind first, so a stale handle, a handle of another kind and a fabricated one refuse without
/// freeing anyth ing; the atlas is then closed through the TEXTURE rung below rather than by destroying anything here, because this rung
/// owns no driver object of its own and the wait-idle guard that belongs to the atlas is the texture rung's
/// (VUID-vkDestroyImage-image-00009, the rule the mesh's buffer destroy and the swapchain's recreate state state for their own objects).
/// The slot's record field is cleared BEFORE the record is freed, the rule every close in this tree states, so a close run twice frees once
/// and the table's own teardown cannot find a record it would have to free twice. XUD_INVALID_ARG on a NULL context or on a handle that
/// does not name a live FONT slot of this context; XUD_UNSUPPORTED on a context that never completed bring-up or on a loader whose rows are
/// not this device's; the texture rung's own code when that close refuses.
XudResult xud_close_font(XudContext* context, XudHandle handle);

/// How wide a string is, in pixels, at a requested em height: raylib's MeasureTextEx (raylib.h 415, "me
/// asure text width for selected font"), and the number a centring or a wrapping call needs. The scale is a division by the bake's own
/// height rather than a second bake at the requested size - the same reason raylib's Font carries a baseSize at all, and a second bake
/// would be a second atlas per size. The spacing AFTER the last glyph is INCLUDED, which is raylib's own answer (its measure returns the
/// pen position after the last glyph, which is what a centring call wants) and is stated here because it is the one place this answer
/// differs from a caller's intuition of ink width. A codepoint the set does not cover is measured as the fallback row, so a measure and a
/// draw of one string cannot disagree about where the pen ends. XUD_INVALID_ARG on a NULL out-width, context or text array, on a textCount
/// of zero, or on a handle that does not name a live FONT slot of this context; XUD_UNSUPPORTED on a context that never completed bring-up.
XudResult xud_measure_text(
    float* outWidth, XudContext* context, XudHandle handle, const uint32_t* text, uint32_t textCount, float pixelSize, float spacing);

/// Draw one string: raylib's DrawText (raylib.h 415, "draw text (with default font size spacing)") and
///   DrawTextEx (raylib.h 416, "draw text with custom font size and spacing") above the pen walk, and the
///   first draw in this tree that writes a real uv. The position is the PEN's start rather than the line's
///   top-left: its x is the string's left edge and its y is the baseline every glyph's box hangs from, which
///   is the row's own spelling read literally (offsetY is the pen's step to the box's TOP edge, and a number
///   that named the top-left instead would be a different number at every glyph). The pen then moves by each
///   row's own advance scaled by the requested em height over the bake's own, plus the tracking, after EVERY
///   glyph including the last one - the same step xud_measure_text() sums, which is what makes a measure and a
///   draw of one string agree about where the pen ends, and is why a centring call can ask for a width and get
///   the string it asked for at that width. A codepoint the set does not cover draws the fallback row, the same
///   row the three lookups above answer, and a codepoint with no outline (a space) draws no quad but still moves
///   the pen, which is the answer the rows state rather than an absent row - skipping both would run two words
///   into one. The quads are appended one glyph at a time, so a string that runs out of batch capacity draws the
///   glyphs that fit and refuses at the next glyph boundary, the same shape xud_draw_model() states for a model
///   whose last mesh did not fit. The quads draw through the textured variant of the pipeline list with the set-1
///   slot resolved from this font's own atlas - the texture record the atlas is names its entry of the sampler
///   array, and the walk moves the batch's material state to that row for the walk and back when it is done, so
///   the glyphs flush as one textured draw row of their own and the shapes appended beside them draw with the
///   state the walk found; a record whose atlas occupies no entry of the array draws through slot 0's white
///   default rather than through a refusal. XUD_INVALID_ARG
///   on a NULL context or text array, on a textCount of zero, or on a pixelSize that is not strictly
///   positive (which refuses zero, a negative height and a NaN in one comparison), or on a handle that does not
///   name a live FONT slot of this context; XUD_UNSUPPORTED on a context whose bring-up never completed, which
///   is the state that has no batch to append into; the shapes rung's own codes for each quad it appends; XUD_OUT
///   _OF_MEMORY when one glyph quad does not fit the batch, with the glyphs before it already appended.
XudResult xud_draw_text(
    XudContext* context, XudHandle handle, const uint32_t* text, uint32_t textCount, XudVector3 position, float pixelSize, XudColor color);

/// The same draw with the tracking raylib's DrawTextEx carries: the extra x step added after EVERY glyph
///   including the last one, which is the same step xud_measure_text() takes a spacing for, so a draw and a
///   measure that pass the same tracking cannot disagree about where the pen ends. A negative tracking is not
///   refused: it is letter-spacing tightened, and a string walked that far can have its boxes overlap, which is
///   what kerning looks like from the far side of it - raylib's Font kerning is a per-pair table this rung has
///   no source for, and a caller that wants the look without one asks for the negative step itself. XUD_INVALID_ARG
///   on the same arguments as the pair above, and on nothing else; the same codes for the same reasons.
XudResult xud_draw_text_ex(XudContext* context, XudHandle handle, const uint32_t* text, uint32_t textCount, XudVector3 position,
    float pixelSize, float spacing, XudColor color);

/// The screen-space text: the same glyph walk as the 3D pair above, but the boxes mapped from pixels
/// to clip space at the framebuffer's own extent rather than placed in world space, so the string is
/// screen-fixed - a 3D camera in effect moves the scene around it rather than the string, raylib's
/// DrawTextEx on a 2D canvas and the same UI column the pipeline list gives the screen surface, depth
/// test and write both off. pos is the pen's top-left corner in PIXELS with y DOWN from the
/// framebuffer's top-left (the 3D pair's position is a world point, this one is a screen point), and
/// the string's rows advance in pixels rather than in the pen's own step. The colour is the quad's
/// own vertex colour for the same reason the textured half above states: the row the run records
/// into carries the white push tint the plain record stamps. XUD_INVALID_ARG on a NULL context, a font handle the table does not
/// name, a zero framebuffer extent, or a quad the batch does not fit; XUD_UNSUPPORTED on a context
/// with no batch to append into; XUD_OUT_OF_MEMORY when one glyph quad does not fit the batch, with
/// the glyphs before it already appended.
XudResult xud_draw_text_screen(XudContext* context, XudHandle handle, const uint32_t* text, uint32_t textCount, int32_t posX, int32_t posY,
    float pixelSize, float spacing, XudColor color);

/// Which of a font's rows a codepoint names: raylib's GetGlyphIndex (raylib.h 405), and the fallback it states rather than a
/// refusal - a codepoint the set does not cover answers the row that holds '?', so a caller drawing a string the font cannot render gets
/// boxes it can still measure rather than an error per character. The index is this library's rather than raylib's glyph-array offset,
/// because a font is a slot of this table rather than a struct with an array inside it, and the index is what the two queries below take.
/// XUD_INVALID_ARG on a NULL out-index or context, or on a handle that does not name a live FONT slot of this context; XUD_UNSUPPORTED on a
/// context that never completed bring-up.
XudResult xud_get_glyph_index(uint32_t* outIndex, XudContext* context, XudHandle handle, uint32_t codepoint);

/// One glyph's pen metrics: raylib's GetGlyphInfo (raylib.h 406, "get advanced glyph info for codepoint"), the same four numbers
/// in the bake's own pixel space - which is why the bake's height is part of every query that turns them into a position. The lookup above
/// runs first, so the codepoint in hand is a codepoint and not an index, and a codepoint the set does not cover answers the fallback row
/// rather than a refusal. XUD_INVALID_ARG on a NULL out-info or context, or on a handle that does not name a live FONT slot of this
/// context; XUD_UNSUPPORTED on a context that never completed bring-up.
XudResult xud_get_glyph_info(XudGlyphInfo* outInfo, XudContext* context, XudHandle handle, uint32_t codepoint);

/// One glyph's box in the atlas, in texels rather than in texture coordinates: raylib's GetGlyphAtlasRec (raylib.h 407, "get re
/// ctangle in texture atlas for codepoint"), whose answer is a Rectangle in texels. The divide that turns it into the [0,1] a sampler wants
/// is a divide by an extent both the caller and this rung can read, so the rung that states the box states it in texels and the rung that
/// draws converts. A width of 0 is a codepoint with no outline, a space, and is the answer rather than an absent row. XUD_INVALID_ARG on a
/// NULL out-rec or context, or on a handle that does not name a live FONT slot of this context; XUD_UNSUPPORTED on a context that never
/// completed bring-up.
XudResult xud_get_glyph_atlas_rec(XudGlyphAtlasRec* outRec, XudContext* context, XudHandle handle, uint32_t codepoint);

/// The atlas extent behind a font, in texels: the public half of the extent the record keeps and the pen walk divides by, and the number a caller
/// that wants the [0,1] a sampler wants from the public rec above does its own divide by. The atlas is square by construction, so the two answers
/// are one number read as a pair, and they are stated as a pair because the record states it as one. XUD_INVALID_ARG on a NULL context or on either
/// out-param, or on a handle that does not name a live FONT slot of this context; XUD_UNSUPPORTED on a context that never completed bring-up.
XudResult xud_get_font_atlas_dim(XudContext* context, XudHandle handle, uint32_t* outWidth, uint32_t* outHeight);

/*-----------------------------------------------------------------------------------------------
 * The audio API - raylib's LoadAudioStream/GetAudioLength half, above the audio rung
 *
 * This is raylib's audio surface (reference/raylib/raylib.h: LoadAudioStream, GetAudioLength)
 * above the audio rung: miniaudio's decoder over one host PCM block, and the AUDIOSTREAM-kind
 * handle that names it. What is here is everything a play row needs beside the device itself -
 * the frames, the frame count, the channel count and the rate, plus the two conversions between
 * frames and seconds - and the play half is the section below this one, above the sound rung, wh
 * ich landed as the second audio rung the audio rung's own note said was missing. The containers
 * this vendored miniaudio has no decoder for (qoa, xm) answer
 * XUD_UNSUPPORTED rather than a half-decoded stream, and there is no per-format loader name here
 * for the reason the value type's own note gives.
 *-----------------------------------------------------------------------------------------------*/

/// Decode one container's bytes into the frames behind an AUDIOSTREAM-kind handle: raylib's LoadAudioStream
/// (raylib.h 505, "load audio stream from file"), except that the bytes are a caller's rather than a path
/// this rung opens. The whole container is decoded into one block, which is the shape raylib's own note
/// that a stream is "already decoded" states. XUD_INVALID_ARG on a NULL out-handle, context or audio
/// description, or on a description this rung's own check refuses (no bytes, a fragment shorter than the
/// shortest header the decoder sniffs, a claimed length past this rung's container bound); XUD_UNSUPPORTED
/// on a context that never completed bring-up or on a buffer the decoder cannot read as a container;
/// XUD_OUT_OF_MEMORY when the record or the block cannot be spent; XUD_INTERNAL_ERROR on the handle
/// layer's own slot bound. On every failure *outHandle names nothing and nothing was spent.
XudResult xud_load_audio(XudHandle* outHandle, XudContext* context, const XudAudioData* audio);

/// Release one audio stream: its block and its record go back and its slot is released. XUD_INVALID_ARG on a
/// NULL context or on a handle that does not name a live AUDIOSTREAM slot of this context - a stale handle,
/// a handle of another kind and a fabricated one all refuse before anything is freed.
XudResult xud_close_audio(XudContext* context, XudHandle handle);

/// A stream's channel count, its rate and its frame count: the three numbers behind the handle, copied into
/// the caller's own XudAudioInfo rather than handed out as a pointer into the slot. XUD_INVALID_ARG on a NULL
/// out-info or context, or on a handle that does not name a live AUDIOSTREAM slot of this context; XUD_UNSUPPORTED
/// on a context that never completed bring-up.
XudResult xud_get_audio_info(XudAudioInfo* outInfo, XudContext* context, XudHandle handle);

/// A stream's length in seconds: raylib's GetAudioLength (raylib.h 506, "get audio stream length in seconds"),
/// computed as the frame count divided by the rate rather than stored beside the load, which is what makes it
/// impossible for the length and the count to disagree. XUD_INVALID_ARG on a NULL out-seconds or context, or on
/// a handle that does not name a live AUDIOSTREAM slot of this context; XUD_UNSUPPORTED on a context that never
/// completed bring-up.
XudResult xud_get_audio_length(float* outSeconds, XudContext* context, XudHandle handle);

/*-----------------------------------------------------------------------------------------------
 * The audio playback API - raylib's PlayAudio/StopAudio/PauseAudioStream/ResumeAudioStream half, above the sound rung
 *
 * This is raylib's other audio half (reference/raylib/raylib.h: PlayAudio, StopAudio, PauseAudioStream, ResumeAudioStream) above the
 * sound rung: miniaudio's engine - its ma_device, its node graph and its mixer - with one node pa
 * ir per AUDIOSTREAM slot attached to that graph, reading the block the audio rung below already
 * decoded. The four here are keyed by the stream rather than by a sound index a caller has to reme
 * mber, which is the one place this surface diverges from raylib's: PlayAudio answers an index and
 * StopAudio(stream, sound) takes it back, and in this repo the thing a caller holds is a handle wi
 * th a generation in it, so the stream's handle is the identity of its row and a stream played twi
 * ce reuses the one node rather than spending a second one. SetAudioFootprint is not here: it is a
 * spatializer question above this rung rather than a device question inside it, and every node her
 * e is initialized NO_SPATIALIZATION for that reason. The rung is spent on the first play rather th
 * an at bring-up, because an audio device is a driver resource a context that only ever draws never
 * asks for.
 *-----------------------------------------------------------------------------------------------*/

/// Start one stream's node: raylib's PlayAudio (raylib.h 507, "play audio stream from an internal
/// AUDIOSTREAM handle"), except that the row is the stream's rather than a fresh sound per call. The
/// node is spent on the first play of the slot and reused after that, and it reads the block the aud
/// io rung owns by reference rather than by copy. A play on a row that had reached the end restarts a
/// t frame 0; a play on a row stopped or held mid-way resumes where it stopped or the hold left it,
/// which is miniaudio's own note that a stopped sound is not rewound. XUD_INVALID_ARG on a NULL
/// context or on a handle that does not name a live AUDIOSTREAM slot of this context; XUD_UNSUPPORTED
/// on a context that never completed bring-up, on a stream whose block has no frames to read, or when
/// no audio backend this vendored miniaudio can drive is present; XUD_ALREADY when the row is already
/// playing; XUD_OUT_OF_MEMORY when the row or the node cannot be spent.
XudResult xud_play_audio(XudContext* context, XudHandle stream);

/// Stop one stream's node: raylib's StopAudio (raylib.h 508, "stop playing audio stream"), with th
/// e stream's handle as the row's identity rather than a sound index. The node stays attached, so the
/// next play of the same slot costs a state change rather than a new node, and the frames go back to th
/// e audio record unchanged. XUD_INVALID_ARG on a NULL context or on a handle that does not name a liv
/// e AUDIOSTREAM slot of this context; XUD_UNSUPPORTED on a context that never completed bring-up; XUD_
/// ALREADY when the slot never had a node or its node is already stopped.
XudResult xud_stop_audio(XudContext* context, XudHandle stream);

/// Hold one stream's node where it is: raylib's PauseAudioStream (raylib.h 745) with the stream's
/// handle as the row's identity rather than a sound index. The stop the hold makes leaves the cursor
/// where the node left it, so the resume below takes it from there rather than from frame 0, and the
/// row itself says held rather than stopped, which is the difference from the stop above: the node
/// stays attached either way and the next play takes the cursor from where it left it either way, but
/// only the held row answers the resume. XUD_INVALID_ARG on a NULL context or on a handle that does
/// not name a live AUDIOSTREAM slot of this context; XUD_UNSUPPORTED on a context that never completed
/// bring-up; XUD_ALREADY when the slot never had a node, its node is not reading, or the row is already
/// held.
XudResult xud_pause_audio(XudContext* context, XudHandle stream);

/// Release one held stream's node: raylib's ResumeAudioStream (raylib.h 746) with the stream's handle
/// as the row's identity rather than a sound index. The node stays attached, so the resume costs a
/// state change on it, and the cursor takes from where the pause left it, so the frames that were
/// never read are the ones still to come. A row that was stopped rather than held is the play call's
/// own business, so it answers ALREADY here and the row stays where it is. XUD_INVALID_ARG on a NULL
/// context or on a handle that does not name a live AUDIOSTREAM slot of this context; XUD_UNSUPPORTED
/// on a context that never completed bring-up; XUD_ALREADY when the slot never had a node, its node is
/// stopped, or the row is already playing.
XudResult xud_resume_audio(XudContext* context, XudHandle stream);

/// Is the mixer reading this stream right now: the one question a row answers, read off this rung's o
/// wn bookkeeping after settling it against the stream's node, rather than off miniaudio's clock, so th
/// e answer is a state this library owns and keeps true - a stream that ended on the audio thread settle
/// s to stopped - and not a race against the thread that pulls frames. XUD_INVALID_ARG on a NULL out-playi
/// ng, context, or on a handle that does not name a live AUDIOSTREAM slot of this context; XUD_UNSUPPORTED o
/// n a context that never completed bring-up; XUD_OK with false on a stream that was never played or that e
/// nded, which is the answer rather than an error.
XudResult xud_is_audio_playing(bool* outPlaying, XudContext* context, XudHandle stream);

/// Set the level a stream plays at: raylib's SetAudioStreamVolume (raylib.h 1748, "1.0 is max
/// level"), 0.0 the silence and 1.0 the block's own peak. The value is kept in the stream's record,
/// which is where a setter that runs before the stream's first play parks it: the node the first
/// play builds takes the record's value, so the level is right from the first frame the stream is
/// heard, and a setter on a stream that is playing right now takes effect on the node from the next
/// frame the engine's own thread pulls - miniaudio's volume write is atomic on the engine side, so
/// there is no frame the caller can see at the old level and no fence to wait for it. A level
/// outside the range is a refusal rather than a clamp: the record's own row is the state a later
/// xud_get_audio_volume() answers, and a clamped one would answer a value the caller never asked
/// for. XUD_INVALID_ARG on a NULL context, on a level outside [0.0, 1.0], or on a handle that does
/// not name a live AUDIOSTREAM slot of this context; XUD_UNSUPPORTED on a context that never
/// completed bring-up.
XudResult xud_set_audio_volume(XudContext* context, XudHandle handle, float volume);

/// The level the volume setter last asked - the record's own row, the default 1.0 included: the
/// state a setter parked before the stream's first play answers back, and the state a play that ran
/// without one left behind. XUD_INVALID_ARG on a NULL out-volume or context, or on a handle that
/// does not name a live AUDIOSTREAM slot of this context; XUD_UNSUPPORTED on a context that never
/// completed bring-up.
XudResult xud_get_audio_volume(float* outVolume, XudContext* context, XudHandle handle);

/// Set the rate a stream plays at: raylib's SetAudioStreamPitch (raylib.h 1749, "1.0 is base
/// level"), 1.0 the tempo the block was decoded at and values above it the block resampled faster.
/// The value is kept in the stream's record, for the reason the volume setter states - a setter
/// that runs before the stream's first play parks where the node the first play builds takes it
/// from, and a setter on a stream that is playing right now takes effect from the next frame the
/// engine's thread pulls, the write being atomic on the engine side. A non-positive rate is a
/// refusal rather than a no-op: the engine would take one and do nothing with it, leaving the
/// record and a live row naming different pitches, which is the state a read-back must not answer.
/// XUD_INVALID_ARG on a NULL context, on a non-positive rate, or on a handle that does not name a
/// live AUDIOSTREAM slot of this context; XUD_UNSUPPORTED on a context that never completed bring-up.
XudResult xud_set_audio_pitch(XudContext* context, XudHandle handle, float pitch);

/// The rate the pitch setter last asked - the record's own row, the default 1.0 included: the
/// state a setter parked before the stream's first play answers back, and the state a play that ran
/// without one left behind. XUD_INVALID_ARG on a NULL out-pitch or context, or on a handle that
/// does not name a live AUDIOSTREAM slot of this context; XUD_UNSUPPORTED on a context that never
/// completed bring-up.
XudResult xud_get_audio_pitch(float* outPitch, XudContext* context, XudHandle handle);

/// Set the position a stream plays at: raylib's SetAudioStreamPan (raylib.h 1750, "-1.0 left,
/// 0.0 center, 1.0 right"), -1.0 the full left and 1.0 the full right with 0.0 the centre. The
/// value is kept in the stream's record, for the reason the volume setter states - a setter that
/// runs before the stream's first play parks where the node the first play builds takes it from,
/// and a setter on a stream that is playing right now takes effect on the node from the next
/// frame the engine's own thread pulls. The position pans mono and stereo blocks alike: the
/// engine's panner runs after the row's spatialization, which the row's sound was built without.
/// A position outside the range is a refusal rather than a clamp: the record's own row is the
/// state a later xud_get_audio_pan() answers, and a clamped one would answer a value the caller
/// never asked for. XUD_INVALID_ARG on a NULL context, on a position outside [-1.0, 1.0], or on a
/// handle that does not name a live AUDIOSTREAM slot of this context; XUD_UNSUPPORTED on a
/// context that never completed bring-up.
XudResult xud_set_audio_pan(XudContext* context, XudHandle handle, float pan);

/// The position the pan setter last asked - the record's own row, the default 0.0 included: the
/// state a setter parked before the stream's first play answers back, and the state a play that
/// ran without one left behind. XUD_INVALID_ARG on a NULL out-pan or context, or on a handle that
/// does not name a live AUDIOSTREAM slot of this context; XUD_UNSUPPORTED on a context that never
/// completed bring-up.
XudResult xud_get_audio_pan(float* outPan, XudContext* context, XudHandle handle);

/*-----------------------------------------------------------------------------------------------
 * The positional audio API - raylib's SetSoundPosition/AttachAudioListener half, above the sound rung
 *
 * raylib's spatial half (reference/raylib/examples/audio/audio_sound_positioning.c: SetSoundPosition, SetListenerPosition, SetListenerVelocity,
 * AttachAudioListener) reshaped for a surface that hands out handles over a context rather than Sound structs with a listener attached to each:
 * ONE listener per audio device - where the ear is and which way it looks - and ONE source position per handle. The listener is context-level
 * state rather than a row's, so the frame that moves the camera calls xud_set_audio_listener_position() ONCE and every placed handle follows it,
 * which is the one thing the example above needs. The two velocities raylib stores and never reads are absent, with the Doppler they would have
 * driven: this rung has no Doppler path, and a vector that is stored and never read is a promise the library cannot keep.
 *
 * The model, one half at a time. A handle whose source position was NEVER set is NON-spatial: xud_set_audio_volume() and xud_set_audio_pan() are
 * the whole story of what its node hears, exactly as they were before this section existed. A handle whose position WAS set is SPATIAL: those two
 * setters keep recording what the caller asked - and that BASE is what xud_get_audio_volume() and xud_get_audio_pan() answer, the caller's own ask
 * rather than a computed value the caller never named - while the node hears the value the geometry produces, recomputed inside these setters when
 * a source moves and when the ear moves or turns, not on the mixer's thread:
 *
 *   dir    = (source - listener) / d, the zero vector at d = 0, where there is no direction to pan by
 *   right  = normalize(cross(forward, {0,1,0}))
 *   volume = clamp(baseVolume / max(d, 1.0), 0.0, 1.0) - the linear rolloff, the BASE at and inside a unit's distance rather than an infinity
 *   volume = volume * (1 + 0.5 * dot(forward, dir))   - only when that dot is negative: behind the ear is quieter than beside it
 *   pan    = clamp(basePan + dot(dir, right), -1.0, 1.0)
 *
 * The listener's right is DERIVED rather than settable, and a forward parallel to the world's up is refused for the reason the derived right gives:
 * straight up is a facing with no left or right. The listener defaults to the world origin looking down +z, and lives on the audio device - so all
 * four listener calls answer XUD_UNSUPPORTED on a context that never played anything, which is the rung below that is not there rather than a bad
 * argument. The source pair answers through the stream's record alone and so works before the first play: a placed handle played for the first time
 * is placed from its first frame.
 *-----------------------------------------------------------------------------------------------*/

/// Place one stream's source in the world, which is what makes it SPATIAL: from here on the level and the position the two setters recorded are the
/// BASE the geometry is measured against, and the node hears the value the rolloff and the pan below produce instead. There is no un-place - a source
/// position is the whole of what makes a handle spatial, so a stream stays spatial for its own whole life. XUD_INVALID_ARG on a NULL context, on a non-finite
/// component of the position, or on a handle that does not name a live AUDIOSTREAM slot of this context; XUD_UNSUPPORTED on a context that never
/// completed bring-up.
XudResult xud_set_audio_source_position(XudContext* context, XudHandle handle, XudVector3 position);

/// The source position the setter last asked - the world origin for a stream that was never placed, which is the answer that goes with the
/// non-spatial state rather than a flag to ask about beside it. XUD_INVALID_ARG on a NULL out-position or context, or on a handle that does not
/// name a live AUDIOSTREAM slot of this context; XUD_UNSUPPORTED on a context that never completed bring-up.
XudResult xud_get_audio_source_position(XudVector3* outPosition, XudContext* context, XudHandle handle);

/// Move the ear: raylib's SetListenerPosition, one call per frame for a caller that moved its camera, and the reason every placed handle of this
/// context follows it - every spatial row is rewritten before this call answers. The listener is the audio device's own state, so a context that
/// never played anything has no ear to move. XUD_INVALID_ARG on a NULL context or a non-finite component; XUD_UNSUPPORTED on a context that never
/// completed bring-up or that never spent an audio device.
XudResult xud_set_audio_listener_position(XudContext* context, XudVector3 position);

/// Where the ear is - the world origin for a device that was never moved. XUD_INVALID_ARG on a NULL out-position or context; XUD_UNSUPPORTED on a
/// context that never completed bring-up or that never spent an audio device.
XudResult xud_get_audio_listener_position(XudVector3* outPosition, XudContext* context);

/// Turn the ear: the facing the pan is measured against, stored NORMALISED so the read-back answers the unit rather than the caller's own bytes.
/// A facing of no length and a facing parallel to the world's up (straight up or straight down, which is a facing with no left or right) are both
/// refused rather than normalised into a divide by zero. Every spatial row is rewritten before this call answers. XUD_INVALID_ARG on a NULL
/// context, a non-finite component, a zero-length facing or a facing with no right; XUD_UNSUPPORTED on a context that never completed bring-up or
/// that never spent an audio device.
XudResult xud_set_audio_listener_forward(XudContext* context, XudVector3 forward);

/// Which way the ear looks - the unit +z for a device that was never turned. XUD_INVALID_ARG on a NULL out-forward or context; XUD_UNSUPPORTED on
/// a context that never completed bring-up or that never spent an audio device.
XudResult xud_get_audio_listener_forward(XudVector3* outForward, XudContext* context);

/*-----------------------------------------------------------------------------------------------
 * The clock API - raylib's time half, above the clock rung
 *
 * This is raylib's time surface (reference/raylib/raylib.h 1094-1104: GetFrameTime, GetTime, GetFPS
 * and WaitTime) above the clock rung: GLFW's own timer (src/external/glfw/src/input.c 1487-1517, on
 * e process-level offset above one platform tick counter) read through the context, so a caller nam
 * es a context and never a GLFWwindow*. The two queries above the rung's records are raylib's two g
 * et-the-clock answers - seconds since THIS context came up, and the span of the frame that complete
 * d most recently - and the two below them are GLFW's own two remaining timer names passed through u
 * nchanged, for a caller that wants ticks rather than seconds.
 *
 * What is NOT here, and why: raylib's SetTargetFPS and GetFPS (a cap and a statistic over the frame
 * loop - the cap would make xud_end_drawing() sleep, which is a decision about the frame loop rather
 *  than a question about the clock, and the statistic is a count over that same loop) and raylib's Wa
 * itTime (a sleep, a caller's decision to block rather than a question to answer). GLFW's own glfwSe
 * tTime is absent for the reason the clock rung's own note gives: it moves GLFW's process-level offs
 * et, which would move every context's origin at once.
 *-----------------------------------------------------------------------------------------------*/

/// Seconds since THIS context came up - raylib's GetTime (raylib.h 1095), "elapsed time in seconds
/// since InitWindow()", read against the tick the clock rung recorded at its own bring-up rather th
/// an against GLFW's process-level offset, which is a per-process question and would be one clock fo
/// r every context in the process rather than one per context (see the clock rung's own note). Live
/// rather than per-frame: two calls in one frame answer two numbers, and the one that does not move
/// across a frame is xud_get_frame_time() below. XUD_INVALID_ARG on a NULL out-seconds or context; XU
/// D_UNSUPPORTED on a context that never completed bring-up.
XudResult xud_get_time(double* outSeconds, XudContext* context);

/// Seconds the frame that completed most recently took - raylib's GetFrameTime (raylib.h 1094), "time
///  in seconds for last frame drawn (delta time)", and the number a simulation steps by. The span is
///  between two xud_end_drawing() boundaries, so it covers the whole boundary: the draw, the input po
/// ll and any swapchain rebuild that boundary ran. 0.0 before the second boundary, because there is
/// no completed frame to answer about and zero is what makes a first frame's step zero rather than ga
/// rbage. XUD_INVALID_ARG on a NULL out-seconds or context; XUD_UNSUPPORTED on a context that never c
/// ompleted bring-up.
XudResult xud_get_frame_time(float* outSeconds, XudContext* context);

/// Frames a second the last frame ran at - raylib's GetFPS (raylib.h 1096), the number its DrawFPS
/// prints. The one difference from raylib is the smoothing: raylib keeps a ring of thirty samples
/// taken no closer than 1/60th of a second apart and inverts their average (rcore.c 1591-1620), where
/// this inverts xud_get_frame_time() above, so the answer is the frame that just was rather than the
/// last half second of them, and no state is added to the clock to hold it. 0.0 before the second
/// boundary, where there is no completed frame to take a rate of. XUD_INVALID_ARG on a NULL
/// out-frames-per-second or context; XUD_UNSUPPORTED on a context that never completed bring-up.
XudResult xud_get_fps(float* outFramesPerSecond, XudContext* context);

/// GLFW's raw tick counter, passed through - glfwGetTimerValue (input.c 1508), the same number the t
/// wo queries above divide. A caller that wants ticks rather than seconds wants these two: a double s
/// econds answer rounds past 2^53 ticks, a uint64 tick difference does not. XUD_INVALID_ARG on a NULL
///  out-ticks or context; XUD_UNSUPPORTED on a context that never completed bring-up.
XudResult xud_get_timer_value(uint64_t* outTicks, XudContext* context);

/// GLFW's ticks per second, passed through - glfwGetTimerFrequency (input.c 1514), the divisor the t
/// wo queries above use. On this vendored tree's POSIX timer it is 1e9 (posix_time.c 46-50), and it i
/// s read rather than assumed for that reason. XUD_INVALID_ARG on a NULL out-ticks-per-second or con
/// text; XUD_UNSUPPORTED on a context that never completed bring-up.
XudResult xud_get_timer_frequency(uint64_t* outTicksPerSecond, XudContext* context);

/*-----------------------------------------------------------------------------------------------
 * The context's own generator - raylib's GetRandomValue / SetRandomSeed
 *-----------------------------------------------------------------------------------------------*/

#define XUD_RANDOM_DEFAULT_SEED                                                                                                            \
    0x9E3779B9u /* what every context's generator starts on: the golden-ratio
                                               constant, nonzero (zero is xorshift32's fixed point) and
                                               the same number in every run, so a scene built from
                                               xud_get_random_value() is the same scene tomorrow. */

/// Draw one pseudo-random value in the inclusive range [minValue, maxValue] - raylib's GetRandom
/// Value (raylib.h 897), including raylib's rule that a range handed the wrong way round is swapped
/// rather than refused (rcore.c: "Swap minValue and maxValue"). The generator is this context's own
/// and it starts at the same seed every run, so two runs of the same program lay out the same scene
/// unless xud_set_random_seed() says otherwise - the repo's determinism rule, which is why the seed
/// is a constant here rather than the clock raylib seeds from. XUD_INVALID_ARG on a NULL out-value
/// or context.
XudResult xud_get_random_value(int* outValue, XudContext* context, int minValue, int maxValue);

/// Replace the context's generator state: raylib's SetRandomSeed (raylib.h 896), with the difference
/// above stated. A seed of zero is refused rather than stored, because zero is this generator's
/// fixed point; the default seed a fresh context starts on is XUD_RANDOM_DEFAULT_SEED. XUD_INVALID_ARG
/// on a NULL context; XUD_ALREADY on a context already holding the seed asked for.
XudResult xud_set_random_seed(XudContext* context, uint32_t seed);

/*-----------------------------------------------------------------------------------------------
 * The input API - raylib's keyboard and mouse half, above the input rung
 *
 * This is raylib's input surface (reference/raylib/raylib.h 1202-1241: IsKeyPressed, IsKeyPressed
 * Repeat, IsKeyDown, IsKeyReleased, IsKeyUp, GetKeyPressed, GetCharPressed, GetKeyName
 * and the mouse block below it) above the input rung: the state machine is xud_input.h's and the
 * window it reads is the context's, so a caller names a key or a button and never a GLFWwindow*.
 * The tokens below are raylib's own enum values (raylib.h 587-720) mirrored as macros because they
 * are the numbering this rung's state rows are indexed by - xud_input.h static_asserts the mirror
 * against GLFW's own tokens, so the public mirror, GLFW's state array and this rung's rows cannot
 * drift apart.
 *
 * The state a query answers about is the state as of the poll that OPENED this frame: xud_end_dra
 * wing() runs one glfwPollEvents() per frame, which is raylib's own order for the frame pair (ray
 * lib.h 1100: "By default EndDrawing() does this job: draws everything + SwapScreenBuffer() + mana
 * ge frame timing + PollInputEvents()"). Nothing here polls on demand, so a query between two polls
 * reads the same records rather than a fresh window state, and a press a caller cannot see yet is
 * a press in a frame that has not ended yet. The lag is one frame for the window's own events too:
 * the window manager's close flag and resize events are read by the SAME poll, so
 * xud_window_should_close() answers the close state as of the poll that ended the LAST frame, and
 * a window closed or minimized mid-frame is seen on the next one. That is raylib's WindowShould
 * Close's own lag for its exit key, and it is a property of the poll living at the frame boundary
 * rather than a defect - the boundary is the one place two queries in one frame read the same
 * state.
 *
 * What is NOT here, and why: raylib's gamepad block (GLFW's joystick/gamepad rung is a separate
 * subsystem - a device with its own lifetime, its own queries and its own mapping table, and noth
 * ing in this tree opens one) and its touch block (GLFW's Wayland backend registers no touch listen
 * er at all, so there is no touch state above this window to read), and raylib's SetMouseCursor (a
 * GLFWcursor this rung would have to own and destroy, and GLFW's own table calls four of the ten
 * standard shapes only "Maybe" on a Wayland theme).
 *-----------------------------------------------------------------------------------------------*/

/* raylib's KeyboardKey enum (reference/raylib/raylib.h 587-704), one name per token with raylib's
   own values - which are GLFW's, and the assert in xud_input.h is what pins that. XUD_KEY_NULL is
   the "no key" the queue queries answer with, not a key to query: GLFW's key state starts at the
   printable range, so the queries below refuse a token below XUD_KEY_SPACE as XUD_INVALID_ARG rat
   her than answering "up" for a key that does not exist. The four tokens raylib carries below that
   range (KEY_BACK, KEY_MENU, KEY_VOLUME_UP, KEY_VOLUME_DOWN) name Android's keys rather than keys
   GLFW's Wayland window has state for, so they are not mirrored here.
*/
#define XUD_KEY_NULL 0
#define XUD_KEY_APOSTROPHE 39
#define XUD_KEY_COMMA 44
#define XUD_KEY_MINUS 45
#define XUD_KEY_PERIOD 46
#define XUD_KEY_SLASH 47
#define XUD_KEY_ZERO 48
#define XUD_KEY_ONE 49
#define XUD_KEY_TWO 50
#define XUD_KEY_THREE 51
#define XUD_KEY_FOUR 52
#define XUD_KEY_FIVE 53
#define XUD_KEY_SIX 54
#define XUD_KEY_SEVEN 55
#define XUD_KEY_EIGHT 56
#define XUD_KEY_NINE 57
#define XUD_KEY_SEMICOLON 59
#define XUD_KEY_EQUAL 61
#define XUD_KEY_A 65
#define XUD_KEY_B 66
#define XUD_KEY_C 67
#define XUD_KEY_D 68
#define XUD_KEY_E 69
#define XUD_KEY_F 70
#define XUD_KEY_G 71
#define XUD_KEY_H 72
#define XUD_KEY_I 73
#define XUD_KEY_J 74
#define XUD_KEY_K 75
#define XUD_KEY_L 76
#define XUD_KEY_M 77
#define XUD_KEY_N 78
#define XUD_KEY_O 79
#define XUD_KEY_P 80
#define XUD_KEY_Q 81
#define XUD_KEY_R 82
#define XUD_KEY_S 83
#define XUD_KEY_T 84
#define XUD_KEY_U 85
#define XUD_KEY_V 86
#define XUD_KEY_W 87
#define XUD_KEY_X 88
#define XUD_KEY_Y 89
#define XUD_KEY_Z 90
#define XUD_KEY_LEFT_BRACKET 91
#define XUD_KEY_BACKSLASH 92
#define XUD_KEY_RIGHT_BRACKET 93
#define XUD_KEY_GRAVE 96
#define XUD_KEY_SPACE 32
#define XUD_KEY_ESCAPE 256
#define XUD_KEY_ENTER 257
#define XUD_KEY_TAB 258
#define XUD_KEY_BACKSPACE 259
#define XUD_KEY_INSERT 260
#define XUD_KEY_DELETE 261
#define XUD_KEY_RIGHT 262
#define XUD_KEY_LEFT 263
#define XUD_KEY_DOWN 264
#define XUD_KEY_UP 265
#define XUD_KEY_PAGE_UP 266
#define XUD_KEY_PAGE_DOWN 267
#define XUD_KEY_HOME 268
#define XUD_KEY_END 269
#define XUD_KEY_CAPS_LOCK 280
#define XUD_KEY_SCROLL_LOCK 281
#define XUD_KEY_NUM_LOCK 282
#define XUD_KEY_PRINT_SCREEN 283
#define XUD_KEY_PAUSE 284
#define XUD_KEY_F1 290
#define XUD_KEY_F2 291
#define XUD_KEY_F3 292
#define XUD_KEY_F4 293
#define XUD_KEY_F5 294
#define XUD_KEY_F6 295
#define XUD_KEY_F7 296
#define XUD_KEY_F8 297
#define XUD_KEY_F9 298
#define XUD_KEY_F10 299
#define XUD_KEY_F11 300
#define XUD_KEY_F12 301
#define XUD_KEY_LEFT_SHIFT 340
#define XUD_KEY_LEFT_CONTROL 341
#define XUD_KEY_LEFT_ALT 342
#define XUD_KEY_LEFT_SUPER 343
#define XUD_KEY_RIGHT_SHIFT 344
#define XUD_KEY_RIGHT_CONTROL 345
#define XUD_KEY_RIGHT_ALT 346
#define XUD_KEY_RIGHT_SUPER 347
#define XUD_KEY_KB_MENU 348
#define XUD_KEY_KP_0 320
#define XUD_KEY_KP_1 321
#define XUD_KEY_KP_2 322
#define XUD_KEY_KP_3 323
#define XUD_KEY_KP_4 324
#define XUD_KEY_KP_5 325
#define XUD_KEY_KP_6 326
#define XUD_KEY_KP_7 327
#define XUD_KEY_KP_8 328
#define XUD_KEY_KP_9 329
#define XUD_KEY_KP_DECIMAL 330
#define XUD_KEY_KP_DIVIDE 331
#define XUD_KEY_KP_MULTIPLY 332
#define XUD_KEY_KP_SUBTRACT 333
#define XUD_KEY_KP_ADD 334
#define XUD_KEY_KP_ENTER 335
#define XUD_KEY_KP_EQUAL 336

/* raylib's MouseButton enum (reference/raylib/raylib.h 711-720) - the same seven buttons GLFW's
   mouse-button state array is indexed by, and the assert in xud_input.h is what pins that too.
*/
#define XUD_MOUSE_BUTTON_LEFT 0
#define XUD_MOUSE_BUTTON_RIGHT 1
#define XUD_MOUSE_BUTTON_MIDDLE 2
#define XUD_MOUSE_BUTTON_SIDE 3
#define XUD_MOUSE_BUTTON_EXTRA 4
#define XUD_MOUSE_BUTTON_FORWARD 5
#define XUD_MOUSE_BUTTON_BACK 6

/// True if the key transitioned from not held to held at the poll that opened this frame - raylib's
/// IsKeyPressed (raylib.h 1202), "pressed once": a key held across three frames answers true at the
/// first of them and false at the two below it. XUD_INVALID_ARG on a NULL out-param, a NULL context
/// or a key outside GLFW's queryable range (XUD_KEY_NULL included - it names "no key"); XUD_UNSUPP
/// ORTED on a context whose input rung never came up.
XudResult xud_is_key_pressed(bool* outPressed, XudContext* context, int key);

/// True if the key is held AND was already held at the poll below this one - raylib's IsKeyPressed
/// Repeat (raylib.h 1203), which is how a caller tells a held key that is repeating (a key held
/// down long enough that the window system is re-announcing it) from one that was pressed at this
/// frame. XUD_INVALID_ARG as above; XUD_UNSUPPORTED as above.
XudResult xud_is_key_pressed_repeat(bool* outRepeat, XudContext* context, int key);

/// True if the key is held at the poll that opened this frame, however it got there - raylib's IsKe
/// yDown (raylib.h 1204), the one query of this family that is not about a transition, which is wh
/// at a held-down-while-moving control reads through. XUD_INVALID_ARG as above; XUD_UNSUPPORTED as
/// above.
XudResult xud_is_key_down(bool* outDown, XudContext* context, int key);

/// True if the key transitioned from held to not held at the poll that opened this frame - raylib's
/// IsKeyReleased (raylib.h 1205), "released once", the mirror of xud_is_key_pressed. XUD_INVALID_AR
/// G as above; XUD_UNSUPPORTED as above.
XudResult xud_is_key_released(bool* outReleased, XudContext* context, int key);

/// True if the key is not held at the poll that opened this frame - raylib's IsKeyUp (raylib.h 1206
/// ), "NOT being pressed". XUD_INVALID_ARG as above; XUD_UNSUPPORTED as above.
XudResult xud_is_key_up(bool* outUp, XudContext* context, int key);

/// The next key that transitioned into held at the poll that opened this frame, in the order the po
/// ll's scan found them, and 0 when the queue is drained - raylib's GetKeyPressed (raylib.h 1207),
/// "call it multiple times for keys queued, returns 0 when the queue is empty". The queue's bound i
/// s the key count and the proof beside it is why no press can be dropped from it. XUD_INVALID_ARG
/// on a NULL out-param or a NULL context; XUD_UNSUPPORTED as above.
XudResult xud_get_key_pressed(int* outKey, XudContext* context);

/// The next codepoint the window's keyboard typed since the poll below this one, and 0 when the que
/// ue is drained - raylib's GetCharPressed (raylib.h 1208), and the only way a caller sees a charac
/// ter at all on this backend, because a codepoint is only ever delivered through GLFW's char call
/// back (wl_window.c 1372). XUD_OUT_OF_RESOURCES once, after the queue is drained, when more codepo
/// intes arrived between two polls than the queue holds: the list was cut, and saying so is what ma
/// kes it a report rather than a silent truncation. XUD_INVALID_ARG on a NULL out-param or a NULL c
/// ontext; XUD_UNSUPPORTED as above.
XudResult xud_get_char_pressed(int* outChar, XudContext* context);

/// The name of a key on the CURRENT keyboard layout rather than on the layout the tokens above name
/// d - raylib's GetKeyName (raylib.h 1209), "returns string 'q' for KEY_A on an AZERTY keyboard",
/// which is what lets a caller bind an action to whatever key types the letter it wants. The point
/// er is the name GLFW resolved for this key on this layout, NULL when the key types nothing, and
/// it is GLFW's own storage: valid until the next poll, not to be freed and not to be kept past th
/// e frame that ends at it. XUD_INVALID_ARG on a NULL out-param, a NULL context or a key outside GL
/// F W's queryable range; XUD_UNSUPPORTED as above.
XudResult xud_get_key_name(const char** outName, XudContext* context, int key);

/// True if the mouse button transitioned from not held to held at the poll that opened this frame -
/// raylib's IsMouseButtonPressed (raylib.h 1228). XUD_INVALID_ARG on a NULL out-param, a NULL conte
/// xt or a button outside GLFW's mouse-button range; XUD_UNSUPPORTED as above.
XudResult xud_is_mouse_button_pressed(bool* outPressed, XudContext* context, int button);

/// True if the mouse button is held at the poll that opened this frame - raylib's IsMouseButtonDown
/// (raylib.h 1229). XUD_INVALID_ARG as above; XUD_UNSUPPORTED as above.
XudResult xud_is_mouse_button_down(bool* outDown, XudContext* context, int button);

/// True if the mouse button transitioned from held to not held at the poll that opened this frame -
/// raylib's IsMouseButtonReleased (raylib.h 1230). XUD_INVALID_ARG as above; XUD_UNSUPPORTED as abo
/// ve.
XudResult xud_is_mouse_button_released(bool* outReleased, XudContext* context, int button);

/// True if the mouse button is not held at the poll that opened this frame - raylib's IsMouseButto
/// nUp (raylib.h 1231). XUD_INVALID_ARG as above; XUD_UNSUPPORTED as above.
XudResult xud_is_mouse_button_up(bool* outUp, XudContext* context, int button);

/// The cursor's X, as this rung's offset and scale sit above the position the window reported at th
/// e poll that opened this frame, landed on the pixel grid by rounding rather than truncating -
/// raylib's GetMouseX (raylib.h 1232), and the same number xud_get_mouse_position() fills, before
/// its integer landing. XUD_INVALID_ARG on a NULL out-param or a NULL context; XUD_UNSUPPORTED as
/// above.
XudResult xud_get_mouse_x(int* outX, XudContext* context);

/// The cursor's Y, as this rung's offset and scale sit above the position the window reported at th
/// e poll that opened this frame, landed on the pixel grid by rounding rather than truncating -
/// raylib's GetMouseY (raylib.h 1233). XUD_INVALID_ARG as above; XUD_UNSUPPORTED as above.
XudResult xud_get_mouse_y(int* outY, XudContext* context);

/// The cursor's position, as this rung's offset and scale sit above the position the window reporte
/// d at the poll that opened this frame - raylib's GetMousePosition (raylib.h 1234). XUD_INVALID_AR
/// G on a NULL out-param or a NULL context; XUD_UNSUPPORTED as above.
XudResult xud_get_mouse_position(XudVector2* outPosition, XudContext* context);

/// How far the cursor moved between the poll below this frame and the one that opened it - raylib's
/// GetMouseDelta (raylib.h 1235), "the delta between frames", stated against the two polls rather t
/// han against the two calls because nothing here polls on demand. XUD_INVALID_ARG on a NULL out-p
/// aram or a NULL context; XUD_UNSUPPORTED as above.
XudResult xud_get_mouse_delta(XudVector2* outDelta, XudContext* context);

/// Ask the window system to place the cursor - raylib's SetMousePosition (raylib.h 1236). On this
/// backend the answer is XUD_UNSUPPORTED and nothing moved: GLFW's Wayland half refuses to place a
/// cursor it cannot see outside the window (wl_window.c 2970, "The platform does not support settin
/// g the cursor position"), and the refusal is GLFW's own answer read back rather than a guess abo
/// ut the backend. XUD_INVALID_ARG on a NULL context; XUD_UNSUPPORTED on a context whose input run
/// g never came up, on a window that is gone, or on a window system that will not place the cursor.
XudResult xud_set_mouse_position(XudContext* context, int x, int y);

/// Move the origin the cursor's position is read against - raylib's SetMouseOffset (raylib.h 1237
/// ), the one half of the transform above the raw position that is a shift rather than a divisor.
/// XUD_INVALID_ARG on a NULL context; XUD_UNSUPPORTED as above.
XudResult xud_set_mouse_offset(XudContext* context, int offsetX, int offsetY);

/// Divide the cursor's position by a scale - raylib's SetMouseScale (raylib.h 1238), which is how a
/// caller reads a window that renders at a different extent than it shows at. A scale of zero, or
/// one that is not finite, is refused as XUD_INVALID_ARG rather than turned into a division by zero
/// or an infinity behind the queries' backs. XUD_UNSUPPORTED as above.
XudResult xud_set_mouse_scale(XudContext* context, float scaleX, float scaleY);

/// The wheel movement of whichever axis moved more between the poll below this frame and the one th
/// at opened it - raylib's GetMouseWheelMove (raylib.h 1239), the one number a scroll-driven zoom
/// reads. XUD_INVALID_ARG on a NULL out-param or a NULL context; XUD_UNSUPPORTED as above.
XudResult xud_get_mouse_wheel_move(float* outMove, XudContext* context);

/// The wheel movement on both axes between the poll below this frame and the one that opened it -
/// raylib's GetMouseWheelMoveV (raylib.h 1240), for a scroll that can move on either axis. XUD_INV
/// ALID_ARG on a NULL out-param or a NULL context; XUD_UNSUPPORTED as above.
XudResult xud_get_mouse_wheel_move_v(XudVector2* outMove, XudContext* context);

/// Hide the cursor and take the window's pointer into relative mode - raylib's HideCursor, which
/// raylib's own examples call DisableCursor (raylib.h 1245: "Hide cursor and disable cursor
/// influence on all windows"). The window system is asked to stop showing the cursor and to report
/// MOTION rather than a position (GLFW_CURSOR_DISABLED: src/external/glfw/src/wl_window.c 3316
/// locks the pointer through the relative-pointer and pointer-constraints protocols), which is what
/// keeps xud_get_mouse_delta() answering a look-around after the cursor itself has stopped existing.
/// The answer says what GLFW could do and not what the compositor agreed to: pointer lock is granted
/// by the compositor and can be revoked (a focus change, a refusal in some compositors) with no signal
/// GLFW reads back, so XUD_OK means the request is in and the delta is the proof it landed. XUD_INVA
/// LID_ARG on a NULL context; XUD_UNSUPPORTED on a context that never completed bring-up or on a
/// window with no GLFW window behind it; XUD_ALREADY when the cursor is already hidden.
XudResult xud_hide_cursor(XudContext* context);

/// Give the cursor back: raylib's ShowCursor / EnableCursor (raylib.h 1244). The window system shows
/// the cursor again and reports a position rather than motion, and the cursor's position and delta
/// read as they did before xud_hide_cursor(). Same three refusals, XUD_ALREADY when it is already
/// showing.
XudResult xud_show_cursor(XudContext* context);

/// Whether this library has asked for the cursor to be hidden: raylib's IsCursorHidden (raylib.h 1041),
/// the query a first-person camera update opens with so that a cursor the player gave back stops turning
/// the view. The answer is the library's own request and not the compositor's verdict: the pointer lock
/// xud_hide_cursor() asks for is granted and revoked asynchronously on Wayland and GLFW never reports
/// either way, so a true is "hidden as far as this rung knows" and the mouse delta stays the proof. The
/// state belongs to the context, not to the machine: two windows of one program each carry their own.
/// XUD_INVALID_ARG on a NULL out-hidden or a NULL context; XUD_UNSUPPORTED on a context that never
/// completed bring-up.
XudResult xud_is_cursor_hidden(bool* outHidden, XudContext* context);

/*-----------------------------------------------------------------------------------------------
 * UI (nuklear) - the immediate-mode gui rung above the batch, the texture and the input
 *
 * The caller owns the frame's widgets between begin and draw, against the context xud_ui_get
 * _context() hands back; the input is fed from the context's own input rung on the begin, and th
 * e draw converts the frame and folds it into the batch the same way the font walk does, so it 
 * draws through the existing TEXTURED pipeline. The ui is screen-space and assumes the identit
 * y viewProj - a live camera warps it. The atlas is one TEXTURE, so the rung is spent lazily on 
 * the first init, like the sound.
 *-----------------------------------------------------------------------------------------------*/

/// Opens the ui rung on the context: bakes the caller's font file (or the built-in default when
/// the data is NULL) into one atlas image, opens that image as a TEXTURE and names it on every g
/// lyph. XUD_INVALID_ARG on a NULL context or an already-open rung; XUD_UNSUPPORTED on a context
/// that has not completed bring-up; XUD_OUT_OF_MEMORY if the atlas image or the staging will not
/// fit. The font data is read into the atlas and may be freed by the caller on the way back.
XudResult xud_ui_init(XudContext* context, const XudFontData* font);

/// Closes the ui rung: gives the atlas's TEXTURE slot back, frees the nuklear context, the atlas
/// and the staging. XUD_ALREADY on a rung that was never opened.
XudResult xud_ui_close(XudContext* context);

/// The nuklear context the caller's widgets are written against: the frame the caller runs is nk
/// _begin() ... nk_end() against this, between xud_ui_begin() and xud_ui_draw(). NULL on a rung t
/// hat was never opened or a NULL context.
struct nk_context* xud_ui_get_context(XudContext* context);

/// Feeds the context's input rung into the nuklear context - the keys, the mouse, the cursor an
/// d the wheel the caller did not already feed - and opens the frame. XUD_UNSUPPORTED on a rung t
/// hat was never opened.
XudResult xud_ui_begin(XudContext* context);

/// Closes the frame the caller's widgets opened, converts it to vertices and elements and folds
/// them into the batch at the context's own extent, through the atlas's one TEXTURE slot. XUD_UNS
///UPPORTED on a rung that was never opened.
XudResult xud_ui_draw(XudContext* context);

#ifdef __cplusplus
}
#endif

#endif /* XUD_XUD_H_ */
