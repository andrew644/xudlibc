/***********************************************************************************************
 *
 *   xud_light.h - the light rung: the table of suns, points and spots a frame is shaded by
 *
 *   Owns : the meaning of the public XudLight (xud.h) - which fields each kind reads, what range
 *          and the two cone angles mean, and the refusals a record that cannot light anything
 *          earns - plus the one conversion between that record and the frame block's XudLightRow
 *          (xud_descriptor.h), and xud_light_reset(), the seeding of the default table: one white
 *          SUN along normalize(1, 2, 3) over a quarter-white ambient floor, which is the fixed
 *          light and floor the fragment stage used to compile in before this rung existed.
 *   Layer: above the descriptor - the table's BYTES are the descriptor rung's and this rung hands
 *          rows down through its setters, the same accessor rule the camera rung applies to the
 *          viewProj - and beside the camera: the state itself is the context's (xud_core.h holds
 *          the table inline, no allocation, no teardown), and every call here is host arithmetic
 *          over a memcpy away from the block the frame uploads anyway. There is no rung object:
 *          nothing here creates a driver resource, so there is nothing to open, close or order in
 *          a teardown, and the default test run asserts the conversions, the refusals and the
 *          block's bytes with no device present.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#ifndef XUD_LIGHT_H_
#define XUD_LIGHT_H_

#include "xud.h" /* Required for: XudResult, XudLight, XudContext - the public currency this rung implements */
#include "xud_descriptor.h" /* Required for: XudDescriptor, XudLightRow - the block bytes this rung hands down through the descriptor's setters */
#include <math.h> /* Required for: sqrtf/cosf/isfinite - the vector lengths, the cone cosines and the finiteness the refusals read */

/*-----------------------------------------------------------------------------------------------
 * The pure half: one record's refusal and one record's row
 *-----------------------------------------------------------------------------------------------*/

/// The record's refusal pass: what THIS kind reads is what this call checks - a SUN's position may
/// hold anything because a sun lights from its direction alone, and a POINT's cone angles are not
/// read and not checked. XUD_INVALID_ARG on a NULL light, a kind outside the tokens, a non-finite
/// float among the fields the kind reads (the vectors componentwise), a negative intensity or
/// range, a SUN or SPOT whose direction is the zero vector - the Lambert term of a light that
/// points nowhere cannot be taken - or a SPOT whose outer cone is narrower than its inner.
XudResult xud_light_validate(const XudLight* light);

/// The conversion, asked AFTER xud_light_validate() (it refuses nothing a validated record can
/// raise, and it is the validator, not this row, that owns the refusals): the four vec4s the block
/// carries. A SUN's position member holds its NORMALIZED direction and its range member zero - a
/// sun is at infinity and infinity has no attenuation; a POINT's target and cone members are zero;
/// a SPOT's target holds its normalized axis pointing away from the light and its cone members the
/// cosines of the two angles. The color member is the rgb over 255 with the intensity in w, so one
/// multiply in the shader scales the rgb by the intensity.
XudResult xud_light_row_fill(XudLightRow* outRow, const XudLight* light);

/*-----------------------------------------------------------------------------------------------
 * The state half: the context's table and its one push
 *-----------------------------------------------------------------------------------------------*/

/// The seeding xud_init_context() runs right after the renderer opened - there is no block to push
/// into before it, and a context that comes back XUD_OK must already shade like the fixed-light
/// stage did: the default sun and floor go into the context's fields AND into the descriptor's
/// block, one call, both. XUD_INVALID_ARG on a NULL context; XUD_UNSUPPORTED on a context whose
/// descriptor never came up.
XudResult xud_light_reset(XudContext* context);

/*-----------------------------------------------------------------------------------------------
 * The shadow map's geometry (S2.5): the box the casting sun sees, and the matrix that maps it
 *-----------------------------------------------------------------------------------------------*/

#define XUD_SHADOW_MAP_SIZE                                                                                                                \
    2048u /* the occluder map's edge in texels, and the one place that edge is written down: the   \
             renderer sizes its depth image and framebuffer by it, and the block's texel-size      \
             row is its reciprocal - the compare that reads one had better read the other too.     \
             A power of two because clamp-to-edge texel math wants one.                            \
     */
#define XUD_SHADOW_EXTENT                                                                                                                  \
    40.0f /* half-edge of the world cube the map covers, centred on the camera: the box is the     \
             camera's neighbourhood rather than the whole scene because a fixed box around the      \
             action spends its 2048 texels where the player can see them, and a scene-sized box    \
             spends most of them on empty ground. Outside the box a surface is LIT by the          \
             fragment's own out-of-range answer - a stated limit, not a wrong shadow.              \
     */
#define XUD_SHADOW_DEPTH_BIAS                                                                                                              \
    0.0025f /* the depth the compare moves away from the surface asking: reversed-linear depth     \
               over the box's own span, so this is about two parts in a thousand of the near       \
               half of it. Zero bias is acne - rasterization lands a surface's fragment a hair     \
               past the depth its own triangles wrote, and the hair walks in straight lines.       \
     */

/// Pure: the casting sun's view-projection - the 64 bytes the block's lightViewProj row carries,
/// in the same column-major order as the camera's own. The view half is xud_camera_fill_view_proj
///()'s basis (right/up/minus-forward rows, the eye pushed back through them) with the sun's
/// direction as forward and the world +Y as up - swapped to +Z when the sun shines along Y, since
/// a basis whose up is parallel to its forward is no basis. The eye sits two extents behind the
/// focus so the map's depth interval [-extent, +extent] along the light is centred on the camera.
/// The projection half is the REVERSED ORTHO the map's compare reads: x and y scaled by 1/extent,
/// z linear in the distance (1 at the eye-side plane, 0 at the far one - ortho has no perspective
/// divide to fight), w pinned at 1 so what the vertex stage writes IS what the map stores. XUD_
/// INVALID_ARG on a NULL out, focus or direction, a non-finite or non-positive extent, or a non-
/// finite or zero direction - the same refusals the validator makes for a sun's direction.
XudResult xud_light_shadow_view_proj(float* outViewProj, const XudVector3* focus, const XudVector3* sunDirection, float extent);

/// The per-frame push xud_begin_camera_3d() makes beside the camera's own: the FIRST sun in the
/// context's table with a positive intensity is the casting sun, its matrix is filled from the
/// camera's position as the focus, and the block's four shadow rows - matrix, texel size, bias,
/// enable - go to the descriptor in one call. No sun with something to cast: the identity matrix
/// and enable 0.0, which is the fragment stage's standing order to answer every lookup LIT
/// before it reads a texel - the map is still sampled by every draw (binding 1 has contents),
/// it is just asked no more. Several suns: the FIRST casts and ALL read its map - the block
/// carries one matrix, and a second sun's shadows would need a second map and a second row.
/// XUD_INVALID_ARG on a NULL context or focus; XUD_UNSUPPORTED on a context whose descriptor
/// never came up; the setter's own answer otherwise.
XudResult xud_light_shadow_apply(XudContext* context, const XudVector3* focus);

#endif /* XUD_LIGHT_H_ */
