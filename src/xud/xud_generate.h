/***********************************************************************************************
 *
 *   xud_generate.h - the generation rung: the solids raylib's GenMesh* family cuts, as arrays
 *
 *   Owns : the floors every generator reads its description against (XUD_GENERATE_MIN_EXTENT,
 *          XUD_GENERATE_MIN_RADIUS, the three resolution floors) and the count arithmetic those
 *          floors make safe - and nothing else the compiler needs. The two calls are declared in
 *          xud.h beside the value types they fill (xud_mesh_count(), xud_gen_mesh(), XudMeshShape,
 *          XudMeshCounts, XudMeshBuffers) and defined here.
 *   Layer: above the caller's arrays and below everything that draws. Like the collision rung it
 *          takes no context, no descriptor and no driver: no Vulkan or GLFW symbol the linker
 *          could see, so the whole of its correctness is asserted in the headless default run. It
 *          reads the kinds' axis and the shape's numbers, writes floats and indices into memory
 *          the caller sized and still owns, allocates nothing, and so has no open, no close and no
 *          lifetime. XUD_MESH_MAX_VERTICES is borrowed from the mesh rung because a solid the mesh
 *          rung could not upload is a solid worth refusing HERE, where the answer is a code, rather
 *          than a description that only fails once a context is open.
 *
 *   Count first, then fill. raylib's GenMesh* family allocates the arrays inside the call and hands
 *   back a Mesh whose arrays only UnloadMesh() may free; this library keeps nothing a caller did not
 *   hand it, so the rungs split the pair the other way: xud_mesh_count() answers how many vertices
 *   and indices a shape needs and xud_gen_mesh() writes exactly that many into the caller's buffers.
 *   The two calls run the same count arithmetic over the same validation, which is what makes the
 *   second one's writes provably in bounds - the count is not a hint the fill trusts, it is the same
 *   function's answer.
 *
 *   Cloned : GenMeshPlane (rmodels.c 1027), GenMeshCube (1100), GenMeshSphere (1136),
 *          GenMeshHemiSphere (1215), GenMeshCylinder (1287), GenMeshTorus (1374), GenMeshKnot
 *          (1460) and GenMeshPoly (1546). NOT cloned, each for a reason the shape itself gives:
 *          GenMeshGrid (a grid has no inside, and the shapes rung already draws one), GenMeshQuad
 *          and GenMeshTriangle (a flat sheet the shapes rung already draws), GenMeshCubicmap and
 *          GenMeshHeightmap (both read a caller's pixel array, which is the texture rung's input and
 *          a rung this one does not have), GenMeshBezierPatch (no caller asks for one yet), and
 *          GenMeshTangents (a fourth stream for a shader this library has no row for).
 *
 *   Where this is not raylib. GenMeshKnot builds a trefoil with par_shapes'
 *   par_shapes_create_trefoil_knot and FLATTENS it - no index array, three vertices per triangle
 *   with the shared corners duplicated (rmodels.c 1478: vertexCount = ntriangles*3); the knot here
 *   is built with a transported frame along the same curve and keeps its index array, so its
 *   corners are shared and its vertex count is radSeg*sides rather than radSeg*sides*6. And every
 *   generator here answers XUD_INVALID_ARG for a description the shape has no answer to, where
 *   raylib leaves the Mesh all-zero and logs a warning the caller cannot read.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#ifndef XUD_GENERATE_H_
#define XUD_GENERATE_H_

#include "xud.h" /* Required for: XudResult, the shape, the counts and the buffers this rung fills */
#include "xud_mesh.h" /* Required for: XUD_MESH_MAX_VERTICES, the ceiling a shape is refused past */

#define XUD_GENERATE_MIN_EXTENT                                                                                                            \
    1e-6f /* A width, height or length this small or smaller has no inside to fill: the solid                                                \
            would be a sheet of coincident triangles whose normals point in no direction, and                                               \
            a zero extent makes two faces of a cube land on the same plane. Compared against                                                 \
            directly, so a NaN extent is refused by the same test as a negative one.                                                         \
     */

#define XUD_GENERATE_MIN_RADIUS                                                                                                            \
    1e-6f /* A radius this small or smaller is the same silence as the extent above: every                                                   \
            normal of a point-sized sphere is that point's own direction, which is no normal.                                                \
            The knot clamps its radius the way raylib does (rmodels.c 1465 clamps to 0.5..3)                                                 \
            BEFORE this floor is read, so a knot description never refuses on radius alone.                                                  \
     */

#define XUD_GENERATE_MIN_SIDES                                                                                                             \
    3u /* The smallest ring that encloses an area: two vertices are a line, and a poly of two                                                \
         sides is a degenerate fan whose every triangle is a segment. This is raylib's own floor                                            \
         (its GenMesh* guards read `sides >= 3` and `radSeg >= 3`).                                                                          \
     */

#define XUD_GENERATE_MIN_STACKS                                                                                                            \
    1u /* One row of quads is a body: a hemisphere with one stack is the pole's fan and the                                                  \
         equator's ring, which is the whole solid, and a plane with one row is one quad. A                                                  \
         sphere needs two, because one stack would put both poles on one ring; see                                                           \
         XUD_GENERATE_MIN_SPHERE_STACKS.                                                                                                     \
     */

#define XUD_GENERATE_MIN_SPHERE_STACKS                                                                                                     \
    2u /* A sphere's first ring is the north pole and its last is the south, so one stack is a                                               \
         ring of coincident vertices and a mesh with no volume; raylib's own arithmetic                                                       \
         divides by (stacks - 1) implicitly in its fan and gets an empty mesh for stacks of 1.                                               \
     */

#define XUD_GENERATE_PI                                                                                                                    \
    3.1415926535897932f /* The one circle this rung's rings are measured in: the same literal                                              \
         as the camera's degree conversion (xud_camera.h), read as an angle rather than a                                                   \
         divisor.                                                                                                                           \
     */

#define XUD_GENERATE_TWO_PI                                                                                                                \
    6.2831853071795865f /* One turn: the longitude a sphere's ring closes on, and the angle a                              \
         polygon's corners are spaced by.                                                                                    \
     */

#endif /* XUD_GENERATE_H_ */
