#version 460 core

/* bloom_bright.frag - the bloom chain's first stage: the bright-pass.

   The scratch image is full-window float colour; this stage writes a HALF-window image holding
   only the light bright enough to glow. The subtraction is the threshold: a pixel whose luminance
   sits under 1.0 - everything the un-bloomed screen shows as plain white or darker - contributes
   nothing, and a pixel above it keeps its colour scaled by how far over it sits. The scale keeps
   the hue: an orange lamp at luminance 3.0 passes its orange at 2/3 strength, not a white blob.

   The sampler is one entry of the bindless array at set 0 binding 2 - the scratch image's entry,
   named by the push row the frame loop records before this pass's one draw, exactly as the resolv
   e stage samples its own entry. The fullscreen triangle's UVs come from the vertex stage the res
   olve uses; this stage reads one sample per output pixel and the output is half the size, so the
   sampling box covers four source pixels and the hardware's LINEAR filter averages them into the
   one texel.

   The threshold is 1.0 and hard-wired: it is the display's own white line under the ACES curve
   the resolve applies one stage later, and a tunable would be a per-frame push-constant question
   in a pass the plan wants stateless. */

#extension GL_EXT_nonuniform_qualifier : require

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform XudPushRow {
    mat4 modelMatrix;    // 0  - unread
    uint materialIndex;  // 64 - unread
    uint textureIndex;   // 68 - the source entry this level reads
    uint flags;          // 72 - unread
    uint reserved;       // 76
    vec4 tint;           // 80 - unread
} xudPush;

layout(set = 0, binding = 2) uniform sampler2D xudSampler[512];  // XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE, restated for GLSL

void main()
{
    vec3 sampled = texture(xudSampler[nonuniformEXT(xudPush.textureIndex)], inUv).rgb;
    float luminance = dot(sampled, vec3(0.2126, 0.7152, 0.0722));
    float over = max(luminance - 1.0, 0.0);
    outColor = vec4(sampled * (over / max(luminance, 1e-5)), 1.0);
}
