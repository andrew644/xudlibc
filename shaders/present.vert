#version 460 core

/* present.vert - the resolve pass's vertex stage: one full-screen triangle, built from
   gl_VertexIndex alone.

   The triangle covers the whole viewport from three indices and no vertex buffer: the three rows
   (0,0), (2,0), (0,2) of the (clip.xy*0.5+0.5) square span the [-1,1] clip square with one edge
   hanging off each side, and the rasterizer clips what lies outside. A fullscreen QUAD would be
   four vertices and two triangles whose shared diagonal splits the gradients; three vertices and
   one triangle cannot have a seam. This is why the pass draws with vkCmdDraw(3,1,0,0) and why the
   resolve pipeline carries ZERO vertex bindings - the only input this stage reads is the vertex
   index the command itself invents.

   outUv is gl_Position's own xy rescaled to [0,1], with NO vertical flip: in Vulkan the framebuffer
   row 0 is ndc y = -1 and a texture's texel row 0 is the attachment's framebuffer row 0, so the
   identity map is the correct sampling map, and a flip here would sample the target upside down
   (the same fact the batch's own orthographic UI path records in xud_matrix.c's projection note). */

layout(location = 0) out vec2 outUv;

void main()
{
    vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
    outUv = corner;
}
