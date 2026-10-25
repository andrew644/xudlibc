#version 460 core

/* solid.vert - the untextured-solid variant, first triangle.

   The three vertices come from gl_VertexIndex rather than from a vertex
   buffer: the allocator rung that would own a VkBuffer is not in the tree
   yet (see todo.md), and the SoA attribute streams land with the batch.
   The pipeline's vertex-input state therefore declares zero bindings and
   zero attributes, which is a valid Vulkan pipeline. */

const vec2 POS[3] = vec2[3](vec2(-0.5f, -0.5f), vec2(0.5f, -0.5f), vec2(0.0f, 0.5f));
const vec4 COL[3] = vec4[3](
    vec4(1.0f, 0.0f, 0.0f, 1.0f), vec4(0.0f, 1.0f, 0.0f, 1.0f), vec4(0.0f, 0.0f, 1.0f, 1.0f));

layout(location = 0) out vec4 outColor;

void main() {
    gl_Position = vec4(POS[gl_VertexIndex], 0.0f, 1.0f);
    outColor = COL[gl_VertexIndex];
}
