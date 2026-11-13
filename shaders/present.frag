#version 460 core

/* present.frag - the resolve pass's fragment stage: the one place the high-dynamic-range scratch
   image becomes the window's ordinary 8-bit picture, glow included and edges smoothed.

   Three jobs run in the order the picture needs them:

   BLOOM. The frame's bright passes wrote a four-level chain of the over-white light (see
   bloom_bright.frag, bloom_blur.frag). Sampling all four levels and summing with falling weights
   paints a soft halo whose footprint reaches four 16-times-downsampled texels wide - one tight
   ring of glow from the half-res row and three wider, dimmer washes behind it. The sum lands in
   LINEAR light and joins the scratch sample BEFORE the tonemap, so the glow compresses through
   the same ACES curve as the scene and a blown highlight rolls off instead of blowing out twice.

   TONEMAP. An ACES filmic curve maps the unbounded linear values onto the display's [0,1]:
   without it an HDR value of 4.0 would clamp flat white and lose the highlight ROLL-OFF that is
   the whole point of rendering wider than the screen. Narkowicz's rational approximation is the
   one used here: five constants, no transcendental, and every driver on the machine evaluates it
   identically, which is what keeps the readback test deterministic.

   FXAA. The triangle rasteriser decides coverage per pixel with no filter, so a bright wall edge
   against a dark floor is a one-pixel jump. FXAA walks that jump: it measures the four diagonal
   neighbours' luminance, skips every pixel whose local range is small (flat areas and true
   gradients exit with the UNTOUCHED centre pixel - the readback probes of the whole suite read
   through this stage and must keep reading exact values), and on a real edge blends the centre
   with two samples walked along the edge's own direction. The blend happens in linear light -
   the same space the tonemap just produced and the space the SRGB write to the swapchain encodes
   from - and the luminance that steers it is the tonemapped luminance, so the edge metric sees
   what the eye will see while the blended colour stays physically mixed.

   The scratch sampler is the slot set the frame loop binds for this pass - set 1 binding 0, the
   exact shape of set 1's layout - and the bloom chain is set 0 binding 2, the per-frame array.
   There is no push-constant block here; the layout's push range belongs to the batch's draw rows. */

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 1, binding = 0) uniform sampler2D xudSampler;
layout(set = 0, binding = 2) uniform sampler2D xudBloom[4];

const vec3 xudLumaWeights = vec3(0.2126, 0.7152, 0.0722);

vec3 xudTonemapAces(vec3 linear)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((linear * (a * linear + b)) / (linear * (c * linear + d) + e), 0.0, 1.0);
}

vec3 xudSceneColour(vec2 uv)
{
    vec3 bloom = vec3(0.0);
    bloom += textureLod(xudBloom[0], uv, 0.0).rgb * 0.60;
    bloom += textureLod(xudBloom[1], uv, 0.0).rgb * 0.40;
    bloom += textureLod(xudBloom[2], uv, 0.0).rgb * 0.25;
    bloom += textureLod(xudBloom[3], uv, 0.0).rgb * 0.15;
    vec3 sampled = texture(xudSampler, uv).rgb;
    return xudTonemapAces(sampled + bloom);
}

void main()
{
    vec2 texel = 1.0 / vec2(textureSize(xudSampler, 0));
    vec3 centre = xudSceneColour(inUv);
    float centreLuma = dot(centre, xudLumaWeights);

    vec3 northWest = xudSceneColour(inUv + texel * vec2(-0.5, -0.5));
    vec3 northEast = xudSceneColour(inUv + texel * vec2(0.5, -0.5));
    vec3 southWest = xudSceneColour(inUv + texel * vec2(-0.5, 0.5));
    vec3 southEast = xudSceneColour(inUv + texel * vec2(0.5, 0.5));
    float lumaNorthWest = dot(northWest, xudLumaWeights);
    float lumaNorthEast = dot(northEast, xudLumaWeights);
    float lumaSouthWest = dot(southWest, xudLumaWeights);
    float lumaSouthEast = dot(southEast, xudLumaWeights);

    float lumaLow = min(centreLuma, min(min(lumaNorthWest, lumaNorthEast), min(lumaSouthWest, lumaSouthEast)));
    float lumaHigh = max(centreLuma, max(max(lumaNorthWest, lumaNorthEast), max(lumaSouthWest, lumaSouthEast)));
    if ((lumaHigh - lumaLow) < max(0.125 * centreLuma, 0.05)) {
        outColor = vec4(centre, texture(xudSampler, inUv).a);
        return;
    }

    vec2 dir;
    dir.x = -((lumaNorthWest + lumaNorthEast) - (lumaSouthWest + lumaSouthEast));
    dir.y = ((lumaNorthWest + lumaSouthWest) - (lumaNorthEast + lumaSouthEast));
    float dirReduce = max((lumaNorthWest + lumaNorthEast + lumaSouthWest + lumaSouthEast) * (0.25 * 0.25), 0.0078125);
    vec2 dirWalk = clamp(dir * (1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce)), vec2(-8.0), vec2(8.0)) * texel;

    vec3 blendNear = 0.5 * (xudSceneColour(inUv - dirWalk * (1.0 / 3.0)) + xudSceneColour(inUv + dirWalk * (1.0 / 3.0)));
    vec3 blendWide = 0.5 * blendNear + 0.25 * (xudSceneColour(inUv - dirWalk * 0.5) + xudSceneColour(inUv + dirWalk * 0.5));
    float blendWideLuma = dot(blendWide, xudLumaWeights);
    vec3 blended = ((blendWideLuma < lumaLow) || (blendWideLuma > lumaHigh)) ? blendNear : blendWide;
    outColor = vec4(mix(centre, blended, 0.75), texture(xudSampler, inUv).a);
}
