#version 460 core

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

   The sampler is declared as ONE sampler, and the slot is chosen by the frame loop binding set 1
   to the slot set that holds that draw's texture - one descriptor set per texture slot, each set
   holding one view-and-sampler pair - rather than by an index in this stage. That is the whole
   shape of this stage and it is deliberate: a non-constant index into a descriptor array is the
   SPIR-V SampledImageArrayNonUniformIndexing capability, which is the core feature
   shaderSampledImageArrayNonUniformIndexing, which xud_physical.c's wanted-feature list
   DELIBERATELY does not ask for (the "shader*DynamicIndexing quartet" paragraph there). Asking
   for it would be asking for a feature the library has no other use for; and the alternative that
   needs no feature at all - binding a slice of one set - is not a shape the bind call has, because
   vkCmdBindDescriptorSets names sets and their count, with no argument that says which element of
   an array binding the set starts at. So the design is many sets of one sampler each rather than
   one set of a 16-deep array: the pipeline layout's set 1 binding is one descriptor (xud_descrip
   tor.h), XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE slot sets are allocated against that one layout, and
   the frame binds the set of the slot a draw samples, one bind per texture change rather than per
   draw. A stage that declares one sampler at a binding whose layout is one descriptor wide is the
   exact match, and every slot set is written before it is bound, so no update ever lands on a set
   the command buffer has already recorded.

   The two input locations name the vertex stage's outputs: location 0 is outUv and location 1 is
   outColor in shaders/solid.vert, and a frag input whose location names nothing the vertex stage
   writes is refused by the validation layer rather than forgiven, so the pair is a fact and not a
   convention (solid.frag reads location 1 for the colour and location 2 for the normal, which are
   the same two outputs). */

layout(push_constant) uniform XudPushRow {
    mat4 modelMatrix;    // 0  - the draw's own model matrix, column-major
    uint materialIndex;  // 64 - which material slot this draw uses (unread today)
    uint textureIndex;   // 68 - which entry of set 1's sampler array (unread today)
    uint flags;          // 72 - the variant this draw was flushed under
    uint reserved;       // 76 - keeps the row 16-aligned with no hole
    vec4 tint;           // 80 - the draw's own tint, 0..1 per channel
} xudPush;

layout(location = 0) in vec2 inUv;
layout(location = 1) in vec4 inColor;
layout(location = 0) out vec4 outColor;

layout(set = 1, binding = 0) uniform sampler2D xudSampler;

void main() { outColor = inColor * texture(xudSampler, inUv) * xudPush.tint; }
