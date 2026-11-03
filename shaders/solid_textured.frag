#version 450 core

/* solid_textured.frag - the textured column of the pipeline list's variant axis, the third column
   of the same axis the opaque and transparent columns are the first two of.

   It differs from solid.frag in exactly two things, and both are why a second fragment module
   exists at all: it reads set 1, and it multiplies the interpolated colour by the sampled texel
   instead of by a fixed light's Lambert term. The vertex stage, the attribute locations, the set-0
   block and the push row are shared unchanged - which is what lets the whole list stay ONE
   pipeline layout and every vkCmdBindDescriptorSets name one layout.

   The tint is the interpolated vertex colour rather than a member of the push row: the push row
   XudPushRow has no colour in it (mat4 + four uints, the C layout of XudPushRow in xud_descriptor.h
   read against std140) and the shapes rung writes a per-vertex colour every shape appends, so the
   tint is already a channel that reaches this stage. A glyph atlas's colour comes from the atlas
   itself and its alpha is the glyph's coverage, which is why the multiply below keeps alpha
   (unlike solid.frag, which keeps the vertex alpha and replaces the rgb with a lit shade).

   The sampler is declared as ONE sampler, not as an array, and the slot is chosen by the frame
   loop's vkCmdBindDescriptorSets firstDescriptorIndex rather than by an index in this stage. That
   is the whole shape of this stage and it is deliberate: a non-constant index into a descriptor
   array is the SPIR-V SampledImageArrayNonUniformIndexing capability, which is the core feature
   shaderSampledImageArrayNonUniformIndexing, which xud_physical.c's wanted-feature list
   DELIBERATELY does not ask for (the "shader*DynamicIndexing quartet" paragraph there). Asking
   for it would be asking for a feature the library has no other use for; binding a slice of the
   set instead needs no feature at all and costs one argument on a call the frame loop already
   makes. The layout's binding is still the 16-deep array of XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE in
   xud_descriptor.h, and a shader that declares one descriptor at a binding the layout declares
   N-wide is legal for any N >= 1, which is the direction that VUID reads.

   The two input locations name the vertex stage's outputs: location 0 is outUv and location 1 is
   outColor in shaders/solid.vert, and a frag input whose location names nothing the vertex stage
   writes is refused by the validation layer rather than forgiven, so the pair is a fact and not a
   convention (solid.frag reads location 1 for the colour and location 2 for the normal, which are
   the same two outputs). */

layout(location = 0) in vec2 inUv;
layout(location = 1) in vec4 inColor;
layout(location = 0) out vec4 outColor;

layout(set = 1, binding = 0) uniform sampler2D xudSampler;

void main()
{
    outColor = inColor * texture(xudSampler, inUv);
}
