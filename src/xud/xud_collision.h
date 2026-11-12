/***********************************************************************************************
 *
 *   xud_collision.h - the collision rung: the tests, the casts, and a mesh's bounds
 *
 *   Owns : the two floors the casts read as "parallel" - and nothing else the compiler needs. The
 *          ten calls raylib's rmodels.c 3782-4435 are (GetMeshBoundingBox 3782, DrawBoundingBox
 *          4143, CheckCollisionSpheres 4157, CheckCollisionBoxes 4182, CheckCollisionBoxSphere
 *          4197, GetRayCollisionSphere 4213, GetRayCollisionBox 4264, GetRayCollisionMesh 4324,
 *          GetRayCollisionTriangle 4374, GetRayCollisionQuad 4435) are declared in xud.h beside the
 *          value types they read, and defined in xud_collision.c - all but DrawBoundingBox, which
 *          draws and so lives in the shapes rung beside the wire cube it is.
 *   Layer: above the caller's floats and below everything that draws. The rung takes no context, no
 *          descriptor, no driver: no Vulkan or GLFW symbol the linker could see, so the whole of its
 *          correctness is asserted in the headless default run, and a machine with no Vulkan loader
 *          answers the same refusals as one with a window open. It reads the value types of xud.h -
 *          XudVector3, XudRay (which xud_camera.h's fold FILLS, the reason the area runs
 *          above the camera one), XudBoundingBox, XudMeshData and XudRayCollision - owns no record,
 *          mutates nothing, allocates nothing, and so has no open, no close and no lifetime.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#ifndef XUD_COLLISION_H_
#define XUD_COLLISION_H_

#include "xud.h" /* Required for: XudResult, XudVector3, XudRay, XudBoundingBox, XudRayCollision */

#define XUD_COLLISION_PARALLEL_COMPONENT                                                                                                   \
    1e-6f /* A cast component this small or smaller is PARALLEL to the slab it faces: the                                                 \
            interval it would compute is hundreds of millions of units long, which is a miss                                              \
            or a full pass rather than a surface, and dividing by it is a NaN waiting to be                                               \
            produced by a zero numerator. A direction whose every component is this small is                                              \
            refused outright by every cast, so no slab division ever sees it.                                                              \
     */

#define XUD_COLLISION_PARALLEL_DETERMINANT                                                                                                 \
    1e-6f /* A triangle cast's determinant this small or smaller means the ray lies in the                                                \
            triangle's plane, or that the triangle is too thin to have an inside: the same                                                 \
            1e-6 raylib's own EPSILON names (rmodels.c 4381), read as an AREA because the                                                  \
            determinant is a product of two edge-lengths and a direction, not a length. The                                               \
            distance floor a triangle hit has to clear is the component floor above, which                                                 \
            is a length and is measured in the direction's own units.                                                                      \
     */

#endif /* XUD_COLLISION_H_ */
