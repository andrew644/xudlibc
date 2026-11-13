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
// block's member order and widths are the C layout of XudPushRow read against std140: mat4 at 0 (64 B),
// uint at 64, 68, 72 and 76, vec4 at 80 - 96 B, byte-for-byte the struct in xud_descriptor.h and the
// assert beside XUD_DESCRIPTOR_PUSH_SIZE is the same number from the other side. The materialIndex and
// textureIndex members are unread: set 0's array is chosen by the frame loop's rebind beside the
// draw, not by a read in this stage. The flags member IS read now: it carries the batch's variant
// the draw was flushed under, and the UI column (XUD_BATCH_VARIANT_UI, xud_batch.h) is the one whose
// position is clip space already - the ui rung's mapper emits it there and the column's pipeline row
// turns depth test and write off - so this stage must skip the per-frame viewProj for that draw
// rather than let the caller's 3D camera move the screen-fixed overlay.

layout(binding = 0, std140) uniform XudFrameBlock {
    mat4 viewProj;    // 0  - the VP matrix, column-major, the same 64 B the push row carries
    vec3 cameraPos;   // 64
    float time;       // 76
    vec4 fog;         // 80  - rgb + density
    vec4 ambient;     // 96  - rgb + intensity
    mat4 lightViewProj; // 112 - the shadow-casting sun's VP: read by the fragment stage and the
                        //     shadow vertex, never here, declared so the offsets below stay true
    vec4 shadowParams;  // 176 - texel size, compare bias, enable - unread here, same reason
    uint lightCount;    // 192
    uint reserved;      // 196
} xudFrame;

layout(push_constant) uniform XudPushRow {
    mat4 modelMatrix;    // 0  - the draw's own model matrix, column-major
    uint materialIndex;  // 64 - which material slot this draw uses (unread today)
    uint textureIndex;   // 68 - which entry of set 0's sampler array (unread by the solid column, which samples no texture)
    uint flags;          // 72 - the variant this draw was flushed under (the UI column skips viewProj)
    uint reserved;       // 76 - keeps the row 16-aligned with no hole
    vec4 tint;           // 80 - the draw's own tint: the fragment stage multiplies it in, this stage only carries it
} xudPush;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec4 inColor;
layout(location = 3) in vec3 inNormal;

/* The instance pair (S3.2): bindings 4 and 5 of xud_batch.h, declared here as one mat4 and one
   vec4 - the mat4 consumes locations 4..7, exactly the four attribute rows fill_attribute writes
   one column apiece, and the GLSL mat4 is what reassembles them. READ ONLY when the push row's
   instance bit is set; for a push-placed draw the row still loads, and its bytes are whatever the
   last frame left in instance row 0, which is precisely why the branch below must not read it. */
layout(location = 4) in mat4 inInstanceMatrix;
layout(location = 8) in vec4 inInstanceTint;
layout(location = 0) out vec2 outUv;
layout(location = 1) out vec4 outColor;
layout(location = 2) out vec3 outNormal;
/* The fragment stage's point and spot lights shade by DISTANCE, and a distance needs the
   fragment's world position: the draw's model matrix applied to the vertex, the same space the
   light table's positions live in. Unused by the textured and UI fragment stages, which discard
   it. */
layout(location = 3) out vec3 outWorldPos;
/* The tint reaches the fragment stages through THIS stage now rather than through their own
   read of the push row: for an instanced draw the tint is per COPY and lives in the instance
   stream, and the fragment stage has no way to name the copy - the vertex stage is the only
   stage that ever sees both the instance index and the fragment's future pixels. */
layout(location = 4) out vec4 outTint;

void main(void)
{
    /* Vulkan's clip space is Y-DOWN, the NDC the viewProj and the identity-space draws author
       is Y-UP: flip the Y once, here, at the last point before the rasteriser. Culling is
       disabled (xud_pipeline.h's XUD_PIPELINE_CULL_MODE) so the flip costs no winding change. */
    /* XUD_BATCH_VARIANT_UI (xud_batch.h) and XUD_PUSH_FLAG_INSTANCED / XUD_PUSH_VARIANT_MASK
       (xud_descriptor.h), restated here: the shader and the C axis are two languages, so the
       numbers the branches key on are spelled in both and a rename must touch both. The variant
       lives in the low two bits - four variants - and the instance flag is bit 3 above them, so
       one word carries "which column flushed this" and "who places it" at once.

       A draw flushed on the UI column carries a position that is clip space already (the ui
       rung's mapper emits it in the y-up NDC the identity matrices expect), so placing it through
       the frame's viewProj would move and scale the overlay with the caller's 3D camera -
       raylib's GUI is always screen-fixed, and this skip is what makes that true here. The model
       matrix still applies: identity for a plain screen quad, and the channel a screen-space
       transform of a later step can hand in without a second pipeline.

       The arena-vs-mesh branch: an instanced row's matrix and tint ARE the instance attributes -
       that is the whole of S3.2, thousands of copies placed by the rasteriser rather than by one
       push each - and the push row's members for that draw are the identity/white the batch
       stamped. A push-placed row reads the push and must NOT multiply in the instance bytes,
       which for it are last frame's stale row 0. One multiply per vertex either way. */
    const uint xudPushFlagVariantUI = 3u;
    const uint xudPushVariantMask = 3u;
    const uint xudPushFlagInstanced = 8u;
    bool instanced = (xudPush.flags & xudPushFlagInstanced) != 0u;
    mat4 model = instanced ? inInstanceMatrix : xudPush.modelMatrix;
    vec4 tint = instanced ? inInstanceTint : xudPush.tint;
    vec4 localPos = model * vec4(inPos, 1.0f);
    vec4 clipPos = ((xudPush.flags & xudPushVariantMask) == xudPushFlagVariantUI) ? localPos : (xudFrame.viewProj * localPos);
    gl_Position = vec4(clipPos.x, -clipPos.y, clipPos.z, clipPos.w);
    outUv = inUv;
    outColor = inColor;
    outNormal = (model * vec4(inNormal, 0.0f)).xyz;
    outWorldPos = localPos.xyz;
    outTint = tint;
}
