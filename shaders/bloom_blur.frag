#version 460 core

/* bloom_blur.frag - the bloom chain's blur stage: each level blurs the one before it while
   halving again, so a wide glow costs one small pass per level instead of many full-window ones.

   The source is set 1 binding 0 - the frame loop's slot shape, the same one the bright pass
   samples the scratch image through and the resolve samples it through - and the frame loop
   binds a different slot per level. One sampler, one pipeline for the whole chain tail. The
   chain's array in set 0 binding 2 stays the RESOLVE's view of it: a blur pipeline that read
   the array would statically reference every level, and validation reads "statically" strictly
   - the level this very pass is writing sits in COLOR_ATTACHMENT_OPTIMAL while its draw runs,
   and an array member naming SHADER_READ_ONLY in the descriptor is a mismatch the draw inherits
   just for existing in the array. A single bound sampler cannot make that claim about a level
   it does not name, which is also why the bright pass has always taken the scratch image as a
   slot rather than as a row of something.

   The output level itself is the framebuffer this pass draws into, never the sampler: writing a
   level while sampling it is the one hazard no layout can police, and the level-before-source
   rule is what keeps every read a pass behind.

   The kernel is the 3x3 Gaussian walked at 2.5 texels, nine bilinear taps: at half the source's
   size each tap is already a 2x2 average of the level above, so nine samples sweep a wide enough
   footprint for a soft glow and every driver evaluates the same nine texture() calls
   identically, which is what keeps the readback test deterministic. The weights are the binomial
   1-2-1 walked twice, normalised by the 16 they sum to. */

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 1, binding = 0) uniform sampler2D xudSource;

void main()
{
    vec2 texel = 1.0 / vec2(textureSize(xudSource, 0));
    vec2 offset = texel * 2.5;
    vec3 sum = vec3(0.0);

    sum += texture(xudSource, inUv + vec2(-offset.x, -offset.y)).rgb * 1.0;
    sum += texture(xudSource, inUv + vec2(0.0, -offset.y)).rgb * 2.0;
    sum += texture(xudSource, inUv + vec2(offset.x, -offset.y)).rgb * 1.0;
    sum += texture(xudSource, inUv + vec2(-offset.x, 0.0)).rgb * 2.0;
    sum += texture(xudSource, inUv).rgb * 4.0;
    sum += texture(xudSource, inUv + vec2(offset.x, 0.0)).rgb * 2.0;
    sum += texture(xudSource, inUv + vec2(-offset.x, offset.y)).rgb * 1.0;
    sum += texture(xudSource, inUv + vec2(0.0, offset.y)).rgb * 2.0;
    sum += texture(xudSource, inUv + vec2(offset.x, offset.y)).rgb * 1.0;
    outColor = vec4(sum * (1.0 / 16.0), 1.0);
}
