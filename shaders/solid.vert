#version 460 core
// xud_solid_spv.h - the vertex stage of the solid pipeline, built from the batch's attribute streams
//
// Owns: the vertex stage of the one solid pipeline the renderer builds.
// Layer: shaders. Compiled at build time by CMake into build/gen/xud_solid_spv.h - never at runtime.
//
// The position is no longer a hard-coded triangle off gl_VertexIndex: it is read from the batch's
// four SoA streams, at the locations xud_batch_fill_attribute writes and xud_pipeline_fill_vertex_input
// hands the pipeline. The batch's index buffer is what picks which vertices a draw reads, so the
// index is the draw's, not this stage's.
//
// The push-constant model matrix and the set-0 viewProj are NOT read here yet: the frame loop has to
// bind both before a stage can read them, and that binding is the next step's half. Until it lands,
// the geometry stays in clip space and the pipeline layout's push range and set layouts stay declared
// but unread, which is legal - a shader need not touch everything its layout declares.

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec4 inColor;
layout(location = 3) in vec3 inNormal;

layout(location = 0) out vec2 outUv;
layout(location = 1) out vec4 outColor;
layout(location = 2) out vec3 outNormal;

void main(void)
{
    gl_Position = vec4(inPos, 1.0f);
    outUv = inUv;
    outColor = inColor;
    outNormal = inNormal;
}
