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

#endif /* XUD_LIGHT_H_ */
