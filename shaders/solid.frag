#version 460 core

/* solid.frag - the untextured-solid variant, shaded by the frame's light table.

   One fixed direction and a compile-in ambient floor used to live here. They are gone: the
   direction, the floor and now a TABLE of up to 8 lights come out of set 0's block, filled by
   the light rung (xud_light.c) from the caller's xud_set_lights() and xud_set_ambient_light().
   The block member order, widths and the 8 are the C layout of XudFrameUniformRow and
   XudLightRow (xud_descriptor.h) read against std140 - mat4 at 0, vec3 at 64, float at 76,
   vec4s at 80 and 96, the sun's mat4 at 112, the shadow vec4 at 176, uints at 192 and 196,
   the 8-byte pad at 200, the struct array at 208 with stride 64 - and the asserts beside
   those structs are the same numbers from the other side. The 8 restates XUD_MAX_LIGHTS
   because shader and C are two languages and one number must be spelled in both, the way
   the UI variant's 3 is.

   The sun's own two block rows and the shadow map at set 0's binding 1 are the depth pre-pass's
   stage-mates (S2.5): the FIRST sun in the table casts, the shadow pass has just rasterized
   the scene's opaque triangles from that sun's point of view into a depth-only map, and
   xudShadowFactor() below asks that map, per shaded point, whether the sun's own ray reaches
   it. One map, one casting sun, one enable flag - a table of eight shadow casters would be
   eight passes and eight sampler slots, and the first sun is the one every scene lights its
   floor with.


   The shade is a SUM and is deliberately not clamped: the scene draws into the renderer's
   float scratch image, a white sun at intensity 2 is legal, and the resolve stage's ACES
   curve is what folds brightness back into displayable range. The min() against 1 this stage
   used to end with would have been the curve's job done twice, badly.

   The three kinds branch on params.w - the same numbers as xud.h's XUD_LIGHT_KIND_* tokens.
   A SUN's position member carries its unit direction (normalized by the rung, not here); a
   POINT and a SPOT light from their position. Attenuation is (1 - d/range)^2 clamped at 0,
   the squared-linear form raylib's lighting example uses: continuous at the range boundary
   where a plain 1/d^2 is a wall of infinity. A SPOT additionally fades across its cone with
   smoothstep over the two cosines the row carries - cosines because cosine falls as the
   angle opens, so the smoothstep reads outer-first, inner-second and lights the axis fully.
   Standing on a point light is the one degenerate position (distance 0 has no direction to
   take); the contribution falls to zero there rather than dividing by it.

   The UI variant (flags == 3, xud_batch.h's XUD_BATCH_VARIANT_UI) takes NONE of this: its
   positions are clip space already, a world-space light would shade screen-fixed overlays by
   distances they do not live in, and raylib's 2D is flat for the same reason. A UI row gets
   shade 1 - exactly the flat colour its append carried, which is what the old clamp happened
   to produce for these rows and what the branch now states instead of implying.

   The normal is still renormalized here: a sphere's interpolated vertex normals shorten
   toward the middle of its triangles, and a SHORTER normal dims a face that faces the light
   dead-on. The world position is the vertex stage's location 3 - the draw's model matrix
   applied to the vertex position, the space the lights' positions and the camera live in. */

struct XudLightRow {
    vec4 position; // 0  - the light's position; a SUN stores its unit direction-THERE instead
    vec4 target;   // 16 - a SPOT's unit cone axis pointing AWAY from the light; zero otherwise
    vec4 color;    // 32 - rgb 0..1 and w = the intensity the rgb is scaled by
    vec4 params;   // 48 - x range (0 = no falloff), y inner cone cosine, z outer cone cosine, w kind
};

layout(binding = 0, std140) uniform XudFrameBlock {
    mat4 viewProj;      // 0   - the VP matrix, column-major
    vec3 cameraPos;     // 64  - unread here: Lambert needs no eye
    float time;         // 76  - unread
    vec4 fog;           // 80  - unread: the fog step's
    vec4 ambient;       // 96  - rgb + intensity, the floor every face keeps
    mat4 lightViewProj; // 112 - the casting sun's own VP: the matrix that folds the shaded point
                        //     into the shadow map's texel space below
    vec4 shadowParams;  // 176 - x,y texel size, z compare bias, w 1.0 while a casting sun lives
    uint lightCount;    // 192 - how many table rows to sum
    uint reserved;      // 196
    vec2 tablePad;      // 200 - the std140 struct-array alignment, spelled vec2 and NOT float[2]:
                        //     a std140 scalar array strides to 8, which would slide this member to
                        //     216 and shade the scene out of existence one language at a time
    XudLightRow lights[8]; // 208 - XUD_MAX_LIGHTS (xud.h), restated for the shader's language
} xudFrame;

layout(set = 0, binding = 1) uniform sampler2DShadow xudShadowMap;

layout(push_constant) uniform XudPushRow {
    mat4 modelMatrix;    // 0  - the draw's own model matrix, column-major
    uint materialIndex;  // 64 - which material slot this draw uses (unread today)
    uint textureIndex;   // 68 - which entry of set 1's sampler array (unread today)
    uint flags;          // 72 - the variant this draw was flushed under
    uint reserved;       // 76 - keeps the row 16-aligned with no hole
    vec4 tint;           // 80 - the draw's own tint, 0..1 per channel
} xudPush;

layout(location = 1) in vec4 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec3 inWorldPos;
layout(location = 0) out vec4 outColor;

const uint xudLightKindSun = 0u;   // XUD_LIGHT_KIND_SUN (xud.h), restated for this language
const uint xudLightKindSpot = 2u;  // XUD_LIGHT_KIND_SPOT (xud.h), same
const uint xudPushFlagVariantUI = 3u; // XUD_BATCH_VARIANT_UI (xud_batch.h), same rule

float xudShadowFactor(vec3 worldPos)
{
    if (xudFrame.shadowParams.w <= 0.0f) {
        return 1.0f;
    }

    vec4 lightPos = xudFrame.lightViewProj * vec4(worldPos, 1.0f);
    vec3 lightNdc = lightPos.xyz / lightPos.w;
    /* The map's rows follow the framebuffer's y-down convention - the shadow vertex stage's
       flip lands this surface's depth at row (−lightNdc.y * 0.5 + 0.5) * S - so the sample's
       v is the negated light-space y. */
    vec2 texel = vec2(lightNdc.x, -lightNdc.y) * 0.5 + 0.5;

    /* Outside the sun's box there is no occluder the map could have recorded, and a clamp to the
       edge would sample a neighbour's depth: out of range is LIT, stated, not sampled. Above 1 in
       light depth is between the surface and the sun's near plane, which the shadow pass clips,
       and it gets the same honest answer. */
    if ((texel.x < 0.0) || (texel.x > 1.0) || (texel.y < 0.0) || (texel.y > 1.0) || (lightNdc.z > 1.0)) {
        return 1.0f;
    }

    /* The compare: the map stores, per texel, the depth NEAREST the sun (the reversed contest
       the shadow pass writes with GREATER, and D32 stores the larger value for nearer). A
       comparison sampler evaluates compareOp(reference, texel) and the shadow sampler is built
       GREATER_OR_EQUAL: lit when this surface's own biased depth reaches the stored owner's.
       The surface that OWNS its texel stores its own depth, equal but for rounding, and the
       bias (shadowParams.z) keeps that equality a hair toward LIT - rasterization slop across
       a texel boundary is acne and acne walks in straight lines. An occluder nearer the sun
       stored a larger value than the reference, the compare fails, and the surface is shadowed.
    */
    float reference = lightNdc.z + xudFrame.shadowParams.z;

    /* 3x3 PCF: one tap is a hard edge of whole texels, and the map's texels are wide in world
       space this far from the sun; nine taps cost nothing against the shade they smooth. */
    float lit = 0.0;
    for (int offsetY = -1; offsetY <= 1; offsetY++) {
        for (int offsetX = -1; offsetX <= 1; offsetX++) {
            vec2 tapped = texel + vec2(float(offsetX), float(offsetY)) * xudFrame.shadowParams.xy;
            lit += texture(xudShadowMap, vec3(tapped, reference));
        }
    }
    return lit / 9.0;
}

vec3 xudLightContribution(XudLightRow light, vec3 normal, vec3 worldPos, float shadow)
{
    vec3 toLight;
    float distance = 0.0f;

    if (light.params.w == xudLightKindSun) {
        toLight = light.position.xyz;
    } else {
        vec3 delta = light.position.xyz - worldPos;
        distance = length(delta);
        toLight = (distance > 0.0f) ? (delta / distance) : vec3(0.0f);
    }

    float lambert = max(dot(normal, toLight), 0.0f);

    float attenuation = 1.0f;
    if (light.params.x > 0.0f) {
        attenuation = max(1.0f - (distance / light.params.x), 0.0f);
        attenuation = attenuation * attenuation;
    }

    if (light.params.w == xudLightKindSpot) {
        attenuation *= smoothstep(light.params.z, light.params.y, dot(-toLight, light.target.xyz));
    }

    /* The shadow rides the sun's term alone: the map was rendered from the casting sun's point of
       view, so its answer means nothing to a point or a spot, and the caller hands 1.0 for every
       row that is not a sun rather than making this branch own the lookup.
    */
    float sunshade = (light.params.w == xudLightKindSun) ? shadow : 1.0f;
    return light.color.rgb * light.color.w * lambert * attenuation * sunshade;
}

void main()
{
    vec3 shade;

    if (xudPush.flags == xudPushFlagVariantUI) {
        shade = vec3(1.0f);
    } else {
        shade = xudFrame.ambient.rgb * xudFrame.ambient.w;
        float sunShadow = xudShadowFactor(inWorldPos);
        for (uint index = 0u; index < xudFrame.lightCount; index++) {
            shade += xudLightContribution(xudFrame.lights[index], normalize(inNormal), inWorldPos, sunShadow);
        }
    }

    outColor = vec4(inColor.rgb * shade * xudPush.tint.rgb, inColor.a * xudPush.tint.a);
}
