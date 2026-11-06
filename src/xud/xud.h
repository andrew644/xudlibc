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
 *          camera_3d() / xud_end_drawing()), the object API above the handle layer (the three crea
 *          te/close pairs xud_create_material() / xud_close_material() / xud_create_texture() / xud_
 *          close_texture() / xud_upload_mesh() / xud_close_mesh() and the scoped-state pair xud_be
 *          gin_material() / xud_end_material()), the value types a caller passes down (XudVector2 /
 *          XudVector3 / XudColor / XudMaterial / XudTextureData / XudMeshData), the input API above the input rung (raylib's keyboard and
 * mouse half - xud_ is_key_pressed() / xud_is_key_pressed_repeat() / xud_is_key_down() / xud_is_key_relea sed() / xud_is_key_up() /
 * xud_get_key_pressed() / xud_get_char_pressed() / xud_get_key _name() / xud_set_exit_key() / xud_get_exit_key() and the mouse block below
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
 * the mesh pair, the model pair, the text pair and the two draws that name a handle (xud_draw_mesh() / xud_draw_model()); what is still
 * missing there is the camera + transform math above the shapes - the model matrix is the identity today, so a mesh is placed by the camera
 * alone - plus the sampler that would put a font's atlas into set 1 for the text pair to draw through, and no object kind is left without a
 * rung above the table today (the FONT kind's public half landed with the font pair, which is the last one that was missing).
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
/// budget is the caller's - the library's depth buffer is D32_SFLOAT and maps [near,far] to [0,1],
/// so a scene that states its own planes gets the whole depth range across its own geometry.
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
#define XUD_HANDLE_KIND_MATERIAL 4u /* a material: a pipeline variant and its set-1 bindings, once the pipelines land */
#define XUD_HANDLE_KIND_MODEL 5u /* a model: the meshes and materials a load produced, once model loading lands */
#define XUD_HANDLE_KIND_AUDIOSTREAM 6u /* an audio stream: the decoded frames and their format, once the audio rung lands */
#define XUD_HANDLE_KIND_MAX                                                                                                                \
    XUD_HANDLE_KIND_AUDIOSTREAM /* the highest kind value in use today - a new caller-addressable kind moves this and its own value,       \
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
 * ne row that reads: the textured variant's frag module samples set 1 and no other row samples any
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

/// Take the 3D camera away: the public EndCamera3D() of the same pair. The frames drawn after here
/// are drawn with no camera at all, which is the identity matrix the descriptor rung opens set 0's
/// block with - the block's matrix is rewritten every frame from the camera state the renderer hol
/// ds rather than left at whatever the previous frame's camera put in it, which is what makes a
/// camera that began and ended inside one frame a no-op rather than a leak of state into the next
/// frame. XUD_ALREADY when no camera was live (the state is already the one this call asks for);
/// XUD_INVALID_ARG on a NULL context; XUD_UNSUPPORTED on a context that never completed bring-up.
XudResult xud_end_camera_3d(XudContext* context);

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
/// t the handle layer's own slot bound or when the block cannot be carved; XUD_OUT_OF_MEMORY when the
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

/// Draw every mesh a model owns, each with the material the load read for it: the model's row of the
/// drawing API, and the reason raylib's DrawModel3D loses its position/rotation/scale arguments. The
/// model matrix is the identity today (the camera + transform step above the shapes has not landed),
/// so a model is placed by the camera alone and a caller that wants it elsewhere moves the camera. A
/// model is a list of MESH slots and one MATERIAL slot per primitive, so this call is the loop over
/// that list rather than one row: each mesh is recorded with the variant and the material slot the
/// load read for it, whatever material was begun around the call, and the batch's state moves to the
/// LAST mesh's pair on success, which is raylib's DrawModel leaving the last mesh's texture state
/// behind it. XUD_INVALID_ARG on a NULL context or on a handle that does not name a live MODEL slot
/// of this context; XUD_UNSUPPORTED on a context that never completed bring-up; XUD_INTERNAL_ERROR if
/// any MESH or MATERIAL slot the load created has lost its buffers or its record, which is a state the
/// model rung's own open made; the batch's codes otherwise, and a row that refused leaves the rows
/// recorded so far recorded - the batch's rows are a list, and a partial model is a frame that drew
/// part of it rather than a frame that drew none of it.
XudResult xud_draw_model(XudContext* context, XudHandle handle);

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
 *   variant of the pipeline list, and the set-1 slot they sample with is the atlas's own entry of the
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
 * The audio playback API - raylib's PlayAudio/StopAudio half, above the sound rung
 *
 * This is raylib's other audio half (reference/raylib/raylib.h: PlayAudio, StopAudio) above the
 * sound rung: miniaudio's engine - its ma_device, its node graph and its mixer - with one node pa
 * ir per AUDIOSTREAM slot attached to that graph, reading the block the audio rung below already
 * decoded. The pair here is keyed by the stream rather than by a sound index a caller has to reme
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
/// t frame 0; a play on a row stopped mid-way resumes where it stopped, which is miniaudio's own note
/// that a stopped sound is not rewound. XUD_INVALID_ARG on a NULL context or on a handle that does no
/// t name a live AUDIOSTREAM slot of this context; XUD_UNSUPPORTED on a context that never completed b
/// ring-up, on a stream whose block has no frames to read, or when no audio backend this vendored min
/// iaudio can drive is present; XUD_ALREADY when the row is already playing; XUD_OUT_OF_MEMORY when th
/// e row or the node cannot be spent.
XudResult xud_play_audio(XudContext* context, XudHandle stream);

/// Stop one stream's node: raylib's StopAudio (raylib.h 508, "stop playing audio stream"), with th
/// e stream's handle as the row's identity rather than a sound index. The node stays attached, so the
/// next play of the same slot costs a state change rather than a new node, and the frames go back to th
/// e audio record unchanged. XUD_INVALID_ARG on a NULL context or on a handle that does not name a liv
/// e AUDIOSTREAM slot of this context; XUD_UNSUPPORTED on a context that never completed bring-up; XUD_
/// ALREADY when the slot never had a node or its node is already stopped.
XudResult xud_stop_audio(XudContext* context, XudHandle stream);

/// Is the mixer reading this stream right now: the one question a row answers, read off this rung's o
/// wn bookkeeping rather than off miniaudio's clock, so the answer is a state this library owns and no
/// t a race against the thread that pulls frames. XUD_INVALID_ARG on a NULL out-playing, context, or on a
/// handle that does not name a live AUDIOSTREAM slot of this context; XUD_UNSUPPORTED on a context that n
/// ever completed bring-up; XUD_OK with false on a stream that was never played, which is the answer rat
/// her than an error.
XudResult xud_is_audio_playing(bool* outPlaying, XudContext* context, XudHandle stream);

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
 * The input API - raylib's keyboard and mouse half, above the input rung
 *
 * This is raylib's input surface (reference/raylib/raylib.h 1202-1241: IsKeyPressed, IsKeyPressed
 * Repeat, IsKeyDown, IsKeyReleased, IsKeyUp, GetKeyPressed, GetCharPressed, GetKeyName, SetExitKey
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
 * a press in a frame that has not ended yet.
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

/// Record the key that ends a run - raylib's SetExitKey (raylib.h 1210), ESC to start with, XUD_KEY
/// _NULL for none. This rung RECORDS it and the library never exits a process of its own: the cal
/// ler asks for it with xud_get_exit_key() and acts, which is what keeps a window that is closed b
/// y a key press a decision the caller made rather than one made inside the library. XUD_INVALID_AR
/// G on a NULL context or on a key that is neither XUD_KEY_NULL nor inside GLFW's queryable range;
/// XUD_UNSUPPORTED as above.
XudResult xud_set_exit_key(XudContext* context, int key);

/// The key that ends a run, as this rung records it - the query beside the setter above, so the def
/// ault is readable rather than only knowable by convention. XUD_INVALID_ARG on a NULL out-param or
/// a NULL context; XUD_UNSUPPORTED as above.
XudResult xud_get_exit_key(int* outKey, XudContext* context);

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
/// e poll that opened this frame - raylib's GetMouseX (raylib.h 1232), and the same number xud_get_
/// mouse_position() fills. XUD_INVALID_ARG on a NULL out-param or a NULL context; XUD_UNSUPPORTED a
/// s above.
XudResult xud_get_mouse_x(int* outX, XudContext* context);

/// The cursor's Y, as this rung's offset and scale sit above the position the window reported at th
/// e poll that opened this frame - raylib's GetMouseY (raylib.h 1233). XUD_INVALID_ARG as above; XU
/// D_UNSUPPORTED as above.
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
