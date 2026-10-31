/***********************************************************************************************
 *   xud_shapes.h - the shapes rung: the geometry a draw call appends into the batch
 *
 *   Owns : one function per shape the public draw calls name - today the triangle of the hello
 *          triangle step and the box above it - and the constants and the face tables every shape
 *          here builds its vertices from.
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
 *   library does not have. What is also not here, and why: the rest of raylib's 3D shape family.
 *   DrawSphere is the next one and it is a tessellation decision - a sphere's vertex count is
 *   (rings+1)*(slices+1), so a default tessellation is a claim about the batch's capacity and the
 *   shapes rung is PURE and cannot ask the batch to grow. DrawLine3D, DrawPoint3D and the wire
 *   forms (DrawCubeWires, DrawSphereWires) are not this rung's to write at all: they are LINE_LIST
 *   and POINT_LIST topologies and the pipeline rung's one topology is TRIANGLE_LIST (xud_pipeline
 *   _h's XUD_PIPELINE_TOPOLOGY), so a line drawn as two thin triangles would be a shape the
 *   caller did not ask for and a new pipeline row would be a pipeline-rung decision, not a shape's.
 *   DrawMesh is the mesh / vertex-buffer abstraction, a rung below this item in todo.md's Later
 *   list. One shape per step that wants one, never a family written ahead of the caller that needs
 *   it - the box is here because a caller who has a triangle has a box.
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

/* The normal the TRIANGLE carries: +Z, the axis a flat, screen-facing shape faces along.
   The batch's NORMAL stream has no value that means "no normal", so a shape that names no
   surface has to answer one, and the lighting that would read it is not in the tree - solid.frag
   writes the interpolated colour and reads nothing else. A real normal is a property of a
   surface, and a triangle the caller gave three points for has one; computing it from those
   three points is a cross product the camera + transform step owns the handedness of. The box
   below does NOT use this: a cube's six faces have six different normals, and those are the
   shape's own attribute rows, in a table, not a constant here. */
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
 * The box: six faces, four vertices each, two triangles each
 *-----------------------------------------------------------------------------------------------*/

/* The box of raylib's DrawCube (reference/raylib/raylib.h 1590, "Draw cube", whose arguments are
   (position, width, height, length, color)): the caller's position is the box's CENTRE and its
   three floats are the full extents along x/y/z, so the extent is halved below - raylib's own
   DrawCube computes start = position - size*0.5 and end = position + size*0.5, and a box whose
   centre was its corner would be a box twice the size the caller asked for. The three extents
   are separate rather than one size because the shape is a box, not a cube: raylib names the
   third extent "length" and the todo item names the shape DrawCube, so the shape's name is the
   cube's and its shape is the box's, exactly as raylib's is.

   Four vertices per face rather than eight corners shared between three faces each: a corner's
   normal belongs to the face that reads it, so a shared corner would be one vertex carrying one
   normal to three faces that want three different ones. 24 vertices is the flat-shaded spelling
   of a box and 36 indices is two triangles per face - and 36 is exactly 3/2 of 24, which is the
   Euler bound xud_batch.h sizes the index headroom at, so a box is the closed-mesh case of that
   bound rather than a shape that needs headroom beyond it. */
#define XUD_SHAPES_CUBE_FACE_COUNT 6u
#define XUD_SHAPES_CUBE_FACE_VERTEX_COUNT 4u
#define XUD_SHAPES_CUBE_FACE_INDEX_COUNT 6u
#define XUD_SHAPES_CUBE_VERTEX_COUNT (XUD_SHAPES_CUBE_FACE_COUNT * XUD_SHAPES_CUBE_FACE_VERTEX_COUNT)
#define XUD_SHAPES_CUBE_INDEX_COUNT (XUD_SHAPES_CUBE_FACE_COUNT * XUD_SHAPES_CUBE_FACE_INDEX_COUNT)
#define XUD_SHAPES_CUBE_HALF_EXTENT 0.5f

static_assert((XUD_SHAPES_CUBE_VERTEX_COUNT == 24u) && (XUD_SHAPES_CUBE_INDEX_COUNT == 36u),
    "xud_shapes.h: a box is six faces of four vertices and six indices, which is the 24/36 the append's fit check is written against");
static_assert(XUD_SHAPES_CUBE_INDEX_COUNT == ((XUD_SHAPES_CUBE_VERTEX_COUNT * 3u) / 2u),
    "xud_shapes.h: a box spends exactly the 3n/2 xud_batch.h sizes the index headroom at, so it is the closed-mesh case of that bound and "
    "not a shape that needs headroom beyond it");
static_assert((XUD_SHAPES_CUBE_VERTEX_COUNT <= XUD_BATCH_INITIAL_VERTICES)
        && (XUD_SHAPES_CUBE_INDEX_COUNT <= XUD_BATCH_INDEX_CAPACITY(XUD_BATCH_INITIAL_VERTICES)),
    "xud_shapes.h: one box fits the batch's INITIAL capacity with no grow asked for, which is what makes it a shape a context can draw "
    "today rather than one that needs a rung above this");
static_assert(XUD_SHAPES_CUBE_FACE_VERTEX_COUNT == 4u,
    "xud_shapes.h: the face's corner count is the four the face tables and the two-triangle order are written against");

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

/// Pure: append one filled box - the caller's centre plus three extents, read as raylib's DrawCube
/// reads them (the centre is the centre, the extents are full, the box is axis-aligned) - as 24
/// vertices and the 36 indices that name them: four vertices per face, so a face's outward normal
/// belongs to that face alone and is the shape's own attribute row rather than the flat +Z
/// constant above, and two triangles per face, wound counter-clockwise seen from OUTSIDE, which is
/// the same winding raylib's DrawTriangle3D documents and the pipeline's XUD_PIPELINE_FRONT_FACE
/// names. The uv is the zero constant above for every vertex, for the reason stated beside it: a
/// box has no texture to unwrap yet, and set 1 is still unallocated. The colour is the caller's at
/// all 24 vertices, because the batch's COLOR stream is per-vertex and a box has one colour.
///
/// The append is ATOMIC in the same shape as the triangle's: both of the batch's bounds are read
/// first and a box that does not fit is refused before a byte is written. Both bounds are checked
/// rather than only the vertex one, and the reason is that an index names a LIVE vertex rather than
/// a new one: a batch whose existing content reuses vertices (the mesh spelling, which is the rung
/// below) spends more than the batch's 3n/2 indices per vertex, so it can be full on indices while
/// it still has twenty-four vertices' worth of room, and a refusal that checked only the vertices
/// would write a box's 24 vertices into a batch that cannot name them.
///
/// XUD_INVALID_ARG on a NULL batch; XUD_OUT_OF_MEMORY when the 24 vertices or the 36 indices do
/// not fit the batch's capacities; XUD_UNSUPPORTED when the batch has no arena mapped to write
/// into (the same answer xud_batch_append_vertex gives for the same reason, one rung up); XUD_OK
/// only when all 24 vertex appends and all 36 index appends completed. A zero or negative extent is
/// not refused: raylib's DrawCube computes start/end from the size without checking it, so a flat
/// box is a flat box and a mirrored one is a mirrored one, and in both cases the shape is still
/// 24 vertices and 36 indices.
XudResult xud_shapes_append_cube(XudBatch* batch, XudVector3 center, float width, float height, float length, XudColor color);

#endif /* XUD_SHAPES_H_ */
