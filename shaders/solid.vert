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
// The set-0 block IS read here now: the frame loop uploads it with xud_descriptor_upload() and binds
// set 0 with vkCmdBindDescriptorSets, so the matrix the vertex is placed by is a matrix that holds
// something rather than a buffer that was never written. The block's member order and types are the
// C layout of XudFrameUniformRow read against std140: mat4 at 0 (64 B), vec3 at 64, float at 76, vec4
// at 80 and 96 (both already 16-aligned), uint at 112 and 116 - which is byte-for-byte the struct in
// xud_descriptor.h, and the assert beside that struct is the same number from the other side.
//
// The push-constant model matrix IS read here now: the frame loop pushes it with vkCmdPushConstants
// beside every draw it records, so the vertex is placed by frame-viewProj * the draw's own model. The
// block's member order and widths are the C layout of XudPushRow read against std140: mat4 at 0 (64
// B), uint at 64, 68, 72 and 76 - 80 B, byte-for-byte the struct in xud_descriptor.h and the assert
// beside XUD_DESCRIPTOR_PUSH_SIZE is the same number from the other side. Only modelMatrix is read:
// a stage need not touch every member of a block its layout declares, and the material/texture/flags
// members are the channel the material, texture and variant steps above will fill.

layout(binding = 0, std140) uniform XudFrameBlock {
    mat4 viewProj;    // 0  - the VP matrix, column-major, the same 64 B the push row carries
    vec3 cameraPos;   // 64
    float time;       // 76
    vec4 fog;         // 80  - rgb + density
    vec4 ambient;     // 96  - rgb + intensity
    uint lightCount;  // 112
    uint reserved;    // 116
} xudFrame;

layout(push_constant) uniform XudPushRow {
    mat4 modelMatrix;    // 0  - the draw's own model matrix, column-major
    uint materialIndex;  // 64 - which material slot this draw uses (unread today)
    uint textureIndex;   // 68 - which entry of set 1's sampler array (unread today, set 1 unbound)
    uint flags;          // 72 - the variant this draw was flushed under (unread today)
    uint reserved;       // 76 - keeps the row 16-aligned with no hole
} xudPush;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec4 inColor;
layout(location = 3) in vec3 inNormal;

layout(location = 0) out vec2 outUv;
layout(location = 1) out vec4 outColor;
layout(location = 2) out vec3 outNormal;

void main(void)
{
    gl_Position = xudFrame.viewProj * xudPush.modelMatrix * vec4(inPos, 1.0f);
    outUv = inUv;
    outColor = inColor;
    outNormal = inNormal;
}
