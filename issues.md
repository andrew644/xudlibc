#Issues

Findings from a full read of `src/xud/` (and the shaders), checked against the
validation-layer-clean build and the GPU test run (4898 checks, 0 failures on the
developer's Intel iGPU). Nothing here is a test failure; these are latent
correctness risks, API-shape gaps against the raylib reference, and performance
costs the tests cannot see.

## Correctness

### 1. (RESOLVED) Render and present are unsynchronized when the two role queues differ

`xud_device.c:416` resolves the two role queues from the same family at index 0
(graphics) and index 1 (present), and the row asks for up to
`XUD_DEVICE_QUEUE_MAX_COUNT` = 2 (`xud_device.h:95`). `xud_frame.c:1131` then
submits the render to the graphics queue and calls `vkQueuePresentKHR` on the
present queue with no semaphore or fence relating the two. That is only correct
because on the developer's machine the chosen family exposes one queue, so both
roles are the same handle — the comment at `xud_frame.c:1122` says exactly this
("Both role queues are the same handle on this machine").

On a device whose selected family exposes two or more queues, the present can
run ahead of the render: two distinct `VkQueue` handles have no implicit
ordering between submissions, and a bare `vkQueuePresentKHR` waits on nothing.
The standard fix is a render-finished semaphore signalled by the graphics
submit and waited on by a no-command submit to the present queue (or an assert
at `xud_device_open` that the two handles are equal, which keeps the current
single-flight model honest).

**Resolved.** The render-finished semaphore is exactly the fix: `XudFrame` owns
a `renderFinished` semaphore, the frame's graphics submit signals it, and a
no-command submit on the present queue (`xud_frame_fill_present_wait`) waits it
before the `vkQueuePresentKHR`, so the present is ordered behind its render by
the queue's own FIFO even when the two role handles differ. On a family that
exposes one queue the wait submit is a no-op. The frame close waits each role
queue idle before destroying the semaphore, since the present-queue wait names
it and has no fence of its own. See `xud_frame.c` (the 5b step) and
`xud_frame.h` (`renderFinished`).

### 2. (RESOLVED) A minimized window makes `xud_end_drawing` fail every frame

Wayland minimize reports a 0x0 framebuffer. `xud_window_framebuffer_size`
(`xud_window.c:265`) refuses that as `XUD_INVALID_ARG`, `xud_renderer_resize`
propagates it, and `xud_end_drawing` (`xud_core.c`) returns it to the app for
every frame until the window is restored. The renderer is not damaged (the
refusal happens before its teardown half), but the frame loop has no way to
express "window has no presentable extent" — the app sees a bad-argument error
loop out of a call it made correctly. A minimal window should probably present
nothing (or hold the last frame) rather than error, and the resize rung should
treat 0x0 as "skip this frame" rather than "invalid argument".

**Resolved.** 0/0 is now the answer it always was: a state, not an error.
`xud_window_framebuffer_size()` returns it as `XUD_OK` (only a *negative*
extent is still refused), and the resize step decides through the new pure
rung `xud_renderer_recreate_needed()` — a 0/0 framebuffer (or a swapchain
already at the framebuffer's extent) clears the recreate ask and answers
`XUD_ALREADY` with no rung touched, so the swapchain holds the last frame
until the window has an extent back, and `xud_end_drawing()` answers `XUD_OK`
for every frame a frame loop spends with its window minimized. A restore to a
size the swapchain does not have re-sets the ask through the out-of-date code
the next acquire answers with.

### 3. The UI is not screen-fixed under a 3D camera, and its quads write depth

`xud_ui.c` maps glyph pixels into clip space on the assumption of identity
matrices (the mapper's own comment: "the y-up clip space the identity matrices
expect", `outPos[2] = 0.0f` at `xud_ui.c:70`). If the caller is inside
`xud_begin_camera_3d`, the UI is transformed by the camera's viewProj — the
overlay moves and scales with the camera instead of sticking to the screen.
raylib's GUI is always screen-fixed.

Separately, the one pipeline the renderer has always runs with
`depthTestEnable = VK_TRUE` and `depthWriteEnable = VK_TRUE`
(`xud_pipeline.c:238-240`) — there is no depth-off row. UI quads land at depth
0.0, the nearest the LESS compare admits, and write that depth back: any 3D
geometry appended in the same frame *after* the UI is occluded where the glyphs
were. Drawing the UI last (raylib's order) works, but the API gives no way to
draw screen-space, depth-bypassing UI, which is what the module promises.

## API gaps against the raylib reference

### 4. (RESOLVED) There is no way to close the window, and the exit key is dead state

`xud_set_exit_key` stores `input->exitKey` (`xud_input.c:557`) and nothing in
the tree ever reads it — no `IsWindowShouldClose`-equivalent query exists, and
no `glfwSetWindowCloseCallback` is registered, so the window-manager close
button does nothing either. An app built on the public API cannot exit cleanly
by user request. This is the single largest functional gap in the input module.

**Resolved.** The exit key is gone and the quit copies raylib's own shape:
`xud_window_should_close()` is the `WindowShouldClose`-equivalent (its window-manager
half only, through `xud_window_check_close()` above `glfwWindowShouldClose`), the
caller checks its own key with `xud_is_key_down()` and quits with
`xud_close_context()` (the `CloseWindow` stand-in — the library never exits a
process of its own, same as raylib). `xud_set_exit_key`/`xud_get_exit_key` and
`XudInput.exitKey` are removed. See `xud.h` (window section) and
`examples/breakout_3d.c`'s frame loop.

### 5. No background / clear colour control

The clear colour is hard-coded to (0.02, 0.02, 0.05) in `xud_renderpass.h:83-86`
and baked into the render pass's clear values. There is no
`ClearBackground`/`SetClearColor` equivalent in `xud.h`, so every app shares the
library's blue-grey and cannot change it without touching the pass.

### 6. No 2D drawing surface and no per-draw transforms

- There is no `xud_draw_texture`: a `XudTextureData` can only be seen through a
  material applied to a 3D shape or as a model's map. raylib's `DrawTexture`
  (a textured screen-space quad) has no counterpart.
- `xud_draw_mesh` and `xud_draw_model` take no position/rotation/scale arguments
  (raylib's `DrawMeshEx`/`DrawModelEx`). The push-constant `modelMatrix` is
  always identity (`xud_frame_fill_push_row`), so meshes and models can only be
  drawn at the origin in the identity pose. The push row's `materialIndex`,
  `textureIndex` and `flags` members are declared but, per the shader's own
  comment, "unread today".
- `xud_draw_text*` takes a `XudVector3` position, so text lives in camera space;
  there is no screen-space text.

These are probably the shape of the next steps rather than defects, but as the
library stands, the raylib-shaped programs (sprites, textured 2D, posed models)
cannot be written.

### 7. (RESOLVED) A context can hold at most ~15 live textures

`XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE` is 16 (`xud_descriptor.h`), slot 0 is
the white default, and the set layout is created once per context (the pipeline
layout freezes the slot width). Font atlases, the UI atlas, and every model
map share the same 15 slots, so a scene that loads a 16th distinct texture
gets `XUD_OUT_OF_RESOURCES` out of `xud_create_texture`. raylib apps routinely
hold far more; if this ceiling is intended it deserves a public constant in
`xud.h` and a doc note, since the failure mode is a texture load refusal
halfway through a game.

**Resolved.** The ceiling is intended (a wider slot axis is a pipeline-layout
recreate the "created once at init" rule forbids at runtime), and it is now
public: `XUD_MAX_TEXTURES` (15) in `xud.h` documents the live-texture ceiling
and its failure mode, `xud_create_texture`'s doc names it, and a
`static_assert` in `xud_descriptor.h` ties the public constant to
`XUD_DESCRIPTOR_SAMPLER_ARRAY_SIZE - 1` so the two cannot drift apart. The
number is unchanged; what changed is that a caller can now read it instead of
discovering it as a refusal.

### 8. Smaller parity gaps

- No `SetTextureFilter`/`SetTextureWrap` equivalent — filter and wrap are fixed
  at `xud_create_texture` and cannot be changed on a live texture (no
  `vkDestroySampler`/recreate path exists in `xud_texture.c`).
- No vsync or target-FPS control — `glfwSwapInterval` is never called, so every
  app runs at the GLFW default (vsync 1) with no way to change it.
- Audio has no volume or pitch surface (both are on the `todo.md` path; the
  sound node is created with `MA_SOUND_FLAG_NO_PITCH` and no mixer node).

## Performance

### 9. The frame loop is synchronous single-flight

`xud_frame_draw` waits on the frame fence with an infinite timeout
(`xud_frame.c:1144`) after every present, so the CPU never starts frame N+1
while the GPU is still drawing frame N. With a two-image swapchain the GPU is
always one frame behind the CPU and the two never overlap. This is correct,
and it is what makes the rest of the loop (one command buffer, one acquire
semaphore, per-image fences) simple, but it caps throughput at
max(CPU frame, GPU frame) and it is a structural decision worth recording:
moving to triple-buffering with per-frame semaphores is a rewrite of the frame
rung, not a patch.

### 10. (RESOLVED) The white default texture is re-cleared every frame

`xud_descriptor_upload` (`xud_descriptor.c`) ran every frame, and inside
it the constant 1x1 white image was transitioned to TRANSFER_DST, cleared with
`vkCmdClearColorImage` and transitioned back to SAMPLED — two barriers and a
clear for an image whose bytes never change after creation. The fill belongs
at bring-up (one command buffer, once) or the image could be created already
in SAMPLED layout with the clear recorded at open.

**Resolved.** The fill is now gated by a `whiteFilled` bit on the descriptor
record: the first frame that uploads records the two barriers and the clear
(the image starts in the SAMPLED layout the descriptor names, so it must move
to TRANSFER_DST and back), and every frame after the first skips all three
because the clear left the image in exactly the layout it ends in. The 1x1
white default is written once, not once per frame.

### 11. Every texture close stalls the whole device

`xud_texture_close` and `xud_texture_close_all` both run
`xud_device_wait_idle` (`xud_texture.c:1197, 1261`), and the allocator close
waits again. Closing N textures therefore takes N full device idles in series.
A per-texture fence (the texture's own upload fence is already the model for
this) would let closes proceed without idling in-flight frames from other
objects.

### 12. Input is one frame behind by design

`xud_input_poll` runs at the end of `xud_end_drawing`, after the present
(`xud_core.c`). This matches raylib's `EndDrawing` ordering and is documented,
but the consequence is worth stating: window events (WM close, resize) and key
presses only become visible on the frame *after* the one they happened in, and
`xud_begin_drawing` itself polls nothing. If issue 4 is fixed by polling a
window-close flag, that flag will also lag by a frame.

## Inconsistencies and minor issues

### 13. Two different window-extent bounds

The public `xud_init_context` refuses extents above `XUD_MAX_WINDOW_EXTENT`
= 16384 (`xud.h:114`), while the window and swapchain rungs accept up to
`XUD_WINDOW_MAX_EXTENT` = INT_MAX (`xud_window.h:104`). The header comment calls
16384 a "host-side sanity bound ... until the real gate exists" (the device's
`maxImageExtent2D` is recorded in the caps but never consulted), so today a
window the machine can present (many drivers allow 32768+) is refused as
`XUD_INVALID_ARG` by the public entry. Either wire the caps value into the
check or state the 16384 cap publicly.

### 14. (RESOLVED) `solid_textured.frag` is `#version 450`, the other two shaders 460

Cosmetic, and harmless at build time (glslang accepts both), but the three
shaders are compiled and validated in the same CMake step and should agree.

**Resolved.** `solid_textured.frag` is now `#version 460 core`, matching
`solid.vert` and `solid.frag`; all three compile and validate in the same
CMake step.

### 15. (RESOLVED) `xud_get_mouse_x`/`xud_get_mouse_y` truncate rather than round

`(int)input->cursorX` truncated toward zero, so a cursor at
pixel 4.9 reported 4. raylib rounds. One `floor(x + 0.5)`-style fix each.

**Resolved.** Both queries now land the cursor on the nearest pixel with
`round()` rather than truncating: `xud_get_mouse_x`/`y` apply `round()` to the
cursor before the integer conversion, so 4.9 reports 5 and -4.9 reports -5.

### 16. (RESOLVED) A model with an out-of-range material index silently takes a wrong material

`xud_model.c`: when a glTF primitive's material index is past
`materials_count`, the load clamped it to `materialCount - 1` — the *last*
successfully created material of that model — rather than refusing. A
corrupt or hostile file therefore drew with an arbitrary material.
`cgltf_validate` catches most of this upstream, but the fallback masked the
rest; refusing the file (the gate's style elsewhere) would be consistent.

**Resolved.** Both the check and the open now refuse such a document rather than
clamp: `xud_model_check` answers `XUD_INVALID_ARG` when a primitive's material
pointer sits past the document's material array, and `xud_model_open` does the
same at the point it would have clamped. The index is read as a pointer
difference against the materials array's base (well-defined one element past
its end) rather than through cgltf's own accessor, which asserts the very bound
being checked.

### 17. A batch that hits its growth bound refuses forever

`xud_batch_grow` answers `XUD_UNSUPPORTED` at the bound, `xud_begin_drawing`
breaks out of the grow loop and keeps the ask set, and every subsequent append
is refused (`xud_shapes_note_refused`). The frame still draws the content that
fits, but the app has no public query to ask *why* appends are failing or how
much capacity it is short by — the only way out is to shrink the scene. A
`xud_get_batch_stats`-style public query (the internal `xud_batch_stats`
already exists) would make the state diagnosable.
