#version 460 core

/* bloom_blur.frag - the bloom chain's blur stage: each level blurs the one before it while
   halving again, so a wide glow costs one small pass per level instead of many full-window ones.

   The source is one entry of the bindless array at set 0 binding 2, named by the push row the
   frame loop records before this level's draw - the bright pass pushes the scratch entry, every
   blur row pushes the level-below's. One sampler declaration, one pipeline for the whole chain ta
   il. The array does hold the level this very pass is WRITING (the chain's four levels are resolv
   e-time entries too), but a layout check bites only the descriptors a draw actually samples, and
   this stage's ten fetches name exactly one entry, always the level-before.

   The output level itself is the framebuffer this pass draws into, never the sampler: writing a
   level while sampling it is the one hazard no layout can police, and the level-before-source
   rule is what keeps every read a pass behind.

   The kernel is the 3x3 Gaussian walked at 2.5 texels, nine bilinear taps: at half the source's
   size each tap is already a 2x2 average of the level above, so nine samples sweep a wide enough
   footprint for a soft glow and every driver evaluates the same nine texture() calls
   identically, which is what keeps the readback test deterministic. The weights are the binomial
   1-2-1 walked twice, normalised by the 16 they sum to. */

#extension GL_EXT_nonuniform_qualifier : require

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform XudPushRow {
    mat4 modelMatrix;    // 0  - unread
    uint materialIndex;  // 64 - unread
    uint textureIndex;   // 68 - the source entry this level blurs
    uint flags;          // 72 - unread
    uint reserved;       // 76
    vec4 tint;           // 80 - unread
} xudPush;

layout(set = 0, binding = 2) uniform sampler2D xudSampler[512];  // XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE, restated for GLSL

void main()
{
    vec2 texel = 1.0 / vec2(textureSize(xudSampler[nonuniformEXT(xudPush.textureIndex)], 0));
    vec2 offset = texel * 2.5;
    vec3 sum = vec3(0.0);

    sum += texture(xudSampler[nonuniformEXT(xudPush.textureIndex)], inUv + vec2(-offset.x, -offset.y)).rgb * 1.0;
    sum += texture(xudSampler[nonuniformEXT(xudPush.textureIndex)], inUv + vec2(0.0, -offset.y)).rgb * 2.0;
    sum += texture(xudSampler[nonuniformEXT(xudPush.textureIndex)], inUv + vec2(offset.x, -offset.y)).rgb * 1.0;
    sum += texture(xudSampler[nonuniformEXT(xudPush.textureIndex)], inUv + vec2(-offset.x, 0.0)).rgb * 2.0;
    sum += texture(xudSampler[nonuniformEXT(xudPush.textureIndex)], inUv).rgb * 4.0;
    sum += texture(xudSampler[nonuniformEXT(xudPush.textureIndex)], inUv + vec2(offset.x, 0.0)).rgb * 2.0;
    sum += texture(xudSampler[nonuniformEXT(xudPush.textureIndex)], inUv + vec2(-offset.x, offset.y)).rgb * 1.0;
    sum += texture(xudSampler[nonuniformEXT(xudPush.textureIndex)], inUv + vec2(0.0, offset.y)).rgb * 2.0;
    sum += texture(xudSampler[nonuniformEXT(xudPush.textureIndex)], inUv + vec2(offset.x, offset.y)).rgb * 1.0;
    outColor = vec4(sum * (1.0 / 16.0), 1.0);
}
