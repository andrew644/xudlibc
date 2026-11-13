#version 460 core

/* present.frag - the resolve pass's fragment stage: the one place the high-dynamic-range scratch
   image becomes the window's ordinary 8-bit picture.

   The scene is drawn into an R16G16B16A16_SFLOAT (or wider) scratch image whose values are LINEAR
   light, and the swapchain's format is an SRGB_NONLINEAR row - so the hardware converts this
   stage's output linear->sRGB on write, and everything below must stay in linear space. An ACES
   filmic curve maps the unbounded linear values onto the display's [0,1]: without it an HDR value
   of 4.0 would clamp flat white and lose the highlight ROLL-OFF that is the whole point of
   rendering wider than the screen. Narkowicz's rational approximation is the one used here: five
   constants, no transcendental, and every driver on the machine evaluates it identically, which is
   what keeps the readback test deterministic.

   The sampler is declared as ONE sampler2D at set 1 binding 0 - the exact shape of set 1's layout
   and of every slot set the descriptor rung allocates against it: the frame loop binds the scratch
   image's own slot set, exactly as a textured batch draw binds its texture's slot set (see the long
   note at the top of shaders/solid_textured.frag for why the slot is a set and not an index).

   There is no push-constant block here. The pipeline layout carries the batch's push range, but a
   stage may ignore a push range it does not read, and the resolve pass pushes nothing. */

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 1, binding = 0) uniform sampler2D xudSampler;

vec3 xudTonemapAces(vec3 linear)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((linear * (a * linear + b)) / (linear * (c * linear + d) + e), 0.0, 1.0);
}

void main()
{
    vec4 sampled = texture(xudSampler, inUv);
    outColor = vec4(xudTonemapAces(sampled.rgb), sampled.a);
}
