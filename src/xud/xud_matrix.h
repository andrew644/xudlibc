/***********************************************************************************************
 *
 *   xud_matrix.h - the matrix rung: the transforms the _ex draws place their object with
 *
 *   Owns : the one layout fact every matrix in this library shares (XUD_MATRIX_ELEMENT_COUNT), the
 *          axis floor xud_matrix_rotate() reads as "no direction", and the finiteness tests every
 *          builder runs before it writes - and nothing else the compiler needs. The eight calls
 *          raymath's Matrix family is (MatrixIdentity 988, MatrixTranslate 1779, MatrixScale 1816,
 *          MatrixRotate 1792, MatrixRotateXYZ 1897, MatrixMultiply 968, Vector3Transform 1122) plus
 *          the compose and decompose-free convenience the _ex draws ask for are declared in xud.h
 *          and defined in xud_matrix.c.
 *   Layer: above the caller's floats and below everything that draws. Like the collision and
 *          generation rungs it takes no context, no descriptor and no driver: no Vulkan or GLFW
 *          symbol the linker could see, so the whole of its correctness is asserted in the headless
 *          default run. It reads XudVector3, writes float[16] the caller owns, allocates nothing,
 *          and so has no open, no close and no lifetime. It is the rung the mesh and model rungs'
 *          _ex forms consume: xud_draw_mesh_ex(), xud_draw_model_ex() and xud_get_ray_collision_mesh()
 *          all take the same 16 floats and none of them builds one, which is why the builders live
 *          here rather than beside the draws that spend them.
 *
 *   Column-major, and the vector is a COLUMN. raymath stores the same transform ROW-major and
 *   multiplies the row vector from the LEFT (v' = v*M, raymath.h 90-98), which is why its
 *   MatrixTranslate writes the translation at m12..m14 of the fourth ROW and why its rotations carry
 *   negated angles (MatrixRotateXYZ 1904: cosf(-angle.z)). This rung writes what the vertex stage
 *   reads - `modelMatrix * vec4(position, 1.0)` - so the translation sits at 12, 13, 14 of the
 *   fourth COLUMN and the 3x3 block is raymath's transpose. The two layouts hold the same transform
 *   either way: M here equals raymath's M transposed, so a caller reading a matrix out of a glTF
 *   file (which is column-major, and so is this) needs no conversion, and a caller porting a raymath
 *   expression needs the order of the factors reversed and nothing else.
 *
 *   Multiplication order follows from that: xud_matrix_multiply(out, left, right) is left*right,
 *   which applied to a column vector spends `right` FIRST. That is why a placement is built
 *   translate*rotate*scale rather than the other way round - the scale meets the vertex before the
 *   rotation does, and the rotation before the translation does, which is what xud_matrix_compose()
 *   exists to get right once.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#ifndef XUD_MATRIX_H_
#define XUD_MATRIX_H_

#include "xud.h" /* Required for: XudResult, XudVector3 */

#define XUD_MATRIX_ELEMENT_COUNT                                                                                                           \
    16u /* The length of every matrix this rung writes, in ONE place \
                                       rather than as a 16 at every loop bound and every guard. It \
                                       is the same 16 xud.h's XUD_CAMERA_MATRIX_ELEMENT_COUNT names \
                                       and the same float[16] the descriptor's block and the _ex    \
                                       draws take, so the count is a fact about the layout rather   \
                                       than a choice.                                             \
                                    */

#define XUD_MATRIX_AXIS_MIN_LENGTH_SQUARED                                                                                                 \
    1e-12f /* An axis whose SQUARED length is this small or smaller has no direction: the  \
             normalise divides by a length that a float got to by cancelling nearly equal  \
             components, so the rotation it would build is a spin about an axis that is  \
             mostly rounding error. The square is compared rather than the length because  \
             the length needs a sqrt that the well-formedness test does not. 1e-12 is the  \
             square of raymath's own EPSILON (raymath.h 15: 1e-8f), which is the floor its  \
             Vector3Normalize uses before it hands back the zero vector.                   \
          */

#endif /* XUD_MATRIX_H_ */
