#version 460 core

/* solid.frag - the untextured-solid variant.

   The interpolated colour is written straight to the swapchain image: no
   descriptor set, no push constant and no texture is read here, because
   the descriptor layout and the sampler pool are later items in todo.md.

   The colour is read at location 1, which is the location xud_batch_fill_attribute gives the
   COLOR stream's attribute and the vertex stage's outColor the location that follows UV. A
   frag input's location must name a vertex output, and the validation layer refuses one that
   does not, so the pair is a fact rather than a convention. */

layout(location = 1) in vec4 inColor;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = inColor;
}
