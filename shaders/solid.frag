#version 460 core

/* solid.frag - the untextured-solid variant, first triangle.

   The interpolated colour is written straight to the swapchain image: no
   descriptor set, no push constant and no texture is read here, because
   the descriptor layout and the sampler pool are later items in todo.md. */

layout(location = 0) in vec4 inColor;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = inColor;
}
