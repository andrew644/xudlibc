#version 460 core
// prepass.vert - the vertex stage of the depth pre-pass, built from the batch's position stream
//
// Owns: the sole stage of the pre-pass pipeline: the colorless draw list that fills the HDR
// depth buffer before the shaded pass replays it with an equal-or-nearer test.
// Layer: shaders. Compiled at build time by CMake into build/gen/xud_solid_spv.h's
// xudPrepassVertSpv - never at runtime.
//
// This stage computes ONE thing and stops: gl_Position, bit-for-bit what solid.vert computes for
// the same vertex - same block, same push row, same Y-flip, same UI skip - because the pre-pass
// and the shaded pass must rasterize the SAME triangles into the SAME depth values, and a depth
// that differs by one bit turns the shaded pass's GEQUAL into a discard. The uv, colour, normal
// and world-position outputs, the whole fragment stage, the light block reads and the tint are
// what the pre-pass exists to skip: a hidden surface should pay a vertex transform and nothing
// else. The frame loop's pre-pass replay skips the TRANSPARENT and UI columns, which is why the
// UI branch is carried here anyway: a draw flushed on those columns never arrives, but the one
// number the position branch keys on stays stated in both languages, and the flags push row is
// what keeps the clip-space UI positions from being run through viewProj if a later step ever
// replays a wider list.
//
// The block and push-row member orders are the std140 layouts of XudFrameUniformRow and
// XudPushRow (xud_descriptor.h) - the two asserts beside those structs are the same numbers
// from the other side, and this stage reads only viewProj and modelMatrix out of them.

layout(binding = 0, std140) uniform XudFrameBlock {
    mat4 viewProj;    // 0  - the VP matrix, column-major, the same 64 B the shaded vertex stage reads
    vec3 cameraPos;   // 64
    float time;       // 76
    vec4 fog;         // 80  - rgb + density
    vec4 ambient;     // 96  - rgb + intensity
    mat4 lightViewProj; // 112 - the shadow-casting sun's VP - unread here, declared so the offsets
                        //     below stay true against the C block
    vec4 shadowParams;  // 176 - texel size, compare bias, enable - unread here, same reason
    uint lightCount;    // 192
    uint reserved;      // 196
} xudFrame;

layout(push_constant) uniform XudPushRow {
    mat4 modelMatrix;    // 0  - the draw's own model matrix, column-major
    uint materialIndex;  // 64 - unread here
    uint textureIndex;   // 68 - unread here
    uint flags;          // 72 - the variant this draw was flushed under (the UI column skips viewProj)
    uint reserved;       // 76 - keeps the row 16-aligned with no hole
    vec4 tint;           // 80 - unread here
} xudPush;

layout(location = 0) in vec3 inPos;

void main(void)
{
    /* The three lines solid.vert's placement boils down to once the outputs it exists to feed
       are gone: model, view-projection, Y-flip - identical arithmetic, identical gl_Position. */
    const uint xudPushFlagVariantUI = 3u;
    vec4 localPos = xudPush.modelMatrix * vec4(inPos, 1.0f);
    vec4 clipPos = (xudPush.flags == xudPushFlagVariantUI) ? localPos : (xudFrame.viewProj * localPos);
    gl_Position = vec4(clipPos.x, -clipPos.y, clipPos.z, clipPos.w);
}
