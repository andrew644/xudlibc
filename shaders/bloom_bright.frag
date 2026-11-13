#version 460 core

/* bloom_bright.frag - the bloom chain's first stage: the bright-pass.

   The scratch image is full-window float colour; this stage writes a HALF-window image holding
   only the light bright enough to glow. The subtraction is the threshold: a pixel whose luminance
   sits under 1.0 - everything the un-bloomed screen shows as plain white or darker - contributes
   nothing, and a pixel above it keeps its colour scaled by how far over it sits. The scale keeps
   the hue: an orange lamp at luminance 3.0 passes its orange at 2/3 strength, not a white blob.

   The sampler is the scratch image's own slot set - set 1 binding 0, the same shape the resolve
   stage samples - bound by the frame loop for this pass exactly as it binds the resolve's. The
   fullscreen triangle's UVs come from the vertex stage the resolve uses; this stage reads one
   sample per output pixel and the output is half the size, so the sampling box covers four
   source pixels and the hardware's LINEAR filter averages them into the one texel.

   The threshold is 1.0 and hard-wired: it is the display's own white line under the ACES curve
   the resolve applies one stage later, and a tunable would be a per-frame push-constant question
   in a pass the plan wants stateless. */

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 1, binding = 0) uniform sampler2D xudSampler;

void main()
{
    vec3 sampled = texture(xudSampler, inUv).rgb;
    float luminance = dot(sampled, vec3(0.2126, 0.7152, 0.0722));
    float over = max(luminance - 1.0, 0.0);
    outColor = vec4(sampled * (over / max(luminance, 1e-5)), 1.0);
}
