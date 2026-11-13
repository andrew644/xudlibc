#version 460 core
// shadow.vert - the vertex stage of the shadow pass, built from the batch's position stream
//
// Owns: the sole stage of the shadow pipeline: the colorless draw list the casting sun sees,
// rasterized into the shadow map's depth before any surface is shaded by it.
// Layer: shaders. Compiled at build time by CMake into build/gen/xud_solid_spv.h's
// xudShadowVertSpv - never at runtime.
//
// One member separates this stage from prepass.vert, and it is the whole feature: the matrix is
// the SUN's lightViewProj, not the camera's viewProj. Everything else is the same arithmetic on
// the same position stream, because a shadow is only trustworthy when the occluder's depth comes
// from the same triangles the camera pass shades - same vertices, same Y-flip, same clip test.
// The Y-flip matters twice over: the shadow map's texel rows run the same direction as the
// pre-pass's framebuffer rows the fragment stage already trusts, and the fragment stage that
// samples this map flips its lookup the same way - the two flips are one convention spelled in
// two files, and only the light's matrix differs.
//
// What is NOT here is the camera's UI escape: the shadow pass replays only the TRIANGLES rows of
// the OPAQUE and TEXTURED columns (xud_frame.c's pre-pass gate, the shadow list reuses), so no
// clip-space position ever reaches this stage, and the push row's flags field is read only to
// say so - a UI draw that somehow arrived would be drawn through the sun's matrix and clipped
// away rather than poisoning the map.
//
// The block and push-row member orders are the std140 layouts of XudFrameUniformRow and
// XudPushRow (xud_descriptor.h) - the two asserts beside those structs are the same numbers
// from the other side, and this stage reads only lightViewProj out of the block.

layout(binding = 0, std140) uniform XudFrameBlock {
    mat4 viewProj;      // 0   - the camera's VP: unread here, declared so the offsets below stay true
    vec3 cameraPos;     // 64
    float time;         // 76
    vec4 fog;           // 80  - rgb + density
    vec4 ambient;       // 96  - rgb + intensity
    mat4 lightViewProj; // 112 - THE matrix: the casting sun's view-projection, column-major
    vec4 shadowParams;  // 176 - texel size, compare bias, enable - a fragment-stage question
    uint lightCount;    // 192
    uint reserved;      // 196
} xudFrame;

layout(push_constant) uniform XudPushRow {
    mat4 modelMatrix;    // 0  - the draw's own model matrix, column-major
    uint materialIndex;  // 64 - unread here
    uint textureIndex;   // 68 - unread here
    uint flags;          // 72 - the variant this draw was flushed under (read to be stated, not to branch)
    uint reserved;       // 76 - keeps the row 16-aligned with no hole
    vec4 tint;           // 80 - unread here
} xudPush;

layout(location = 0) in vec3 inPos;

void main(void)
{
    /* Sun, not camera: world space goes through the casting sun's view-projection and the same
       Y-flip the camera passes apply, and gl_Position is the texel-addressable depth this map
       exists to store. Ortho means w is 1 and the z this writes is the reversed linear depth the
       map's GREATER_OR_EQUAL compare reads directly - no divide, no remap, nothing that could
       the occluder's depth disagree with the fragment's own lookup of it. */
    vec4 worldPos = xudPush.modelMatrix * vec4(inPos, 1.0f);
    vec4 clipPos = xudFrame.lightViewProj * worldPos;
    gl_Position = vec4(clipPos.x, -clipPos.y, clipPos.z, clipPos.w);
}
