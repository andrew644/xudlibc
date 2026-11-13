#version 460 core

/* solid_textured.frag - the textured column of the pipeline list's variant axis, the third column
   of the same axis the opaque and transparent columns are the first two of.

   It differs from solid.frag in three things, and each is why a second fragment module exists
   at all: it reads the sampler array, it multiplies the interpolated colour by the sampled texel,
   and it takes NO light. Glyph atlases, UI atlases and model maps are authored as the final colour
   - a glyph that darkens when it turns away from a lamp is a bug report, not a lighting effect - s
   o this stage shades by nothing and the shade sum, the kinds and the ambient of solid.frag all
   stay over there. The vertex stage, the attribute locations and the push row are shared
   unchanged - which is what lets the whole list stay ONE pipeline layout and every pass make
   exactly one descriptor bind.

   The tint is the interpolated vertex colour rather than a member of the push row: the push row
   XudPushRow has no colour in it (mat4 + four uints, the C layout of XudPushRow in xud_descriptor.h
   read against std140) and the shapes rung writes a per-vertex colour every shape appends, so the
   tint is already a channel that reaches this stage. A glyph atlas's colour comes from the atlas
   itself and its alpha is the glyph's coverage, which is why the multiply below keeps alpha
   (unlike solid.frag, which keeps the vertex alpha and replaces the rgb with a lit shade).

   The sampler is one entry of the bindless array at set 0 binding 2, named by the push row's
   textureIndex - the frame loop pushes the row beside every draw, so the texture a draw samples
   travels with the draw and nothing binds between the pass's one set-0 bind and the last draw.
   The non-constant index is the SPIR-V SampledImageArrayNonUniformIndexing capability, spelled by
   GL_EXT_nonuniform_qualifier's nonuniformEXT(): the feature shaderSampledImageArrayNonUniformInd
   exing is core since Vulkan 1.2, under this library's 1.3 floor, so it needs no host-side enable
   - only the capability the extension line below emits. Every entry of the array carries a sampler
   and a written view from bring-up (entry 0 is the white default an untextured draw's zeroed inde
   x lands on), so no draw can index an unwritten slot.

   The two input locations name the vertex stage's outputs: location 0 is outUv and location 1 is
   outColor in shaders/solid.vert, and a frag input whose location names nothing the vertex stage
   writes is refused by the validation layer rather than forgiven, so the pair is a fact and not a
   convention (solid.frag reads location 1 for the colour and location 2 for the normal, which are
   the same two outputs). */

#extension GL_EXT_nonuniform_qualifier : require

layout(push_constant) uniform XudPushRow {
    mat4 modelMatrix;    // 0  - the draw's own model matrix, column-major
    uint materialIndex;  // 64 - which material slot this draw uses (unread today)
    uint textureIndex;   // 68 - which entry of set 0's sampler array this draw samples
    uint flags;          // 72 - the variant this draw was flushed under
    uint reserved;       // 76 - keeps the row 16-aligned with no hole
    vec4 tint;           // 80 - the draw's own tint, 0..1 per channel
} xudPush;

layout(location = 0) in vec2 inUv;
layout(location = 1) in vec4 inColor;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 2) uniform sampler2D xudSampler[512];  // XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE, restated for GLSL

void main()
{
    outColor = inColor * texture(xudSampler[nonuniformEXT(xudPush.textureIndex)], inUv) * xudPush.tint;
}
