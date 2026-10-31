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
 *   library does not have. What is also not here, and why: the wire forms (DrawCubeWires,
 *   DrawSphereWires) and DrawMesh. The wires are the same LINE_LIST topology the line and point
 *   forms below now use, so they are no longer a topology question - they are a shape question,
 *   and one this step has not answered: a cube's twelve edges and a sphere's ring/slice grid are
 *   tables this rung would have to write, and the todo item asks for them beside the filled forms
 *   rather than below it. DrawMesh is the mesh / vertex-buffer abstraction, a rung below this item
 *   in todo.md's Later list. One shape per step that wants one, never a family written ahead of
 *   the caller that needs it - the box is here because a caller who has a triangle has a box, the
 *   sphere because a caller who has a box has something round to ask for, and the line because a
 *   caller who has a sphere has an axis to look at.
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
 * The line: two vertices and the two indices that name them
 *-----------------------------------------------------------------------------------------------*/

/* The line of raylib's DrawLine3D (reference/raylib/raylib.h 1585, "Draw a 3D line", whose
   arguments are (startPos, endPos, color) and whose own comment says it is "drawn as a line using
   immediate mode (rlBegin(RL_LINES))"). Two vertices and the two indices that name them: the
   indices are not decoration here, because the batch's one index buffer is what every draw is
   replayed through (vkCmdDrawIndexed names no buffer and no topology, both of which are pipeline
   state) - a line with no indices would be a line the frame loop could not draw.

   The topology this shape moves the batch to is stated by the APPEND rather than by a caller, and
   that is raylib's shape read literally: rlBegin(mode) is called by the shape function, not by the
   caller (raylib.h 1585's own comment says the shape calls it), and a caller has no topology to
   ask for - a line is a line because of its geometry. xud_batch_set_topology() is the call that
   moves the state and closes the run that was pending under the OLD topology into a row of the
   same batch, which is what makes a frame that draws a box and a line two rows rather than one
   row drawn at whichever topology happened to come last.
*/
#define XUD_SHAPES_LINE_VERTEX_COUNT 2u
#define XUD_SHAPES_LINE_INDEX_COUNT 2u

/* The point of raylib's DrawPoint3D (reference/raylib/raylib.h 1586, "Draw a point in 3D space",
   "actually a small line", whose arguments are (position, color)): a point in the shape this
   library is modelled on is a short line, so it needs no POINT_LIST row of the pipeline list and
   no third topology - it is this very append with its endpoints placed either side of the caller's
   position. raylib's rshapes.c draws it as two rlVertex3f calls offset +/- 0.05 along z; that file
   is NOT in the reference set here (only the headers are), so the half-length is written as this
   shape's own named constant rather than as a value read off a file, and the axis is z because that
   is the axis the flat +Z normal above already faces along. The length is in the same space every
   other shape here is in - the clip space the camera step has not replaced - so 0.1 of it is what
   a point spans on screen today, and a point that wanted a screen-relative size would want the
   camera + transform step above this one. */
#define XUD_SHAPES_POINT_HALF_LENGTH 0.05f

static_assert((XUD_SHAPES_LINE_INDEX_COUNT == XUD_SHAPES_LINE_VERTEX_COUNT) && (XUD_SHAPES_LINE_VERTEX_COUNT == 2u),
    "xud_shapes.h: a line names two vertices and takes the two indices that name them, which is what "
    "makes the LINE_LIST topology below read two at a time rather than three");

/*-----------------------------------------------------------------------------------------------
 * The sphere: rings latitude divisions, slices longitudes, and two single-vertex poles
 *-----------------------------------------------------------------------------------------------*/

/* The sphere of raylib's DrawSphere (reference/raylib/raylib.h 1594, "Draw sphere", whose
   arguments are (centerPos, radius, color)): the caller's float is the RADIUS and the position is
   the centre, which is the same centre-not-corner reading DrawCube above already makes.

   The tessellation below is a claim about the BATCH's capacity, not about the shape: this rung is
   PURE and cannot ask the batch to grow, so a sphere that does not fit the capacity a context
   opens its batch at is refused rather than drawn, and the default tessellation is the finest one
   that fits that capacity. It is the finest because a sphere is the shape that spends its indices
   on SHARED vertices: a closed manifold triangulated surface has F = 2V-4 faces by Euler (3F = 2E
   and V-E+F = 2), so it needs 3F = 6V-12 indices - FOUR times the 3n/2 per vertex CAPACITY headroo
   m
   xud_batch.h's XUD_BATCH_INDEX_CAPACITY sizes (the box's 1.5 per vertex is the SPLIT-per-face spe
   lling,
   not that bound) - and the sphere's 90 over its 17 vertices is exactly 6V-12, asserted below. So
   for a sphere the index bound is the one that refuses first and the vertex bound has 47 slots to
   spare. The two asserts below are that fact read both ways: one sphere fits the initial headroom
   and two do not, which is what makes the refusal the tests area checks a refusal of a whole secon
   d
   sphere rather than of a half one.

   The poles are single vertices rather than one row of SLICE coincident vertices each: a row of
   coincident poles makes the pole band's quads half-degenerate - triangles with a zero cross
   product, which the rasteriser discards and which no winding check can read - and it spends 2*SL
   ICE vertices to say what two vertices say. So the vertex count is 2 + (rings-1)*slices rather
   than the (rings+1)*(slices+1) grid a seam-duplicated row would give, and there is no seam column
   either: the seam exists to make a UV wrap around the longitude, and the UV is the zero constant
   this header states for every shape here (set 1 is still unallocated and no sampler's array can
   be addressed), so there is nothing to wrap. A sphere that grows to want a texture will want the
   seam column back, and that is the texture step's to notice, not one invented here.

   The minimums are the closed-surface counts, not a style choice: two rings are the two poles plus
   at least one row between them, and three slices are the fewest longitudes that close a band
   without two faces lying on top of each other. Below either the mesh is not a surface, and it is
   refused rather than drawn as a degenerate one. */
#define XUD_SHAPES_SPHERE_RING_MIN 2u
#define XUD_SHAPES_SPHERE_SLICE_MIN 3u
#define XUD_SHAPES_SPHERE_RING_COUNT 4u
#define XUD_SHAPES_SPHERE_SLICE_COUNT 5u
#define XUD_SHAPES_SPHERE_PI 3.1415926535897932f
#define XUD_SHAPES_SPHERE_VERTEX_COUNT (2u + ((XUD_SHAPES_SPHERE_RING_COUNT - 1u) * XUD_SHAPES_SPHERE_SLICE_COUNT))
#define XUD_SHAPES_SPHERE_TRIANGLE_COUNT (2u * XUD_SHAPES_SPHERE_SLICE_COUNT * (XUD_SHAPES_SPHERE_RING_COUNT - 1u))
#define XUD_SHAPES_SPHERE_INDEX_COUNT (3u * XUD_SHAPES_SPHERE_TRIANGLE_COUNT)

static_assert(
    (XUD_SHAPES_SPHERE_RING_COUNT >= XUD_SHAPES_SPHERE_RING_MIN) && (XUD_SHAPES_SPHERE_SLICE_COUNT >= XUD_SHAPES_SPHERE_SLICE_MIN),
    "xud_shapes.h: the default tessellation is inside the tessellation the append accepts, so the default is a tessellation and not a "
    "refusal");
static_assert(
    (XUD_SHAPES_SPHERE_VERTEX_COUNT == 17u) && (XUD_SHAPES_SPHERE_TRIANGLE_COUNT == 30u) && (XUD_SHAPES_SPHERE_INDEX_COUNT == 90u),
    "xud_shapes.h: the default sphere is 17 vertices (two single poles plus three rows of five) and 90 indices (two fans of five plus two "
    "bands of five quads), which is what the append's fit check counts");
static_assert(XUD_SHAPES_SPHERE_INDEX_COUNT == (3u * XUD_SHAPES_SPHERE_TRIANGLE_COUNT),
    "xud_shapes.h: every triangle of the sphere is named by three indices, which is what makes the index count the triangle count times "
    "three rather than a second count to keep honest");
static_assert(XUD_SHAPES_SPHERE_INDEX_COUNT == ((6u * XUD_SHAPES_SPHERE_VERTEX_COUNT) - 12u),
    "xud_shapes.h: a closed manifold triangulated surface has F = 2V-4 faces by Euler and so 3F = 6V-12 indices, and the sphere's 90 over "
    "its 17 vertices is exactly that - four times the batch's 3n/2 headroom per vertex, which is why the batch's note calls 3n/2 a choice "
    "rather than a bound");
static_assert(XUD_SHAPES_SPHERE_INDEX_COUNT > ((XUD_SHAPES_SPHERE_VERTEX_COUNT * 3u) / 2u),
    "xud_shapes.h: a sphere spends 90 indices over 17 vertices, which is above the batch's 3n/2 per-vertex headroom, so the sphere is the "
    "shape whose INDEX bound refuses first and the box's opposite");
static_assert((XUD_SHAPES_SPHERE_VERTEX_COUNT <= XUD_BATCH_INITIAL_VERTICES)
        && (XUD_SHAPES_SPHERE_INDEX_COUNT <= XUD_BATCH_INDEX_CAPACITY(XUD_BATCH_INITIAL_VERTICES)),
    "xud_shapes.h: one sphere fits the capacity a context's batch opens at, so drawing one asks nothing of the grow path - the same claim "
    "the box's assert makes");
static_assert((2u * XUD_SHAPES_SPHERE_INDEX_COUNT) > XUD_BATCH_INDEX_CAPACITY(XUD_BATCH_INITIAL_VERTICES),
    "xud_shapes.h: two spheres do not fit that same headroom, so the refusal the tests area checks is a refusal of a whole second sphere "
    "and not of half of one");

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

/// Pure: append one sphere - the caller's centre plus a radius, read as raylib's DrawSphere reads
/// them - as the closed mesh above states: two single-vertex poles, RING_COUNT-1 rows of SLICE_CO
/// UNT vertices between them, and the indices that close it as two fans of SLICE_COUNT triangles
/// plus (RING_COUNT-2) bands of SLICE_COUNT quads. The vertex is the centre plus the radius times
/// a unit direction, and the NORMAL is that same unit direction: a sphere's normal is radial by
/// definition, which is the reason a sphere shares vertices where a box cannot - a box's corner
/// wants three normals and a sphere's vertex wants exactly one, and it is the vertex's own.
/// The tessellation is the header's, not the caller's, because a tessellation the caller chose is
/// a claim about the batch's capacity rather than about the sphere (see the note above); the
/// append takes rings and slices so the rung's own refusals can be checked at a tessellation the
/// public call never asks for, and xud_draw_sphere() above passes the header's constants.
///
/// The append is ATOMIC in the same shape as the triangle's and the box's, and BOTH of the batch's
/// bounds are read first - for a sphere the index one is the one that trips, which is the opposite
/// of the triangle's and is why neither bound is enough on its own here either.
///
/// XUD_INVALID_ARG on a NULL batch, on a tessellation below the closed-surface minimums above (fe
/// wer than two rings is not a closed surface, fewer than three slices is a band whose faces lie
/// on each other), or on a radius that is not strictly positive (a sphere with no radius has no
/// surface and no radial direction, and the normal is that direction divided by it - a NaN in a
/// float stream is not a value the pipeline can read, which is a stronger reason than the box's
/// flat face has; a NaN radius is refused by the same test); XUD_OUT_OF_MEMORY when the 17 vertic
/// es or the 90 indices do not fit the batch's capacities; XUD_UNSUPPORTED when the batch has no
/// arena mapped to write into (the same answer xud_batch_append_vertex gives for the same reason,
/// one rung up); XUD_OK only when all 17 vertex appends and all 90 index appends completed.
XudResult xud_shapes_append_sphere(XudBatch* batch, XudVector3 center, float radius, uint32_t rings, uint32_t slices, XudColor color);

/// Pure: append one line - the caller's two endpoints and the two indices that name them - under
/// the batch's LINE_LIST topology, which the append moves the batch to before a byte is written
/// (see the note beside the counts above for why the shape rather than the caller owns that). The
/// normal is the flat +Z constant above: a line has no surface, so it answers the same way the
/// triangle does, and the lighting that would read it is not in the tree.
///
/// The append is ATOMIC in the same shape as the triangle's, the box's and the sphere's: both of
/// the batch's bounds are read first and a line that does not fit is refused before a byte is
/// written AND before the topology state moves, so a refused line leaves the batch exactly as it
/// was - including its topology state, which is why a refused line cannot split a run.
///
/// XUD_INVALID_ARG on a NULL batch; XUD_OUT_OF_MEMORY when the two vertices or the two indices do
/// not fit the batch's capacities; XUD_UNSUPPORTED when the batch has no arena mapped to write
/// into (the same answer xud_batch_append_vertex gives for the same reason, one rung up) or when
/// xud_batch_set_topology refuses to move the state; XUD_OK only when the state moved, both vertex
/// appends and both index appends completed.
XudResult xud_shapes_append_line(XudBatch* batch, XudVector3 start, XudVector3 end, XudColor color);

#endif /* XUD_SHAPES_H_ */
