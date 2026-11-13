/***********************************************************************************************
 * xud_light.c - the light rung's refusals, its one conversion and the table's push
 *
 *   The public pair (xud_set_lights and xud_set_ambient_light, xud.h) lives here beside the rung's
 * two pure rows, and all four reach the driver through ONE door: the descriptor's block setters.
 * The table is stored on the context (xud_core.h) in the PUBLIC record - the human-readable form a
 * caller could hand back to xud_set_lights() unchanged - and the block's XudLightRow bytes exist
 * only in the publish step's stack frame, rebuilt whole on every change. A patched-row publish
 * would need the block's rows to stay current with the table across every other writer of the
 * block, and "every other writer" is exactly the kind of invariant this repo does not hold in
 * comments; a whole-table publish makes the count and its rows one statement (xud_descriptor_set_
 * lights()'s own note) and costs 512 B of copying at the speed of a caller's keypress.
 *
 *   Nothing here creates, records or destroys a driver object. There is no rung struct and no open
 * or close: the state is four inline fields of the context, the defaults are seeded by
 * xud_light_reset() at the end of bring-up, and the frame's upload is the descriptor's, already
 * running every frame for viewProj.
 ***********************************************************************************************/

#include "xud_light.h"

#include "xud.h" /* Required for: XUD_LIGHT_KIND_*, XUD_MAX_LIGHTS - the axis and the bound */
#include "xud_core.h" /* Required for: XudContext - the context whose inline fields hold the table */
#include "xud_descriptor.h" /* Required for: xud_descriptor_set_lights, xud_descriptor_set_ambient - the one door to the block */

#include <stddef.h> /* Required for: NULL */

#define XUD_LIGHT_DEGREES_PER_RADIAN                                                                                                       \
    57.29577951308232087680f /* 180 / pi: the cone angles arrive in degrees because raylib's                                               \
                                examples' lights are authored in degrees, and the block's                                                \
                                cone members are cosines - the fold lives here, once.                                                    \
     */

/*-----------------------------------------------------------------------------------------------
 * The pure half: one record's refusal and one record's row
 *-----------------------------------------------------------------------------------------------*/

static bool xudLightVectorOk(XudVector3 vector) {
    return (isfinite(vector.x) != 0) && (isfinite(vector.y) != 0) && (isfinite(vector.z) != 0);
}

static float xudLightVectorLength(XudVector3 vector) {
    return sqrtf((vector.x * vector.x) + (vector.y * vector.y) + (vector.z * vector.z));
}

XudResult xud_light_validate(const XudLight* light) {
    if (light == NULL) {
        return XUD_INVALID_ARG;
    }
    if ((isfinite(light->intensity) == 0) || (light->intensity < 0.0f)) {
        return XUD_INVALID_ARG;
    }

    switch (light->kind) {
    case XUD_LIGHT_KIND_SUN:
        /* The one vector a sun has, and the one vector whose zero cannot be lit by: normalize()
           of the zero vector is a NaN, and a NaN direction is a Lambert term against nothing.
        */
        if ((xudLightVectorOk(light->direction) == false) || (xudLightVectorLength(light->direction) == 0.0f)) {
            return XUD_INVALID_ARG;
        }
        break;

    case XUD_LIGHT_KIND_POINT:
        if ((xudLightVectorOk(light->position) == false) || (isfinite(light->range) == 0) || (light->range < 0.0f)) {
            return XUD_INVALID_ARG;
        }
        break;

    case XUD_LIGHT_KIND_SPOT:
        /* A cone of zero opening lights nothing and is a record's mistake rather than a light's;
           the equality inner == outer is legal - it is the sharpest edge a spotlight can have.
        */
        if ((xudLightVectorOk(light->position) == false) || (xudLightVectorOk(light->direction) == false)
            || (xudLightVectorLength(light->direction) == 0.0f) || (isfinite(light->range) == 0) || (light->range < 0.0f)
            || (isfinite(light->innerConeDegrees) == 0) || (isfinite(light->outerConeDegrees) == 0) || (light->innerConeDegrees < 0.0f)
            || (light->outerConeDegrees < light->innerConeDegrees) || (light->outerConeDegrees > 180.0f)) {
            return XUD_INVALID_ARG;
        }
        break;

    default:
        return XUD_INVALID_ARG;
    }

    return XUD_OK;
}

XudResult xud_light_row_fill(XudLightRow* outRow, const XudLight* light) {
    float length = 0.0f;

    if ((outRow == NULL) || (light == NULL)) {
        return XUD_INVALID_ARG;
    }

    /* Every member zeroed before the kind's fields overwrite their own: the members a kind does
       not read must read ZERO in the block rather than stack noise, because the shader branches on
       the kind but the members are one shared row.
    */
    for (uint32_t index = 0u; index < 4u; index++) {
        outRow->position[index] = 0.0f;
        outRow->target[index] = 0.0f;
        outRow->color[index] = 0.0f;
        outRow->params[index] = 0.0f;
    }

    outRow->color[0u] = (float)light->color.r / 255.0f;
    outRow->color[1u] = (float)light->color.g / 255.0f;
    outRow->color[2u] = (float)light->color.b / 255.0f;
    outRow->color[3u] = light->intensity;

    switch (light->kind) {
    case XUD_LIGHT_KIND_SUN:
        /* The direction is normalized HERE rather than in the shader: the row then carries unit
           vectors by construction, the shader's dot() is a Lambert term without a normalize(), and
           the default sun's normalize(1, 2, 3) lands on the same float the fragment stage used to
           compile in.
        */
        length = xudLightVectorLength(light->direction);
        outRow->position[0u] = light->direction.x / length;
        outRow->position[1u] = light->direction.y / length;
        outRow->position[2u] = light->direction.z / length;
        outRow->params[0u] = 0.0f;
        break;

    case XUD_LIGHT_KIND_POINT:
        outRow->position[0u] = light->position.x;
        outRow->position[1u] = light->position.y;
        outRow->position[2u] = light->position.z;
        outRow->params[0u] = light->range;
        break;

    case XUD_LIGHT_KIND_SPOT:
        length = xudLightVectorLength(light->direction);
        outRow->position[0u] = light->position.x;
        outRow->position[1u] = light->position.y;
        outRow->position[2u] = light->position.z;
        outRow->target[0u] = light->direction.x / length;
        outRow->target[1u] = light->direction.y / length;
        outRow->target[2u] = light->direction.z / length;
        outRow->params[0u] = light->range;
        outRow->params[1u] = cosf(light->innerConeDegrees / XUD_LIGHT_DEGREES_PER_RADIAN);
        outRow->params[2u] = cosf(light->outerConeDegrees / XUD_LIGHT_DEGREES_PER_RADIAN);
        break;

    default:
        return XUD_INVALID_ARG;
    }

    outRow->params[3u] = (float)light->kind;

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The state half: the context's table and its one push
 *-----------------------------------------------------------------------------------------------*/

static XudResult xudLightPublish(const XudContext* context) {
    XudLightRow rows[XUD_MAX_LIGHTS];
    float ambient[4];
    XudResult result = XUD_OK;

    for (uint32_t index = 0u; index < context->lightCount; index++) {
        /* Unreachable refusal: every row of the table went through xud_light_validate() on the
           call that stored it, and the row builder refuses only what the validator refuses.
        */
        result = xud_light_row_fill(&rows[index], &context->lights[index]);
        if (result != XUD_OK) {
            return result;
        }
    }

    result = xud_descriptor_set_lights(context->descriptor, context->lightCount, rows);
    if (result != XUD_OK) {
        return result;
    }

    ambient[0u] = (float)context->ambientColor.r / 255.0f;
    ambient[1u] = (float)context->ambientColor.g / 255.0f;
    ambient[2u] = (float)context->ambientColor.b / 255.0f;
    ambient[3u] = context->ambientIntensity;

    return xud_descriptor_set_ambient(context->descriptor, ambient);
}

XudResult xud_light_reset(XudContext* context) {
    if (context == NULL) {
        return XUD_INVALID_ARG;
    }
    if (context->descriptor == NULL) {
        /* No block to publish into: the same "never completed bring-up" answer the public pair
           gives, and xud_init_context() is the only caller that can reach it - the call sits right
           after the renderer opened, so on the way that matters the field is never NULL.
        */
        return XUD_UNSUPPORTED;
    }

    context->lightCount = 1u;
    context->lights[0u].position = (XudVector3) { 0.0f, 0.0f, 0.0f };
    context->lights[0u].direction = (XudVector3) { 1.0f, 2.0f, 3.0f };
    context->lights[0u].color = (XudColor) { 255u, 255u, 255u, 255u };
    context->lights[0u].intensity = 1.0f;
    context->lights[0u].range = 0.0f;
    context->lights[0u].innerConeDegrees = 0.0f;
    context->lights[0u].outerConeDegrees = 0.0f;
    context->lights[0u].kind = XUD_LIGHT_KIND_SUN;
    context->ambientColor = (XudColor) { 255u, 255u, 255u, 255u };
    context->ambientIntensity = 0.25f;

    return xudLightPublish(context);
}

XudResult xud_set_lights(XudContext* context, const XudLight* lights, uint32_t count) {
    if (context == NULL) {
        return XUD_INVALID_ARG;
    }
    if (context->descriptor == NULL) {
        return XUD_UNSUPPORTED;
    }
    if ((count > XUD_MAX_LIGHTS) || ((lights == NULL) && (count != 0u))) {
        return XUD_INVALID_ARG;
    }

    /* All of them validated before any of them is stored: a table refused halfway would leave the
       context shading by the first half of a call that answered failure, and the repo's rule is
       the refusal leaves the state alone.
    */
    for (uint32_t index = 0u; index < count; index++) {
        if (xud_light_validate(&lights[index]) != XUD_OK) {
            return XUD_INVALID_ARG;
        }
    }

    for (uint32_t index = 0u; index < count; index++) {
        context->lights[index] = lights[index];
    }
    context->lightCount = count;

    return xudLightPublish(context);
}

XudResult xud_set_ambient_light(XudContext* context, XudColor color, float intensity) {
    float ambient[4];

    if (context == NULL) {
        return XUD_INVALID_ARG;
    }
    if (context->descriptor == NULL) {
        return XUD_UNSUPPORTED;
    }
    if ((isfinite(intensity) == 0) || (intensity < 0.0f)) {
        return XUD_INVALID_ARG;
    }

    context->ambientColor = color;
    context->ambientIntensity = intensity;

    ambient[0u] = (float)color.r / 255.0f;
    ambient[1u] = (float)color.g / 255.0f;
    ambient[2u] = (float)color.b / 255.0f;
    ambient[3u] = intensity;

    return xud_descriptor_set_ambient(context->descriptor, ambient);
}
