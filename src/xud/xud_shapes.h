/***********************************************************************************************
 *   xud_shapes.h - the shapes rung: the geometry a draw call appends into the batch
 *
 *   Owns : one function per shape the public draw calls name - today the one triangle of the
 *          hello-triangle step - and the constants every shape here builds its vertices from.
 *   Layer: above the batch, below the public API. This is the "feature modules (shapes, text,
 *   textures, models, audio, camera)" rung of the plan's layer ladder, sitting between the two
 *   rungs it joins: the batch below owns the four SoA streams and the one index buffer a shape's
 *   bytes go into, and xud.h above declares the public call whose arguments this rung turns into
 *   those bytes. The rung is where the turn happens because it is the only place both halves are
 *   in sight: the batch's append pair speaks the streams' own widths (a pos is 12 B, a colour is
 *   4 B) and the public call speaks raylib's value types (a Vector3, a Color), and nothing below
 *   this rung has a reason to know the second, and nothing above it has a reason to know the
 *   first.
 *
 *   Every function here is PURE: it calls no driver entry point and reaches no loader row. What
 *   a shape does is arithmetic on the batch's live counts and a write into the arena the batch
 *   already owns, which is what makes the whole rung headless-testable by construction - the
 *   repo's rule that the pure half of a rung lives in functions that take structs and never call
 *   the driver, read at the rung the repo's "Shapes module" item lands in.
 *
 *   What this rung does NOT have, and why: no camera and no transform. The vertices a shape
 *   appends are the vertices of the shape, in the space the frame's viewProj and the draw's model
 *   matrix are read against, and both are identity today (xud_descriptor_fill_uniform_block()
 *   writes an identity viewProj and xud_frame_fill_push_row() an identity model matrix) - so the
 *   space a shape's coordinates are in is, right now, the clip space the vertex shader lands in.
 *   Choosing a space for a shape is a camera + transform decision, and that is the todo.md item
 *   below this one; a shape that invented its own projection would be a second camera the
 *   library does not have. What is also not here: the rest of raylib's 3D shape family
 *   (DrawCube, DrawSphere, DrawLine3D, DrawPoint3D) - one shape per step that wants one, never a
 *   family written ahead of the caller that needs it.
 ***********************************************************************************************/

#ifndef XUD_SHAPES_H_
#define XUD_SHAPES_H_

#include "xud.h" /* Required for: XudResult, XudVector3, XudColor - the public value types a shape's arguments are */
#include "xud_batch.h" /* Required for: XudBatch, xud_batch_append_vertex/xud_batch_append_index, xud_batch_stats - the batch's own appends and counters */

#include <assert.h> /* Required for: static_assert, the counts below that cannot disagree */

/*-----------------------------------------------------------------------------------------------
 * The shape counts, and the two attribute rows every shape here fills with a constant
 *-----------------------------------------------------------------------------------------------*/

/* The vertices and the indices one triangle is: three of each, and the indices are the three
   vertices this very call appended, in the order the caller named them. The order is load-
   bearing twice over - raylib's DrawTriangle3D is documented "counter-clockwise vertex order"
   (reference/raylib/raylib.h 1588) and the pipeline's front face is the same counter-clockwise
   (xud_pipeline.h's XUD_PIPELINE_FRONT_FACE) - and it is not enforced here, because the
   pipeline's cull mode is VK_CULL_MODE_NONE and a shape that refused a clockwise caller's
   triangle would be refusing a shape the renderer draws perfectly well. */
#define XUD_SHAPES_TRIANGLE_VERTEX_COUNT 3u
#define XUD_SHAPES_TRIANGLE_INDEX_COUNT 3u

/* The normal every shape here carries: +Z, the axis a flat, screen-facing shape faces along.
   The batch's NORMAL stream has no value that means "no normal", so a shape that names no
   surface has to answer one, and the lighting that would read it is not in the tree - solid.frag
   writes the interpolated colour and reads nothing else. A real normal is a property of a
   surface, and a triangle the caller gave three points for has one; computing it from those
   three points is a cross product the camera + transform step owns the handedness of. */
#define XUD_SHAPES_FLAT_NORMAL_X 0.0f
#define XUD_SHAPES_FLAT_NORMAL_Y 0.0f
#define XUD_SHAPES_FLAT_NORMAL_Z 1.0f

/* The uv every shape here carries: (0, 0). The batch's UV stream is read by nothing today
   (solid.frag reads the colour at location 1 only), and set 1 is declared but never allocated,
   so there is no sampler whose array a uv could address. The value is a corner of a texture
   that does not exist, which is what a stream that nothing reads has to be filled with. */
#define XUD_SHAPES_UV_S 0.0f
#define XUD_SHAPES_UV_T 0.0f

static_assert((XUD_SHAPES_TRIANGLE_INDEX_COUNT == XUD_SHAPES_TRIANGLE_VERTEX_COUNT) && (XUD_SHAPES_TRIANGLE_INDEX_COUNT == 3u),
    "xud_shapes.h: a triangle names three vertices and takes the three indices that name them, which "
    "is what makes the index bound below a count of three rather than a count of one shape");

/*-----------------------------------------------------------------------------------------------
 * The layer's contract
 *-----------------------------------------------------------------------------------------------*/

/// Pure: append one filled triangle - three vertices, then the three indices that name exactly
/// those three - into the batch's four SoA streams and its index buffer. The position is the
/// caller's three Vector3s read against the streams' own widths; the colour is the caller's
/// XudColor read against the COLOR stream's byte order (R, G, B, A, which is the note in xud_bat
/// ch.h about VK_FORMAT_R8G8B8A8_UNORM); the uv and the normal are the two constants above, for
/// the reasons stated beside each of them.
///
/// The append is ATOMIC: the batch's counters are read first and a triangle that does not fit is
/// refused before a single byte is written, so a refused shape leaves the batch exactly as it was
/// rather than as a batch holding one and a half triangles. That is a strengthening of the batch
/// rung's own contract, not a change to it - xud_batch_append_vertex refuses at the capacity
/// before it writes, and this rung is what makes the refusal happen at a shape boundary instead
/// of at a vertex inside one.
///
/// XUD_INVALID_ARG on a NULL batch; XUD_OUT_OF_MEMORY when the three vertices or the three indi
/// ces do not fit the batch's capacities (the batch is full, and full is the batch rung's own
/// answer, see xud_batch_append_vertex/xud_batch_append_index); XUD_UNSUPPORTED when the batch
/// has no arena mapped to write into (the same answer xud_batch_append_vertex gives for the same
/// reason, one rung up); XUD_OK only when all six appends completed.
XudResult xud_shapes_append_triangle(XudBatch* batch, XudVector3 v0, XudVector3 v1, XudVector3 v2, XudColor color);

#endif /* XUD_SHAPES_H_ */
