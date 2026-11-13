#version 460 core

/* solid.frag - the untextured-solid variant, shaded by the frame's light table.

   One fixed direction and a compile-in ambient floor used to live here. They are gone: the
   direction, the floor and now a TABLE of up to 8 lights come out of set 0's block, filled by
   the light rung (xud_light.c) from the caller's xud_set_lights() and xud_set_ambient_light().
   The block member order, widths and the 8 are the C layout of XudFrameUniformRow and
   XudLightRow (xud_descriptor.h) read against std140 - mat4 at 0, vec3 at 64, float at 76,
   vec4s at 80 and 96, uints at 112 and 116, the 8-byte pad at 120, the struct array at 128
   with stride 64 - and the asserts beside those structs are the same numbers from the other
   side. The 8 restates XUD_MAX_LIGHTS because shader and C are two languages and one number
   must be spelled in both, the way the UI variant's 3 is.

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
    mat4 viewProj;    // 0   - the VP matrix, column-major
    vec3 cameraPos;   // 64  - unread here: Lambert needs no eye
    float time;       // 76  - unread
    vec4 fog;         // 80  - unread: the fog step's
    vec4 ambient;     // 96  - rgb + intensity, the floor every face keeps
    uint lightCount;  // 112 - how many table rows to sum
    uint reserved;    // 116
    vec2 lightPad;      // 120 - the std140 struct-array alignment, spelled vec2 and NOT float[2]:
                        //     a std140 scalar array strides to 8, which would slide this member to
                        //     144 and shade the scene out of existence one language at a time
    XudLightRow lights[8]; // 128 - XUD_MAX_LIGHTS (xud.h), restated for the shader's language
} xudFrame;

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

vec3 xudLightContribution(XudLightRow light, vec3 normal, vec3 worldPos)
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

    return light.color.rgb * light.color.w * lambert * attenuation;
}

void main()
{
    vec3 shade;

    if (xudPush.flags == xudPushFlagVariantUI) {
        shade = vec3(1.0f);
    } else {
        shade = xudFrame.ambient.rgb * xudFrame.ambient.w;
        for (uint index = 0u; index < xudFrame.lightCount; index++) {
            shade += xudLightContribution(xudFrame.lights[index], normalize(inNormal), inWorldPos);
        }
    }

    outColor = vec4(inColor.rgb * shade * xudPush.tint.rgb, inColor.a * xudPush.tint.a);
}
