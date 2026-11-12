/***********************************************************************************************
 *
 *   xud_collision.h - the collision rung: the solid-vs-solid tests and the two ray casts
 *
 *   Owns : the component floor a cast reads as "parallel" - and nothing else the compiler needs. The
 *          five calls raylib's rmodels.c 4157-4321 are (CheckCollisionSpheres 4157, CheckCollision
 *          Boxes 4182, CheckCollisionBoxSphere 4197, GetRayCollisionSphere 4213, GetRayCollisionBox
 *          4264) are declared in xud.h beside the value types they read, and defined in xud_colli
 *          sion.c, which is where every other rung's public calls are defined too.
 *   Layer: above the caller's floats and below everything that draws. The rung takes no context, no
 *          descriptor, no driver: no Vulkan or GLFW symbol the linker could see, so the whole of its
 *          correctness is asserted in the headless default run, and a machine with no Vulkan loader
 *          answers the same refusals as one with a window open. It reads the three value types of
 *          xud.h - XudVector3, XudRay (which xud_camera.h's fold FILLS, the reason the area runs
 *          above the camera one) and XudBoundingBox - owns no record, mutates nothing, and so has no
 *          open, no close and no lifetime.
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

#endif /* XUD_COLLISION_H_ */
